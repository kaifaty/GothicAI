"""Disk janitor checks with isolated roots, processes, queue, mounts and tools."""
from __future__ import annotations

from datetime import datetime, timezone
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from types import SimpleNamespace

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "ryzen_janitor.py"
DAY = 24 * 3600
NOW = datetime(2026, 10, 28, 12, tzinfo=timezone.utc).timestamp()


@pytest.fixture(scope="module")
def janitor():
    sys.path.insert(0, str(SCRIPT.parent))
    try:
        spec = importlib.util.spec_from_file_location("ryzen_janitor_test", SCRIPT)
        module = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = module
        spec.loader.exec_module(module)
        return module
    finally:
        sys.path.pop(0)


def age_tree(path, age=8 * DAY):
    for child in sorted(path.rglob("*"), key=lambda item: len(item.parts), reverse=True):
        os.utime(child, (NOW - age, NOW - age), follow_symlinks=False)
    os.utime(path, (NOW - age, NOW - age), follow_symlinks=False)
    return path


@pytest.fixture
def host(tmp_path, monkeypatch, janitor):
    root, nas, uv_cache, proc = (tmp_path / name for name in ("local", "nas", "uv", "proc"))
    for directory in (root, nas, uv_cache, proc):
        directory.mkdir()
    state = root / ".janitor"
    state.mkdir()
    stamp = state / "uv-clean"
    stamp.touch()
    os.utime(stamp, (NOW, NOW))
    mounts = tmp_path / "mounts"
    mounts.write_text(f"//fake/share {nas} cifs rw 0 0\n")
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(root))
    monkeypatch.setenv("GOTHIC_NAS_ROOT", str(nas))
    monkeypatch.setenv("GOTHIC_NAS_MOUNTS", str(mounts))
    host = SimpleNamespace(root=root, nas=nas, uv_cache=uv_cache, proc=proc,
                           mounts=mounts, stamp=stamp, uv=tmp_path / "fake-uv",
                           free=200 * 1024**3, calls=[], uv_busy=False,
                           interrupt_copy=False)
    monkeypatch.setattr(janitor.shutil, "disk_usage", lambda _path: SimpleNamespace(free=host.free))

    def run(command, **kwargs):
        host.calls.append((command, kwargs))
        if command[0] == str(host.uv):
            assert command[1:3] == ["cache", "clean"]
            assert kwargs["env"]["UV_LOCK_TIMEOUT"] == "30"
            if host.uv_busy:
                return subprocess.CompletedProcess(command, 2, "", "Failed to acquire cache lock: timed out")
            for path in uv_cache.iterdir():
                shutil.rmtree(path) if path.is_dir() else path.unlink()
            return subprocess.CompletedProcess(command, 0, "Cache cleaned", "")
        assert command[0] == "rsync", f"unexpected real tool: {command}"
        source, destination = map(Path, command[-2:])
        for child in source.iterdir():
            if "--exclude=pytest-*" in command and child.name.startswith("pytest-"):
                continue
            target = destination / child.name
            if child.is_dir():
                shutil.copytree(child, target, dirs_exist_ok=True)
            else:
                shutil.copy2(child, target)
        if host.interrupt_copy:
            host.interrupt_copy = False
            return subprocess.CompletedProcess(command, 12, "", "interrupted test copy")
        return subprocess.CompletedProcess(command, 0, "", "")

    monkeypatch.setattr(janitor.subprocess, "run", run)

    def task(name, age=8 * DAY):
        path = root / name
        path.mkdir(parents=True)
        (path / "result.bin").write_bytes(b"task data")
        return age_tree(path, age)

    def process(path, kind="cwd", pid="101"):
        process = proc / pid
        process.mkdir(parents=True, exist_ok=True)
        link = process / "cwd" if kind == "cwd" else process / "fd" / "7"
        link.parent.mkdir(exist_ok=True)
        link.symlink_to(path)

    def cleaner(**kwargs):
        return janitor.Janitor(root=root, uv_cache=uv_cache, proc_root=proc,
                               uv=host.uv, now=NOW, **kwargs)

    host.task, host.process, host.cleaner = task, process, cleaner
    return host


def test_task_protection_keep_globs_and_freshness(host):
    protected = ["engine-cache", "game", "play", "lib", "bin", "nav-probe",
                 "body", "body-baseline", "body-training", "gq", "queue", ".hidden"]
    for name in protected:
        host.task(name)
    (host.root / ".janitor-keep").write_text("custom-*\nexact\n")
    for name in ("custom-one", "exact"):
        protected.append(name)
        host.task(name)
    kept = host.task("keep")
    (kept / ".keep").touch()
    age_tree(kept)
    protected.append("keep")
    young = host.task("young", age=6 * DAY)
    marker = host.task("marker") / ".last-used"
    marker.touch()
    os.utime(marker, (NOW, NOW))
    deep = host.task("deep")
    (deep / "one" / "two").mkdir(parents=True)
    fresh = deep / "one" / "two" / "data"
    fresh.write_text("fresh at depth three")
    age_tree(deep)
    os.utime(fresh, (NOW, NOW))
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert all((host.root / name / "result.bin").exists() for name in protected)
    assert young.exists() and marker.exists() and deep.exists()
    assert cleaner.summary["moved"] == 0
    assert sum(cleaner.summary["skipped"].values()) >= len(protected) + 3


@pytest.mark.parametrize("kind", ["cwd", "fd"])
def test_task_with_process_is_skipped(host, kind):
    task = host.task("running")
    host.process(task if kind == "cwd" else task / "result.bin", kind)
    assert host.cleaner().run() == 0
    assert (task / "result.bin").exists()
    assert not (host.nas / "archive").exists()


@pytest.mark.parametrize("field,value", [
    ("command", ["python", "{task}/script.py"]),
    ("command", ["sh", "-c", "cd {task} && work"]),
    ("archive", ["{task}/results"]),
    ("cwd", "{task}"),
])
def test_active_queue_reference_is_skipped(host, field, value):
    task = host.task("queued-work")
    queue = host.root / "queue"
    queue.mkdir()
    value = [entry.format(task=task) for entry in value] if isinstance(value, list) else value.format(task=task)
    (queue / "state.json").write_text(json.dumps({"version": 1, "jobs": [
        {"id": "active", "status": "running", field: value}]}))
    assert host.cleaner().run() == 0
    assert (task / "result.bin").exists()


def test_e2e_uses_marker_fallback_and_nonblocking_lock(host):
    root = host.root / "e2e"
    root.mkdir()
    for name in ("marker-old", "marker-new", "fallback-old", "fallback-new", "locked"):
        directory = host.task(f"e2e/{name}")
        if name.startswith("marker-"):
            marker = directory / ".last-used"
            marker.touch()
            age_tree(directory, 3 * DAY)
            if name == "marker-new":
                os.utime(marker, (NOW, NOW))
            else:
                os.utime(directory / "result.bin", (NOW, NOW))
        if name == "fallback-new":
            (directory / "one").mkdir()
            (directory / "one" / "two").write_text("recent")
            age_tree(directory)
            os.utime(directory / "one" / "two", (NOW, NOW))
    lock_path = root / "locked.lock"
    with lock_path.open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert host.cleaner().run() == 0
    assert not (root / "marker-old").exists()
    assert not (root / "fallback-old").exists()
    assert all((root / name).exists() for name in ("marker-new", "fallback-new", "locked"))
    assert (root / "marker-old.lock").is_file()
    assert (root / "fallback-old.lock").is_file()
    assert lock_path.is_file()


def test_codex_check_skips_process_and_recent_depth_two(host):
    for name in ("old", "cwd", "fd", "recent"):
        directory = host.task(f"codex-check/{name}")
        if name in {"cwd", "fd"}:
            host.process(directory if name == "cwd" else directory / "result.bin", name, pid="101" if name == "cwd" else "102")
        if name == "recent":
            (directory / "one").mkdir()
            fresh = directory / "one" / "two"
            fresh.write_text("recent")
            age_tree(directory)
            os.utime(fresh, (NOW, NOW))
    assert host.cleaner().run() == 0
    assert not (host.root / "codex-check" / "old").exists()
    assert all((host.root / "codex-check" / name).exists() for name in ("cwd", "fd", "recent"))


def test_rebuildable_dirs_removed_but_unmarked_build_archived(host):
    task = host.task("finished")
    rebuildable = [".venv", "nested/build", "cmake-build-debug", "_deps", "__pycache__",
                   ".pytest_cache", "pytest-123"]
    for name in rebuildable:
        directory = task / name
        directory.mkdir(parents=True)
        (directory / "junk").write_text("rebuild me")
    (task / ".venv" / "pyvenv.cfg").touch()
    (task / "nested" / "build" / "CMakeCache.txt").touch()
    (task / "cmake-build-debug" / "build.ninja").touch()
    (task / "build").mkdir()
    (task / "build" / "data.bin").write_bytes(b"irreplaceable")
    (task / "nested" / ".venv").mkdir()
    (task / "nested" / ".venv" / "data.bin").write_bytes(b"not a venv")
    age_tree(task)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    destination = host.nas / "archive" / "finished-20261028"
    assert not task.exists()
    assert (destination / "build" / "data.bin").read_bytes() == b"irreplaceable"
    assert (destination / "nested" / ".venv" / "data.bin").read_bytes() == b"not a venv"
    assert all(not (destination / name).exists() for name in rebuildable)
    assert cleaner.summary["moved"] == 1
    assert cleaner.summary["deleted"] >= len(rebuildable)
    assert json.loads((host.root / ".janitor" / "last.json").read_text())["moved"] == 1


def test_archive_never_overwrites_existing_destination_and_continues(host):
    task = host.task("conflict")
    destination = host.nas / "archive" / "conflict-20261028"
    destination.mkdir(parents=True)
    (destination / "result.bin").write_bytes(b"unrelated")
    second = host.task("other")
    cleaner = host.cleaner()
    assert cleaner.run() == 1
    assert task.exists() and not second.exists()
    assert (destination / "result.bin").read_bytes() == b"unrelated"
    assert (host.nas / "archive" / "other-20261028" / "result.bin").read_bytes() == b"task data"
    assert len(cleaner.summary["errors"]) == 1


def test_interrupted_archive_resumes_same_destination_next_day(host):
    task = host.task("retry")
    host.interrupt_copy = True
    assert host.cleaner().run() == 1
    destination = host.nas / "archive" / "retry-20261028"
    assert task.exists() and (destination / ".janitor-incomplete.json").is_file()
    cleaner = host.cleaner()
    cleaner.now = NOW + DAY
    assert cleaner.run() == 0
    assert not task.exists()
    assert (destination / "result.bin").read_bytes() == b"task data"
    assert not (destination / ".janitor-incomplete.json").exists()
    assert not (host.nas / "archive" / "retry-20261029").exists()


def test_unmounted_nas_still_removes_rebuildable_and_cleans_uv(host):
    task = host.task("offline")
    cache = task / "__pycache__"
    cache.mkdir()
    (cache / "compiled.pyc").write_bytes(b"cached")
    age_tree(task)
    host.mounts.write_text("")
    host.stamp.unlink()
    (host.uv_cache / "cached-wheel").write_bytes(b"wheel")
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert task.exists() and (task / "result.bin").exists()
    assert not cache.exists()
    assert not (host.nas / "archive").exists()
    assert not (host.uv_cache / "cached-wheel").exists()
    assert cleaner.summary["moved"] == 0
    assert sum(cleaner.summary["skipped"].values()) >= 1


@pytest.mark.parametrize("weekly", [True, False])
def test_uv_clean_weekly_or_under_sixty_gib_and_busy_retry(host, weekly):
    os.utime(host.stamp, (NOW - (8 * DAY if weekly else DAY),) * 2)
    original = host.stamp.stat().st_mtime_ns
    host.free = (200 if weekly else 59) * 1024**3
    (host.uv_cache / "wheel").write_bytes(b"cached")
    host.uv_busy = True
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert (host.uv_cache / "wheel").exists()
    assert host.stamp.stat().st_mtime_ns == original
    assert sum(cleaner.summary["skipped"].values()) >= 1
    host.uv_busy = False
    assert host.cleaner().run() == 0
    assert not (host.uv_cache / "wheel").exists()
    assert host.stamp.stat().st_mtime_ns > original


def test_second_instance_exits_without_mutation(host):
    task = host.task("waiting")
    before = sorted(path.relative_to(host.root) for path in host.root.rglob("*"))
    descriptor = os.open(host.root, os.O_RDONLY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        assert host.cleaner().run() == 0
    finally:
        os.close(descriptor)
    assert sorted(path.relative_to(host.root) for path in host.root.rglob("*")) == before
    assert (task / "result.bin").exists()


def test_dry_run_prints_plan_and_changes_nothing(host, capsys):
    task = host.task("dry-task")
    cache = task / "__pycache__"
    cache.mkdir()
    (cache / "compiled.pyc").write_bytes(b"cached")
    age_tree(task)
    host.task("e2e/old")
    host.task("codex-check/old")
    host.stamp.unlink()
    (host.uv_cache / "wheel").write_bytes(b"cached")

    def snapshot(root):
        return {str(path.relative_to(root)): (path.lstat().st_mtime_ns,
                path.read_bytes() if path.is_file() else None) for path in root.rglob("*")}

    before = {str(root): snapshot(root) for root in (host.root, host.nas, host.uv_cache)}
    cleaner = host.cleaner(dry_run=True)
    assert cleaner.run() == 0
    assert {str(root): snapshot(root) for root in (host.root, host.nas, host.uv_cache)} == before
    assert host.calls == []
    output = capsys.readouterr().out
    assert "dry-task" in output and "bytes" in output
    assert "e2e/old" in output and "codex-check/old" in output
    assert "uv" in output


def test_task_activity_rechecked_before_first_deletion(host, janitor, monkeypatch):
    task = host.task("became-active")
    cache = task / "__pycache__"
    cache.mkdir()
    (cache / "compiled.pyc").write_bytes(b"cached")
    age_tree(task)
    original = janitor.latest_mtime
    calls = 0

    def activity(path, depth, touched=None):
        nonlocal calls
        if Path(path) == task:
            calls += 1
            if calls >= 2:
                return NOW
        return original(path, depth, touched)

    monkeypatch.setattr(janitor, "latest_mtime", activity)
    assert host.cleaner().run() == 0
    assert calls >= 2
    assert (cache / "compiled.pyc").exists()
    assert (task / "result.bin").exists()
    assert not (host.nas / "archive").exists()


def test_symlinks_do_not_redirect_cleanup_outside_allowed_roots(host):
    external = host.root.parent / "external"
    external.mkdir()
    (external / "pyvenv.cfg").touch()
    (external / "precious.bin").write_bytes(b"keep outside")
    host.root.joinpath("external-alias").symlink_to(external, target_is_directory=True)
    task = host.task("linked")
    (task / ".venv").symlink_to(external, target_is_directory=True)
    age_tree(task)
    host.mounts.write_text("")
    assert host.cleaner().run() == 0
    assert (external / "precious.bin").read_bytes() == b"keep outside"
    assert (external / "pyvenv.cfg").exists()
    assert (task / ".venv").is_symlink()
