#!/usr/bin/env python3
"""Reclaim rebuildable Ryzen files and archive idle tasks after verification."""
from __future__ import annotations

import argparse
import contextlib
import fcntl
import fnmatch
import io
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import uuid

# Importing the sibling library must not create bytecode during --dry-run.
sys.dont_write_bytecode = True
if Path(__file__).with_name("ryzen_janitor_archive.py").is_file():
    import ryzen_janitor_archive as nas_archive
else:
    import nas_archive


HOUR = 3600
DAY = 24 * HOUR
GIB = 1024**3
PROTECTED = ("e2e", "codex-check", "engine-cache", "game", "play", "lib", "bin",
             "nav-probe", "body*", "gq", "queue", ".*")


def latest_mtime(directory, depth, touched=None):
    """Read bounded activity without following directory or file symlinks."""
    touched = touched or {}

    def timestamp(path):
        value = path.lstat().st_mtime_ns
        changed, original = touched.get(path, (None, None))
        return (original if changed == value else value) / 1e9

    latest = timestamp(directory)
    if depth:
        for child in directory.iterdir():
            try:
                child_latest = timestamp(child)
                if not child.is_symlink() and child.is_dir():
                    child_latest = max(child_latest, latest_mtime(child, depth - 1, touched))
            except FileNotFoundError:
                continue
            latest = max(latest, child_latest)
    return latest


def tree_bytes(path):
    """Count file bytes, including links themselves but never their targets."""
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode):
        return info.st_size
    size = 0
    for child in path.iterdir():
        try:
            size += tree_bytes(child)
        except FileNotFoundError:
            continue
    return size


def process_paths(proc_root):
    paths = []
    for process in proc_root.iterdir():
        if not process.name.isdigit():
            continue
        links = [process / "cwd"]
        try:
            links.extend((process / "fd").iterdir())
        except (FileNotFoundError, PermissionError):
            pass
        for link in links:
            try:
                target = os.readlink(link).removesuffix(" (deleted)")
            except OSError:
                continue
            if target.startswith("/"):
                paths.append(Path(target))
    return paths


def atomic_json(path, data):
    with tempfile.NamedTemporaryFile(mode="w", dir=path.parent, delete=False) as stream:
        temporary = Path(stream.name)
        try:
            json.dump(data, stream, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)


class Janitor:
    def __init__(self, root=None, uv_cache=None, proc_root="/proc", queue_state=None,
                 uv=None, now=None, dry_run=False):
        self.root = Path(root or os.environ.get("GOTHIC_LOCAL_ROOT", "~/gothic-ai")).expanduser().absolute()
        self.uv_cache = Path(uv_cache or "~/.cache/uv").expanduser().absolute()
        self.proc_root = Path(proc_root)
        self.queue_state = Path(queue_state or os.environ.get("GQ_HOME", str(self.root / "queue")))
        if queue_state is None:
            self.queue_state = self.queue_state.expanduser() / "state.json"
        self.uv = str(uv or Path.home() / ".local/bin/uv")
        self.now = time.time() if now is None else now
        self.dry_run = dry_run
        self.state = self.root / ".janitor"
        self.touched = {}
        self.attempts = {}
        self.summary = dict(freed_bytes=0, deleted=0, moved=0, skipped={}, errors=[], dry_run=dry_run)

    def log(self, action, path, reason, size=0):
        print(f"janitor: {action} {path} ({size} bytes; {reason})", flush=True)

    def skip(self, path, reason):
        counts = self.summary["skipped"]
        counts[reason] = counts.get(reason, 0) + 1
        self.log("skip", path, reason)

    def error(self, path, error):
        detail = " ".join(str(error).split())
        self.summary["errors"].append(f"{path}: {detail}")
        self.log("error", path, detail)

    def activity(self, path, kind):
        marker = path / ".last-used"
        if kind == "e2e" and marker.exists():
            return marker.lstat().st_mtime
        latest = latest_mtime(path, 3 if kind == "task" else 2, self.touched)
        if kind == "task" and marker.exists():
            latest = max(latest, marker.lstat().st_mtime)
        return latest

    def save_attempts(self):
        # Ignore only our exact deletion timestamps, including after a restart.
        self.attempts[".touched"] = {
            str(path.relative_to(self.root)): times for path, times in self.touched.items()
            if path.exists() and path.lstat().st_mtime_ns == times[0]
        }
        atomic_json(self.state / "attempts.json", self.attempts)

    def queued(self, path):
        try:
            state = json.loads(self.queue_state.read_text())
        except FileNotFoundError:
            return False
        if state.get("version") != 1 or not isinstance(state.get("jobs"), list):
            raise ValueError("unsupported gq state")
        for job in state["jobs"]:
            if job["status"] not in {"queued", "starting", "running"}:
                continue
            # A bare task name in a shell command is conservatively considered a reference.
            command = job.get("command", [])
            text = command if isinstance(command, str) else " ".join(command)
            if re.search(r"(?<![\w.-])" + re.escape(path.name) + r"(?![\w.-])", text):
                return True
            for value in [job.get("cwd", ""), *job.get("archive", [])]:
                if not value:
                    continue
                reference = Path(value).expanduser()
                if reference == path or reference.is_relative_to(path):
                    return True
        return False

    def reason(self, path, kind):
        if path.is_symlink() or not path.is_dir():
            return "symlink or missing directory"
        if kind == "task":
            if (path / ".keep").exists() or (path / ".keep").is_symlink():
                return ".keep"
        age = 7 * DAY if kind == "task" else 48 * HOUR
        if self.activity(path, kind) >= self.now - age:
            return "fresh"
        if kind != "e2e":
            if any(target == path or target.is_relative_to(path)
                   for target in process_paths(self.proc_root)):
                return "process"
        if kind in {"task", "codex-check"} and self.queued(path):
            return "active gq job"
        return None

    def remove(self, path, kind, owner=None):
        reason = self.reason(owner or path, kind)
        if reason:
            self.skip(owner or path, reason)
            return False
        size = tree_bytes(path)
        reason = self.reason(owner or path, kind)
        if reason:
            self.skip(owner or path, reason)
            return False
        self.log("would delete" if self.dry_run else "delete", path, f"idle {kind}; rebuildable", size)
        if not self.dry_run:
            if owner is not None:
                parent = path.parent
                before = parent.stat().st_mtime_ns
                changed, original = self.touched.get(parent, (None, None))
                if changed == before:
                    before = original
            shutil.rmtree(path)
            self.summary["freed_bytes"] += size
            if owner is not None:
                # Never write old times over real work; ignore our exact timestamps.
                self.touched[parent] = (parent.stat().st_mtime_ns, before)
                self.save_attempts()
        self.summary["deleted"] += 1
        return True

    def ephemeral(self, kind):
        root = self.root / kind
        if root.is_symlink():
            self.skip(root, "symlink")
            return
        if not root.exists():
            return
        for path in sorted(root.iterdir()):
            if path.name.startswith(".") or not path.is_dir() or path.is_symlink():
                continue
            try:
                reason = self.reason(path, kind)
                if reason:
                    self.skip(path, reason)
                    continue
                if kind != "e2e":
                    self.remove(path, kind)
                    continue
                lock_path = root / (path.name + ".lock")
                # A dry run must not create even a missing lock file.
                if self.dry_run and not lock_path.exists() and not lock_path.is_symlink():
                    self.remove(path, kind)
                    continue
                flags = os.O_RDONLY if self.dry_run else os.O_CREAT | os.O_WRONLY
                with os.fdopen(os.open(lock_path, flags | os.O_NOFOLLOW, 0o600),
                               "r" if self.dry_run else "w") as lock:
                    try:
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    except BlockingIOError:
                        self.skip(path, "e2e lock busy")
                        continue
                    if lock_path.stat().st_ino != os.fstat(lock.fileno()).st_ino:
                        self.skip(path, "e2e lock replaced")
                        continue
                    self.remove(path, kind)
            except FileNotFoundError:
                self.skip(path, "disappeared during cleanup")
            except Exception as error:
                self.error(path, error)

    def rebuildable(self, root):
        for path in sorted(root.iterdir()):
            if path.is_symlink() or not path.is_dir():
                continue
            name = path.name
            disposable = name in {"_deps", "__pycache__", ".pytest_cache"} or name.startswith("pytest-")
            if name == ".venv":
                disposable = (path / "pyvenv.cfg").is_file()
            if name == "build" or name.startswith("cmake-build-"):
                disposable = any((path / marker).is_file() for marker in ("CMakeCache.txt", "build.ninja"))
            if disposable:
                yield path
            else:
                yield from self.rebuildable(path)

    def archive_task(self, path):
        nas_root = Path(os.environ.get("GOTHIC_NAS_ROOT", "/mnt/shared/gothic-ai")).expanduser().absolute()
        try:
            mounted = nas_archive.nas_mounted(nas_root)
        except OSError:
            mounted = False
        if not mounted:
            self.skip(path, "NAS not mounted")
            return
        identity = [path.stat().st_dev, path.stat().st_ino]
        attempt = self.attempts.get(path.name)
        if attempt is not None and attempt.get("identity") == identity:
            try:
                nas_archive.check_attempt(nas_root / attempt["destination_relative"],
                                          {"source": str(path.resolve()), "attempt_id": attempt["attempt_id"]})
            except nas_archive.ArchiveError:
                # An unmarked destination is never ours to overwrite or resume.
                self.attempts.pop(path.name)
                if not self.dry_run:
                    self.save_attempts()
                attempt = None
        if attempt is None or attempt.get("identity") != identity:
            date = time.strftime("%Y%m%d", time.localtime(self.now))
            attempt = dict(destination_relative=f"archive/{path.name}-{date}",
                           attempt_id=uuid.uuid4().hex, identity=identity)
        size = tree_bytes(path)
        if self.dry_run:
            size -= sum(tree_bytes(cache) for cache in self.rebuildable(path))
        reason = self.reason(path, "task")
        if reason:
            self.skip(path, reason)
            return
        nas_archive.check_attempt(nas_root / attempt["destination_relative"],
                                  {"source": str(path.resolve()), "attempt_id": attempt["attempt_id"]})
        if not self.dry_run:
            self.attempts[path.name] = attempt
            self.save_attempts()
        self.log("would move" if self.dry_run else "move", path,
                 f"idle task -> {nas_root / attempt['destination_relative']}", size)
        try:
            # The archive dry-run sees caches that our deletion plan already excludes.
            with contextlib.redirect_stdout(io.StringIO()) if self.dry_run else contextlib.nullcontext():
                nas_archive.move(path, destination_relative=attempt["destination_relative"],
                                 attempt_id=attempt["attempt_id"], dry_run=self.dry_run,
                                 cancelled=lambda: self.reason(path, "task") is not None)
        except nas_archive.ArchiveError:
            reason = self.reason(path, "task")
            if reason in {"fresh", "process", "active gq job", ".keep"}:
                self.skip(path, reason)
                return
            raise
        if not self.dry_run:
            self.summary["freed_bytes"] += size
            self.attempts.pop(path.name, None)
            self.save_attempts()
        self.summary["moved"] += 1

    def tasks(self):
        patterns = list(PROTECTED)
        keep = self.root / ".janitor-keep"
        if keep.is_symlink():
            raise ValueError("refusing a symlink for .janitor-keep")
        if keep.exists():
            patterns.extend(line.strip() for line in keep.read_text().splitlines() if line.strip())
        for path in sorted(self.root.iterdir()):
            if not path.is_dir() or path.is_symlink():
                continue
            try:
                if any(fnmatch.fnmatchcase(path.name, pattern) for pattern in patterns):
                    self.skip(path, "protected")
                    continue
                reason = self.reason(path, "task")
                if reason:
                    self.skip(path, reason)
                    continue
                for cache in list(self.rebuildable(path)):
                    if not self.remove(cache, "task", path):
                        break
                else:
                    self.archive_task(path)
            except FileNotFoundError:
                self.skip(path, "disappeared during cleanup")
            except Exception as error:
                self.error(path, error)

    def clean_uv(self):
        marker = self.state / "uv-clean"
        if marker.is_symlink() or self.uv_cache.is_symlink():
            raise ValueError("refusing a symlink for the uv cache or cleanup marker")
        due = not marker.exists() or self.now - marker.stat().st_mtime >= 7 * DAY
        if not due and shutil.disk_usage(self.root).free >= 60 * GIB:
            self.skip(self.uv_cache, "uv cleanup not due")
            return
        try:
            size = tree_bytes(self.uv_cache) if self.uv_cache.exists() else 0
        except FileNotFoundError:
            self.skip(self.uv_cache, "uv cache busy")
            return
        self.log("would clean" if self.dry_run else "clean", self.uv_cache,
                 "weekly or free space below 60 GiB", size)
        if self.dry_run:
            return
        result = subprocess.run([self.uv, "cache", "clean"], capture_output=True, text=True,
                                env={**os.environ, "UV_CACHE_DIR": str(self.uv_cache), "UV_LOCK_TIMEOUT": "30"})
        if result.returncode:
            detail = " ".join((result.stderr or result.stdout).split())
            if "lock" in detail.lower() and any(word in detail.lower() for word in ("timeout", "timed out", "busy", "acquir")):
                self.skip(self.uv_cache, "uv cache busy")
                return
            raise RuntimeError(f"uv cache clean failed ({result.returncode}): {detail}")
        atomic_json(marker, {"time": self.now})
        os.utime(marker, (self.now, self.now), follow_symlinks=False)
        try:
            remaining = tree_bytes(self.uv_cache) if self.uv_cache.exists() else 0
        except FileNotFoundError:
            self.skip(self.uv_cache, "uv cache busy")
            return
        self.summary["freed_bytes"] += max(0, size - remaining)

    def finish(self, save=True):
        if save and not self.dry_run:
            try:
                atomic_json(self.state / "last.json", self.summary)
            except Exception as error:
                self.error(self.state / "last.json", error)
        print("janitor: summary " + json.dumps(self.summary, sort_keys=True), flush=True)
        return int(bool(self.summary["errors"]))

    def run(self):
        try:
            if self.root.is_symlink():
                raise ValueError("refusing a symlink for the local root")
            # A directory flock is our private lock, requiring no writes in dry-run.
            with contextlib.ExitStack() as resources:
                lock = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
                resources.callback(os.close, lock)
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    self.skip(self.root, "janitor already running")
                    return self.finish(save=False)
                if self.state.is_symlink():
                    raise ValueError("refusing a symlink for janitor state")
                if not self.dry_run:
                    self.state.mkdir(exist_ok=True, mode=0o700)
                pending = self.state / "attempts.json"
                self.attempts = json.loads(pending.read_text()) if pending.exists() else {}
                self.touched = {self.root / relative: times
                                for relative, times in self.attempts.pop(".touched", {}).items()}
                for action in (lambda: self.ephemeral("e2e"), lambda: self.ephemeral("codex-check"),
                               self.tasks, self.clean_uv):
                    try:
                        action()
                    except Exception as error:
                        self.error(self.root, error)
                return self.finish()
        except Exception as error:
            self.error(self.root, error)
            return self.finish(save=False)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true", help="print the plan without changing files")
    args = parser.parse_args(argv)
    return Janitor(dry_run=args.dry_run).run()


if __name__ == "__main__":
    raise SystemExit(main())
