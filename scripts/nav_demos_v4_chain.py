#!/usr/bin/env python3
"""Run one bounded recording phase, then queue its continuation when needed.

The caller grants this process one gq batch allocation. A continuation receives
its own allocation; this process never waits for that job or trains a student.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
from contextlib import contextmanager
from datetime import datetime, timezone
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
from types import SimpleNamespace
import uuid


MAX_BYTES = 15_000_000_000
WALL_MINUTES = 80
GQ = Path.home() / "gothic-ai/bin/gq"


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


collect = module("nav_demos_v4_collect")
budget = module("nav_demos_v4_budget")


def read_json(path, default=None):
    path = Path(path)
    return json.loads(path.read_text()) if path.is_file() else default


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)


def now():
    return datetime.now(timezone.utc).isoformat()


def emit(event, **values):
    print(json.dumps(dict(event=event, **values), ensure_ascii=False), flush=True)


def disk_check():
    """Show the shared machine's disk before each phase and submission."""
    subprocess.run(["df", "-h", str(Path.home())], check=True)


@contextmanager
def own_task(task):
    # A just-submitted continuation can start before its parent exits. It waits
    # briefly here, outside collection, until the parent's final state is saved.
    with (task / "collection-chain.lock").open("a") as stream:
        fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def episode_count(summary):
    return len(summary.get("episodes", []))


def rejection_count(summary):
    return len({(budget.family(value), value["id"]) for value in budget.rejection_rows(summary)})


def exhausted(summary):
    return any(value.get("candidate_pool_exhausted") and not value.get("complete")
               for value in summary.get("by_family", {}).values())


def collector_options(task, plan):
    return SimpleNamespace(task=task, include_follow="no", follow_below_gate=True,
        parallel=3, limit_ms=240000, max_bytes=MAX_BYTES, wall_minutes=WALL_MINUTES,
        max_new=2000, round_size=12, verify_existing_only=False, expect_existing=None,
        point_limit=plan["point_limit"])


def submit_command(task, *, script=None, owner="nav-demos-v4", extra=()):
    command = [str(GQ), "submit", "--class", "batch", "--threads", "12",
        "--cpu-quota", "1200%", "--mem", "8G", "--max", "2h", "--name",
        owner.replace("-", " ") + " recording chain", "--", str(task / ".venv/bin/python"),
        "-I", str(Path(__file__).with_name("nav_job.py").resolve()), "--max-seconds", "7200", "--",
        str(task / ".venv/bin/python"), "-X", "faulthandler", "-I",
        str(Path(script or __file__).resolve()), "--task", str(task), *extra]
    return command


def enqueue(task, request, *, script=None, owner="nav-demos-v4", extra=()):
    """Keep gq's detached pump alive in this separate transient user unit."""
    identifier = None
    try:
        disk_check()
        result = subprocess.run(submit_command(task, script=script, owner=owner, extra=extra),
                            check=True, capture_output=True, text=True,
                            env={**os.environ, "GQ_OWNER": owner})
        identifier = result.stdout.strip()
        if not re.fullmatch(r"[0-9a-f]{12}", identifier):
            raise ValueError(f"gq returned an invalid continuation ID: {identifier!r}")
        atomic_json(request, dict(status="submitted", job=identifier))
    except Exception as error:
        # If publishing the ID fails, cancel only the exact request just made.
        if identifier and re.fullmatch(r"[0-9a-f]{12}", identifier):
            subprocess.run([str(GQ), "cancel", identifier], check=False,
                           env={**os.environ, "GQ_OWNER": owner})
        atomic_json(request, dict(status="failed", error=type(error).__name__, detail=str(error)))
        raise
    # gq submit leaves a pump in its caller's cgroup. Waiting here keeps the
    # helper's unit alive, while the recording allocation can finish normally.
    result = subprocess.run([str(GQ), "wait", identifier], check=False,
                            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    atomic_json(request, dict(status="finished", job=identifier, returncode=result.returncode))
    return result.returncode


def submit_next(task, *, script=None, owner="nav-demos-v4", extra=()):
    disk_check()
    token = uuid.uuid4().hex[:12]
    unit = owner + "-queue-" + token
    request = task / "chain-queue" / (token + ".json")
    request.parent.mkdir(parents=True, exist_ok=True)
    command = ["systemd-run", "--user", "--collect", "--quiet", "--service-type=exec",
               "--unit", unit, "--property", f"WorkingDirectory={task / 'source'}"]
    runtime_names = ("PATH", "VULKAN_SDK", "OMP_NUM_THREADS", "OPENBLAS_NUM_THREADS",
                     "LD_LIBRARY_PATH", "GQ_HOME", "GQ_PROFILE", "GQ_LEGACY_LOCK")
    for name in runtime_names:
        if name in os.environ:
            command.append(f"--setenv={name}={os.environ[name]}")
    command.extend(["--setenv=GQ_OWNER=" + owner, str(task / ".venv/bin/python"),
                    "-I", str(Path(script or __file__).resolve()), "--task", str(task), *extra,
                    "--enqueue", str(request)])
    subprocess.run(command, check=True, capture_output=True, text=True)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        response = read_json(request, {})
        if response.get("status") == "failed":
            raise ValueError(f"Continuation submission failed: {response.get('detail')}")
        identifier = response.get("job")
        if identifier:
            if not re.fullmatch(r"[0-9a-f]{12}", identifier):
                raise ValueError(f"Invalid continuation ID in {request}")
            return identifier
        time.sleep(.1)
    subprocess.run(["systemctl", "--user", "stop", unit], check=False, timeout=15)
    raise TimeoutError("Continuation helper did not publish a job ID within 30 seconds")


def save_plan(task, plan, phases, phase):
    atomic_json(task / "recordings/collection-plan.json",
                dict(plan, phase=phase, phases=phases))


def refresh_completed_summary(task, plan):
    """Apply a final smaller road quota to aggregates, preserving episode rows."""
    path = task / "recordings/collection-metrics.json"
    summary = deepcopy(read_json(path, {}))
    groups = summary.get("by_family", {})
    point = groups.get("point", {})
    if point.get("episodes", 0) < plan["point_limit"]:
        raise ValueError("Completed plan has fewer recorded points than its final quota")
    point.update(target=plan["point_limit"], complete=True, candidate_pool_exhausted=False)
    if not groups or not all(value.get("complete") for value in groups.values()):
        raise ValueError("Completed plan disagrees with saved nonpoint goals")
    summary.update(point_limit=plan["point_limit"], complete=True, stop_reason="complete")
    atomic_json(path, summary)
    state_path = task / "recordings/collection-state.json"
    collection_state = read_json(state_path, {})
    collection_state["status"] = "complete"
    atomic_json(state_path, collection_state)


def run(task):
    task = Path(task)
    if not task.is_absolute() or not task.is_dir():
        raise ValueError("An existing absolute task directory is required")
    with own_task(task):
        state_path = task / "collection-chain.json"
        state = read_json(state_path, {})
        phases = list(state.get("phases", []))
        phase = len(phases) + 1
        state.update(schema_version=1, phase=phase, status="planning", next_job=None,
                     student_training_run=False)
        atomic_json(state_path, state)
        disk_check()
        plan = budget.task_plan(task, max_bytes=MAX_BYTES)
        summary_path = task / "recordings/collection-metrics.json"
        before = read_json(summary_path, {})
        entry = dict(phase=phase, started=now(), plan=plan,
                     episodes_before=episode_count(before), rejected_before=rejection_count(before))
        phases.append(entry)
        state["phases"] = phases
        save_plan(task, plan, phases, phase)
        emit("collection_chain_plan", phase=phase, plan=plan)

        def stop(status, **values):
            entry.update(finished=now(), status=status, **values)
            state.update(status=status, **values)
            atomic_json(state_path, state)
            save_plan(task, plan, phases, phase)
            emit("collection_chain_stopped", phase=phase, status=status, **values)
            return state

        if not plan.get("can_collect"):
            if plan.get("reason") == "complete":
                refresh_completed_summary(task, plan)
            return stop("complete" if plan.get("reason") == "complete" else "limit_reached",
                        reason=plan.get("reason"))
        state.update(status="collecting")
        atomic_json(state_path, state)
        try:
            result = collect.run(collector_options(task, plan))
        except Exception as error:
            stop("failed", error=type(error).__name__, detail=str(error))
            raise
        entry.update(episodes_after=episode_count(result), rejected_after=rejection_count(result),
                     complete=result.get("complete", False))
        if result.get("complete"):
            return stop("complete")
        if exhausted(result):
            return stop("limit_reached", reason="candidate_pool_exhausted")
        if result.get("stop_reason") not in (None, "wall_limit", "episode_limit"):
            return stop("limit_reached", reason=result["stop_reason"])
        if (episode_count(result) <= episode_count(before)
                and rejection_count(result) <= rejection_count(before)):
            return stop("no_progress", reason="No new finalized episodes or rejected candidates in this phase")

        # Forecast again before admitting another allocation. This also makes
        # the latest reduced road target authoritative for the next phase.
        disk_check()
        next_plan = budget.task_plan(task, max_bytes=MAX_BYTES)
        entry["next_plan"] = next_plan
        save_plan(task, next_plan, phases, phase)
        if not next_plan.get("can_collect"):
            plan = next_plan
            if plan.get("reason") == "complete":
                refresh_completed_summary(task, plan)
            return stop("complete" if next_plan.get("reason") == "complete" else "limit_reached",
                        reason=next_plan.get("reason"))
        try:
            identifier = submit_next(task)
        except Exception as error:
            stop("submission_failed", error=type(error).__name__, detail=str(error))
            raise
        entry.update(finished=now(), status="continued", next_job=identifier)
        state.update(status="continued", next_job=identifier)
        atomic_json(state_path, state)
        save_plan(task, next_plan, phases, phase)
        emit("collection_chain_continued", phase=phase, next_job=identifier)
        return state


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--enqueue", type=Path,
                        help=argparse.SUPPRESS)
    options = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Run on Ryzen/Linux inside a gq batch")
    if options.enqueue is not None:
        if (not options.task.is_absolute() or not options.enqueue.is_absolute()
                or options.enqueue.parent != options.task / "chain-queue"):
            parser.error("Queue request must be an absolute path in the task's chain-queue directory")
        return enqueue(options.task, options.enqueue)
    run(options.task)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
