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
HOUR = 3600
DAY = 24 * HOUR
GIB = 1024**3
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
    root, nas, uv_cache, proc, base_cache, system_root = (
        tmp_path / name for name in ("local", "nas", "uv", "proc", "base", "system"))
    for directory in (root, nas, uv_cache, proc, base_cache, system_root):
        directory.mkdir()
    state = root / ".janitor"
    state.mkdir()
    mounts = tmp_path / "mounts"
    mounts.write_text(f"//fake/share {nas} cifs rw 0 0\n")
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(root))
    monkeypatch.setenv("GOTHIC_NAS_ROOT", str(nas))
    monkeypatch.setenv("GOTHIC_NAS_MOUNTS", str(mounts))
    monkeypatch.delenv("GQ_HOME", raising=False)
    host = SimpleNamespace(root=root, nas=nas, uv_cache=uv_cache, proc=proc,
                           mounts=mounts,
                           base_cache=base_cache, system_root=system_root,
                           free=200 * 1024**3, calls=[],
                           interrupt_copy=False)
    monkeypatch.setattr(janitor.shutil, "disk_usage", lambda _path: SimpleNamespace(free=host.free))

    def run(command, **kwargs):
        host.calls.append((command, kwargs))
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

    monkeypatch.setattr(janitor.nas_archive.subprocess, "run", run)

    def task(name, age=8 * DAY):
        path = root / name
        path.mkdir(parents=True)
        (path / "result.bin").write_bytes(b"task data")
        return age_tree(path, age)

    def process(path, kind="cwd", pid="101"):
        process = proc / pid
        process.mkdir(parents=True, exist_ok=True)
        link = process / kind if kind in {"cwd", "exe"} else process / "fd" / "7"
        link.parent.mkdir(exist_ok=True)
        link.symlink_to(path)

    def cleaner(**kwargs):
        return janitor.Janitor(root=root, uv_cache=uv_cache, proc_root=proc,
                               base_cache=base_cache, system_root=system_root, now=NOW, **kwargs)

    host.task, host.process, host.cleaner = task, process, cleaner
    return host


def test_task_protection_keep_globs_and_freshness(host):
    protected = ["game", "game-settings", "play", "lib", "bin", "gq", "queue", ".hidden"]
    for name in protected:
        host.task(name)
    (host.root / ".janitor-keep").write_text("custom-*\nexact\ne2e\ncodex-*\n")
    for name in ("custom-one", "exact", "e2e/kept-session", "codex-check/kept-session"):
        protected.append(name)
        host.task(name)
    kept = host.task("keep")
    (kept / ".keep").touch()
    age_tree(kept)
    protected.append("keep")
    young = host.task("young", age=DAY)
    marker = host.task("marker") / ".last-used"
    marker.touch()
    os.utime(marker, (NOW, NOW))
    deep = host.task("deep")
    (deep / "one" / "two" / "three" / "four").mkdir(parents=True)
    fresh = deep / "one" / "two" / "three" / "four" / "data"
    fresh.write_text("fresh below former depth limit")
    age_tree(deep)
    os.utime(fresh, (NOW, NOW))
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert all((host.root / name / "result.bin").exists() for name in protected)
    assert young.exists() and marker.exists() and deep.exists()
    assert cleaner.summary["moved"] == 0
    assert sum(cleaner.summary["skipped"].values()) >= len(protected) + 3


def test_body_and_nav_probe_are_protected_in_emergency(host, capsys):
    protected = [host.task(name) for name in ("body", "body-net", "nav-probe")]
    for task in protected:
        for name in (".venv", "build"):
            directory = task / name
            directory.mkdir()
            (directory / "data.bin").write_bytes(b"preserve")
        age_tree(task)
    host.free = 59 * GIB
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert cleaner.summary["emergency"]
    assert cleaner.summary["skipped"]["protected"] >= len(protected)
    assert cleaner.summary["deleted"] == cleaner.summary["moved"] == 0
    output = capsys.readouterr().out
    for task in protected:
        assert f"skip {task} (0 bytes; protected)" in output
        assert (task / "result.bin").exists()
        assert (task / ".venv/data.bin").read_bytes() == b"preserve"
        assert (task / "build/data.bin").read_bytes() == b"preserve"


@pytest.mark.parametrize("prefix", ["", "e2e/", "codex-check/"])
def test_nested_keep_protects_entire_owner(host, prefix):
    task = host.task(prefix + "kept")
    build = task / "build"
    build.mkdir()
    (build / ".keep").touch()
    (build / "binary").write_bytes(b"keep this build")
    age_tree(task)
    sibling = host.task(prefix + "other-session") if prefix else None
    host.free = 59 * GIB
    assert host.cleaner().run() == 0
    assert (build / "binary").read_bytes() == b"keep this build"
    assert (task / "result.bin").exists()
    if sibling is not None:
        assert not sibling.exists()


@pytest.mark.parametrize("prefix", ["", "e2e/", "codex-check/"])
@pytest.mark.parametrize("keep_path", [
    "source/server/.venv/lib/python3.12/site-packages/openai/lib/.keep",
    "source/server/site-packages/openai/lib/.keep",
    "source/server/dist-packages/openai/lib/.keep",
    "node_modules/package/.keep",
    "source/server/venv/package/.keep",
])
def test_package_keep_does_not_protect_idle_owner_in_emergency(host, prefix, keep_path):
    task = host.task(prefix + "package-keep")
    rebuildable = ["build", "source/server/.venv"]
    for name in rebuildable:
        directory = task / name
        directory.mkdir(parents=True)
        (directory / "junk").write_bytes(b"rebuild me")
    marker = task / keep_path
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.touch()
    age_tree(task, 18 * HOUR)
    host.free = 59 * GIB
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert cleaner.summary["emergency"]
    assert not task.exists()
    archives = list((host.nas / "archive").rglob("result.bin"))
    assert len(archives) == 1
    assert archives[0].read_bytes() == b"task data"
    assert all(not (archives[0].parent / name).exists() for name in rebuildable)
    assert cleaner.summary["moved"] == 1
    assert cleaner.summary["deleted_bytes"] == 2 * len(b"rebuild me")


@pytest.mark.parametrize("prefix,kind", [("", "cwd"), ("", "fd"), ("", "exe"), ("e2e/", "cwd"), ("e2e/", "fd"),
                                      ("codex-check/", "fd")])
def test_task_with_process_is_skipped(host, prefix, kind):
    task = host.task(prefix + "running")
    if kind == "exe":
        binary = task / "build" / "Gothic2Notr"
        binary.parent.mkdir()
        binary.write_bytes(b"running executable")
        age_tree(task)
        host.process(host.root.parent, "cwd")
        host.process(host.root.parent / "unrelated.log", "fd")
        host.process(binary, "exe")
    else:
        host.process(task if kind == "cwd" else task / "result.bin", kind)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert cleaner.summary["skipped"]["process"] == 1
    assert (task / "result.bin").exists()
    if kind == "exe":
        assert binary.read_bytes() == b"running executable"
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



def test_e2e_queued_job_protects_session(host):
    task = host.task("e2e/queued")
    queue = host.root / "queue"
    queue.mkdir()
    (queue / "state.json").write_text(json.dumps({"version": 1, "jobs": [
        {"status": "queued", "cwd": str(task), "command": ["work"]}]}))
    assert host.cleaner().run() == 0
    assert (task / "result.bin").exists()


def test_e2e_activity_includes_data_after_marker_and_nonblocking_lock(host):
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
    assert (root / "marker-old").exists()
    assert not (root / "fallback-old").exists()
    assert [p.read_bytes() for p in (host.nas / "archive").rglob("result.bin")] == [b"task data"]
    assert all((root / name).exists() for name in ("marker-new", "fallback-new", "locked"))
    assert lock_path.is_file()


def test_codex_check_skips_process_and_recent_depth_two(host):
    for name in ("old", "cwd", "fd", "recent", "queued"):
        directory = host.task(f"codex-check/{name}")
        if name in {"cwd", "fd"}:
            host.process(directory if name == "cwd" else directory / "result.bin", name, pid="101" if name == "cwd" else "102")
        if name == "recent":
            (directory / "one").mkdir()
            fresh = directory / "one" / "two"
            fresh.write_text("recent")
            age_tree(directory)
            os.utime(fresh, (NOW, NOW))
    queue = host.root / "queue"
    queue.mkdir()
    (queue / "state.json").write_text(json.dumps({"version": 1, "jobs": [
        {"status": "queued", "command": ["sh", "-c", "cd codex-check/queued && build"]}]}))
    assert host.cleaner().run() == 0
    assert not (host.root / "codex-check" / "old").exists()
    assert all((host.root / "codex-check" / name).exists() for name in ("cwd", "fd", "recent", "queued"))


@pytest.mark.parametrize("kind", ["e2e", "task"])
def test_concurrent_cleanup_is_a_skip_and_other_folders_continue(host, janitor, monkeypatch, kind):
    prefix = "e2e/" if kind == "e2e" else ""
    task = host.task(prefix + "gone")
    other = host.task(prefix + "z-other")
    original = janitor.latest_mtime

    def disappear(path, *args, **kwargs):
        if path == task:
            shutil.rmtree(path)
        return original(path, *args, **kwargs)

    monkeypatch.setattr(janitor, "latest_mtime", disappear)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert not task.exists() and not other.exists()
    assert cleaner.summary["errors"] == []
    assert cleaner.summary["skipped"]["disappeared during cleanup"] == 1


@pytest.mark.parametrize("operation", ["lstat", "iterdir"])
def test_size_and_activity_ignore_disappearing_children(host, janitor, monkeypatch, operation):
    task = host.task("measured")
    missing = task / "gone"
    missing.mkdir()
    (missing / "data").write_bytes(b"disappeared")
    age_tree(task)
    os.utime(missing, (NOW, NOW))
    original = getattr(Path, operation)

    def disappear(path, *args, **kwargs):
        if path == missing:
            raise FileNotFoundError("entry disappeared")
        return original(path, *args, **kwargs)

    monkeypatch.setattr(Path, operation, disappear)
    assert janitor.tree_bytes(task) == len(b"task data")
    assert janitor.latest_mtime(task, 3) == NOW - 8 * DAY


@pytest.mark.parametrize("prefix", ["", "e2e/", "codex-check/"])
def test_rebuildable_names_deleted_after_two_days_and_data_verified(host, janitor, prefix):
    task = host.task(prefix + ("build-fix" if prefix else "finished"), age=3 * DAY)
    rebuildable = [".venv", "venv", "nested/build", "build-release", "storygraph-build",
                   "cmake-build-debug", "_deps", "__pycache__", ".pytest_cache", "pytest-123",
                   "uv-cache", "cache", "hd-cache"]
    for name in rebuildable:
        directory = task / name
        directory.mkdir(parents=True)
        (directory / "junk").write_text("rebuild me")
    (task / "records").mkdir()
    (task / "records" / "episode.bin").write_bytes(b"keep recordings")
    age_tree(task, 3 * DAY)
    original_bytes = janitor.tree_bytes(task)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert not task.exists()
    archives = list((host.nas / "archive").rglob("result.bin"))
    assert len(archives) == 1
    destination = archives[0].parent
    assert archives[0].read_bytes() == b"task data"
    assert (destination / "records" / "episode.bin").read_bytes() == b"keep recordings"
    assert all(not (destination / name).exists() for name in rebuildable)
    assert cleaner.summary["moved"] == 1
    assert cleaner.summary["deleted"] == len(rebuildable)
    assert cleaner.summary["freed_bytes"] == original_bytes
    assert json.loads((host.root / ".janitor" / "last.json").read_text())["moved"] == 1


def test_readonly_nested_build_is_deleted_in_emergency(host):
    task = host.task("texture-intent", age=18 * HOUR)
    build = task / "inputs" / "run" / "runtime" / "build"
    nested = build / "nested"
    nested.mkdir(parents=True)
    (build / "engine.bin").write_bytes(b"frozen engine")
    (nested / "object.o").write_bytes(b"rebuild me")
    outside = host.root.parent / "outside.txt"
    outside.write_bytes(b"preserve outside target")
    outside.chmod(0o444)
    (build / "outside-link").symlink_to(outside)
    age_tree(task, 18 * HOUR)
    for path in (build / "engine.bin", nested / "object.o"):
        path.chmod(0o444)
    for path in (nested, build):
        path.chmod(0o555)
    host.free = 59 * GIB

    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert cleaner.summary["emergency"]
    assert cleaner.summary["errors"] == []
    assert cleaner.summary["deleted"] == cleaner.summary["moved"] == 1
    assert not task.exists()
    assert outside.read_bytes() == b"preserve outside target"
    assert outside.stat().st_mode & 0o777 == 0o444


def test_readonly_build_with_different_owner_reports_error_without_chmod(host, janitor, monkeypatch):
    task = host.task("foreign-owner", age=18 * HOUR)
    build = task / "build"
    build.mkdir()
    data = build / "engine.bin"
    data.write_bytes(b"preserve foreign build")
    age_tree(task, 18 * HOUR)
    data.chmod(0o444)
    build.chmod(0o555)
    host.free = 59 * GIB
    chmod_calls = []
    original_chmod = os.chmod
    foreign_uid = build.lstat().st_uid + 1

    def chmod(*args, **kwargs):
        chmod_calls.append(args)
        return original_chmod(*args, **kwargs)

    try:
        with monkeypatch.context() as patch:
            patch.setattr(janitor.os, "getuid", lambda: foreign_uid)
            patch.setattr(janitor.os, "chmod", chmod)
            cleaner = host.cleaner()
            assert cleaner.run() == 1
            assert chmod_calls == []
        assert len(cleaner.summary["errors"]) == 1
        assert "Permission denied" in cleaner.summary["errors"][0]
        assert cleaner.summary["deleted"] == cleaner.summary["moved"] == 0
        assert data.read_bytes() == b"preserve foreign build"
        assert build.stat().st_mode & 0o777 == 0o555
    finally:
        if build.exists():
            build.chmod(0o755)


def test_archive_never_overwrites_existing_destination_and_continues(host):
    task = host.task("conflict")
    (task / "__pycache__").mkdir()
    age_tree(task)
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
    assert "conflict" not in json.loads((host.root / ".janitor" / "attempts.json").read_text())
    cleaner = host.cleaner()
    cleaner.now = NOW + DAY
    assert cleaner.run() == 0
    assert not task.exists()
    assert (host.nas / "archive" / "conflict-20261029" / "result.bin").read_bytes() == b"task data"
    assert (destination / "result.bin").read_bytes() == b"unrelated"


@pytest.mark.parametrize("phase", ["copy", "marker"])
def test_interrupted_archive_resumes_same_destination_next_day(host, monkeypatch, phase):
    task = host.task("retry")
    (task / "__pycache__").mkdir()
    age_tree(task)
    if phase == "copy":
        host.interrupt_copy = True
    else:
        original_open = Path.open

        def interrupt_marker(path, *args, **kwargs):
            if path.name == ".janitor-incomplete.json" and args == ("x",):
                raise OSError("interrupted marker creation")
            return original_open(path, *args, **kwargs)

        monkeypatch.setattr(Path, "open", interrupt_marker)
    assert host.cleaner().run() == 1
    destination = host.nas / "archive" / "retry-20261028"
    assert task.exists() and destination.exists()
    if phase == "copy":
        assert (destination / ".janitor-incomplete.json").is_file()
    else:
        monkeypatch.setattr(Path, "open", original_open)
        assert not (destination / ".janitor-incomplete.json").exists()
    cleaner = host.cleaner()
    cleaner.now = NOW + DAY
    assert cleaner.run() == 0
    assert not task.exists()
    completed = destination if phase == "copy" else host.nas / "archive" / "retry-20261029"
    assert (completed / "result.bin").read_bytes() == b"task data"
    assert not (destination / ".janitor-incomplete.json").exists()
    if phase == "copy":
        assert not (host.nas / "archive" / "retry-20261029").exists()


@pytest.mark.parametrize("reactivated", [False, True])
def test_unmounted_nas_still_removes_rebuildable_and_cleans_uv(host, reactivated):
    task = host.task("offline")
    cache = task / "__pycache__"
    cache.mkdir()
    (cache / "compiled.pyc").write_bytes(b"cached")
    age_tree(task)
    host.mounts.write_text("")
    (host.uv_cache / "cached-wheel").write_bytes(b"wheel")
    age_tree(host.uv_cache, 3 * DAY)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert task.exists() and (task / "result.bin").exists()
    assert not cache.exists()
    assert not (host.nas / "archive").exists()
    assert not (host.uv_cache / "cached-wheel").exists()
    assert cleaner.summary["moved"] == 0
    assert sum(cleaner.summary["skipped"].values()) >= 1
    assert cleaner.summary["freed_bytes"] == len(b"cached") + len(b"wheel")
    host.mounts.write_text(f"//fake/share {host.nas} cifs rw 0 0\n")
    if reactivated:
        (task / "new-work").write_text("working again")
        os.utime(task, (NOW + 3600, NOW + 3600))
        os.utime(task / "new-work", (NOW + 3600, NOW + 3600))
    cleaner = host.cleaner()
    cleaner.now = NOW + 6 * 3600
    assert cleaner.run() == 0
    assert task.exists() == reactivated
    assert cleaner.summary["moved"] == (0 if reactivated else 1)


@pytest.mark.parametrize("age,free,deleted", [(3 * DAY, 200, True), (DAY, 200, False),
                                                 (13 * HOUR, 59, True), (11 * HOUR, 59, False)])
def test_uv_cache_obeys_idle_cutoff(host, age, free, deleted):
    host.free = free * GIB
    (host.uv_cache / "wheel").write_bytes(b"cached")
    age_tree(host.uv_cache, age)
    assert host.cleaner().run() == 0
    assert (host.uv_cache / "wheel").exists() != deleted
    assert host.calls == []


@pytest.mark.parametrize("keep_path,deleted", [("archive-v0/x/openai/lib/.keep", True), (".keep", False)])
def test_uv_cache_only_root_keep_protects(host, keep_path, deleted):
    (host.uv_cache / "wheel").write_bytes(b"cached")
    marker = host.uv_cache / keep_path
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.touch()
    age_tree(host.uv_cache, 3 * DAY)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert (host.uv_cache / "wheel").exists() != deleted
    assert marker.exists() != deleted
    assert cleaner.summary["deleted_bytes"] == (len(b"cached") if deleted else 0)
    assert host.calls == []


@pytest.mark.parametrize("protection", ["keep", "cwd", "fd"])
def test_uv_cache_in_use_is_preserved(host, protection):
    (host.uv_cache / "wheel").write_bytes(b"cached")
    if protection == "keep":
        (host.uv_cache / ".keep").touch()
    else:
        host.process(host.uv_cache if protection == "cwd" else host.uv_cache / "wheel", protection)
    age_tree(host.uv_cache, 3 * DAY)
    host.free = 59 * GIB
    assert host.cleaner().run() == 0
    assert (host.uv_cache / "wheel").exists()




@pytest.mark.parametrize("pressured", ["tasks", "system"])
def test_emergency_tracks_two_filesystems_and_stops_at_target(host, janitor, monkeypatch, capsys, pressured):
    oldest = host.task("z-oldest", age=30 * HOUR)
    middle = host.task("m-middle", age=20 * HOUR)
    host.task("a-younger", age=13 * HOUR)
    host.task("too-recent", age=11 * HOUR)
    (host.uv_cache / "wheel").write_bytes(b"cached")
    age_tree(host.uv_cache, 18 * HOUR)
    monkeypatch.setattr(janitor.Janitor, "device", staticmethod(
        lambda path: 1 if Path(path).is_relative_to(host.root) else 2))

    def disk_usage(path):
        role = "tasks" if Path(path) == host.root else "system"
        return SimpleNamespace(free=(50 if role == pressured else 200) * GIB)

    monkeypatch.setattr(janitor.shutil, "disk_usage", disk_usage)
    monkeypatch.setattr(janitor, "tree_reclaimable", lambda path: (60 if Path(path) == host.uv_cache else 30) * GIB)
    cleaner = host.cleaner(dry_run=True)
    assert cleaner.run() == 0
    output = capsys.readouterr().out
    if pressured == "tasks":
        assert cleaner.summary["moved"] == 2
        assert cleaner.summary["deleted"] == 0
        assert output.index(f"would move {oldest} ") < output.index(f"would move {middle} ")
        assert f"would move {host.root / 'a-younger'} " not in output
        assert f"would delete {host.uv_cache} " not in output
    else:
        assert cleaner.summary["moved"] == 0
        assert cleaner.summary["deleted"] == 1
        assert f"would delete {host.uv_cache} " in output
    assert "would move " + str(host.root / "too-recent") not in output
    assert host.calls == []


def test_top_level_root_symlink_resolves_but_children_are_never_followed(host, janitor):
    alias = host.root.parent / "task-alias"
    alias.symlink_to(host.root, target_is_directory=True)
    task = host.task("old-task")
    cleaner = janitor.Janitor(root=alias, uv_cache=host.uv_cache, proc_root=host.proc,
                              base_cache=host.base_cache, system_root=host.system_root, now=NOW)
    assert cleaner.run() == 0
    assert alias.is_symlink()
    assert not task.exists()
    assert (host.nas / "archive" / "old-task-20261028" / "result.bin").read_bytes() == b"task data"


def test_base_cache_keeps_current_leased_two_newest_and_missing_backup(host):
    current = host.base_cache / "current"
    current.mkdir()
    fingerprints = [str(index) * 64 for index in range(1, 8)]
    paths = []
    for index, fingerprint in enumerate(fingerprints):
        path = host.base_cache / fingerprint
        path.mkdir()
        (path / "data.bin").write_bytes(b"base data")
        (host.base_cache / (fingerprint + ".use")).touch()
        if index < 6:
            destination = host.nas / "base" / fingerprint
            destination.mkdir(parents=True)
            (destination / "data.bin").write_bytes(b"base data")
        age_tree(path, (index + 3) * DAY)
        paths.append(path)
    (current / "game.json").write_text(json.dumps({"version": 1,
        "base": {"fingerprint": fingerprints[2], "path": str(paths[2])}}))
    (paths[4] / ".keep").touch()
    age_tree(paths[4], 7 * DAY)
    lease = host.base_cache / (fingerprints[3] + ".use")
    with lease.open("r") as stream:
        fcntl.flock(stream, fcntl.LOCK_SH | fcntl.LOCK_NB)
        cleaner = host.cleaner()
        assert cleaner.run() == 0
    assert current.exists()
    assert all(path.exists() for path in paths[:5])
    assert not paths[5].exists()
    assert paths[6].exists()
    # Lock inodes must remain stable for other games to acquire the same lease.
    assert lease.exists()
    assert (host.base_cache / (fingerprints[5] + ".use")).exists()


def test_base_cache_retains_recent_fingerprints_in_emergency(host):
    paths = []
    for index in range(3):
        path = host.base_cache / (str(index) * 64)
        path.mkdir()
        (path / "data.bin").write_bytes(b"base data")
        (host.base_cache / (path.name + ".use")).touch()
        destination = host.nas / "base" / path.name
        destination.mkdir(parents=True)
        (destination / "data.bin").write_bytes(b"base data")
        age_tree(path, (index + 1) * 8 * HOUR)
        paths.append(path)
    host.free = 59 * GIB
    assert host.cleaner().run() == 0
    assert all(path.exists() for path in paths)


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


@pytest.mark.parametrize("du", [False, True])
def test_dry_run_prints_plan_and_changes_nothing(host, capsys, du):
    task = host.task("dry-task")
    cache = task / "__pycache__"
    cache.mkdir()
    (cache / "compiled.pyc").write_bytes(b"cached")
    age_tree(task)
    host.task("e2e/old")
    host.task("codex-check/old")
    (host.uv_cache / "wheel").write_bytes(b"cached")
    age_tree(host.uv_cache, 3 * DAY)

    def snapshot(root):
        return {str(path.relative_to(root)): (path.lstat().st_mtime_ns,
                path.read_bytes() if path.is_file() else None) for path in root.rglob("*")}

    before = {str(root): snapshot(root) for root in (host.root, host.nas, host.uv_cache)}
    cleaner = host.cleaner(dry_run=True, du=du)
    assert cleaner.run() == 0
    assert {str(root): snapshot(root) for root in (host.root, host.nas, host.uv_cache)} == before
    assert host.calls == []
    output = capsys.readouterr().out
    assert f"would delete {cache} " in output
    assert f"would move {task} " in output
    assert output.count(f"would move {task} ") == 1
    assert f"would move {host.root / 'e2e/old'} " in output
    assert f"would move {host.root / 'codex-check/old'} " in output
    assert f"would delete {host.uv_cache} " in output
    assert cleaner.summary["deleted"] == 2
    assert cleaner.summary["moved"] == 3
    if du:
        assert "PATH\tSIZE GiB\tIDLE hours\tREBUILDABLE GiB\tACTION" in output
        assert str(task) in output


@pytest.mark.parametrize("activity", ["fresh", "process", "queue", "modified", "vanished"])
def test_task_activity_during_transfer_is_a_skip(host, janitor, monkeypatch, activity):
    task = host.task("resumed-work")
    original_run = janitor.nas_archive.subprocess.run

    def resume_work(command, **kwargs):
        result = original_run(command, **kwargs)
        if command[0] == "rsync":
            if activity in {"fresh", "vanished"}:
                os.utime(task, (NOW, NOW))
            elif activity == "process":
                host.process(task)
            elif activity == "queue":
                queue = host.root / "queue"
                queue.mkdir()
                (queue / "state.json").write_text(json.dumps({"version": 1, "jobs": [
                    {"status": "running", "command": [str(task / "work.py")]}]}))
            else:
                (task / "result.bin").write_bytes(b"new work")
                os.utime(task / "result.bin", (NOW, NOW))
            if activity == "vanished":
                return subprocess.CompletedProcess(command, 24, "", "some source files vanished")
        return result

    monkeypatch.setattr(janitor.nas_archive.subprocess, "run", resume_work)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert task.exists()
    assert cleaner.summary["errors"] == []
    assert cleaner.summary["moved"] == 0
    assert cleaner.summary["skipped"][{"queue": "active gq job", "modified": "fresh", "vanished": "fresh"}.get(activity, activity)] >= 1


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


def test_run_prints_summary_when_service_fails(tmp_path, monkeypatch):
    home = tmp_path / "home"
    state = home / "gothic-ai" / ".janitor"
    state.mkdir(parents=True)
    summary = '{"errors": ["copy failed"]}\n'
    (state / "last.json").write_text(summary)
    binaries = tmp_path / "bin"
    binaries.mkdir()
    ssh = binaries / "ssh"
    ssh.write_text("#!/bin/sh\nfor argument do command=$argument; done\nexec /bin/sh -c \"$command\"\n")
    systemctl = binaries / "systemctl"
    systemctl.write_text('#!/bin/sh\n[ "$*" = "--user start gothic-janitor.service" ] || exit 90\nexit 1\n')
    for executable in (ssh, systemctl):
        executable.chmod(0o755)
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("PATH", str(binaries) + os.pathsep + os.environ["PATH"])
    result = subprocess.run(["bash", str(SCRIPT.with_name("ryzen-janitor.sh")), "run"],
                            capture_output=True, text=True)
    assert result.returncode == 1
    assert result.stdout == summary


def test_installed_janitor_keeps_private_archive_library(tmp_path, monkeypatch):
    root = tmp_path / "home" / "gothic-ai"
    library = root / "lib"
    library.mkdir(parents=True)
    shared = library / "nas_archive.py"
    shared.write_text("raise RuntimeError('shared gq library imported')\n")
    original_shared = shared.read_bytes()
    binaries = tmp_path / "bin"
    binaries.mkdir()
    (binaries / "ssh").write_text(
        '#!/bin/sh\nfor argument do command=$argument; done\nexec /bin/sh -c "$command"\n')
    (binaries / "systemctl").write_text("#!/bin/sh\nexit 0\n")
    for executable in binaries.iterdir():
        executable.chmod(0o755)
    monkeypatch.setenv("HOME", str(root.parent))
    monkeypatch.setenv("PATH", str(binaries) + os.pathsep + os.environ["PATH"])
    wrapper = ["bash", str(SCRIPT.with_name("ryzen-janitor.sh"))]
    result = subprocess.run([*wrapper, "install"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert shared.read_bytes() == original_shared
    private = library / "ryzen_janitor_archive.py"
    assert private.read_bytes() == SCRIPT.with_name("nas_archive.py").read_bytes()
    shared.write_text("raise RuntimeError('old gq worktree replaced shared library')\n")
    task = root / "finished"
    task.mkdir()
    (task / "result.bin").write_bytes(b"result")
    age_tree(task)
    state = root / ".janitor"
    state.mkdir()
    (state / "uv-clean").touch()
    os.utime(state / "uv-clean", (NOW, NOW))
    proc, nas, cache = (tmp_path / name for name in ("proc", "nas", "uv"))
    for directory in (proc, nas, cache):
        directory.mkdir()
    mounts = tmp_path / "mounts"
    mounts.write_text(f"//fake/share {nas} cifs rw 0 0\n")
    monkeypatch.setenv("GOTHIC_LOCAL_ROOT", str(root))
    monkeypatch.setenv("GOTHIC_NAS_ROOT", str(nas))
    monkeypatch.setenv("GOTHIC_NAS_MOUNTS", str(mounts))
    monkeypatch.delenv("GQ_HOME", raising=False)
    program = """import runpy, shutil, sys
from types import SimpleNamespace
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]).parent))
shutil.disk_usage = lambda _path: SimpleNamespace(free=200 * 1024**3)
module = runpy.run_path(sys.argv[1])
raise SystemExit(module['Janitor'](root=sys.argv[2], proc_root=sys.argv[3],
    uv_cache=sys.argv[4], now=float(sys.argv[5]), dry_run=True).run())
"""
    result = subprocess.run([sys.executable, "-B", "-c", program,
                             str(library / "ryzen_janitor.py"), str(root), str(proc),
                             str(cache), str(NOW)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr + result.stdout
    assert f"would move {task}" in result.stdout
    (library / "ryzen_janitor.py").write_text("raise RuntimeError('stale installed janitor')\n")
    result = subprocess.run([*wrapper, "run", "--dry-run"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr + result.stdout
    assert '"dry_run": true' in result.stdout
    assert (task / "result.bin").read_bytes() == b"result"
    result = subprocess.run([*wrapper, "uninstall"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert not private.exists()
    assert not (library / "ryzen_janitor.py").exists()
    assert shared.read_text() == "raise RuntimeError('old gq worktree replaced shared library')\n"
