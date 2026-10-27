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
    monkeypatch.delenv("GQ_HOME", raising=False)
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
    protected = ["e2e", "codex-check", "engine-cache", "game", "play", "lib", "bin", "nav-probe",
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


def test_rebuildable_dirs_removed_but_unmarked_build_archived(host, janitor):
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
    original_bytes = janitor.tree_bytes(task)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    destination = host.nas / "archive" / "finished-20261028"
    assert not task.exists()
    assert (destination / "build" / "data.bin").read_bytes() == b"irreplaceable"
    assert (destination / "nested" / ".venv" / "data.bin").read_bytes() == b"not a venv"
    assert all(not (destination / name).exists() for name in rebuildable)
    assert cleaner.summary["moved"] == 1
    assert cleaner.summary["deleted"] >= len(rebuildable)
    assert cleaner.summary["freed_bytes"] == original_bytes
    assert json.loads((host.root / ".janitor" / "last.json").read_text())["moved"] == 1


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


@pytest.mark.parametrize("phase", ["before", "after"])
def test_changing_uv_cache_is_a_skip_without_errors(host, janitor, monkeypatch, capsys, phase):
    host.stamp.unlink()
    (host.uv_cache / "wheel").write_bytes(b"cached")
    original = janitor.tree_bytes

    def changing_cache(path):
        if path == host.uv_cache and (phase == "before" or host.calls):
            raise FileNotFoundError("cache changed during size calculation")
        return original(path)

    monkeypatch.setattr(janitor, "tree_bytes", changing_cache)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert cleaner.summary["errors"] == []
    assert cleaner.summary["skipped"]["uv cache busy"] == 1
    assert capsys.readouterr().out.count("; uv cache busy)") == 1
    assert len(host.calls) == (0 if phase == "before" else 1)
    assert host.stamp.exists() == (phase == "after")
    if phase == "after":
        assert host.stamp.stat().st_mtime == NOW


def test_uv_cleanup_not_due_at_sixty_gib(host):
    host.free = 60 * 1024**3
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert host.calls == []
    assert cleaner.summary["skipped"]["uv cleanup not due"] == 1


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
    assert f"would delete {cache} (6 bytes; idle task; rebuildable)" in output
    assert f"would move {task} (9 bytes; idle task -> " in output
    assert output.count(f"would move {task}") == 1
    assert f"would delete {host.root / 'e2e/old'} (9 bytes; idle e2e; rebuildable)" in output
    assert f"would delete {host.root / 'codex-check/old'} (9 bytes; idle codex-check; rebuildable)" in output
    assert f"would clean {host.uv_cache} (6 bytes; weekly or free space below 60 GiB)" in output


@pytest.mark.parametrize("activity", ["fresh", "process", "queue", "modified", "vanished"])
def test_task_activity_during_transfer_is_a_skip(host, janitor, monkeypatch, activity):
    task = host.task("resumed-work")
    original_run = janitor.subprocess.run

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

    monkeypatch.setattr(janitor.subprocess, "run", resume_work)
    cleaner = host.cleaner()
    assert cleaner.run() == 0
    assert task.exists()
    assert cleaner.summary["errors"] == []
    assert cleaner.summary["moved"] == 0
    assert cleaner.summary["skipped"][{"queue": "active gq job", "modified": "fresh", "vanished": "fresh"}.get(activity, activity)] == 1


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
    result = subprocess.run([*wrapper, "uninstall"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert not private.exists()
    assert not (library / "ryzen_janitor.py").exists()
    assert shared.read_text() == "raise RuntimeError('old gq worktree replaced shared library')\n"
