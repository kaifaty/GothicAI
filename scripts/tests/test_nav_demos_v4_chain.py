"""Queue continuations are finite, resource-bounded and always resume safely."""
from copy import deepcopy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

import pytest

SPEC = importlib.util.spec_from_file_location("nav_demos_v4_chain",
    Path(__file__).resolve().parents[1] / "nav_demos_v4_chain.py")
chain = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(chain)


def summary(count, *, complete=False, **family):
    return dict(episodes=[dict(id=f"saved-{index}") for index in range(count)],
                complete=complete, by_family=dict(point=dict(episodes=count, complete=complete, **family)))


def plan(**changes):
    return dict(point_limit=360, forecast_bytes=14_500_000_000, used_bytes=6_560_000_000,
                max_bytes=chain.MAX_BYTES, can_collect=True, reason="point_reduced_for_budget",
                **changes)


@pytest.fixture
def setup(tmp_path, monkeypatch):
    task = tmp_path / "task"
    task.mkdir()
    before = summary(483)
    chain.atomic_json(task / "recordings/collection-metrics.json", before)
    calls = dict(disk=0, collector=[], submit=[], planner=[])
    def disk():
        calls["disk"] += 1
    def planner(current, **kwargs):
        calls["planner"].append((current, kwargs))
        return plan()
    def collect(options):
        calls["collector"].append(options)
        value = summary(495)
        chain.atomic_json(task / "recordings/collection-metrics.json", value)
        return value
    def submit(current):
        calls["submit"].append(current)
        return "abcdef012345"
    monkeypatch.setattr(chain, "disk_check", disk)
    monkeypatch.setattr(chain.budget, "task_plan", planner)
    monkeypatch.setattr(chain.collect, "run", collect)
    monkeypatch.setattr(chain, "submit_next", submit)
    return task, calls


def test_bounded_phase_resumes_with_planned_point_target_and_queues_exactly_one_continuation(setup):
    task, calls = setup
    state = chain.run(task)
    assert state["status"] == "continued" and state["next_job"] == "abcdef012345"
    assert calls["submit"] == [task] and len(calls["collector"]) == 1
    options = calls["collector"][0]
    assert options.task == task
    assert options.point_limit == 360 and options.max_bytes == 15_000_000_000
    assert options.include_follow == "no" and options.follow_below_gate
    assert options.parallel == 3 and options.limit_ms == 240000
    assert options.wall_minutes == 80 and options.max_new == 2000
    assert not options.verify_existing_only and options.expect_existing is None
    assert calls["disk"] == 2
    saved = json.loads((task / "recordings/collection-plan.json").read_text())
    assert saved["point_limit"] == 360
    assert saved["phases"][0]["episodes_before"] == 483
    assert saved["phases"][0]["episodes_after"] == 495
    assert saved["phases"][0]["plan"]["reason"] == "point_reduced_for_budget"


def test_complete_phase_never_submits_or_plans_again(setup, monkeypatch):
    task, calls = setup
    monkeypatch.setattr(chain.collect, "run", lambda options: summary(500, complete=True))
    state = chain.run(task)
    assert state["status"] == "complete"
    assert not calls["submit"] and len(calls["planner"]) == 1


@pytest.mark.parametrize("reason", ["budget_blocked_nonpoint", "budget_blocked_preserve_recorded_points",
                                    "candidate_pool_exhausted", "disk_budget_exhausted", "complete"])
def test_limits_or_already_complete_stop_before_collection(setup, monkeypatch, reason):
    task, calls = setup
    value = plan()
    value.update(can_collect=False, reason=reason)
    if reason == "complete":
        chain.atomic_json(task / "recordings/collection-metrics.json", summary(483, complete=True))
    monkeypatch.setattr(chain.budget, "task_plan", lambda *args, **kwargs: value)
    state = chain.run(task)
    assert state["status"] == ("complete" if reason == "complete" else "limit_reached")
    assert state["reason"] == reason
    assert not calls["collector"] and not calls["submit"]


def test_collector_error_is_saved_and_never_queues_a_retry(setup, monkeypatch):
    task, calls = setup
    def fail(options):
        raise ValueError("Broken recording")
    monkeypatch.setattr(chain.collect, "run", fail)
    with pytest.raises(ValueError, match="Broken recording"):
        chain.run(task)
    state = json.loads((task / "collection-chain.json").read_text())
    assert state["status"] == "failed" and state["detail"] == "Broken recording"
    assert not calls["submit"]


@pytest.mark.parametrize("result, expected", [
    (summary(483), "no_progress"),
    (summary(495, candidate_pool_exhausted=True), "limit_reached"),
    (dict(summary(495), stop_reason="disk_budget_exhausted"), "limit_reached"),
])
def test_unproductive_or_blocked_collector_result_stops_chain(setup, monkeypatch, result, expected):
    task, calls = setup
    monkeypatch.setattr(chain.collect, "run", lambda options: deepcopy(result))
    assert chain.run(task)["status"] == expected
    assert not calls["submit"]


def test_budget_is_rechecked_before_continuation(setup, monkeypatch):
    task, calls = setup
    plans = [plan(), dict(plan(), can_collect=False, reason="budget_blocked_nonpoint")]
    monkeypatch.setattr(chain.budget, "task_plan", lambda *args, **kwargs: plans.pop(0))
    state = chain.run(task)
    assert state["status"] == "limit_reached" and state["reason"] == "budget_blocked_nonpoint"
    assert not calls["submit"]
    saved = json.loads((task / "recordings/collection-plan.json").read_text())
    assert saved["reason"] == "budget_blocked_nonpoint"
    assert saved["phases"][0]["next_plan"]["reason"] == "budget_blocked_nonpoint"


def test_rejection_only_phase_queues_next_phase_when_candidate_pool_remains(setup, monkeypatch):
    task, calls = setup
    value = dict(summary(483), stop_reason="wall_limit", rejected_candidates=[dict(
        id="follow-bad-route", family="follow_below_gate", training_eligible=False,
        reason="moving route needs distinct connected points")])
    monkeypatch.setattr(chain.collect, "run", lambda options: value)
    state = chain.run(task)
    assert state["status"] == "continued" and calls["submit"] == [task]
    assert state["phases"][0]["episodes_before"] == state["phases"][0]["episodes_after"] == 483
    assert state["phases"][0]["rejected_before"] == 0
    assert state["phases"][0]["rejected_after"] == 1


def test_old_rejections_do_not_count_as_new_progress(setup, monkeypatch):
    task, calls = setup
    value = dict(summary(483), rejected_candidates=[dict(id="follow-bad-route", family="follow_below_gate")])
    chain.atomic_json(task / "recordings/collection-metrics.json", value)
    monkeypatch.setattr(chain.collect, "run", lambda options: deepcopy(value))
    assert chain.run(task)["status"] == "no_progress"
    assert not calls["submit"]


def test_rejection_progress_still_stops_when_candidate_pool_is_exhausted(setup, monkeypatch):
    task, calls = setup
    value = dict(summary(483, candidate_pool_exhausted=True), rejected_candidates=[dict(
        id="follow-last-route", family="follow_below_gate")])
    monkeypatch.setattr(chain.collect, "run", lambda options: value)
    state = chain.run(task)
    assert state["status"] == "limit_reached" and state["reason"] == "candidate_pool_exhausted"
    assert not calls["submit"]


def test_final_reduced_quota_refreshes_only_summary_without_another_recording_phase(setup, monkeypatch):
    task, calls = setup
    before = summary(483)
    before["by_family"]["point"].update(episodes=126, target=400)
    before["by_family"]["object"] = dict(episodes=100, training_eligible=100, complete=True)
    old = deepcopy(before["episodes"])
    def collect(options):
        value = deepcopy(before)
        value["episodes"].append(dict(id="new-object"))
        chain.atomic_json(task / "recordings/collection-metrics.json", value)
        return value
    plans = [plan(), dict(plan(), point_limit=126, can_collect=False, reason="complete")]
    monkeypatch.setattr(chain.collect, "run", collect)
    monkeypatch.setattr(chain.budget, "task_plan", lambda *args, **kwargs: plans.pop(0))
    state = chain.run(task)
    assert state["status"] == "complete" and not calls["submit"]
    saved = json.loads((task / "recordings/collection-metrics.json").read_text())
    assert saved["complete"] and saved["point_limit"] == 126
    assert saved["by_family"]["point"]["target"] == 126
    assert saved["by_family"]["point"]["complete"]
    assert saved["episodes"][:-1] == old
    assert saved["by_family"]["object"] == before["by_family"]["object"]


def test_next_phase_keeps_history_and_resumes_from_updated_summary(setup):
    task, calls = setup
    chain.run(task)
    state = chain.run(task)
    assert state["phase"] == 2
    assert [entry["episodes_before"] for entry in state["phases"]] == [483, 495]
    assert state["status"] == "no_progress" and len(calls["submit"]) == 1
    assert len(calls["collector"]) == 2


@pytest.mark.parametrize("reason", ["wall_limit", "episode_limit"])
def test_bounded_phase_stop_reason_still_queues_resume_when_it_made_progress(setup, monkeypatch, reason):
    task, calls = setup
    value = dict(summary(495), stop_reason=reason)
    monkeypatch.setattr(chain.collect, "run", lambda options: value)
    assert chain.run(task)["status"] == "continued"
    assert calls["submit"] == [task]


def test_enqueue_helper_uses_remote_gq_resources_and_waits_outside_recording_job(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(chain, "disk_check", lambda: calls.append("df"))
    def run(command, **kwargs):
        calls.append((command, kwargs))
        if "wait" in command:
            assert json.loads(request.read_text())["job"] == "012345abcdef"
        return SimpleNamespace(stdout="012345abcdef\n", returncode=0)
    monkeypatch.setattr(chain.subprocess, "run", run)
    task = tmp_path / "task"
    request = task / "chain-queue/request.json"
    assert chain.enqueue(task, request) == 0
    assert calls[0] == "df"
    command, kwargs = calls[1]
    assert command == [str(chain.GQ), "submit", "--class", "batch", "--threads", "12",
        "--cpu-quota", "1200%", "--mem", "8G", "--max", "2h", "--name",
        "nav demos v4 recording chain", "--", str(task / ".venv/bin/python"), "-I",
        str(Path(chain.__file__).with_name("nav_job.py").resolve()), "--max-seconds", "7200", "--",
        str(task / ".venv/bin/python"), "-X", "faulthandler", "-I",
        str(Path(chain.__file__).resolve()), "--task", str(task)]
    assert Path(command[0]).is_absolute()
    assert kwargs["env"]["GQ_OWNER"] == "nav-demos-v4"
    assert kwargs["check"] and kwargs["capture_output"] and kwargs["text"]
    command, kwargs = calls[2]
    assert command == [str(chain.GQ), "wait", "012345abcdef"]
    assert kwargs["stdout"] == kwargs["stderr"] == chain.subprocess.DEVNULL
    assert json.loads(request.read_text()) == dict(status="finished", job="012345abcdef", returncode=0)


@pytest.mark.parametrize("shell", [False, True])
def test_navigation_guard_has_independent_pulse_timeout_and_python_stack(shell):
    if shell and sys.platform != "linux":
        pytest.skip("Descendant Python stacks use Linux /proc")
    guard = Path(chain.__file__).with_name("nav_job.py")
    command = [sys.executable, "-X", "faulthandler", "-I", "-c", "import time; time.sleep(10)"]
    if shell:
        command = ["bash", "-c", '"$@" & wait', "nav-job-test", *command]
    # A short pulse exercises the same supervision loop without a 30-second test.
    program = ("import resource,runpy,sys; resource.setrlimit(resource.RLIMIT_CORE,(0,0)); "
               f"guard=runpy.run_path({str(guard)!r}); "
               f"sys.exit(guard['run']({command!r},.5,heartbeat_seconds=.05))")
    result = subprocess.run([sys.executable, "-I", "-c", program], capture_output=True, text=True, timeout=10)
    assert result.returncode == 124
    events = [json.loads(line) for line in result.stdout.splitlines()]
    assert events[0]["timeout_seconds"] == .45 < events[0]["gq_max_seconds"]
    assert any(row["event"] == "nav_job_heartbeat" for row in events)
    assert any(row["event"] == "nav_job_timeout" for row in events)
    assert "Fatal Python error" in result.stderr and '<string>' in result.stderr
    with pytest.raises(ProcessLookupError):
        os.kill(events[0]["pid"], 0)


def test_navigation_guard_preserves_command_failure():
    guard = Path(chain.__file__).with_name("nav_job.py")
    result = subprocess.run([sys.executable, "-I", str(guard), "--max-seconds", "60", "--",
                             sys.executable, "-I", "-c", "raise SystemExit(7)"],
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 7
    events = [json.loads(line) for line in result.stdout.splitlines()]
    assert [row["event"] for row in events] == ["nav_job_start", "nav_job_end"]
    assert events[-1]["exit"] == 7


def test_invalid_job_id_fails_instead_of_claiming_chain_started(tmp_path, monkeypatch):
    monkeypatch.setattr(chain, "disk_check", lambda: None)
    monkeypatch.setattr(chain.subprocess, "run", lambda *args, **kwargs: SimpleNamespace(stdout="not an ID\n"))
    with pytest.raises(ValueError, match="invalid continuation ID"):
        chain.enqueue(tmp_path, tmp_path / "request.json")


def test_next_submission_uses_separate_transient_unit_and_only_selected_runtime_environment(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(chain, "disk_check", lambda: calls.append("df"))
    monkeypatch.setenv("VULKAN_SDK", "/toolchain/vulkan")
    monkeypatch.setenv("OMP_NUM_THREADS", "2")
    monkeypatch.setenv("OPENBLAS_NUM_THREADS", "1")
    monkeypatch.setenv("LD_LIBRARY_PATH", "/runtime/libs")
    monkeypatch.setenv("GQ_PROFILE", "ryzen")
    monkeypatch.setenv("PRIVATE_TOKEN", "must-not-be-forwarded")
    def run(command, **kwargs):
        calls.append((command, kwargs))
        request = Path(command[-1])
        assert "--enqueue" == command[-2]
        chain.atomic_json(request, dict(status="submitted", job="123456abcdef"))
        return SimpleNamespace(stdout="", returncode=0)
    monkeypatch.setattr(chain.subprocess, "run", run)
    assert chain.submit_next(tmp_path) == "123456abcdef"
    assert calls[0] == "df" and len(calls) == 2
    command, kwargs = calls[1]
    assert command[:6] == ["systemd-run", "--user", "--collect", "--quiet", "--service-type=exec", "--unit"]
    assert command[6].startswith("nav-demos-v4-queue-")
    assert f"WorkingDirectory={tmp_path / 'source'}" in command
    for value in ("VULKAN_SDK=/toolchain/vulkan", "OMP_NUM_THREADS=2", "OPENBLAS_NUM_THREADS=1",
                  "LD_LIBRARY_PATH=/runtime/libs", "GQ_PROFILE=ryzen", "GQ_OWNER=nav-demos-v4"):
        assert "--setenv=" + value in command
    assert not any("PRIVATE_TOKEN" in value for value in command)
    assert kwargs == dict(check=True, capture_output=True, text=True)


def test_enqueue_failure_does_not_wait_or_publish_a_job(tmp_path, monkeypatch):
    calls = []
    monkeypatch.setattr(chain, "disk_check", lambda: None)
    def fail(command, **kwargs):
        calls.append(command)
        raise chain.subprocess.CalledProcessError(1, command)
    monkeypatch.setattr(chain.subprocess, "run", fail)
    request = tmp_path / "request.json"
    with pytest.raises(chain.subprocess.CalledProcessError):
        chain.enqueue(tmp_path, request)
    assert len(calls) == 1
    assert json.loads(request.read_text())["status"] == "failed"


def test_disk_check_shows_home_before_any_submission(monkeypatch):
    calls = []
    monkeypatch.setattr(chain.subprocess, "run", lambda *args, **kwargs: calls.append((args, kwargs)))
    chain.disk_check()
    assert calls == [((["df", "-h", str(Path.home())],), dict(check=True))]
