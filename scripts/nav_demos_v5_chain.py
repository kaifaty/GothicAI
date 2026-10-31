#!/usr/bin/env python3
"""Record memory-v4 demonstrations in bounded gq phases, without training."""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import shutil
import subprocess
import sys
from types import SimpleNamespace

import importlib.util


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


v4 = module("nav_demos_v4_chain")
collect = v4.collect
MAX_BYTES = 30_000_000_000
LOCAL_FREE_BYTES = 20 * 1024**3
OWNER = "nav-demos-v5"
TARGETS = dict(point=400, object=100, approach=60, follow_below_gate=60, jump=168, water=40)
SUCCESS_FAMILIES = {"object", "approach", "follow_below_gate", "water"}


def files(root):
    """Inventory regular files, rejecting links before copy or deletion."""
    root = Path(root)
    if root.is_symlink():
        raise ValueError(f"Symlink is forbidden in demonstration storage: {root}")
    result = {}
    for path in root.rglob("*"):
        if path.is_symlink():
            raise ValueError(f"Symlink is forbidden in demonstration storage: {path}")
        if path.is_file():
            result[path.relative_to(root)] = path
    return result


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def used_bytes(task, archive_root):
    archived = files(archive_root)
    used = sum(path.stat().st_size for path in archived.values())
    # Controls are mirrored, while an interrupted transfer may have both copies.
    for relative, path in files(task / "recordings").items():
        target = archived.get(Path("recordings") / relative)
        used += max(0, path.stat().st_size - (target.stat().st_size if target else 0))
    return used


def require_room(task, archive_root):
    v4.disk_check()
    used = used_bytes(task, archive_root)
    free = shutil.disk_usage(task).free
    if used >= MAX_BYTES or free < LOCAL_FREE_BYTES:
        raise ValueError(f"Demonstration storage limit: used={used}, max={MAX_BYTES}, "
                         f"local_free={free}, local_floor={LOCAL_FREE_BYTES}")
    return used


def relocate(value, source, target):
    if isinstance(value, dict):
        return {key: relocate(item, source, target) for key, item in value.items()}
    if isinstance(value, list):
        return [relocate(item, source, target) for item in value]
    if isinstance(value, str) and value.startswith(str(source) + "/"):
        return str(target / Path(value).relative_to(source))
    return value


def archive_provenance(task, archive_root):
    for name in ("input", "recording-input", "knownroads"):
        source, target = task / name, archive_root / name
        if not source.is_dir():
            raise ValueError(f"Missing demonstration provenance: {source}")
        incoming, present = files(source), files(target)
        additional = sum(max(0, path.stat().st_size -
            (present[relative].stat().st_size if relative in present else 0))
            for relative, path in incoming.items())
        if used_bytes(task, archive_root) + additional > MAX_BYTES:
            raise ValueError("Provenance would exceed the corpus byte budget")
        expected = {relative: digest(path) for relative, path in incoming.items()}
        target.mkdir(parents=True, exist_ok=True)
        files(target)
        subprocess.run(["rsync", "-a", "--no-links", "--", str(source) + "/",
                        str(target) + "/"], check=True)
        copied = {relative: digest(path) for relative, path in files(target).items()}
        if copied != expected:
            raise ValueError(f"Archived provenance checksum mismatch: {target}")
    require_room(task, archive_root)


def archive_phase(task, archive_root):
    """Copy a finished phase, verify every file, then remove its local episodes."""
    source, target = task / "recordings", archive_root / "recordings"
    target.mkdir(parents=True, exist_ok=True)
    files(source)
    files(target)
    if used_bytes(task, archive_root) > MAX_BYTES:
        raise ValueError("Finished phase would exceed the corpus byte budget")
    directories = sorted(path for path in source.glob("*/train/*") if path.is_dir())
    before = {path: {relative: digest(file) for relative, file in files(path).items()}
              for path in directories}
    if directories:
        subprocess.run(["rsync", "-a", "--no-links", "--exclude=/collection*", "--",
                        str(source) + "/", str(target) + "/"], check=True)
        # Verify the entire phase before deleting any source directory.
        for path, expected in before.items():
            destination = target / path.relative_to(source)
            copied = {relative: digest(file) for relative, file in files(destination).items()}
            if copied != expected:
                raise ValueError(f"Archived phase checksum mismatch: {destination}")
        for path in directories:
            shutil.rmtree(path)
    for name in ("collection-metrics.json", "collection-state.json"):
        path = source / name
        if path.is_file():
            value = relocate(v4.read_json(path), source, target)
            v4.atomic_json(path, value)
            v4.atomic_json(target / name, value)
    size = used_bytes(task, archive_root)
    if size > MAX_BYTES:
        raise ValueError(f"Archived corpus exceeds its byte budget: {size} > {MAX_BYTES}")
    return dict(archived_directories=len(directories), corpus_bytes=size)


def queue_args(archive_root, stage):
    return ("--archive-root", str(archive_root), "--stage", stage)


def submit_next(task, archive_root, *, stage="record"):
    if not task.is_absolute() or not archive_root.is_absolute():
        raise ValueError("Queue submissions require absolute task and archive directories")
    require_room(task, archive_root)
    return v4.submit_next(task, script=__file__, owner=OWNER,
                          extra=queue_args(archive_root, stage))


def enqueue(task, archive_root, request, *, stage="record"):
    return v4.enqueue(task, request, script=__file__, owner=OWNER,
                      extra=queue_args(archive_root, stage))


def collector_options(task, archive_root):
    return SimpleNamespace(task=task, corpus_version=5, archive_root=archive_root,
        include_follow="no", follow_below_gate=True, point_limit=400,
        parallel=3, limit_ms=240000, max_bytes=MAX_BYTES, wall_minutes=80,
        max_new=2000, round_size=12, verify_existing_only=False, expect_existing=None)


def complete(summary):
    groups = summary.get("by_family", {})
    return all(groups.get(name, {}).get("training_eligible" if name in SUCCESS_FAMILIES
                else "episodes", 0) >= target for name, target in TARGETS.items())


def run(task, archive_root, *, stage="record"):
    task, archive_root = Path(task), Path(archive_root)
    if not task.is_absolute() or not task.is_dir() or not archive_root.is_absolute():
        raise ValueError("Existing absolute task and absolute archive directories are required")
    if archive_root == task or task in archive_root.parents or archive_root in task.parents:
        raise ValueError("Active task and shared archive must be separate directories")
    archive_root.mkdir(parents=True, exist_ok=True)
    with v4.own_task(task):
        state_path = task / "collection-chain.json"
        state = v4.read_json(state_path, {})
        phases = list(state.get("phases", []))
        entry = dict(phase=len(phases) + 1, stage=stage, started=v4.now())
        phases.append(entry)
        recovering_archive = state.get("archive_pending", False)
        state.update(schema_version=1, phase=entry["phase"], phases=phases, status="running",
                     next_job=None, student_training_run=False, targets=TARGETS)

        def save(status, **values):
            entry.update(status=status, **values)
            state.update(status=status, **values)
            v4.atomic_json(state_path, state)
            v4.atomic_json(archive_root / "collection-chain.json", state)
            v4.emit("collection_chain_v5", phase=entry["phase"], status=status, **values)
            return state

        try:
            save("running")
            require_room(task, archive_root)
            if stage == "prepare":
                module("nav_demos_v5_water").prepare(task, archive_root)
                collect.prepare_residency(collector_options(task, archive_root))
                collect.prepare_v5_selections(task)
                archive_provenance(task, archive_root)
                return save("continued", finished=v4.now(),
                            next_job=submit_next(task, archive_root))
            if recovering_archive:
                archive_phase(task, archive_root)
                save("running", archive_pending=False)
            summary_path = task / "recordings/collection-metrics.json"
            before = v4.read_json(summary_path, {})
            entry.update(episodes_before=v4.episode_count(before), rejected_before=v4.rejection_count(before))
            if complete(before):
                result = before
            else:
                result = collect.run(collector_options(task, archive_root))
            entry.update(episodes_after=v4.episode_count(result), rejected_after=v4.rejection_count(result))
            save("archiving", archive_pending=True)
            entry.update(archive_phase(task, archive_root))
            save("finalizing", archive_pending=False)
            if complete(result):
                module("nav_demos_v5_report").finalize(task, archive_root)
                if (task / "build").exists():
                    shutil.rmtree(task / "build")
                return save("complete", finished=v4.now())
            if v4.exhausted(result):
                return save("limit_reached", reason="candidate_pool_exhausted", finished=v4.now())
            if result.get("stop_reason") not in (None, "wall_limit", "episode_limit"):
                return save("limit_reached", reason=result["stop_reason"], finished=v4.now())
            if (v4.episode_count(result) <= v4.episode_count(before)
                    and v4.rejection_count(result) <= v4.rejection_count(before)):
                return save("no_progress", finished=v4.now())
            return save("continued", finished=v4.now(), next_job=submit_next(task, archive_root))
        except Exception as error:
            save("failed", finished=v4.now(), error=type(error).__name__, detail=str(error))
            raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--archive-root", type=Path, required=True)
    parser.add_argument("--stage", choices=("prepare", "record"), default="record")
    parser.add_argument("--submit", action="store_true")
    parser.add_argument("--enqueue", type=Path, help=argparse.SUPPRESS)
    options = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Run on Ryzen/Linux inside a gq batch")
    if options.enqueue is not None:
        if (not options.task.is_absolute() or not options.enqueue.is_absolute()
                or options.enqueue.parent != options.task / "chain-queue"):
            parser.error("Queue request must be an absolute path in the task's chain-queue directory")
        return enqueue(options.task, options.archive_root, options.enqueue, stage=options.stage)
    if options.submit:
        print(submit_next(options.task, options.archive_root, stage=options.stage), flush=True)
        return 0
    run(options.task, options.archive_root, stage=options.stage)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
