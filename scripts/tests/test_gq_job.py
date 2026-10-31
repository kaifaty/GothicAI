"""Shell submit/wait integration; the fake never touches a queue or systemd."""
from __future__ import annotations

import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

import pytest


HELPER = Path(__file__).resolve().parents[1] / "gq-job.sh"
JOB_ID = "012345abcdef"
ARGS = ["--class", "quiet", "--threads", "3", "--mem", "4G", "--gpu-mem", "2G",
        "--display", "--cpu-quota", "200%", "--max", "30m", "--name", "test phase",
        "--", "env", "PHASE=value with spaces", "printf", "%s\\n", "literal $value"]

FAKE_GQ = r'''
import json
import os
from pathlib import Path
import signal
import sys
import time

root = Path(os.environ["FAKE_GQ_HOME"])
args = sys.argv[1:]
with (root / "calls.jsonl").open("a") as output:
    output.write(json.dumps({"args": args, "cwd": os.getcwd(),
                             "owner": os.environ.get("GQ_OWNER"),
                             "env": os.environ.get("TEST_ENV"),
                             "pid": os.getpid(),
                             "parent": os.getppid()}) + "\n")
if args[0] == "submit":
    if os.environ.get("FAKE_SUBMIT_CODE"):
        sys.exit(int(os.environ["FAKE_SUBMIT_CODE"]))
    if os.environ.get("FAKE_SUBMIT_PAUSE"):
        (root / "submitting").write_text("1")
        time.sleep(float(os.environ["FAKE_SUBMIT_PAUSE"]))
    print(os.environ.get("FAKE_SUBMIT_ID", "012345abcdef"))
elif args[0] == "wait":
    # The helper must protect wait from group signals while it cancels the job.
    # Like real gq wait, this fake installs no signal handlers of its own.
    (root / "waiting").write_text(str(os.getppid()))
    if os.environ.get("FAKE_WAIT_BLOCK"):
        deadline = time.monotonic() + 10
        while not (root / "cancelled").exists():
            if time.monotonic() > deadline:
                sys.exit(99)
            time.sleep(0.01)
    print("job stdout", flush=True)
    print("job stderr", file=sys.stderr, flush=True)
    sys.exit(int(os.environ.get("FAKE_WAIT_CODE", "0")))
elif args[0] == "cancel":
    attempts = sum(json.loads(line)["args"][0] == "cancel"
                   for line in (root / "calls.jsonl").read_text().splitlines())
    (root / f"cancelling-{attempts}").write_text(str(os.getpid()))
    if os.environ.get("FAKE_CANCEL_BLOCK") and attempts == 1:
        deadline = time.monotonic() + 10
        while not (root / "release-cancel").exists():
            if time.monotonic() > deadline:
                sys.exit(97)
            time.sleep(0.01)
    if os.environ.get("FAKE_CANCEL_FAIL_ONCE") and attempts == 1:
        (root / "cancel-failed").write_text("1")
        sys.exit(42)
    (root / "cancelled").write_text(args[1])
else:
    sys.exit(98)
'''


@pytest.fixture
def host(tmp_path):
    root = tmp_path / "fake"
    root.mkdir()
    home = tmp_path / "home"
    work = home / "gothic-ai" / "nav-dagger" / "code"
    work.mkdir(parents=True)
    binary = root / "fake-gq"
    binary.write_text(f"#!{sys.executable}\n" + FAKE_GQ)
    binary.chmod(0o755)
    env = {**os.environ, "HOME": str(home), "GQ_BIN": str(binary),
           "FAKE_GQ_HOME": str(root), "TEST_ENV": "inherited value"}
    env.pop("GQ_OWNER", None)
    env.pop("GQ_JOB_ID_FILE", None)
    return {"root": root, "home": home, "work": work, "binary": binary, "env": env}


def command(body='gq_job "$@"'):
    return ["bash", "-c", 'source "$1"; shift; ' + body, "test-gq-job", str(HELPER), *ARGS]


def calls(host):
    path = host["root"] / "calls.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


def run(host, *, body='gq_job "$@"', cwd=None, env=None):
    return subprocess.run(command(body), cwd=cwd or host["work"],
                          env=env or host["env"], stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=10)


def test_submit_wait_preserves_arguments_environment_and_cwd_at_eof(host):
    result = run(host)
    assert result.returncode == 0, result.stderr
    assert result.stdout == "job stdout\n"
    assert "job stderr" in result.stderr
    assert f"submitted {JOB_ID} (nav-dagger)" in result.stderr
    records = calls(host)
    assert [record["args"] for record in records] == [["submit", *ARGS], ["wait", JOB_ID]]
    assert all(record["owner"] == "nav-dagger" for record in records)
    assert all(record["env"] == "inherited value" for record in records)
    assert all(record["cwd"] == str(host["work"]) for record in records)


def test_repeated_archive_options_reach_submit_unchanged(host):
    first, second = host["work"] / "records", host["work"] / "weights"
    options = ["--archive", str(first), "--archive", str(second)]
    args = command()
    separator = args.index("--")
    args[separator:separator] = options
    result = subprocess.run(args, cwd=host["work"], env=host["env"],
                            stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    expected = list(ARGS)
    expected[expected.index("--"):expected.index("--")] = options
    assert calls(host)[0]["args"] == ["submit", *expected]


@pytest.mark.parametrize("exit_code", [0, 17, 143])
def test_returns_wait_exit_status(host, exit_code):
    host["env"]["FAKE_WAIT_CODE"] = str(exit_code)
    assert run(host).returncode == exit_code


def test_explicit_owner_allows_submission_outside_task_folder(host):
    host["env"]["GQ_OWNER"] = "body-other-session"
    result = run(host, cwd=host["root"])
    assert result.returncode == 0, result.stderr
    assert all(record["owner"] == "body-other-session" for record in calls(host))


@pytest.mark.parametrize("relative", ["", "gothic-ai", "gothic-ai/bin", "gothic-ai/queue"])
def test_missing_task_owner_fails_before_submit(host, relative):
    cwd = host["home"] / relative
    cwd.mkdir(parents=True, exist_ok=True)
    result = run(host, cwd=cwd)
    assert result.returncode == 2
    assert "set GQ_OWNER" in result.stderr
    assert calls(host) == []


def test_job_id_file_contains_only_submitted_id(host):
    id_file = host["root"] / "job-id"
    host["env"]["GQ_JOB_ID_FILE"] = str(id_file)
    assert run(host).returncode == 0
    assert id_file.read_text() == JOB_ID + "\n"


def test_submit_failure_does_not_wait_or_cancel(host):
    host["env"]["FAKE_SUBMIT_CODE"] = "42"
    assert run(host).returncode == 42
    assert [record["args"][0] for record in calls(host)] == ["submit"]


def test_invalid_submit_id_does_not_wait_or_cancel(host):
    host["env"]["FAKE_SUBMIT_ID"] = "invalid id"
    result = run(host)
    assert result.returncode == 1
    assert "invalid job id" in result.stderr
    assert [record["args"][0] for record in calls(host)] == ["submit"]


def test_default_binary_path_and_gq_available(host):
    binary = host["home"] / "gothic-ai" / "bin" / "gq"
    binary.parent.mkdir()
    binary.symlink_to(host["binary"])
    host["env"].pop("GQ_BIN")
    result = run(host, body='gq_available && gq_job "$@"')
    assert result.returncode == 0, result.stderr
    binary.unlink()
    assert run(host, body="gq_available").returncode == 1


def test_unavailable_override_fails_without_submitting(host):
    host["env"]["GQ_BIN"] = str(host["root"] / "absent")
    result = run(host)
    assert result.returncode == 127
    assert calls(host) == []


def wait_for_file(process, path):
    deadline = time.monotonic() + 5
    while not path.exists() or not path.read_text():
        assert process.poll() is None
        assert time.monotonic() < deadline, f"fake gq did not create {path.name}"
        time.sleep(0.01)


@pytest.mark.parametrize("signum", [signal.SIGINT, signal.SIGTERM, signal.SIGHUP])
@pytest.mark.parametrize("signal_group", [False, True], ids=["launcher-pid", "process-group"])
def test_signals_cancel_own_job_and_return_wait_status(host, signum, signal_group):
    host["env"].update(FAKE_WAIT_BLOCK="1", FAKE_WAIT_CODE="73")
    process = subprocess.Popen(command(),
                               cwd=host["work"], env=host["env"],
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        waiting = host["root"] / "waiting"
        wait_for_file(process, waiting)
        helper_pid = int(waiting.read_text())
        assert helper_pid == process.pid  # The helper traps in the launcher shell.
        if signal_group:
            os.killpg(process.pid, signum)
        else:
            process.send_signal(signum)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 73, stderr
        assert stdout == "job stdout\n"
        assert [record["args"] for record in calls(host)] == [
            ["submit", *ARGS], ["wait", JOB_ID], ["cancel", JOB_ID]]
        assert (host["root"] / "cancelled").read_text() == JOB_ID
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


@pytest.mark.parametrize("signum", [signal.SIGINT, signal.SIGTERM, signal.SIGHUP])
def test_repeated_group_signals_allow_in_flight_cancel_to_finish(host, signum):
    host["env"].update(FAKE_WAIT_BLOCK="1", FAKE_WAIT_CODE="73", FAKE_CANCEL_BLOCK="1")
    process = subprocess.Popen(command(), cwd=host["work"], env=host["env"],
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        wait_for_file(process, host["root"] / "waiting")
        os.killpg(process.pid, signum)
        cancelling = host["root"] / "cancelling-1"
        wait_for_file(process, cancelling)
        # Both later signals arrive before the fake persists its cancellation.
        os.killpg(process.pid, signum)
        time.sleep(0.03)
        os.killpg(process.pid, signum)
        assert process.poll() is None
        assert not (host["root"] / "cancelled").exists()
        (host["root"] / "release-cancel").write_text("1")
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 73, stderr
        assert stdout == "job stdout\n"
        assert [record["args"] for record in calls(host)] == [
            ["submit", *ARGS], ["wait", JOB_ID], ["cancel", JOB_ID]]
        assert (host["root"] / "cancelled").read_text() == JOB_ID
        assert "could not cancel" not in stderr
        for record in calls(host):
            with pytest.raises(ProcessLookupError):
                os.kill(record["pid"], 0)
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


@pytest.mark.parametrize("signal_during_cancel", [False, True], ids=["after-failure", "in-flight"])
def test_failed_cancel_can_be_retried_by_another_signal(host, signal_during_cancel):
    host["env"].update(FAKE_WAIT_BLOCK="1", FAKE_WAIT_CODE="73", FAKE_CANCEL_FAIL_ONCE="1")
    if signal_during_cancel:
        host["env"]["FAKE_CANCEL_BLOCK"] = "1"
    process = subprocess.Popen(command(), cwd=host["work"], env=host["env"],
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        wait_for_file(process, host["root"] / "waiting")
        os.killpg(process.pid, signal.SIGINT)
        marker = "cancelling-1" if signal_during_cancel else "cancel-failed"
        wait_for_file(process, host["root"] / marker)
        assert not (host["root"] / "cancelled").exists()
        os.killpg(process.pid, signal.SIGINT)
        if signal_during_cancel:
            (host["root"] / "release-cancel").write_text("1")
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 73, stderr
        assert stdout == "job stdout\n"
        assert [record["args"] for record in calls(host)] == [
            ["submit", *ARGS], ["wait", JOB_ID], ["cancel", JOB_ID], ["cancel", JOB_ID]]
        assert (host["root"] / "cancelled").read_text() == JOB_ID
        assert stderr.count("could not cancel") == 1
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


def test_function_does_not_replace_caller_traps(host):
    result = run(host, body='trap "printf caller-trap\\\\n" TERM; before=$(trap -p TERM); '
                           'gq_job "$@"; [[ "$(trap -p TERM)" == "$before" ]]')
    assert result.returncode == 0, result.stderr


@pytest.mark.parametrize("submit_code", ["0", "42"])
def test_function_restores_all_caller_traps_and_owner(host, submit_code):
    if submit_code != "0":
        host["env"]["FAKE_SUBMIT_CODE"] = submit_code
    result = run(host, body='trap ": caller-int" INT; trap ": caller-term" TERM; '
                           'trap ": caller-hup" HUP; before=$(trap -p INT TERM HUP); '
                           'gq_job "$@"; result=$?; '
                           '[[ "$result" == "' + submit_code + '" && '
                           '"$(trap -p INT TERM HUP)" == "$before" && ! ${GQ_OWNER+x} ]]')
    assert result.returncode == 0, result.stderr


def test_pid_only_signal_during_submit_cancels_after_id_arrives(host):
    host["env"].update(FAKE_SUBMIT_PAUSE="0.3", FAKE_WAIT_BLOCK="1", FAKE_WAIT_CODE="73")
    process = subprocess.Popen(command(), cwd=host["work"], env=host["env"],
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        wait_for_file(process, host["root"] / "submitting")
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 73, stderr
        assert stdout == "job stdout\n"
        assert [record["args"] for record in calls(host)] == [
            ["submit", *ARGS], ["cancel", JOB_ID], ["wait", JOB_ID]]
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)


@pytest.mark.parametrize("launcher", ["nav-drive-batch.sh", "nav-demos-batch.sh",
    "body-rl-ppo-loop.sh", "body-rl-ppo-loop.sh:update", "body-rl-ppo-eval.sh",
    "body-stage9-duels.sh"])
def test_pid_only_sigterm_cancels_job_from_real_launcher(host, launcher):
    host["env"].update(FAKE_WAIT_BLOCK="1", FAKE_WAIT_CODE="73")
    scripts = HELPER.parent
    script, _, phase = launcher.partition(":")
    arguments = []
    if script.startswith("nav-"):
        binary_dir = host["root"] / "bin"
        binary_dir.mkdir()
        uname = binary_dir / "uname"
        uname.write_text("#!/bin/sh\necho Linux\n")
        uname.chmod(0o755)
        python = binary_dir / "python3"
        python.write_text(f"#!{sys.executable}\nimport os, sys\n"
                          'if "--list-indices" in sys.argv: print("0 1")\n'
                          'else: os.execv(sys.executable, [sys.executable, *sys.argv[1:]])\n')
        python.chmod(0o755)
        host["env"]["PATH"] = str(binary_dir) + os.pathsep + host["env"]["PATH"]
        arguments = ["--runner", "fake-runner"]
        if script == "nav-demos-batch.sh":
            arguments.insert(0, "collect")
    elif script.startswith("body-rl-ppo"):
        host["env"].update(PPO_CODE=str(scripts.parent), PPO_RUNNER="fake-runner",
                           PPO_WORK=str(host["work"]), PPO_START="fake.pt",
                           PPO_REFERENCE="reference.pt", PPO_POOL="peer.pt", PPO_ITERATIONS="1")
        if phase == "update":
            # Resume with a critic checkpoint, as the server's update phase
            # does; keep arrays populated under macOS's Bash 3 with set -u.
            host["env"].update(PPO_UPDATE_ARGS="--device cuda --policy-epochs 0", PPO_ITERATIONS="2")
            update = host["work"] / "iter-001" / "update"
            update.mkdir(parents=True)
            (update / "best.pt").write_text("checkpoint")
            collect = host["work"] / "iter-002" / "collect"
            collect.mkdir(parents=True)
            (collect / "summary.json").write_text("{}")
        if script == "body-rl-ppo-eval.sh":
            arguments = ["policy", "fake.pt", str(host["work"] / "eval")]
    else:
        config = host["work"] / "duels.json"
        config.write_text("{}")
        host["env"].update(DUELS_CODE=str(scripts.parent), DUELS_WORK=str(host["work"]),
                           DUELS_CONFIG=str(config), DUELS_MODELS="bc")
    process = subprocess.Popen(["bash", str(scripts / script), *arguments],
                               cwd=host["work"], env=host["env"],
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        wait_for_file(process, host["root"] / "waiting")
        assert int((host["root"] / "waiting").read_text()) == process.pid
        process.send_signal(signal.SIGTERM)
        stdout, stderr = process.communicate(timeout=5)
        assert process.returncode == 73, stderr + stdout
        records = calls(host)
        assert [record["args"][0] for record in records] == ["submit", "wait", "cancel"]
        assert records[-1]["args"] == ["cancel", JOB_ID]
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)
