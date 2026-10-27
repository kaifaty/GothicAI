"""Admission and lifecycle checks; never touch the machine's real queue or units."""
from __future__ import annotations

from contextlib import contextmanager
import fcntl
from importlib.machinery import SourceFileLoader
import importlib.util
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "gq.py"
GIB = 1024**3


@pytest.fixture(scope="module")
def gq():
    spec = importlib.util.spec_from_file_location("gq_test_module", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


@pytest.fixture(autouse=True)
def preserve_test_process_priority(gq, monkeypatch):
    # Renicing the imported module would otherwise change pytest's own process.
    monkeypatch.setattr(gq.os, "nice", lambda _increment: 19)


FAKE_SYSTEMD = r'''#!/usr/bin/env python3
import json
import os
from pathlib import Path
import sys

root = Path(os.environ["FAKE_SYSTEMD_HOME"])
root.mkdir(parents=True, exist_ok=True)
args = sys.argv[1:]
with (root / "calls.jsonl").open("a") as stream:
    stream.write(json.dumps({"tool": Path(sys.argv[0]).name, "args": args}) + "\n")

def unit_name():
    for index, value in enumerate(args):
        if value.startswith("--unit="):
            return value.split("=", 1)[1].removesuffix(".service")
        if value in ("--unit", "-u"):
            return args[index + 1].removesuffix(".service")
    for value in args:
        if value.startswith("gq-"):
            return value.removesuffix(".service")
    return ""

unit = unit_name()
path = root / (unit + ".json")
if Path(sys.argv[0]).name == "systemd-run":
    if os.environ.get("FAKE_SYSTEMD_FAIL"):
        print("fake systemd launch failed", file=sys.stderr)
        sys.exit(1)
    path.write_text(json.dumps({"ActiveState": "active", "SubState": "running",
                               "Result": "success", "ExecMainCode": "1",
                               "ExecMainStatus": "0", "LoadState": "loaded"}))
    print("Running as unit: " + unit + ".service", file=sys.stderr)
    sys.exit(0)
state = json.loads(path.read_text()) if path.exists() else {
    "ActiveState": "inactive", "SubState": "dead", "Result": "success",
    "ExecMainCode": "1", "ExecMainStatus": "0", "LoadState": "not-found"}
command = next((value for value in args if value in
                ("show", "stop", "is-active", "reset-failed", "status", "set-property")), "show")
if command == "set-property":
    if os.environ.get("FAKE_SYSTEMD_SET_PROPERTY_FAIL"):
        print("fake property update failed", file=sys.stderr)
        sys.exit(1)
    state.update(dict(value.split("=", 1) for value in args if "=" in value))
    path.write_text(json.dumps(state))
    sys.exit(0)
if command in ("stop", "reset-failed"):
    state.update(ActiveState="inactive", SubState="dead")
    path.write_text(json.dumps(state))
    sys.exit(0)
if command == "is-active":
    print(state["ActiveState"])
    sys.exit(0 if state["ActiveState"] == "active" else 3)
if "--value" in args:
    fields = []
    for index, value in enumerate(args):
        if value in ("--property", "-p"):
            fields.extend(args[index + 1].split(","))
        elif value.startswith("--property="):
            fields.extend(value.split("=", 1)[1].split(","))
    for field in fields:
        print(state.get(field, ""))
else:
    for key, value in state.items():
        print(key + "=" + str(value))
'''


@pytest.fixture
def host(tmp_path, monkeypatch):
    """All unit calls and compatibility-lock inspection are isolated fakes."""
    binaries = tmp_path / "bin"
    binaries.mkdir()
    for name in ("systemd-run", "systemctl"):
        executable = binaries / name
        executable.write_text(FAKE_SYSTEMD)
        executable.chmod(0o755)
    home = tmp_path / "queue"
    locks = tmp_path / "proc-locks"
    locks.write_text("")
    legacy = tmp_path / "legacy.lock"
    legacy.touch()
    monkeypatch.setenv("PATH", str(binaries) + os.pathsep + os.environ["PATH"])
    monkeypatch.setenv("FAKE_SYSTEMD_HOME", str(tmp_path / "systemd"))
    monkeypatch.setenv("GQ_HOME", str(home))
    monkeypatch.setenv("GQ_PROFILE", "ryzen")
    monkeypatch.setenv("GQ_LEGACY_LOCK", str(legacy))
    monkeypatch.setenv("GQ_OWNER", "test-worktree")
    return {"home": home, "legacy": legacy, "locks": locks,
            "systemd": tmp_path / "systemd", "env": dict(os.environ)}


@pytest.fixture
def queue(gq, host, monkeypatch):
    q = gq.Queue(home=host["home"], legacy_lock=host["legacy"], proc_locks=host["locks"])
    monkeypatch.setattr(q, "now", lambda: 1000.0)
    monkeypatch.setattr(q, "disk_free", lambda: 100 * GIB)
    # The scheduler is exercised independently from the systemd lifecycle.
    monkeypatch.setattr(q, "reconcile", lambda: None)
    monkeypatch.setattr(q, "launch", lambda job: q.started(job["id"]))
    return q


def spec(kind="quick", *, owner="session-a", threads=4, mem=GIB,
         gpu_mem=0, display=False, display_shared=False, display_exclusive=False,
         interactive=False, maximum=60, name="test", command=None):
    return {"class": kind, "threads": threads, "mem": mem,
            "gpu_mem": gpu_mem, "display": display, "display_shared": display_shared,
            "display_exclusive": display_exclusive,
            "interactive": interactive,
            "max": maximum,
            "name": name, "command": command or ["true"], "owner": owner}


def enqueue(q, kind="quick", **kwargs):
    return q.submit(spec(kind, **kwargs))


def start(q, kind="quick", **kwargs):
    ident = enqueue(q, kind, **kwargs)
    q.tick()
    assert q.get(ident)["status"] == "running"
    return ident


def test_quick_runs_with_batch_even_when_all_batch_threads_are_claimed(queue):
    batch = start(queue, "batch", threads=28, mem=16 * GIB)
    quick = start(queue, threads=4, mem=4 * GIB)
    assert queue.get(batch)["status"] == queue.get(quick)["status"] == "running"


def test_interactive_does_not_wait_for_background_memory_or_gpu(queue):
    batch = start(queue, "batch", threads=28, mem=20 * GIB, gpu_mem=10 * GIB)
    player = start(queue, threads=8, mem=8 * GIB, gpu_mem=3 * GIB,
                   display_exclusive=True, interactive=True, maximum=6 * 3600)
    assert queue.get(batch)["status"] == queue.get(player)["status"] == "running"
    ordinary = enqueue(queue)
    queue.tick()
    assert queue.get(ordinary)["status"] == "queued"


@pytest.mark.parametrize("quiet_active", [False, True])
def test_interactive_bypasses_quiet_and_legacy_without_admitting_other_games(
        queue, monkeypatch, quiet_active):
    quiet = start(queue, "quiet") if quiet_active else enqueue(queue, "quiet")
    earlier_game = enqueue(queue, display_shared=True, display=True)
    player = enqueue(queue, display_exclusive=True, interactive=True)

    @contextmanager
    def unavailable_legacy(*_args):
        yield False

    monkeypatch.setattr(queue, "legacy_probe", unavailable_legacy)
    monkeypatch.setattr(queue, "legacy_waiting", lambda: True)
    queue.tick()
    assert queue.get(player)["status"] == "running"
    assert queue.get(earlier_game)["status"] == "queued"
    assert queue.get(quiet)["status"] == ("running" if quiet_active else "queued")


def test_interactive_waits_for_active_games_and_blocks_all_new_windows(queue):
    active = start(queue, display_shared=True, display=True)
    earlier_game = enqueue(queue, "quiet", display_shared=True, display=True)
    player = enqueue(queue, display_exclusive=True, interactive=True)
    later_game = enqueue(queue, display_shared=True, display=True)
    queue.tick()
    assert all(queue.get(ident)["status"] == "queued"
               for ident in (earlier_game, player, later_game))
    queue.finish(active, "success")
    queue.tick()
    assert queue.get(player)["status"] == "running"
    assert all(queue.get(ident)["status"] == "queued" for ident in (earlier_game, later_game))
    queue.finish(player, "success")
    queue.tick()
    assert queue.get(earlier_game)["status"] == "running"


@pytest.mark.parametrize("overrides", [
    {"display_exclusive": False}, {"kind": "batch"}, {"kind": "quiet"},
    {"maximum": 6 * 3600 + 1},
])
def test_interactive_rejects_other_classes_shared_windows_and_long_runtime(queue, gq, overrides):
    request = {"display_exclusive": True, "interactive": True, **overrides}
    with pytest.raises(gq.QueueError, match="interactive"):
        queue.submit(spec(**request))


@pytest.mark.parametrize("resource", ["mem", "gpu_mem"])
def test_quick_waits_for_its_memory_gpu_or_window(queue, resource):
    first = {"mem": 12 * GIB, "gpu_mem": 6 * GIB, "display": True}
    waiting = {"mem": 9 * GIB} if resource == "mem" else {"gpu_mem": 5 * GIB}
    running = start(queue, **first)
    ident = enqueue(queue, **waiting)
    queue.tick()
    assert queue.get(ident)["status"] == "queued"
    queue.finish(running, "success")
    queue.tick()
    assert queue.get(ident)["status"] == "running"


def test_three_displays_run_with_distinct_slots_and_fourth_waits(queue):
    running = [enqueue(queue, display=True, display_shared=True) for _ in range(3)]
    waiting = enqueue(queue, display=True, display_shared=True)
    queue.tick()
    assert all(queue.get(ident)["status"] == "running" for ident in running)
    assert {queue.get(ident)["display_slot"] for ident in running} == {0, 1, 2}
    assert queue.get(waiting)["status"] == "queued"
    assert "display_slot" not in queue.get(waiting)
    queue.finish(running[1], "success")
    queue.tick()
    assert queue.get(waiting)["status"] == "running"
    assert queue.get(waiting)["display_slot"] == 1


def test_display_slots_remain_reserved_until_cancelled_unit_stops(queue):
    running = [start(queue, display=True, display_shared=True) for _ in range(3)]
    waiting = enqueue(queue, display=True, display_shared=True)
    queue.cancel(running[0], "session-a")
    queue.tick()
    assert queue.get(waiting)["status"] == "queued"
    queue.finish(running[0], "success")
    queue.tick()
    assert queue.get(running[0])["status"] == "cancelled"
    assert queue.get(waiting)["status"] == "running"
    assert queue.get(waiting)["display_slot"] == 0


def test_cancelled_queued_game_never_reserves_a_slot(queue):
    cancelled = enqueue(queue, display=True, display_shared=True)
    queue.cancel(cancelled, "session-a")
    assert "display_slot" not in queue.get(cancelled)
    assert {queue.get(start(queue, display=True, display_shared=True))["display_slot"] for _ in range(3)} == {0, 1, 2}


def test_display_capacity_is_taken_from_profile(queue):
    queue.profile = {**queue.profile, "display": 1}
    first = start(queue, display=True, display_shared=True)
    second = enqueue(queue, display=True, display_shared=True)
    queue.tick()
    assert queue.get(second)["status"] == "queued"
    queue.finish(first, "success")
    queue.tick()
    assert queue.get(second)["display_slot"] == 0


@pytest.mark.parametrize("has_slot", [False, True])
def test_persisted_legacy_display_reserves_all_slots(queue, has_slot):
    first = start(queue, display=True)
    with queue.transaction() as state:
        legacy = next(job for job in state["jobs"] if job["id"] == first)
        legacy.pop("display_shared")
        legacy.pop("display_exclusive")
        if not has_slot:
            legacy.pop("display_slot")
    waiting = enqueue(queue, display=True, display_shared=True)
    queue.tick()
    assert queue.get(waiting)["status"] == "queued"
    queue.finish(first, "success")
    queue.tick()
    assert queue.get(waiting)["status"] == "running"
    assert queue.get(waiting)["display_slot"] == 0


@pytest.mark.parametrize("exclusive_request", [
    {"display_exclusive": True},
    {"display": True},
    {"display": True, "display_shared": True, "display_exclusive": True},
])
@pytest.mark.parametrize("persisted", [False, True])
def test_display_exclusive_drains_all_slots_and_blocks_new_games(queue, exclusive_request, persisted):
    running = [start(queue, display=True, display_shared=True) for _ in range(3)]
    exclusive = enqueue(queue, **exclusive_request)
    if persisted:
        with queue.transaction() as state:
            legacy = next(job for job in state["jobs"] if job["id"] == exclusive)
            # Old queued requests have neither field; explicit exclusivity does
            # not depend on whether an old client persisted display_shared.
            if not exclusive_request.get("display_shared"):
                legacy.pop("display_shared")
            if not exclusive_request.get("display_exclusive"):
                legacy.pop("display_exclusive")
    later = enqueue(queue, display=True, display_shared=True)
    cpu_only = enqueue(queue)
    queue.tick()
    assert queue.get(cpu_only)["status"] == "running"
    assert queue.get(exclusive)["status"] == queue.get(later)["status"] == "queued"
    for ident in running[:-1]:
        queue.finish(ident, "success")
        queue.tick()
        assert queue.get(exclusive)["status"] == queue.get(later)["status"] == "queued"
    queue.finish(running[-1], "success")
    queue.tick()
    assert queue.get(exclusive)["status"] == "running"
    assert queue.get(exclusive)["display"] and queue.get(exclusive)["display_slot"] == 0
    assert queue.get(later)["status"] == "queued"
    queue.finish(exclusive, "success")
    queue.tick()
    assert queue.get(later)["status"] == "running"


def test_waiting_exclusive_cannot_be_starved_by_a_stream_of_new_games(queue):
    running = start(queue, display=True, display_shared=True)
    exclusive = enqueue(queue, display_exclusive=True)
    for _ in range(5):
        later = enqueue(queue, display=True, display_shared=True)
        queue.tick()
        assert queue.get(later)["status"] == "queued"
    queue.finish(running, "success")
    queue.tick()
    assert queue.get(exclusive)["status"] == "running"
    assert all(job["status"] == "queued" for job in queue.jobs()
               if job["id"] not in {running, exclusive})


def test_display_exclusives_are_fifo_and_cancellation_reopens_admission(queue):
    running = start(queue, display=True, display_shared=True)
    first = enqueue(queue, display_exclusive=True)
    second = enqueue(queue, display_exclusive=True)
    later = enqueue(queue, display=True, display_shared=True)
    queue.finish(running, "success")
    queue.tick()
    assert queue.get(first)["status"] == "running"
    assert queue.get(second)["status"] == queue.get(later)["status"] == "queued"
    queue.cancel(second, "session-a")
    queue.finish(first, "success")
    queue.tick()
    assert queue.get(later)["status"] == "running"


def test_display_exclusive_preserves_fifo_of_earlier_batches(queue):
    earlier = enqueue(queue, "batch", display=True, display_shared=True)
    exclusive = enqueue(queue, "batch", display_exclusive=True)
    later = enqueue(queue, "batch", display=True, display_shared=True)
    queue.tick()
    assert queue.get(earlier)["status"] == "running"
    assert queue.get(exclusive)["status"] == queue.get(later)["status"] == "queued"
    queue.finish(earlier, "success")
    queue.tick()
    assert queue.get(exclusive)["status"] == "running"
    assert queue.get(later)["status"] == "queued"


@pytest.mark.parametrize("exclusive_kind", ["quick", "batch"])
@pytest.mark.parametrize("quiet_first", [True, False])
def test_display_exclusive_keeps_quiet_class_priority_without_deadlock(
        queue, exclusive_kind, quiet_first):
    if quiet_first:
        quiet = enqueue(queue, "quiet", display=True, display_shared=True)
    exclusive = enqueue(queue, exclusive_kind, display_exclusive=True)
    if not quiet_first:
        quiet = enqueue(queue, "quiet", display=True, display_shared=True)
    later = enqueue(queue, display=True, display_shared=True)
    queue.tick()
    assert queue.get(quiet)["status"] == "running"
    assert queue.get(exclusive)["status"] == queue.get(later)["status"] == "queued"
    queue.finish(quiet, "success")
    queue.tick()
    assert queue.get(exclusive)["status"] == "running"
    assert queue.get(later)["status"] == "queued"


def test_games_already_ahead_of_exclusive_drain_but_later_games_wait(queue):
    running = start(queue, display=True, display_shared=True)
    earlier = enqueue(queue, display=True, display_shared=True)
    exclusive = enqueue(queue, display_exclusive=True)
    later = enqueue(queue, display=True, display_shared=True)
    queue.tick()
    assert queue.get(earlier)["status"] == "running"
    assert queue.get(exclusive)["status"] == queue.get(later)["status"] == "queued"
    queue.finish(running, "success")
    queue.tick()
    assert queue.get(exclusive)["status"] == queue.get(later)["status"] == "queued"
    queue.finish(earlier, "success")
    queue.tick()
    assert queue.get(exclusive)["status"] == "running"
    assert queue.get(later)["status"] == "queued"


def test_batch_thread_budget_includes_all_running_batches(queue):
    first = start(queue, "batch", threads=16)
    second = start(queue, "batch", owner="session-b", threads=12)
    third = enqueue(queue, "batch", owner="session-c", threads=1)
    queue.tick()
    assert queue.get(third)["status"] == "queued"
    queue.finish(first, "success")
    queue.tick()
    assert queue.get(second)["status"] == queue.get(third)["status"] == "running"


def test_batch_fifo_does_not_allow_a_smaller_job_to_jump_ahead(queue):
    running = start(queue, "batch", owner="running", threads=20)
    first = enqueue(queue, "batch", owner="first", threads=12)
    second = enqueue(queue, "batch", owner="second", threads=8)
    queue.tick()
    assert queue.get(first)["status"] == queue.get(second)["status"] == "queued"
    queue.finish(running, "success")
    queue.tick()
    assert queue.get(first)["status"] == queue.get(second)["status"] == "running"


def test_a_session_does_not_start_second_batch_while_another_session_waits(queue):
    start(queue, "batch", owner="session-a", threads=8)
    same_session = enqueue(queue, "batch", owner="session-a", threads=8)
    other_session = enqueue(queue, "batch", owner="session-b", threads=20)
    queue.tick()
    assert queue.get(same_session)["status"] == "queued"
    assert queue.get(other_session)["status"] == "running"


def test_fairness_skip_preserves_fifo_for_all_later_batches_of_that_owner(queue):
    first = start(queue, "batch", owner="session-a", threads=8)
    second = enqueue(queue, "batch", owner="session-a", threads=8)
    other = enqueue(queue, "batch", owner="session-b", threads=8)
    third = enqueue(queue, "batch", owner="session-a", threads=8)
    queue.tick()
    assert queue.get(first)["status"] == queue.get(other)["status"] == "running"
    assert queue.get(second)["status"] == queue.get(third)["status"] == "queued"


def test_same_session_can_share_budget_when_no_other_session_is_waiting(queue):
    start(queue, "batch", owner="session-a", threads=8)
    start(queue, "batch", owner="session-a", threads=8)


def test_first_quiet_stops_batch_admission_and_waits_for_all_jobs(queue):
    batch = start(queue, "batch", maximum=120)
    quick = start(queue, maximum=30)
    quiet = enqueue(queue, "quiet")
    next_batch = enqueue(queue, "batch", owner="session-b")
    queue.tick()
    assert queue.get(quiet)["status"] == queue.get(next_batch)["status"] == "queued"
    queue.finish(batch, "success")
    queue.tick()
    assert queue.get(quiet)["status"] == "queued"
    queue.finish(quick, "success")
    queue.tick()
    assert queue.get(quiet)["status"] == "running"
    assert queue.get(next_batch)["status"] == "queued"
    queue.finish(quiet, "success")
    queue.tick()
    assert queue.get(next_batch)["status"] == "running"


def test_short_quick_can_finish_before_last_batch_ahead_of_quiet(queue, monkeypatch):
    start(queue, "batch", owner="one", maximum=60)
    start(queue, "batch", owner="two", maximum=120)
    quiet = enqueue(queue, "quiet")
    monkeypatch.setattr(queue, "now", lambda: 1010.0)
    quick = start(queue, maximum=100)
    assert queue.get(quiet)["status"] == "queued"
    assert queue.get(quick)["status"] == "running"


@pytest.mark.parametrize("maximum", [110, 111])
def test_quick_must_end_strictly_before_last_batch_for_waiting_quiet(queue, monkeypatch, maximum):
    start(queue, "batch", maximum=120)
    enqueue(queue, "quiet")
    monkeypatch.setattr(queue, "now", lambda: 1010.0)
    quick = enqueue(queue, maximum=maximum)
    queue.tick()
    assert queue.get(quick)["status"] == "queued"


def test_pending_quiet_without_running_batch_blocks_new_quick(queue):
    first = start(queue, maximum=120)
    quiet = enqueue(queue, "quiet")
    quick = enqueue(queue, maximum=1)
    queue.tick()
    assert queue.get(quick)["status"] == queue.get(quiet)["status"] == "queued"
    queue.finish(first, "success")
    queue.tick()
    assert queue.get(quiet)["status"] == "running"
    assert queue.get(quick)["status"] == "queued"


def test_quiet_jobs_are_exclusive_and_ordered(queue):
    first = start(queue, "quiet")
    second = enqueue(queue, "quiet")
    quick = enqueue(queue)
    queue.tick()
    assert queue.get(second)["status"] == queue.get(quick)["status"] == "queued"
    queue.finish(first, "success")
    queue.tick()
    assert queue.get(second)["status"] == "running"
    assert queue.get(quick)["status"] == "queued"


@pytest.mark.parametrize("kind", ["quick", "batch", "quiet"])
def test_disk_floor_blocks_new_jobs_and_allows_exact_threshold(queue, monkeypatch, kind):
    monkeypatch.setattr(queue, "disk_free", lambda: 20 * GIB - 1)
    ident = enqueue(queue, kind)
    queue.tick()
    assert queue.get(ident)["status"] == "queued"
    monkeypatch.setattr(queue, "disk_free", lambda: 20 * GIB)
    queue.tick()
    assert queue.get(ident)["status"] == "running"


def test_batch_maximum_is_two_hours(queue, gq):
    with pytest.raises(gq.QueueError, match="(?i)(max|2|7200|час)"):
        enqueue(queue, "batch", maximum=7201)
    assert enqueue(queue, "batch", maximum=7200)


def write_legacy_waiter(host, *, inode=None, blocked=True, mode="WRITE"):
    stat = host["legacy"].stat()
    key = f"{os.major(stat.st_dev):02x}:{os.minor(stat.st_dev):02x}:{inode or stat.st_ino}"
    host["locks"].write_text(f"12: {'-> ' if blocked else ''}FLOCK ADVISORY {mode} 12345 {key} 0 EOF\n")


@pytest.mark.parametrize("kind", ["quick", "batch", "quiet"])
def test_old_exclusive_lock_prevents_all_new_admissions(queue, host, kind):
    ident = enqueue(queue, kind)
    with host["legacy"].open("a+") as old_client:
        fcntl.flock(old_client, fcntl.LOCK_EX | fcntl.LOCK_NB)
        queue.tick()
        assert queue.get(ident)["status"] == "queued"
    queue.tick()
    assert queue.get(ident)["status"] == "running"


def test_failed_legacy_probe_stops_admission_even_if_old_client_releases_lock(
        queue, host, monkeypatch):
    first = enqueue(queue, "batch", owner="first", threads=28)
    second = enqueue(queue, "batch", owner="second", threads=28)
    original_probe = queue.legacy_probe
    probes = []

    with host["legacy"].open("a+") as old_client:
        fcntl.flock(old_client, fcntl.LOCK_EX | fcntl.LOCK_NB)

        @contextmanager
        def probe_then_release(quiet=False):
            with original_probe(quiet) as available:
                probes.append(available)
                if not available:
                    fcntl.flock(old_client, fcntl.LOCK_UN)
                yield available

        monkeypatch.setattr(queue, "legacy_probe", probe_then_release)
        queue.tick()

    assert probes == [False]
    assert queue.get(first)["status"] == queue.get(second)["status"] == "queued"


def test_blocked_old_exclusive_waiter_drains_batches_like_quiet(queue, host):
    running = start(queue, "batch", maximum=120)
    write_legacy_waiter(host)
    assert queue.legacy_waiting()
    batch = enqueue(queue, "batch", owner="session-b")
    short_quick = enqueue(queue, maximum=30)
    long_quick = enqueue(queue, maximum=120)
    queue.tick()
    assert queue.get(batch)["status"] == queue.get(long_quick)["status"] == "queued"
    assert queue.get(short_quick)["status"] == "running"
    queue.finish(running, "success")
    queue.finish(short_quick, "success")
    queue.tick()
    assert queue.get(batch)["status"] == queue.get(long_quick)["status"] == "queued"
    host["locks"].write_text("")
    queue.tick()
    assert queue.get(batch)["status"] == queue.get(long_quick)["status"] == "running"


@pytest.mark.parametrize("options", [
    {"inode": 1}, {"blocked": False}, {"mode": "READ"},
])
def test_unrelated_or_nonwaiting_proc_locks_do_not_stop_admission(queue, host, options):
    write_legacy_waiter(host, **options)
    assert not queue.legacy_waiting()
    start(queue, "batch")


def records(q):
    path = q.home / "journal.jsonl"
    return [json.loads(line) for line in path.read_text().splitlines()]


@pytest.mark.parametrize("result,exit_code,exit_status,status,code", [
    ("success", "exited", "0", "done", 0),
    ("success", "exited", "7", "failed", 7),
    ("exit-code", "exited", "2", "failed", 2),
    ("signal", "killed", "9", "failed", 137),
    ("timeout", "killed", "15", "timeout", 124),
])
def test_completion_reason_code_and_resources_are_journalled(
        queue, monkeypatch, result, exit_code, exit_status, status, code):
    ident = start(queue, mem=3 * GIB, gpu_mem=GIB, name="journal-name")
    monkeypatch.setattr(queue, "now", lambda: 1012.0)
    queue.finish(ident, result, exit_code, exit_status)
    job = queue.get(ident)
    assert (job["status"], job["code"]) == (status, code)
    submitted, started, finished = records(queue)
    assert [entry["event"] for entry in (submitted, started, finished)] == ["submitted", "started", "finished"]
    assert finished["id"] == ident
    assert finished["reason"] == status
    assert finished["code"] == code
    assert finished["duration"] == 12
    assert finished["wait"] == 0
    assert finished["owner"] == "session-a"
    assert finished["mem"] == 3 * GIB and finished["gpu_mem"] == GIB
    assert finished["name"] == "journal-name"
    queue.finish(ident, result, exit_code, exit_status)
    assert len(records(queue)) == 3, "repeated callbacks must not duplicate completion"


def test_cancellation_checks_owner_and_records_queued_request(queue, gq):
    ident = enqueue(queue, owner="owner-a")
    with pytest.raises(gq.QueueError, match="force"):
        queue.cancel(ident, "owner-b")
    assert queue.get(ident)["status"] == "queued"
    queue.cancel(ident, "owner-b", force=True)
    assert queue.get(ident)["status"] == "cancelled"
    finished = records(queue)[-1]
    assert finished["reason"] == "cancelled" and finished["code"] == 143


def test_cli_cancel_does_not_admit_its_queued_job_before_cancelling(
        queue, gq, host, monkeypatch):
    marker = host["home"].parent / "cancelled-command-ran"
    ident = enqueue(queue, owner="test-worktree", command=[
        sys.executable, "-c", f"from pathlib import Path; Path({str(marker)!r}).touch()"])
    following = enqueue(queue, owner="other-worktree")
    launched = []

    def launch_command(job):
        launched.append(job["id"])
        assert queue.started(job["id"])
        subprocess.run(job["command"], cwd=job["cwd"], env=job["env"], check=True)
        queue.finish(job["id"], "success")

    monkeypatch.setattr(queue, "launch", launch_command)
    monkeypatch.setattr(gq, "Queue", lambda: queue)
    assert gq.main(["cancel", ident]) == 0
    assert not marker.exists()
    assert queue.get(ident)["status"] == "cancelled"
    assert launched == [following], "cancel must still tick after removing its queued job"
    assert queue.get(following)["status"] == "done"


def test_run_wait_max_returns_75_and_cancels_request(queue, gq, monkeypatch, capsys):
    ident = enqueue(queue)
    monkeypatch.setattr(queue, "disk_free", lambda: 0)
    clock = [1000.0]
    monkeypatch.setattr(queue, "now", lambda: clock[0])
    monkeypatch.setattr(gq.select, "select", lambda *args: ([], [], []))
    monkeypatch.setattr(gq.time, "sleep", lambda seconds: clock.__setitem__(0, clock[0] + 1))
    assert gq.wait_job(queue, ident, run=True, wait_max=2) == 75
    assert queue.get(ident)["status"] == "cancelled"
    assert "Ryzen занят" in capsys.readouterr().err
    assert clock[0] == 1002.0
    assert records(queue)[-1]["reason"] == "cancelled"


@pytest.mark.parametrize("transition", ["running", "finished"])
def test_wait_max_does_not_cancel_a_job_admitted_after_its_snapshot(
        queue, gq, monkeypatch, capsys, transition):
    ident = enqueue(queue)
    original_jobs = queue.jobs
    raced = False

    def jobs_then_admit():
        nonlocal raced
        snapshot = original_jobs()
        if not raced:
            raced = True
            with queue.transaction() as state:
                state["jobs"][0].update(status="starting", reserved=queue.now())
            assert queue.started(ident)
            if transition == "finished":
                queue.finish(ident, "exit-code", "exited", "7")
        return snapshot

    def finish_running_job(_seconds):
        queue.finish(ident, "exit-code", "exited", "7")

    monkeypatch.setattr(queue, "tick", lambda: None)
    monkeypatch.setattr(queue, "jobs", jobs_then_admit)
    monkeypatch.setattr(gq.select, "select", lambda *args: ([], [], []))
    monkeypatch.setattr(gq.time, "sleep", finish_running_job)
    assert gq.wait_job(queue, ident, run=True, wait_max=0) == 7
    assert raced and queue.get(ident)["status"] == "failed"
    assert "Ryzen занят" not in capsys.readouterr().err
    assert records(queue)[-1]["code"] == 7
    assert not any(record.get("reason") == "cancelled" for record in records(queue))


def test_eof_from_disconnected_ssh_cancels_waiting_run(queue, gq, monkeypatch):
    ident = enqueue(queue)
    monkeypatch.setattr(queue, "disk_free", lambda: 0)
    read_end, write_end = os.pipe()
    os.close(write_end)
    with os.fdopen(read_end) as ssh_stdin:
        monkeypatch.setattr(gq.sys, "stdin", ssh_stdin)
        assert gq.wait_job(queue, ident, run=True) == 143
    assert queue.get(ident)["status"] == "cancelled"
    assert records(queue)[-1]["reason"] == "cancelled"


@pytest.mark.parametrize("action", ["wait", "run"])
@pytest.mark.parametrize("transition", ["running", "cancelled"])
def test_wait_progress_survives_a_concurrent_admission_or_cancellation(
        queue, gq, monkeypatch, capsys, action, transition):
    ident = enqueue(queue)
    original_jobs = queue.jobs
    original_wait = gq.wait_job
    armed = action == "run"
    transitioned = False

    def racing_jobs():
        nonlocal transitioned
        snapshot = original_jobs()
        if armed and not transitioned:
            transitioned = True
            # Another scheduler changes the state just after this snapshot is read.
            if transition == "running":
                with queue.transaction() as state:
                    state["jobs"][0].update(status="starting", reserved=queue.now())
                assert queue.started(ident)
            else:
                queue.cancel(ident, "session-a")
        return snapshot

    def wait_with_race(*args, **kwargs):
        nonlocal armed
        armed = True
        return original_wait(*args, **kwargs)

    def complete_after_progress(_seconds):
        assert transitioned
        if transition == "running":
            queue.finish(ident, "success")

    monkeypatch.setattr(queue, "tick", lambda: None)
    monkeypatch.setattr(queue, "jobs", racing_jobs)
    monkeypatch.setattr(gq, "Queue", lambda: queue)
    monkeypatch.setattr(gq, "wait_job", wait_with_race)
    monkeypatch.setattr(gq.select, "select", lambda *args: ([], [], []))
    monkeypatch.setattr(gq.time, "sleep", complete_after_progress)
    code = gq.main(["wait", ident]) if action == "wait" else gq.wait_job(queue, ident, run=True)
    assert code == (0 if transition == "running" else 143)
    assert queue.get(ident)["status"] == ("done" if transition == "running" else "cancelled")
    assert records(queue)[-1]["reason"] == ("done" if transition == "running" else "cancelled")
    assert "list.index" not in capsys.readouterr().err


def make_queue(gq, host, monkeypatch):
    q = gq.Queue(home=host["home"], legacy_lock=host["legacy"], proc_locks=host["locks"])
    monkeypatch.setattr(q, "disk_free", lambda: 100 * GIB)
    # Lifecycle race tests deliberately reconcile immediately after each mutation.
    # The shared five-second cadence has its own test using unmodified Queue instances.
    monkeypatch.setattr(q, "claim_reconcile", lambda: True)
    return q


def test_dead_waiter_is_removed_but_detached_submit_is_preserved(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    monkeypatch.setattr(q, "disk_free", lambda: 0)
    dead = q.submit(spec(), waiter={"pid": 999999999, "start": "dead-process"})
    detached = q.submit(spec())
    q.tick()
    assert q.get(dead)["status"] == "cancelled"
    assert q.get(detached)["status"] == "queued"
    assert records(q)[-1]["id"] == dead
    assert records(q)[-1]["reason"] == "cancelled"


def systemd_state(host, ident, **overrides):
    path = host["systemd"] / f"gq-{ident}.json"
    state = json.loads(path.read_text())
    state.update(overrides)
    path.write_text(json.dumps(state))


def calls(host, tool):
    path = host["systemd"] / "calls.jsonl"
    return [record["args"] for record in map(json.loads, path.read_text().splitlines())
            if record["tool"] == tool]


@pytest.mark.parametrize("kind,weight,nice", [("quick", 100, 0), ("batch", 10, 19), ("quiet", 100, 0)])
def test_systemd_launch_sets_affinity_limits_runtime_and_stop_callback(gq, host, monkeypatch, kind, weight, nice):
    q = make_queue(gq, host, monkeypatch)
    ident = q.submit(spec(kind, mem=3 * GIB, maximum=40))
    q.tick()
    assert q.get(ident)["status"] == "starting"
    arguments = calls(host, "systemd-run")[0]
    assert "--user" in arguments
    assert arguments[arguments.index("--unit") + 1] == f"gq-{ident}"
    properties = dict(value.removeprefix("--property=").split("=", 1)
                      for value in arguments if value.startswith("--property="))
    assert properties["CPUAffinity"] == "2-15 18-31"
    assert "AllowedCPUs" not in properties
    assert properties["CPUWeight"] == str(weight)
    assert properties["Nice"] == str(nice)
    assert properties["MemoryMax"] == str(3 * GIB)
    assert properties["RuntimeMaxSec"] == "50s"
    assert properties["TimeoutStopSec"] == "60"
    assert properties["CollectMode"] == "inactive-or-failed"
    assert "_finish" in properties["ExecStopPost"] and ident in properties["ExecStopPost"]
    assert properties["StandardOutput"] == f"append:{q.home}/logs/{ident}.log"
    assert "_execute" in arguments and arguments[-1] == ident


def test_cli_interactive_uses_live_cpu_weight_and_exact_six_hour_limit(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    monkeypatch.setattr(gq, "Queue", lambda: q)
    monkeypatch.setattr(gq, "wait_job", lambda *_args, **_kwargs: 0)
    assert gq.main(["run", "--class", "quick", "--threads", "8", "--mem", "8G",
                    "--gpu-mem", "3G", "--display-exclusive", "--interactive", "--max", "6h",
                    "--name", "Gothic AI", "--", "true"]) == 0
    q.tick()
    player, = q.jobs()
    assert player["interactive"] and player["display"] and player["display_exclusive"]
    properties = dict(value.removeprefix("--property=").split("=", 1)
                      for value in calls(host, "systemd-run")[0] if value.startswith("--property="))
    assert properties["CPUWeight"] == "1000"
    assert properties["CPUAffinity"] == "2-15 18-31"
    assert properties["MemoryMax"] == str(8 * GIB)
    assert properties["RuntimeMaxSec"] == "21600s"


def test_interactive_execute_starts_with_background_exclusive_legacy_lock(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    marker = host["home"].parent / "interactive-started"
    ident = q.submit(spec(display_exclusive=True, interactive=True, command=[
        sys.executable, "-c", "from pathlib import Path; import sys; Path(sys.argv[1]).touch()", str(marker)]))
    with host["legacy"].open("a+") as background:
        fcntl.flock(background, fcntl.LOCK_EX | fcntl.LOCK_NB)
        q.tick()
        assert q.get(ident)["status"] == "starting"
        systemd_state(host, ident, CPUWeight="100", RuntimeMaxUSec="27000000000")
        assert gq.execute(q, ident) == 0
        assert marker.exists()
        properties = json.loads((host["systemd"] / f"gq-{ident}.json").read_text())
        assert properties["CPUWeight"] == "1000"
        assert ["--user", "set-property", "--runtime", f"gq-{ident}.service", "CPUWeight=1000"] in calls(host, "systemctl")


def test_interactive_property_failure_prevents_command_start(gq, host, monkeypatch, capsys):
    q = make_queue(gq, host, monkeypatch)
    marker = host["home"].parent / "must-not-start"
    ident = q.submit(spec(display_exclusive=True, interactive=True, command=[
        sys.executable, "-c", "from pathlib import Path; import sys; Path(sys.argv[1]).touch()", str(marker)]))
    q.tick()
    monkeypatch.setenv("FAKE_SYSTEMD_SET_PROPERTY_FAIL", "1")
    monkeypatch.setattr(gq, "Queue", lambda: q)
    assert gq.main(["_execute", ident]) == 2
    assert q.get(ident)["status"] == "starting"
    assert not marker.exists()
    report = capsys.readouterr().err
    assert "приоритет игры" in report and "fake property update failed" in report
    assert "Traceback" not in report


@pytest.mark.parametrize("phase", ["command", "orphan"])
def test_interactive_helper_timeout_stops_children_without_systemd_timer(gq, host, monkeypatch, phase):
    q = make_queue(gq, host, monkeypatch)
    marker = host["home"].parent / "started"
    source = ("from pathlib import Path; import sys,time; Path(sys.argv[1]).touch(); time.sleep(5)"
              if phase == "command" else
              "from pathlib import Path; import subprocess,sys; "
              "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(5)']); "
              "Path(sys.argv[1]).write_text(str(child.pid))")
    ident = q.submit(spec(display_exclusive=True, interactive=True, maximum=1,
                         command=[sys.executable, "-c", source, str(marker)]))
    q.tick()
    systemd_state(host, ident, RuntimeMaxUSec="infinity")
    command = None
    stopped = False
    checking_orphans = False
    original_popen, original_systemctl = gq.subprocess.Popen, gq.systemctl

    def spawn(argv, *args, **kwargs):
        nonlocal command
        process = original_popen(argv, *args, **kwargs)
        if "_command" in argv:
            command = process
        return process

    def stop_unit(*args):
        nonlocal stopped
        result = original_systemctl(*args)
        if args[0] == "stop":
            stopped = True
            os.kill(command.pid if phase == "command" else int(marker.read_text()), signal.SIGTERM)
        return result

    def has_orphans(_job_id):
        nonlocal checking_orphans
        checking_orphans = True
        return phase == "orphan" and not stopped

    monkeypatch.setattr(gq.subprocess, "Popen", spawn)
    monkeypatch.setattr(gq, "systemctl", stop_unit)
    monkeypatch.setattr(gq, "unit_has_children", has_orphans)
    monkeypatch.setattr(gq.time, "monotonic", lambda: (
        1002 if (marker.exists() if phase == "command" else checking_orphans) else 1000))
    try:
        gq.execute(q, ident)
        assert marker.exists() and stopped
        assert q.get(ident)["cancel_reason"] == "timeout"
        q.finish(ident, "success")
        assert q.get(ident)["status"] == "timeout" and q.get(ident)["code"] == 124
    finally:
        if command is not None and command.poll() is None:
            command.terminate()
            command.wait()
        if phase == "orphan" and marker.exists() and not stopped:
            try:
                os.kill(int(marker.read_text()), signal.SIGTERM)
            except ProcessLookupError:
                pass


def test_interactive_wait_reports_current_window_owners(queue, gq, monkeypatch, capsys):
    game = start(queue, name="e2e stage", owner="other-session", display=True)
    start(queue, "batch", name="background-training")
    player = enqueue(queue, interactive=True, display_exclusive=True)
    monkeypatch.setattr(gq.time, "sleep", lambda _seconds: queue.cancel(player, "session-a"))
    assert gq.wait_job(queue, player) == 143
    report = capsys.readouterr().err
    assert "ждём игры" in report
    assert game in report and "other-session" in report and "e2e stage" in report
    assert "background-training" not in report


def test_starting_units_reserve_slots_and_receive_their_slot_environment(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    monkeypatch.setenv("GQ_DISPLAY_SLOT", "99")
    running = [q.submit(spec(display=True, display_shared=True)) for _ in range(3)]
    waiting = q.submit(spec(display=True, display_shared=True))
    q.tick()
    assert all(q.get(ident)["status"] == "starting" for ident in running)
    assert q.get(waiting)["status"] == "queued"
    arguments = calls(host, "systemd-run")
    assert len(arguments) == 3
    for slot, args in enumerate(arguments):
        assert f"--setenv=GQ_DISPLAY_SLOT={slot}" in args
        assert "--setenv=GQ_DISPLAY_SLOT=99" not in args


@pytest.mark.parametrize("action", ["run", "submit"])
def test_cli_display_shared_allows_two_games_together(gq, host, monkeypatch, action):
    q = make_queue(gq, host, monkeypatch)
    monkeypatch.setattr(gq, "Queue", lambda: q)
    monkeypatch.setattr(gq, "spawn_pump", lambda *_: None)
    monkeypatch.setattr(gq, "wait_job", lambda *_args, **_kwargs: 0)
    for _ in range(2):
        assert gq.main([action, "--class", "quick", "--threads", "4", "--mem", "5G",
                        "--display", "--display-shared", "--max", "1m", "--name", "shared",
                        "--", "true"]) == 0
    q.tick()
    assert all(job["status"] == "starting" and job["display_shared"] for job in q.jobs())
    assert {job["display_slot"] for job in q.jobs()} == {0, 1}


@pytest.mark.parametrize("action", ["run", "submit"])
def test_cli_display_exclusive_claims_the_display_without_requiring_display_flag(
        gq, host, monkeypatch, action):
    q = make_queue(gq, host, monkeypatch)
    monkeypatch.setattr(gq, "Queue", lambda: q)
    monkeypatch.setattr(gq, "spawn_pump", lambda *_: None)
    monkeypatch.setattr(gq, "wait_job", lambda *_args, **_kwargs: 0)
    assert gq.main([action, "--class", "quick", "--threads", "4", "--mem", "5G",
                    "--display-exclusive", "--max", "1m", "--name", "exclusive",
                    "--", "true"]) == 0
    job = q.jobs()[0]
    assert job["display"] and job["display_exclusive"]
    q.tick()
    assert job["id"] == q.jobs()[0]["id"] and q.jobs()[0]["display_slot"] == 0
    assert "--setenv=GQ_DISPLAY_SLOT=0" in calls(host, "systemd-run")[0]


def test_non_display_units_do_not_inherit_an_outer_display_slot(gq, host, monkeypatch):
    monkeypatch.setenv("GQ_DISPLAY_SLOT", "2")
    q = make_queue(gq, host, monkeypatch)
    q.submit(spec())
    q.tick()
    assert not any(arg.startswith("--setenv=GQ_DISPLAY_SLOT=")
                   for arg in calls(host, "systemd-run")[0])


@pytest.mark.parametrize("action", ["run", "submit"])
@pytest.mark.parametrize("quota,expected", [("200%", "200%"), ("200", "200%"),
                                          ("12.5%", "12.5%"), ("12.5", "12.5%")])
def test_cli_cpu_quota_is_saved_and_applied_to_unit(
        gq, host, monkeypatch, action, quota, expected):
    q = make_queue(gq, host, monkeypatch)
    monkeypatch.setattr(gq, "Queue", lambda: q)
    monkeypatch.setattr(gq, "spawn_pump", lambda *_: None)
    monkeypatch.setattr(gq, "wait_job", lambda *_args, **_kwargs: 0)
    assert gq.main([action, "--class", "quiet", "--threads", "2", "--mem", "1G",
                    "--cpu-quota", quota, "--max", "1m", "--name", "quota test",
                    "--", "true"]) == 0
    job = q.jobs()[0]
    assert job["cpu_quota"] == expected
    assert json.loads((q.home / "state.json").read_text())["jobs"][0]["cpu_quota"] == expected
    q.tick()
    arguments = calls(host, "systemd-run")[0]
    assert f"--property=CPUQuota={expected}" in arguments
    assert records(q)[0]["cpu_quota"] == expected


def test_submit_hands_off_printed_id_without_cancelling_request(queue, gq, monkeypatch, capsys):
    pumped = []
    previous = {sig: signal.getsignal(sig)
                for sig in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT)}
    monkeypatch.setattr(queue, "tick", lambda: None)
    monkeypatch.setattr(gq, "Queue", lambda: queue)
    monkeypatch.setattr(gq, "spawn_pump", lambda _queue, ident: pumped.append(ident))
    assert gq.main(["submit", "--class", "quick", "--threads", "1", "--mem", "1G",
                    "--max", "1m", "--name", "normal submit", "--", "true"]) == 0
    job = queue.jobs()[0]
    assert job["status"] == "queued" and job["waiter"] is None
    assert pumped == [job["id"]]
    assert capsys.readouterr().out == job["id"] + "\n"
    assert [entry["event"] for entry in records(queue)] == ["submitted"]
    assert all(signal.getsignal(sig) == handler for sig, handler in previous.items())


@pytest.mark.parametrize("signum", [signal.SIGHUP, signal.SIGTERM, signal.SIGINT])
@pytest.mark.parametrize("stage", ["transaction", "after-persistence"])
def test_interrupted_submit_cancels_before_id_returns(
        queue, gq, monkeypatch, capsys, signum, stage):
    foreign = enqueue(queue, owner="another-session")
    previous = {sig: signal.getsignal(sig)
                for sig in (signal.SIGHUP, signal.SIGTERM, signal.SIGINT)}
    original_event = queue.event
    original_submit = queue.submit
    pumped = []

    def interrupt_in_transaction(job, event, **extra):
        original_event(job, event, **extra)
        if event == "submitted":
            # The handler must let this transaction persist, retaining a way to
            # identify and cancel the request once submit() returns.
            os.kill(os.getpid(), signum)

    def interrupt_after_persistence(*args, **kwargs):
        ident = original_submit(*args, **kwargs)
        assert queue.get(ident)["status"] == "queued"
        os.kill(os.getpid(), signum)
        return ident

    if stage == "transaction":
        monkeypatch.setattr(queue, "event", interrupt_in_transaction)
    else:
        monkeypatch.setattr(queue, "submit", interrupt_after_persistence)
    monkeypatch.setattr(queue, "tick", lambda: None)
    monkeypatch.setattr(gq, "Queue", lambda: queue)
    monkeypatch.setattr(gq, "spawn_pump", lambda *_: pumped.append(True))
    assert gq.main(["submit", "--class", "quick", "--threads", "1", "--mem", "1G",
                    "--max", "1m", "--name", "interrupted submit", "--", "true"]) == 128 + signum
    jobs = queue.jobs()
    assert len(jobs) == 2
    assert queue.get(foreign)["status"] == "queued"
    job = next(job for job in jobs if job["id"] != foreign)
    assert job["status"] == "cancelled" and job["code"] == 143
    assert job["waiter"] is None
    assert [entry["event"] for entry in records(queue) if entry["id"] == job["id"]] == [
        "submitted", "finished"]
    assert not pumped and capsys.readouterr().out == ""
    assert all(signal.getsignal(sig) == handler for sig, handler in previous.items())


@pytest.mark.parametrize("signum", [signal.SIGHUP, signal.SIGTERM, signal.SIGINT])
def test_submit_signal_to_process_group_cancels_persisted_job_before_id_output(
        gq, host, tmp_path, signum):
    ready = tmp_path / "submit-saved"
    harness = r'''
import importlib.util
from pathlib import Path
import sys
import time

module_spec = importlib.util.spec_from_file_location("signal_submit_gq", sys.argv[1])
gq = importlib.util.module_from_spec(module_spec)
module_spec.loader.exec_module(gq)
gq.Queue.tick = lambda self: None

def pause_after_persistence(queue, ident):
    Path(sys.argv[2]).write_text(ident)
    while True:
        time.sleep(10)

gq.spawn_pump = pause_after_persistence
sys.exit(gq.main(["submit", "--class", "quick", "--threads", "1", "--mem", "1G",
                  "--max", "1m", "--name", "signal before output", "--", "true"]))
'''
    child = subprocess.Popen([sys.executable, "-c", harness, str(SCRIPT), str(ready)],
                             env=host["env"], start_new_session=True,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while not ready.exists() and child.poll() is None and time.monotonic() < deadline:
            time.sleep(0.01)
        assert ready.exists(), "submit did not persist its request"
        os.killpg(child.pid, signum)
        output, errors = child.communicate(timeout=5)
        assert child.returncode == 128 + signum, errors
        assert output == ""
        queue = gq.Queue(home=host["home"], legacy_lock=host["legacy"], proc_locks=host["locks"])
        jobs = queue.jobs()
        assert len(jobs) == 1 and jobs[0]["id"] == ready.read_text()
        assert jobs[0]["status"] == "cancelled" and jobs[0]["waiter"] is None
        assert [entry["event"] for entry in records(queue)] == ["submitted", "finished"]
        assert not (host["systemd"] / "calls.jsonl").exists()
    finally:
        if child.poll() is None:
            child.kill()
        child.communicate(timeout=5)


def test_failed_submit_pump_start_cancels_request_without_printing_id(
        queue, gq, monkeypatch, capsys):
    def fail_pump(*_args):
        raise OSError("cannot spawn pump")

    monkeypatch.setattr(queue, "tick", lambda: None)
    monkeypatch.setattr(gq, "Queue", lambda: queue)
    monkeypatch.setattr(gq, "spawn_pump", fail_pump)
    assert gq.main(["submit", "--class", "quick", "--threads", "1", "--mem", "1G",
                    "--max", "1m", "--name", "failed pump", "--", "true"]) == 2
    assert len(queue.jobs()) == 1 and queue.jobs()[0]["status"] == "cancelled"
    output = capsys.readouterr()
    assert output.out == "" and "cannot spawn pump" in output.err


@pytest.mark.parametrize("action", ["run", "submit"])
@pytest.mark.parametrize("quota", ["0", "0%", "-1", "nan", "inf", "200%%", "", "9" * 400])
def test_cli_rejects_invalid_cpu_quota(gq, action, quota):
    with pytest.raises(SystemExit) as failure:
        gq.parser().parse_args([action, "--class", "quick", "--threads", "2", "--mem", "1G",
                                "--cpu-quota", quota, "--max", "1m", "--name", "bad quota",
                                "--", "true"])
    assert failure.value.code == 2


def test_old_state_without_cpu_quota_launches_without_quota_property(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    ident = q.submit(spec())
    assert "cpu_quota" not in q.get(ident)
    # Reload an existing state written before this optional field was introduced.
    reloaded = make_queue(gq, host, monkeypatch)
    reloaded.tick()
    assert reloaded.get(ident)["status"] == "starting"
    assert not any(value.startswith("--property=CPUQuota=")
                   for value in calls(host, "systemd-run")[0])


def test_reconcile_recovers_systemd_timeout_without_waiter_or_callback(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    clock = [1000.0]
    monkeypatch.setattr(q, "now", lambda: clock[0])
    ident = q.submit(spec(maximum=1))
    q.tick()
    q.started(ident)
    systemd_state(host, ident, ActiveState="failed", Result="timeout", ExecMainCode="2", ExecMainStatus="15")
    clock[0] = 1006.0
    q.reconcile()
    assert q.get(ident)["status"] == "timeout"
    finished = records(q)[-1]
    assert finished["reason"] == "timeout" and finished["code"] == 124
    assert any("show" in args for args in calls(host, "systemctl"))


def test_cancel_running_unit_requests_stop_and_callback_records_cancelled(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    ident = q.submit(spec(owner="session-a"))
    q.tick()
    q.started(ident)
    q.cancel(ident, "session-a")
    stop = calls(host, "systemctl")[-1]
    assert "stop" in stop and f"gq-{ident}.service" in stop
    q.finish(ident, "signal", "killed", "15")
    assert q.get(ident)["status"] == "cancelled"
    assert records(q)[-1]["reason"] == "cancelled"


def test_cli_reports_a_failed_stop_and_reconcile_retries_until_it_stops(
        gq, host, monkeypatch, capsys):
    q = make_queue(gq, host, monkeypatch)
    clock = [1000.0]
    monkeypatch.setattr(q, "now", lambda: clock[0])
    ident = q.submit(spec(owner="test-worktree"))
    q.tick()
    assert q.started(ident)
    original_systemctl = gq.systemctl
    stop_calls = []

    def fail_first_stop(*args):
        if "stop" in args:
            stop_calls.append(args)
            if len(stop_calls) == 1:
                return subprocess.CompletedProcess(args, 1, "", "fake stop refused")
        return original_systemctl(*args)

    monkeypatch.setattr(gq, "systemctl", fail_first_stop)
    monkeypatch.setattr(gq, "Queue", lambda: q)
    assert gq.main(["cancel", ident]) != 0
    error = capsys.readouterr().err
    assert "fake stop refused" in error and "stop" in error
    assert q.get(ident)["status"] == "running"
    assert q.get(ident)["cancel_reason"] == "cancelled"
    assert len(stop_calls) == 1

    clock[0] += 6
    q.reconcile()
    assert len(stop_calls) == 2
    state = json.loads((host["systemd"] / f"gq-{ident}.json").read_text())
    assert state["ActiveState"] == "inactive"
    q.reconcile()
    assert q.get(ident)["status"] == "cancelled"
    assert records(q)[-1]["reason"] == "cancelled"


def test_failed_systemd_launch_is_terminal_and_logged(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    monkeypatch.setenv("FAKE_SYSTEMD_FAIL", "1")
    ident = q.submit(spec())
    q.tick()
    assert q.get(ident)["status"] == "failed"
    assert "fake systemd launch failed" in (q.home / "logs" / f"{ident}.log").read_text()
    assert records(q)[-1]["reason"] == "failed"


def test_timed_out_launch_without_a_unit_releases_resources_and_finishes_once(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    clock = [1000.0]
    monkeypatch.setattr(q, "now", lambda: clock[0])
    monkeypatch.setattr(gq, "process_token", lambda _pid: "live-launcher")
    original_run = gq.subprocess.run
    timed_out = False

    def timeout_first_launch(args, **kwargs):
        nonlocal timed_out
        if args[0] == "systemd-run" and not timed_out:
            timed_out = True
            raise subprocess.TimeoutExpired(args, 15)
        return original_run(args, **kwargs)

    monkeypatch.setattr(gq.subprocess, "run", timeout_first_launch)
    ident = q.submit(spec("batch", threads=28, mem=20 * GIB))
    q.tick()
    clock[0] += 600
    q.tick()
    assert timed_out
    assert q.get(ident)["status"] in {"cancelled", "failed"}
    finished = [entry for entry in records(q) if entry["event"] == "finished"]
    assert len(finished) == 1 and finished[0]["id"] == ident
    assert not any(entry["event"] == "started" for entry in records(q))
    q.finish(ident, "signal", "killed", "15")
    replacement = q.submit(spec("batch", owner="replacement", threads=28, mem=20 * GIB))
    q.tick()
    assert q.get(replacement)["status"] == "starting"
    assert len([entry for entry in records(q) if entry["event"] == "finished"]) == 1


def test_timed_out_launch_with_a_live_unit_keeps_resources_until_stopped(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    clock = [1000.0]
    monkeypatch.setattr(q, "now", lambda: clock[0])
    monkeypatch.setattr(gq, "process_token", lambda _pid: "live-launcher")
    original_run = gq.subprocess.run
    original_systemctl = gq.systemctl
    timed_out = False
    stops = []

    def timeout_after_creating_unit(args, **kwargs):
        nonlocal timed_out
        result = original_run(args, **kwargs)
        if args[0] == "systemd-run" and not timed_out:
            timed_out = True
            raise subprocess.TimeoutExpired(args, 15)
        return result

    def stopping_asynchronously(*args):
        if "stop" in args:
            stops.append(args)
            # --no-block only requests a stop; the unit is still running.
            return subprocess.CompletedProcess(args, 0, "", "")
        return original_systemctl(*args)

    monkeypatch.setattr(gq.subprocess, "run", timeout_after_creating_unit)
    monkeypatch.setattr(gq, "systemctl", stopping_asynchronously)
    ident = q.submit(spec("batch", threads=28, mem=20 * GIB))
    q.tick()
    replacement = q.submit(spec("batch", owner="replacement", threads=28, mem=20 * GIB))
    clock[0] += 600
    q.tick()
    assert stops and q.get(ident)["status"] == "starting"
    assert q.get(replacement)["status"] == "queued"
    assert not any(entry["event"] == "finished" for entry in records(q))
    q.finish(ident, "signal", "killed", "15")
    q.tick()
    assert q.get(ident)["status"] == "cancelled"
    assert q.get(replacement)["status"] == "starting"
    finished = [entry for entry in records(q) if entry["event"] == "finished"]
    assert len(finished) == 1 and finished[0]["reason"] == "cancelled"


def test_exec_stop_post_callback_records_timeout_even_without_waiter(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    ident = q.submit(spec())
    q.tick()
    q.started(ident)
    env = {**host["env"], "SERVICE_RESULT": "timeout", "EXIT_CODE": "killed", "EXIT_STATUS": "15"}
    completed = subprocess.run([sys.executable, str(SCRIPT), "_finish", ident],
                               env=env, capture_output=True, text=True, timeout=5)
    assert completed.returncode == 0, completed.stderr
    assert q.get(ident)["status"] == "timeout"
    assert records(q)[-1]["reason"] == "timeout"


@pytest.mark.parametrize("kind,shared_allowed", [("quick", True), ("batch", True), ("quiet", False)])
def test_unit_process_holds_legacy_lock_until_command_finishes(gq, host, monkeypatch, kind, shared_allowed):
    q = make_queue(gq, host, monkeypatch)
    ready = host["home"].parent / "ready"
    release = host["home"].parent / "release"
    command = [sys.executable, "-c", (
        "from pathlib import Path; import sys,time; "
        "Path(sys.argv[1]).write_text('ready'); "
        "deadline=time.monotonic()+10; "
        "exec('while not Path(sys.argv[2]).exists() and time.monotonic()<deadline: time.sleep(.01)')"
    ), str(ready), str(release)]
    ident = q.submit(spec(kind, command=command))
    q.tick()
    process = subprocess.Popen([sys.executable, str(SCRIPT), "_execute", ident],
                               env=host["env"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while not ready.exists() and process.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        assert ready.exists(), "unit helper did not start the command"
        assert q.get(ident)["status"] == "running"
        with host["legacy"].open("a+") as old_client:
            with pytest.raises(BlockingIOError):
                fcntl.flock(old_client, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with host["legacy"].open("a+") as new_client:
            if shared_allowed:
                fcntl.flock(new_client, fcntl.LOCK_SH | fcntl.LOCK_NB)
            else:
                with pytest.raises(BlockingIOError):
                    fcntl.flock(new_client, fcntl.LOCK_SH | fcntl.LOCK_NB)
    finally:
        release.touch()
        stdout, stderr = process.communicate(timeout=12)
    assert process.returncode == 0, stderr
    with host["legacy"].open("a+") as old_client:
        fcntl.flock(old_client, fcntl.LOCK_EX | fcntl.LOCK_NB)


@pytest.mark.parametrize("when", ["before-popen", "after-popen"])
@pytest.mark.parametrize("cancel_request", [False, True])
def test_stop_during_spawn_prevents_the_user_command_from_running(
        gq, host, monkeypatch, when, cancel_request):
    q = make_queue(gq, host, monkeypatch)
    marker = host["home"].parent / "ran-after-stop"
    command = [sys.executable, "-c", (
        "from pathlib import Path; import sys,time; "
        "time.sleep(.3); Path(sys.argv[1]).write_text('ran')"
    ), str(marker)]
    ident = q.submit(spec(command=command))
    q.tick()
    original_popen = subprocess.Popen
    children = []
    stopped = False

    def request_stop():
        nonlocal stopped
        stopped = True
        if cancel_request:
            assert q.cancel(ident, "session-a")
        handler = signal.getsignal(signal.SIGTERM)
        assert callable(handler), "the unit helper must handle TERM while it owns the lock"
        handler(signal.SIGTERM, None)

    def spawning_with_stop(*args, **kwargs):
        # Deliver TERM after handler installation, immediately around the actual
        # spawn. The child does not exist yet or is not assigned in execute yet.
        if when == "before-popen":
            request_stop()
        child = original_popen(*args, **kwargs)
        children.append(child)
        if when == "after-popen":
            request_stop()
        return child

    monkeypatch.setattr(gq, "systemctl", lambda *args: subprocess.CompletedProcess(args, 0, "", ""))
    monkeypatch.setattr(gq.subprocess, "Popen", spawning_with_stop)
    previous = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
    try:
        assert gq.execute(q, ident) == 143
        assert stopped
        assert not marker.exists(), "a user command ran after cancellation/TERM during spawn"
    finally:
        for child in children:
            if child.poll() is None:
                child.kill()
            child.wait(timeout=5)
        for sig, handler in previous.items():
            signal.signal(sig, handler)


@pytest.mark.parametrize("waiter_state,existing_reason,expected_reason,expected_code", [
    ("dead", None, "cancelled", 143),
    ("live", None, "failed", 143),
    ("detached", None, "failed", 143),
    ("dead", "timeout", "timeout", 124),
])
def test_dead_waiter_at_gate_records_cancellation_when_child_already_exited(
        queue, gq, monkeypatch, waiter_state, existing_reason, expected_reason, expected_code):
    waiter = None if waiter_state == "detached" else {"pid": 999999999, "start": "waiter"}
    ident = queue.submit(spec(), waiter=waiter)
    monkeypatch.setattr(queue, "launch", lambda _job: None)
    queue.tick()
    assert queue.get(ident)["status"] == "starting"
    polls = []
    permits = []
    original_write = os.write

    class FinishedGate:
        returncode = 143

        def poll(self):
            polls.append(self.returncode)
            return self.returncode

    def already_exited_gate(*_args, **_kwargs):
        if existing_reason:
            with queue.transaction() as state:
                state["jobs"][0]["cancel_reason"] = existing_reason
        return FinishedGate()

    def capture_permission(fd, token):
        permits.append(token)
        return original_write(fd, token)

    def unexpected_retry(*_args, **_kwargs):
        pytest.fail("the child exited before either monitoring loop could cancel")

    monkeypatch.setattr(gq.subprocess, "Popen", already_exited_gate)
    monkeypatch.setattr(gq.os, "write", capture_permission)
    monkeypatch.setattr(gq, "alive", lambda process: process is None or waiter_state == "live")
    monkeypatch.setattr(gq, "unit_has_children", lambda _ident: False)
    monkeypatch.setattr(queue, "retry_cancel", unexpected_retry)
    monkeypatch.setattr(gq.time, "sleep", lambda _seconds: pytest.fail("monitor loop ran"))

    assert gq.execute(queue, ident) == 143
    assert polls == [143]
    assert permits == ([b"1"] if waiter_state in {"live", "detached"} else [])
    queue.finish(ident, "exit-code", "exited", "143")
    assert queue.get(ident)["status"] == expected_reason
    finished = [record for record in records(queue) if record["event"] == "finished"]
    assert len(finished) == 1
    assert (finished[0]["reason"], finished[0]["code"]) == (expected_reason, expected_code)


def test_execute_keeps_the_user_commands_nonzero_exit_code(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    ident = q.submit(spec(command=[sys.executable, "-c", "raise SystemExit(7)"]))
    q.tick()
    previous = {sig: signal.getsignal(sig) for sig in (signal.SIGTERM, signal.SIGINT)}
    try:
        assert gq.execute(q, ident) == 7
        assert not any("set-property" in args for args in calls(host, "systemctl"))
    finally:
        for sig, handler in previous.items():
            signal.signal(sig, handler)


@pytest.mark.parametrize("action", ["run", "submit"])
def test_archive_options_require_absolute_paths_below_local_root(gq, queue, monkeypatch, tmp_path, action):
    root = tmp_path / "local"
    root.mkdir()
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(root))
    arguments = [action, "--class", "quick", "--threads", "1", "--mem", "1G",
                 "--max", "1m", "--name", "archive"]
    args = gq.parser().parse_args(arguments + ["--archive", str(root / "one"),
                                             "--archive", str(root / "two"), "--", "true"])
    ident = queue.submit(dict(spec(), archive=args.archive))
    assert queue.get(ident)["archive"] == [str(root / "one"), str(root / "two")]
    for invalid in (root, tmp_path / "outside", Path("relative")):
        with pytest.raises(gq.QueueError, match="absolute|local root"):
            queue.submit(dict(spec(), archive=[str(invalid)]))
    assert len(queue.jobs()) == 1


@pytest.mark.parametrize("code,first_missing", [(0, False), (7, False), (17, True)])
def test_archive_after_command_preserves_exit_code_and_logs_each_result(
        gq, host, monkeypatch, tmp_path, code, first_missing, capsys):
    root, nas = tmp_path / "local", tmp_path / "nas"
    root.mkdir()
    nas.mkdir()
    mounts = tmp_path / "mounts"
    mounts.write_text(f"//server/share {nas} cifs rw 0 0\n")
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(root))
    monkeypatch.setenv("GOTHIC_NAS_ROOT", str(nas))
    monkeypatch.setenv("GOTHIC_NAS_MOUNTS", str(mounts))
    archiver = gq.archive_module()
    monkeypatch.setattr(archiver.shutil, "disk_usage", lambda _path: type("Usage", (), {"free": 10**9})())
    first, second = root / "first", root / "second"
    if not first_missing:
        first.mkdir()
        (first / "data").write_bytes(b"first")
    second.mkdir()
    q = make_queue(gq, host, monkeypatch)
    command = [sys.executable, "-c", "from pathlib import Path; "
               f"Path({str(second / 'data')!r}).write_bytes(b'second'); raise SystemExit({code})"]
    ident = q.submit(dict(spec(command=command), archive=[str(first), str(second)]))
    q.tick()
    assert second.exists() and not (nas / "second").exists()
    assert gq.execute(q, ident) == code
    assert second.exists(), "transfer must start after the command completes"
    monkeypatch.setattr(gq, "unit_cgroup", lambda job_id: f"/gq-{job_id}.service")
    q.finish(ident, "success" if code == 0 else "exit-code", "exited", str(code))
    assert q.get(ident)["code"] == code
    assert not second.exists() and (nas / "second" / "data").read_bytes() == b"second"
    outcomes = [record for record in records(q) if record["event"] == "archive"]
    assert [(record["outcome"], record["bytes"]) for record in outcomes] == [
        ("error", 0) if first_missing else ("moved", 5), ("moved", 6)]
    properties = dict(value.removeprefix("--property=").split("=", 1)
                      for value in calls(host, "systemd-run")[0] if value.startswith("--property="))
    assert properties["RuntimeMaxSec"] == "75s" and properties["TimeoutStopSec"] == "60"
    assert properties["NotifyAccess"] == "exec"
    q.finish(ident, "success")
    assert len([record for record in records(q) if record["event"] == "archive"]) == 2
    monkeypatch.setattr(gq, "Queue", lambda: q)
    assert gq.main(["logs", ident]) == 0
    output = capsys.readouterr().out
    assert "6 bytes" in output and str(second) in output
    assert ("warning: archive" in output) == first_missing


def test_reconcile_missing_unit_skips_archive_from_another_process(gq, host, monkeypatch, tmp_path):
    root = tmp_path / "local"
    source = root / "results"
    source.mkdir(parents=True)
    (source / "data").write_bytes(b"keep")
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(root))
    q = make_queue(gq, host, monkeypatch)
    ident = q.submit(dict(spec(), archive=[str(source)]))
    q.tick()
    assert q.started(ident)
    with q.transaction() as state:
        state["jobs"][0]["reserved"] -= 10
    (host["systemd"] / f"gq-{ident}.json").unlink()
    moved = tmp_path / "move-called"
    recovery = (
        "import gq\nfrom pathlib import Path\n"
        f"gq.archive_module().move = lambda *args, **kwargs: Path({str(moved)!r}).write_text('called')\n"
        f"gq.Queue(proc_locks={str(host['locks'])!r}).reconcile()\n"
    )
    result = subprocess.run([sys.executable, "-c", recovery], cwd=SCRIPT.parent,
                            env={**host["env"], "GQ_OWNER": "another-process"},
                            capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    assert not moved.exists()
    assert (source / "data").read_bytes() == b"keep"
    assert q.get(ident)["status"] == "failed"
    assert q.get(ident)["code"] == 1
    outcomes = [record for record in records(q) if record["event"] == "archive"]
    assert [(record["directory"], record["outcome"], record["bytes"]) for record in outcomes] == [
        (str(source), "skipped", 0)]
    line = f"gq: warning: archive {source}: skipped after recovery; local data kept (0 bytes)"
    assert (q.home / "logs" / f"{ident}.log").read_text() == line + "\n"


@pytest.mark.parametrize("running", [False, True])
def test_cancelled_jobs_do_not_archive(gq, queue, monkeypatch, tmp_path, running):
    root = tmp_path / "local"
    source = root / "results"
    source.mkdir(parents=True)
    (source / "data").write_bytes(b"keep")
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(root))
    ident = queue.submit(dict(spec(), archive=[str(source)]))
    if running:
        queue.tick()
    queue.cancel(ident, "session-a")
    queue.finish(ident, "signal", "killed", "15")
    assert source.exists() and (source / "data").read_bytes() == b"keep"
    assert queue.get(ident)["status"] == "cancelled"
    assert not any(record["event"] == "archive" for record in records(queue))


def test_archive_extends_stop_timeout_only_during_transfer(gq, host, monkeypatch, tmp_path):
    root = tmp_path / "local"
    root.mkdir()
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(root))
    q = make_queue(gq, host, monkeypatch)
    plain = q.submit(spec())
    ident = q.submit(dict(spec(), archive=[str(root / "results")]))
    q.tick()
    q.started(plain)
    q.started(ident)
    properties = [dict(value.removeprefix("--property=").split("=", 1)
                       for value in args if value.startswith("--property="))
                  for args in calls(host, "systemd-run")]
    assert "NotifyAccess" not in properties[0] and properties[1]["NotifyAccess"] == "exec"
    with tempfile.TemporaryDirectory(prefix="gq-notify-") as temporary:
        address = str(Path(temporary) / "notify")
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as receiver:
            receiver.bind(address)
            receiver.settimeout(2)
            monkeypatch.setenv("NOTIFY_SOCKET", address)
            q.finish(plain, "success")
            receiver.setblocking(False)
            with pytest.raises(BlockingIOError):
                receiver.recv(1024)
            receiver.settimeout(2)

            def transfer(_job):
                assert receiver.recv(1024) == b"EXTEND_TIMEOUT_USEC=60000000"
                assert any(thread.name == "gq-archive-timeout" for thread in threading.enumerate())

            monkeypatch.setattr(q, "archive", transfer)
            monkeypatch.setattr(gq, "unit_cgroup", lambda job_id: f"/gq-{job_id}.service")
            q.finish(ident, "success")
            assert not any(thread.name == "gq-archive-timeout" for thread in threading.enumerate())


def test_execute_forwards_term_to_a_command_that_has_already_started(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    ready = host["home"].parent / "command-ready"
    marker = host["home"].parent / "command-survived-term"
    command = [sys.executable, "-c", (
        "from pathlib import Path; import sys,time; "
        "Path(sys.argv[1]).touch(); time.sleep(.6); Path(sys.argv[2]).touch()"
    ), str(ready), str(marker)]
    ident = q.submit(spec(command=command))
    q.tick()
    helper = subprocess.Popen([sys.executable, str(SCRIPT), "_execute", ident],
                              env=host["env"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        deadline = time.monotonic() + 5
        while not ready.exists() and helper.poll() is None and time.monotonic() < deadline:
            time.sleep(.01)
        assert ready.exists(), "the command did not start"
        helper.terminate()
        _stdout, stderr = helper.communicate(timeout=5)
        assert helper.returncode == 143, stderr
        assert not marker.exists(), "TERM reached the helper but not its command"
    finally:
        if helper.poll() is None:
            helper.kill()
        helper.communicate(timeout=5)


def test_owner_and_default_submission_are_stable_from_worktree_subdirectories(
        queue, gq, monkeypatch, tmp_path):
    worktree = tmp_path / "worktrees" / "session-a" / "gothic"
    subdirectory = worktree / "server"
    subdirectory.mkdir(parents=True)
    (worktree / ".git").write_text("gitdir: /unused/worktrees/session-a\n")
    monkeypatch.delenv("GQ_OWNER", raising=False)
    monkeypatch.chdir(worktree)
    request = spec()
    request.pop("owner")
    ident = queue.submit(request)
    assert queue.get(ident)["owner"] == gq.owner() == "session-a"
    monkeypatch.chdir(subdirectory)
    assert gq.owner() == "session-a"
    assert queue.cancel(ident, gq.owner())
    assert queue.get(ident)["status"] == "cancelled"


def test_worktrees_with_the_same_repository_basename_have_distinct_owners(gq, monkeypatch, tmp_path):
    monkeypatch.delenv("GQ_OWNER", raising=False)
    owners = []
    for session in ("session-a", "session-b"):
        root = tmp_path / "worktrees" / session / "gothic"
        (root / "server").mkdir(parents=True)
        (root / ".git").write_text(f"gitdir: /unused/worktrees/{session}\n")
        monkeypatch.chdir(root / "server")
        owners.append(gq.owner())
    assert owners == ["session-a", "session-b"]


def test_owner_uses_repository_root_for_a_regular_checkout(gq, monkeypatch, tmp_path):
    root = tmp_path / "my-repository"
    (root / ".git").mkdir(parents=True)
    (root / "server" / "nested").mkdir(parents=True)
    monkeypatch.delenv("GQ_OWNER", raising=False)
    monkeypatch.chdir(root / "server" / "nested")
    assert gq.owner() == "my-repository"


def test_explicit_owner_overrides_worktree_identity_for_submission(queue, gq, monkeypatch, tmp_path):
    monkeypatch.chdir(tmp_path)
    monkeypatch.setenv("GQ_OWNER", "explicit-session")
    request = spec()
    request.pop("owner")
    ident = queue.submit(request)
    assert gq.owner() == queue.get(ident)["owner"] == "explicit-session"


def test_owner_uses_source_worktree_when_called_from_outside_a_repository(gq, monkeypatch, tmp_path):
    root = tmp_path / "worktrees" / "source-session" / "gothic"
    (root / "scripts").mkdir(parents=True)
    elsewhere = tmp_path / "elsewhere"
    elsewhere.mkdir()
    monkeypatch.delenv("GQ_OWNER", raising=False)
    monkeypatch.setattr(gq, "SCRIPT", str(root / "scripts" / "gq.py"))
    monkeypatch.chdir(elsewhere)
    assert gq.owner() == "source-session"


def test_installed_queue_requires_owner_when_no_worktree_can_be_identified(
        queue, gq, monkeypatch, tmp_path):
    installed = tmp_path / "gothic-ai" / "bin"
    installed.mkdir(parents=True)
    monkeypatch.delenv("GQ_OWNER", raising=False)
    monkeypatch.setattr(gq, "SCRIPT", str(installed / "gq"))
    monkeypatch.chdir(tmp_path)
    with pytest.raises(gq.QueueError, match="GQ_OWNER"):
        gq.owner()
    # Explicit callers already supplied ownership; do not evaluate the fallback.
    ident = queue.submit(spec(owner="explicit-session"))
    assert queue.get(ident)["owner"] == "explicit-session"


def test_format_version_mismatch_refuses_to_modify_state(gq, host):
    host["home"].mkdir()
    original = json.dumps({"version": gq.FORMAT_VERSION + 1, "jobs": []})
    (host["home"] / "state.json").write_text(original)
    with pytest.raises(gq.QueueError, match="версия формата"):
        gq.Queue(home=host["home"], legacy_lock=host["legacy"], proc_locks=host["locks"])
    assert (host["home"] / "state.json").read_text() == original


def test_estimate_adds_ahead_limits_and_uses_same_name_median(queue, monkeypatch):
    for duration in (10, 30, 20):
        ident = start(queue, name="repeat")
        monkeypatch.setattr(queue, "now", lambda duration=duration: 1000.0 + duration)
        queue.finish(ident, "success")
        monkeypatch.setattr(queue, "now", lambda: 1000.0)
    start(queue, name="other", maximum=100)
    enqueue(queue, name="ahead", maximum=50)
    target = enqueue(queue, name="repeat", maximum=40)
    monkeypatch.setattr(queue, "now", lambda: 1010.0)
    assert queue.estimate(target) == {"upper": 170, "typical": 20}


def test_cli_requires_declared_maximum(gq):
    with pytest.raises(SystemExit) as failure:
        gq.parser().parse_args(["run", "--class", "quick", "--threads", "4", "--mem", "1G", "--name", "missing", "--", "true"])
    assert failure.value.code == 2


def test_wait_streams_log_and_returns_command_exit_code(queue, gq, capsys):
    ident = start(queue)
    (queue.home / "logs" / f"{ident}.log").write_text("command output\n")
    queue.finish(ident, "exit-code", "exited", "7")
    assert gq.wait_job(queue, ident) == 7
    assert capsys.readouterr().out == "command output\n"


def test_status_exposes_owner_class_resources_limit_and_estimate(queue, gq, capsys):
    ident = start(queue, "batch", owner="visible-owner", threads=12, mem=8 * GIB,
                  gpu_mem=2 * GIB, display=True, display_shared=True, maximum=90)
    gq.status(queue)
    output = capsys.readouterr().out
    assert ident in output and "visible-owner" in output
    assert "batch" in output and "running" in output
    assert "12t 8G gpu=2G окно=1" in output
    assert "слот=0" in output
    assert "90s" in output and "ОЦЕНКА" in output


@pytest.mark.parametrize("persisted", [False, True])
def test_status_marks_legacy_display_as_all_slots(queue, gq, capsys, persisted):
    ident = start(queue, display=True)
    if persisted:
        with queue.transaction() as state:
            next(job for job in state["jobs"] if job["id"] == ident).pop("display_shared")
    gq.status(queue)
    line = next(line for line in capsys.readouterr().out.splitlines() if ident in line)
    assert "окно=все" in line


def test_fairness_only_counts_other_sessions_still_waiting(queue):
    first = enqueue(queue, "batch", owner="session-a", threads=8)
    other = enqueue(queue, "batch", owner="session-b", threads=8)
    second = enqueue(queue, "batch", owner="session-a", threads=8)
    queue.tick()
    assert all(queue.get(ident)["status"] == "running" for ident in (first, other, second))


def test_reconcile_recovers_missing_unit_after_launcher_died(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    clock = [1000.0]
    monkeypatch.setattr(q, "now", lambda: clock[0])
    ident = q.submit(spec())
    q.tick()
    with q.transaction() as state:
        state["jobs"][0]["launcher"] = {"pid": 999999999, "start": "dead"}
    (host["systemd"] / f"gq-{ident}.json").unlink()
    clock[0] += 6
    q.reconcile()
    assert q.get(ident)["status"] == "failed"


def test_live_launcher_keeps_pending_reservations_during_sequential_launches(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    clock = [1000.0]
    monkeypatch.setattr(q, "now", lambda: clock[0])
    monkeypatch.setattr(gq, "process_token", lambda _pid: "live-launcher")
    original_launch = q.launch
    starts = []

    def slow_launch(job):
        q.reconcile()
        original_launch(job)
        starts.append(q.started(job["id"]))
        clock[0] += 11

    monkeypatch.setattr(q, "launch", slow_launch)
    ids = [q.submit(spec(mem=GIB)) for _ in range(4)]
    q.tick()
    assert clock[0] == 1044.0
    assert starts == [True] * 4
    assert all(q.get(ident)["status"] == "running" for ident in ids)
    assert len(calls(host, "systemd-run")) == 4
    assert not any(record.get("reason") == "failed" for record in records(q))


def test_missing_running_unit_is_recovered_even_while_launcher_lives(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    clock = [1000.0]
    monkeypatch.setattr(q, "now", lambda: clock[0])
    monkeypatch.setattr(gq, "process_token", lambda _pid: "live-launcher")
    ident = q.submit(spec())
    q.tick()
    assert q.started(ident)
    (host["systemd"] / f"gq-{ident}.json").unlink()
    clock[0] += 6
    q.reconcile()
    assert q.get(ident)["status"] == "failed"
    assert records(q)[-1]["reason"] == "failed"


def test_follow_logs_reads_output_written_before_terminal_state(queue, gq, monkeypatch, capsys):
    ident = start(queue)
    log = queue.home / "logs" / f"{ident}.log"
    log.write_text("first\n")
    original_stream = gq.stream_log
    completed = False

    def completing_stream(q, job_id, offset):
        nonlocal completed
        offset = original_stream(q, job_id, offset)
        if not completed:
            completed = True
            with log.open("a") as output:
                output.write("last\n")
            q.finish(job_id, "success")
        return offset

    monkeypatch.setattr(gq, "Queue", lambda: queue)
    monkeypatch.setattr(gq, "stream_log", completing_stream)
    assert gq.main(["logs", ident, "-f"]) == 0
    assert capsys.readouterr().out == "first\nlast\n"


def test_legacy_retry_waits_for_prior_exec_stop_post_to_unload(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    ident = q.submit(spec())
    q.tick()
    with q.transaction() as state:
        state["jobs"][0]["retry"] = True
    q.finish(ident, "success")
    q.tick()
    assert q.get(ident)["status"] == "queued"
    assert len(calls(host, "systemd-run")) == 1
    (host["systemd"] / f"gq-{ident}.json").unlink()
    q.tick()
    assert q.get(ident)["status"] == "starting"
    assert len(calls(host, "systemd-run")) == 2


@pytest.mark.parametrize("readmit", [False, True, "same-clock"])
def test_stale_reconcile_completion_cannot_finish_a_deferred_or_readmitted_job(
        gq, host, monkeypatch, readmit):
    q = make_queue(gq, host, monkeypatch)
    clock = [1000.0]
    monkeypatch.setattr(q, "now", lambda: clock[0])
    ident = q.submit(spec())
    q.tick()
    with q.transaction() as state:
        state["jobs"][0]["retry"] = True
    clock[0] += 6
    original_systemctl = gq.systemctl
    raced = False

    def show_after_stop_callback(*args):
        nonlocal raced
        if "show" in args and not raced:
            raced = True
            # The old attempt lost the legacy-lock race. ExecStopPost updates
            # the queue while another process is inspecting that same unit.
            q.finish(ident, "success")
            if readmit:
                (host["systemd"] / f"gq-{ident}.json").unlink()
                observed_at = clock[0]
                # A clock adjustment can give the next reservation the same timestamp;
                # its distinct attempt must still invalidate the stale observation.
                clock[0] = 1000.0 if readmit == "same-clock" else observed_at + 1
                q.tick()
                clock[0] = observed_at + 1
                assert q.get(ident)["status"] == "starting"
            return subprocess.CompletedProcess(args, 0, (
                "LoadState=loaded\nActiveState=inactive\nResult=success\n"
                "ExecMainCode=1\nExecMainStatus=0\n"), "")
        return original_systemctl(*args)

    monkeypatch.setattr(gq, "systemctl", show_after_stop_callback)
    q.reconcile()
    job = q.get(ident)
    assert raced
    assert job["status"] == ("starting" if readmit else "queued")
    if not readmit:
        assert job["retry_pending"]
    assert [entry["event"] for entry in records(q)] == ["submitted", "deferred"]
    if not readmit:
        (host["systemd"] / f"gq-{ident}.json").unlink()
        q.tick()
    assert q.started(ident)
    q.finish(ident, "success")
    assert q.get(ident)["status"] == "done"
    assert [entry["event"] for entry in records(q)] == ["submitted", "deferred", "started", "finished"]
    assert len(calls(host, "systemd-run")) == 2


def test_stale_unloaded_observation_does_not_clear_a_new_attempts_retry(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    monkeypatch.setattr(q, "now", lambda: 1000.0)
    ident = q.submit(spec())
    q.tick()

    def defer_current_attempt():
        with q.transaction() as state:
            state["jobs"][0]["retry"] = True
        q.finish(ident, "success")

    defer_current_attempt()
    old_attempt = q.get(ident)["attempt"]
    original_systemctl = gq.systemctl
    raced = False

    def show_after_next_attempt_deferred(*args):
        nonlocal raced
        if "show" in args and not raced:
            raced = True
            (host["systemd"] / f"gq-{ident}.json").unlink()
            q.tick()
            assert q.get(ident)["status"] == "starting"
            defer_current_attempt()
            systemd_state(host, ident, ActiveState="deactivating", SubState="stop-post")
            return subprocess.CompletedProcess(args, 0, "LoadState=not-found\n", "")
        return original_systemctl(*args)

    monkeypatch.setattr(gq, "systemctl", show_after_next_attempt_deferred)
    q.reconcile()
    current = q.get(ident)
    assert raced and current["attempt"] != old_attempt
    assert current["status"] == "queued" and current["retry_pending"]
    q.tick()
    assert q.get(ident)["status"] == "queued"
    assert len(calls(host, "systemd-run")) == 2
    assert [entry["event"] for entry in records(q)] == ["submitted", "deferred", "deferred"]


def test_deferred_batch_keeps_fifo_while_quick_can_run(gq, host, monkeypatch):
    q = make_queue(gq, host, monkeypatch)
    first = q.submit(spec("batch", owner="first", threads=28))
    q.tick()
    with q.transaction() as state:
        state["jobs"][0]["retry"] = True
    q.finish(first, "success")
    systemd_state(host, first, ActiveState="deactivating", SubState="stop-post")
    second = q.submit(spec("batch", owner="second", threads=28))
    quick = q.submit(spec())
    q.tick()
    assert q.get(first)["status"] == "queued" and q.get(first)["retry_pending"]
    assert q.get(second)["status"] == "queued"
    assert q.get(quick)["status"] == "starting"
    assert q.started(quick)
    q.finish(quick, "success")
    (host["systemd"] / f"gq-{first}.json").unlink()
    q.tick()
    assert q.get(first)["status"] == "starting"
    assert q.get(second)["status"] == "queued"
    assert q.started(first)
    q.finish(first, "success")
    q.tick()
    assert q.get(second)["status"] == "starting"
    assert q.started(second)
    started = [entry["id"] for entry in records(q) if entry["event"] == "started"]
    assert started == [quick, first, second]


def test_unit_lock_holder_detects_orphaned_children_in_its_own_cgroup(gq, tmp_path):
    membership = tmp_path / "membership"
    group = tmp_path / "user.slice" / "gq-abc.service"
    group.mkdir(parents=True)
    membership.write_text("0::/user.slice/gq-abc.service\n")
    (group / "cgroup.procs").write_text(f"{os.getpid()}\n999999\n")
    assert gq.unit_has_children("abc", membership, tmp_path)
    assert not gq.unit_has_children("another-unit", membership, tmp_path)
    (group / "cgroup.procs").write_text(f"{os.getpid()}\n")
    assert not gq.unit_has_children("abc", membership, tmp_path)
    (group / "cgroup.procs").unlink()
    assert gq.unit_has_children("abc", membership, tmp_path), "unreadable unit fails closed"


def test_finish_journals_oom_kill_count_and_memory_peak_from_its_unit(
        queue, gq, tmp_path, monkeypatch):
    ident = start(queue, display=True, display_shared=True)
    group = f"/user.slice/gq-{ident}.service"
    directory = tmp_path / group.lstrip("/")
    directory.mkdir(parents=True)
    (directory / "memory.events").write_text("low 0\nhigh 0\nmax 7\noom 2\noom_kill 1\n")
    (directory / "memory.peak").write_text("1832411136\n")
    membership = tmp_path / "membership"
    membership.write_text(f"0::{group}\n")
    assert gq.unit_cgroup(ident, membership) == group
    assert gq.unit_cgroup("another", membership) is None
    original_stats = gq.cgroup_memory_stats
    monkeypatch.setattr(gq, "unit_cgroup", lambda _ident: group)
    monkeypatch.setattr(gq, "cgroup_memory_stats",
                        lambda job_id, saved: original_stats(job_id, saved, tmp_path))
    queue.finish(ident, "signal", "killed", "9")
    finished = records(queue)[-1]
    assert finished["oom_kill"] == 1 and finished["memory_peak"] == 1832411136
    assert finished["code"] == 137 and finished["display_slot"] == 0


def test_missing_cgroup_counters_are_unknown_instead_of_zero(queue, gq, tmp_path):
    assert gq.cgroup_memory_stats("missing", "/missing", tmp_path) == {
        "oom_kill": None, "memory_peak": None}
    ident = start(queue)
    queue.finish(ident, "success")
    finished = records(queue)[-1]
    assert finished["oom_kill"] is None and finished["memory_peak"] is None


def test_reconcile_prunes_a_thousand_old_jobs_but_wait_and_logs_use_the_archive(
        gq, host, monkeypatch, capsys):
    q = gq.Queue(home=host["home"], legacy_lock=host["legacy"], proc_locks=host["locks"])
    now = 200000.0
    monkeypatch.setattr(q, "now", lambda: now)
    monkeypatch.setattr(q, "disk_free", lambda: 100 * GIB)
    seed = q.submit(spec())
    template = q.get(seed)
    old_end = now - 24 * 3600 - 1
    old_jobs = [dict(template, id=f"old-{index:04d}", status="failed", reason="failed",
                     code=7, submitted=old_end - 10, start=old_end - 5, end=old_end,
                     env={"ONLY_OLD_JOB_ENV": "x" * 1000}) for index in range(1000)]
    recent = dict(template, id="recent", status="done", reason="done", code=0, end=now - 60)
    with q.transaction() as state:
        state["jobs"] = [*old_jobs, recent]
    archived = old_jobs[0]
    journal = dict(archived, event="finished", time=old_end)
    journal.pop("env")
    journal_path = q.home / "journal.jsonl"
    journal_path.write_text(json.dumps(journal) + "\n")
    log_path = q.home / "logs" / f"{archived['id']}.log"
    log_path.write_text("archived output\n")

    q.reconcile()
    assert [job["id"] for job in q.jobs()] == ["recent"]
    assert "ONLY_OLD_JOB_ENV" not in (q.home / "state.json").read_text()
    assert q.get(archived["id"])["code"] == 7
    assert journal_path.read_text() == json.dumps(journal) + "\n"
    assert log_path.read_text() == "archived output\n"

    monkeypatch.setattr(gq, "Queue", lambda: q)
    assert gq.main(["wait", archived["id"]]) == 7
    assert capsys.readouterr().out == "archived output\n"
    assert gq.main(["logs", archived["id"]]) == 0
    assert capsys.readouterr().out == "archived output\n"


def test_logs_can_read_a_preserved_file_without_a_state_or_journal_entry(queue, gq, monkeypatch, capsys):
    ident = "logs-only"
    (queue.home / "logs" / f"{ident}.log").write_text("preserved log\n")
    monkeypatch.setattr(gq, "Queue", lambda: queue)
    assert gq.main(["logs", ident]) == 0
    assert capsys.readouterr().out == "preserved log\n"


def test_ten_ticks_in_five_seconds_reconcile_each_active_job_once_across_processes(
        gq, host, monkeypatch):
    queues = [gq.Queue(home=host["home"], legacy_lock=host["legacy"], proc_locks=host["locks"])
              for _ in range(2)]
    clock = [1000.0]
    for q in queues:
        monkeypatch.setattr(q, "now", lambda: clock[0])
        monkeypatch.setattr(q, "disk_free", lambda: 100 * GIB)
    first = queues[0].submit(spec())
    second = queues[0].submit(spec())
    queues[0].tick()
    assert queues[0].started(first) and queues[0].started(second)

    for index in range(10):
        clock[0] = 1005.0 + index / 2
        queues[index % 2].tick()
    shows = [args for args in calls(host, "systemctl") if "show" in args]
    assert len(shows) == 2
    for ident in (first, second):
        assert sum(f"gq-{ident}.service" in args for args in shows) == 1

    clock[0] = 1010.0
    queues[1].tick()
    assert len([args for args in calls(host, "systemctl") if "show" in args]) == 4


@pytest.mark.parametrize("action", ["wait", "_pump", "logs"])
def test_waiters_lower_priority_once_and_poll_queued_jobs_once_per_second(
        queue, gq, monkeypatch, action):
    ident = enqueue(queue)
    priorities = []
    sleeps = []

    def cancel_after_sleep(seconds):
        sleeps.append(seconds)
        queue.cancel(ident, "session-a")

    monkeypatch.setattr(queue, "tick", lambda: None)
    monkeypatch.setattr(gq, "Queue", lambda: queue)
    monkeypatch.setattr(gq.os, "nice", lambda increment: priorities.append(increment))
    monkeypatch.setattr(gq.time, "sleep", cancel_after_sleep)
    args = [action, ident] + (["-f"] if action == "logs" else [])
    assert gq.main(args) == (143 if action == "wait" else 0)
    assert sleeps == [1]
    assert priorities == [19]


@pytest.mark.parametrize("source", ["environment", "file", "missing"])
def test_profile_selection_uses_environment_then_private_file_otherwise_errors(
        gq, host, monkeypatch, tmp_path, source):
    private_home = tmp_path / "profile-home"
    private_home.mkdir()
    monkeypatch.setenv("HOME", str(private_home))
    profile_path = private_home / "gothic-ai" / "bin" / "gq.profile"
    if source == "environment":
        monkeypatch.setenv("GQ_PROFILE", "ryzen")
        profile_path.parent.mkdir(parents=True)
        profile_path.write_text("file-must-not-override-environment\n")
    else:
        monkeypatch.delenv("GQ_PROFILE", raising=False)
        if source == "file":
            profile_path.parent.mkdir(parents=True)
            profile_path.write_text("ryzen\n")

    kwargs = dict(home=host["home"], legacy_lock=host["legacy"], proc_locks=host["locks"])
    if source == "missing":
        with pytest.raises(gq.QueueError, match="GQ_PROFILE|gq\\.profile"):
            gq.Queue(**kwargs)
    else:
        q = gq.Queue(**kwargs)
        assert q.profile_name == "ryzen" and q.profile == gq.PROFILES["ryzen"]


def test_install_copies_executable_and_private_profile_to_remote_home(gq, monkeypatch, tmp_path):
    wrapper_path = SCRIPT.with_name("gq")
    loader = SourceFileLoader("gq_wrapper_test_module", str(wrapper_path))
    module_spec = importlib.util.spec_from_loader(loader.name, loader)
    wrapper = importlib.util.module_from_spec(module_spec)
    loader.exec_module(wrapper)
    remote_home = tmp_path / "remote-home"
    remote_home.mkdir()
    ssh_calls = []

    def local_ssh(args, *, stdin):
        ssh_calls.append(args)
        result = subprocess.run(["sh", "-c", args[-1]], stdin=stdin,
                                env={**os.environ, "HOME": str(remote_home)},
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 0, result.stderr
        return result.returncode

    monkeypatch.delenv("GQ_PROFILE", raising=False)
    monkeypatch.setattr(wrapper.subprocess, "call", local_ssh)
    assert wrapper.install("ryzen") == 0
    assert len(ssh_calls) == 1 and ssh_calls[0][-2] == "ryzen"
    installed = remote_home / "gothic-ai" / "bin"
    assert (installed / "gq").read_bytes() == SCRIPT.read_bytes()
    assert os.access(installed / "gq", os.X_OK)
    assert (installed / "gq.profile").read_text() == "ryzen\n"
    assert sorted(path.name for path in installed.iterdir()) == ["gq", "gq.profile"]


def test_linked_worktrees_with_matching_directory_names_use_their_git_identity(gq, monkeypatch, tmp_path):
    monkeypatch.delenv('GQ_OWNER', raising=False)
    owners = []
    for session in ('linked-a', 'linked-b'):
        root = tmp_path / 'checkouts' / session / 'gothic'
        (root / 'server').mkdir(parents=True)
        gitdir = tmp_path / 'common.git' / 'worktrees' / session
        (root / '.git').write_text(f'gitdir: {gitdir}\n')
        monkeypatch.chdir(root / 'server')
        owners.append(gq.owner())
    assert owners == ['linked-a', 'linked-b']


def test_started_records_systemd_invocation_for_gpu_monitor(queue, monkeypatch):
    q = queue
    job_id = q.submit(spec())
    with q.transaction() as state:
        state["jobs"][0].update(status="starting", attempt="1"*32)
    monkeypatch.setenv("INVOCATION_ID", "a"*32)
    assert q.started(job_id)
    assert q.get(job_id)["invocation_id"] == "a"*32
