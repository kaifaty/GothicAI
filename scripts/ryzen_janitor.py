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
PROTECTED = ("game", "game-settings", "play", "lib", "bin", "body*", "nav-probe",
             "gq", "queue", ".*")
# Names used by the build and training scripts; never infer disposable data from size.
REBUILDABLE = ("build*", "*-build", "cmake-build-*", ".venv", "venv", "uv", "uv-cache",
               ".uv-cache", "_deps", "__pycache__", ".pytest_cache", ".ruff_cache", "pytest-*",
               "engine-cache", "cache", "hd-cache", "hd-measure-cache", "recording-cache",
               "validation-main-cache", "validation-detour-cache")


def latest_mtime(directory, depth=None, touched=None):
    """Read activity without following directory or file symlinks."""
    touched = touched or {}

    def timestamp(path):
        value = path.lstat().st_mtime_ns
        changed, original = touched.get(path, (None, None))
        return (original if changed == value else value) / 1e9

    latest = timestamp(directory)
    if depth is None or depth:
        for child in directory.iterdir():
            try:
                child_latest = timestamp(child)
                if not child.is_symlink() and child.is_dir():
                    child_latest = max(child_latest, latest_mtime(child, None if depth is None else depth - 1, touched))
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


def tree_reclaimable(path):
    """Conservative disk-space estimate; external hardlinks keep their blocks."""
    inodes = {}
    def visit(entry):
        info = entry.lstat()
        key = (info.st_dev, info.st_ino)
        count, blocks, links = inodes.get(key, (0, getattr(info, "st_blocks", 0) * 512, info.st_nlink))
        inodes[key] = (count + 1, blocks, links)
        if stat.S_ISDIR(info.st_mode):
            inodes[key] = (links, blocks, links)
            for child in entry.iterdir():
                try:
                    visit(child)
                except FileNotFoundError:
                    continue
    visit(path)
    return sum(blocks for count, blocks, links in inodes.values() if count >= links)


def process_paths(proc_root):
    paths = []
    for process in proc_root.iterdir():
        if not process.name.isdigit():
            continue
        links = [process / "cwd", process / "exe"]
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
                 now=None, dry_run=False, base_cache=None, system_root="/", du=False):
        # Only this configured root may be a link (the planned /data migration).
        self.root = Path(root or os.environ.get("GOTHIC_LOCAL_ROOT", "~/gothic-ai")).expanduser().resolve()
        self.uv_cache = Path(uv_cache or "~/.cache/uv").expanduser().absolute()
        self.base_cache = Path(base_cache or "~/.cache/gothic-ai/base").expanduser().absolute()
        self.system_root = Path(system_root)
        self.proc_root = Path(proc_root)
        self.queue_state = Path(queue_state or os.environ.get("GQ_HOME", str(self.root / "queue")))
        if queue_state is None:
            self.queue_state = self.queue_state.expanduser() / "state.json"
        self.now = time.time() if now is None else now
        self.du = du
        self.dry_run = dry_run or du
        self.state = self.root / ".janitor"
        self.touched = {}
        self.attempts = {}
        self.rows = []
        self.actions = []
        self.outcomes = {}
        self.projected = {}
        self.emergency = set()
        self.idle = 2 * DAY
        self.summary = dict(freed_bytes=0, deleted_bytes=0, moved_bytes=0, deleted=0,
                            moved=0, skipped={}, errors=[], dry_run=self.dry_run)

    @staticmethod
    def device(path):
        return path.stat().st_dev

    def disks(self):
        return {name: dict(path=path, device=self.device(path), free=shutil.disk_usage(path).free)
                for name, path in (("tasks", self.root), ("system", self.system_root))}

    def space(self):
        readings = self.disks()
        for name, disk in readings.items():
            # Count each filesystem once, even when both paths share it.
            disk["free"] += self.projected.get(disk["device"], 0) if self.dry_run else 0
        return readings

    def needed(self, path):
        if not self.emergency:
            return True
        device = self.device(path)
        return any(disk["device"] == device and disk["free"] < 100 * GIB
                   for disk in self.space().values() if disk["device"] in self.emergency)

    def record(self, action, path, size, rule, owner=None, reclaim=0):
        self.actions.append(dict(action=action, path=str(path), bytes=size, rule=rule))
        self.outcomes.setdefault(owner or path, []).append(f"{action} {size / GIB:.2f} GiB")
        self.summary[action + "d" if action == "move" else "deleted"] += 1
        self.summary["moved_bytes" if action == "move" else "deleted_bytes"] += size
        self.summary["freed_bytes"] += size
        if self.dry_run:
            device = self.device(owner or path)
            self.projected[device] = self.projected.get(device, 0) + reclaim

    def log(self, action, path, reason, size=0):
        print(f"janitor: {action} {path} ({size} bytes; {reason})", flush=True)

    def skip(self, path, reason):
        counts = self.summary["skipped"]
        counts[reason] = counts.get(reason, 0) + 1
        self.outcomes.setdefault(path, []).append("keep: " + reason)
        if not self.du:
            self.log("skip", path, reason)

    def error(self, path, error):
        detail = " ".join(str(error).split())
        self.summary["errors"].append(f"{path}: {detail}")
        self.log("error", path, detail)

    def activity(self, path, kind):
        return latest_mtime(path, None, self.touched)

    def kept(self, path):
        # Keeping a nested directory also prevents archiving/deleting its owner.
        for parent in (path, *path.parents):
            marker = parent / ".keep"
            if marker.exists() or marker.is_symlink():
                return True
            if parent == self.root or parent == Path.home():
                break
        for directory, children, files in os.walk(path, followlinks=False):
            if ".keep" in children or ".keep" in files:
                return True
            children[:] = [name for name in children if not (Path(directory) / name).is_symlink()]
        return False

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
                reference = Path(value).expanduser().resolve()
                if reference == path or reference.is_relative_to(path):
                    return True
        return False

    def reason(self, path, kind):
        if path.is_symlink() or not path.is_dir():
            return "symlink or missing directory"
        if self.kept(path):
            return ".keep"
        age = 2 * DAY if kind == "base" else self.idle
        if self.activity(path, kind) > self.now - age:
            return "fresh"
        if any(target == path or target.is_relative_to(path)
               for target in process_paths(self.proc_root)):
            return "process"
        if self.queued(path):
            return "active gq job"
        return None

    @contextlib.contextmanager
    def lock_file(self, path):
        # Do not unlink lock files: a game may be waiting on the same inode.
        if self.dry_run and not path.exists() and not path.is_symlink():
            yield
            return
        flags = os.O_RDONLY if self.dry_run else os.O_CREAT | os.O_RDWR
        descriptor = os.open(path, flags | os.O_NOFOLLOW, 0o600)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            if path.stat().st_ino != os.fstat(descriptor).st_ino:
                raise BlockingIOError("lock replaced")
            yield
        finally:
            os.close(descriptor)

    def remove(self, path, kind, owner=None):
        owner = owner or path
        reason = self.reason(owner, kind)
        if reason:
            self.skip(owner, reason)
            return False
        if not self.needed(path):
            self.skip(owner, "free-space target reached or different filesystem")
            return False
        if path.is_symlink():
            self.skip(path, "symlink")
            return False
        size = tree_bytes(path)
        reclaim = tree_reclaimable(path)
        reason = self.reason(owner, kind)
        if reason:
            self.skip(owner, reason)
            return False
        rule = f"idle >= {2 * DAY if kind == 'base' else self.idle:g}s; rebuildable {kind}"
        self.log("would delete" if self.dry_run else "delete", path, rule, size)
        if not self.dry_run:
            parent = path.parent
            before = parent.stat().st_mtime_ns
            changed, original = self.touched.get(parent, (None, None))
            if changed == before:
                before = original
            shutil.rmtree(path)
            if parent.is_relative_to(self.root):
                self.touched[parent] = (parent.stat().st_mtime_ns, before)
                self.save_attempts()
        self.record("delete", path, size, rule, owner, reclaim)
        return True

    def rebuildable(self, root):
        for path in sorted(root.iterdir()):
            if path.is_symlink() or not path.is_dir():
                continue
            if any(fnmatch.fnmatchcase(path.name, pattern) for pattern in REBUILDABLE):
                yield path
            else:
                yield from self.rebuildable(path)

    def archive_task(self, path, kind="task"):
        if not self.needed(path):
            self.skip(path, "free-space target reached or different filesystem")
            return
        key = str(path.relative_to(self.root))
        nas_root = Path(os.environ.get("GOTHIC_NAS_ROOT", "/mnt/shared/gothic-ai")).expanduser().absolute()
        try:
            mounted = nas_archive.nas_mounted(nas_root)
        except OSError:
            mounted = False
        if not mounted:
            self.skip(path, "NAS not mounted")
            return
        identity = [path.stat().st_dev, path.stat().st_ino]
        attempt = self.attempts.get(key)
        if attempt is not None and attempt.get("identity") == identity:
            try:
                nas_archive.check_attempt(nas_root / attempt["destination_relative"],
                                          {"source": str(path.resolve()), "attempt_id": attempt["attempt_id"]})
            except nas_archive.ArchiveError:
                # An unmarked destination is never ours to overwrite or resume.
                self.attempts.pop(key)
                if not self.dry_run:
                    self.save_attempts()
                attempt = None
        if attempt is None or attempt.get("identity") != identity:
            date = time.strftime("%Y%m%d", time.localtime(self.now))
            attempt = dict(destination_relative=f"archive/{key}-{date}",
                           attempt_id=uuid.uuid4().hex, identity=identity)
        size = tree_bytes(path)
        reclaim = tree_reclaimable(path)
        if self.dry_run:
            caches = list(self.rebuildable(path))
            size -= sum(tree_bytes(cache) for cache in caches)
            reclaim = max(0, reclaim - sum(tree_reclaimable(cache) for cache in caches))
        reason = self.reason(path, kind)
        if reason:
            self.skip(path, reason)
            return
        nas_archive.check_attempt(nas_root / attempt["destination_relative"],
                                  {"source": str(path.resolve()), "attempt_id": attempt["attempt_id"]})
        if not self.dry_run:
            self.attempts[key] = attempt
            self.save_attempts()
        rule = f"idle >= {self.idle:g}s; verified archive -> {nas_root / attempt['destination_relative']}"
        self.log("would move" if self.dry_run else "move", path, rule, size)
        try:
            # The archive dry-run sees caches that our deletion plan already excludes.
            with contextlib.redirect_stdout(io.StringIO()) if self.dry_run else contextlib.nullcontext():
                nas_archive.move(path, destination_relative=attempt["destination_relative"],
                                 attempt_id=attempt["attempt_id"], dry_run=self.dry_run,
                                 cancelled=lambda: self.reason(path, kind) is not None)
        except nas_archive.ArchiveError:
            reason = self.reason(path, kind)
            if reason in {"fresh", "process", "active gq job", ".keep"}:
                self.skip(path, reason)
                return
            raise
        if not self.dry_run:
            self.attempts.pop(key, None)
            self.save_attempts()
        self.record("move", path, size, rule, reclaim=reclaim)

    def candidates(self):
        patterns = []
        keep = self.root / ".janitor-keep"
        if keep.is_symlink():
            raise ValueError("refusing a symlink for .janitor-keep")
        if keep.exists():
            patterns.extend(line.strip() for line in keep.read_text().splitlines() if line.strip())
        for path in sorted(self.root.iterdir()):
            if not path.is_dir() and not path.is_symlink():
                continue
            if any(fnmatch.fnmatchcase(path.name, pattern) for pattern in patterns):
                yield path, "task", "protected"
            elif path.name in {"e2e", "codex-check"} and path.is_dir() and not path.is_symlink():
                if any((parent / ".keep").exists() or (parent / ".keep").is_symlink()
                       for parent in (path, self.root)):
                    yield path, "task", ".keep"
                else:
                    for session in sorted(path.iterdir()):
                        if session.is_dir() or session.is_symlink():
                            yield session, path.name, None
            elif path.is_dir() or path.is_symlink():
                protected = any(fnmatch.fnmatchcase(path.name, pattern) for pattern in PROTECTED)
                if path.name in {".venv", ".uv-cache"}:
                    protected = False
                yield path, "task", "protected" if protected else None
        if self.uv_cache.exists() or self.uv_cache.is_symlink():
            yield self.uv_cache, "uv", None
        if self.base_cache.exists() and not self.base_cache.is_symlink():
            for cache in (self.base_cache, self.base_cache / "videos"):
                if cache.is_symlink() or not cache.is_dir():
                    continue
                for path in sorted(cache.iterdir()):
                    if re.fullmatch(r"[0-9a-f]{64}", path.name):
                        yield path, "base", None
        elif self.base_cache.is_symlink():
            self.skip(self.base_cache, "symlink")

    def base_retained(self, path):
        cache = path.parent
        fingerprints = [child for child in cache.iterdir()
                        if not child.is_symlink() and child.is_dir()
                        and re.fullmatch(r"[0-9a-f]{64}", child.name)]
        freshest = sorted(fingerprints, key=lambda child: child.stat().st_mtime, reverse=True)[:2]
        if path in freshest:
            return "two freshest base fingerprints"
        current = self.base_cache / "current"
        if current.is_symlink():
            return "symlink current pointer"
        if current.is_dir():
            for pointer in current.glob("*.json"):
                if pointer.is_symlink():
                    return "symlink current pointer"
                data = json.loads(pointer.read_text())
                for kind in ("base", "videos"):
                    part = data.get(kind) or {}
                    if part.get("fingerprint") == path.name:
                        return "current base fingerprint"
        return None

    def clean_candidate(self, path, kind):
        with contextlib.ExitStack() as resources:
            try:
                if kind == "e2e":
                    resources.enter_context(self.lock_file(path.parent / (path.name + ".lock")))
                if kind == "base":
                    # Engine selection/import and our pruning must serialize together.
                    resources.enter_context(self.lock_file(path.parent / ".lock"))
                    resources.enter_context(self.lock_file(path.with_name(path.name + ".use")))
                    retained = self.base_retained(path)
                    if retained:
                        self.skip(path, retained)
                        return
                    nas_root = Path(os.environ.get("GOTHIC_NAS_ROOT", "/mnt/shared/gothic-ai")).expanduser().absolute()
                    backup = nas_root / "base" / path.relative_to(self.base_cache)
                    if (not nas_archive.nas_mounted(nas_root) or backup.is_symlink()
                            or not backup.is_dir()):
                        self.skip(path, "base copy missing on NAS")
                        return
            except BlockingIOError:
                self.skip(path, "base in use" if kind == "base" else "e2e lock busy")
                return
            if kind in {"base", "uv"} or (kind == "task" and any(
                    fnmatch.fnmatchcase(path.name, pattern) for pattern in REBUILDABLE)):
                self.remove(path, kind)
                return
            for cache in list(self.rebuildable(path)):
                if not self.remove(cache, kind, path):
                    return
            self.archive_task(path, kind)

    def clean(self):
        candidates = []
        for path, kind, protected in self.candidates():
            try:
                if path.is_symlink():
                    self.skip(path, "symlink")
                    continue
                if protected and not self.du:
                    self.skip(path, protected)
                    continue
                activity = self.activity(path, kind)
                if self.du:
                    size = tree_bytes(path)
                    caches = size if kind in {"base", "uv"} or (kind == "task" and any(
                        fnmatch.fnmatchcase(path.name, p) for p in REBUILDABLE)) else sum(
                            tree_bytes(p) for p in self.rebuildable(path))
                    self.rows.append((path, size, max(0, self.now - activity), caches))
                reason = protected or self.reason(path, kind)
                if reason:
                    self.skip(path, reason)
                    continue
                candidates.append((activity, path, kind))
            except FileNotFoundError:
                self.skip(path, "disappeared during cleanup")
            except Exception as error:
                self.error(path, error)
        for _, path, kind in sorted(candidates):
            try:
                if not self.needed(path):
                    self.skip(path, "free-space target reached or different filesystem")
                    continue
                self.clean_candidate(path, kind)
            except FileNotFoundError:
                self.skip(path, "disappeared during cleanup")
            except Exception as error:
                self.error(path, error)

    def finish(self, save=True):
        self.summary["largest"] = sorted(self.actions, key=lambda action: action["bytes"], reverse=True)[:20]
        if self.du:
            print("PATH\tSIZE GiB\tIDLE hours\tREBUILDABLE GiB\tACTION")
            for path, size, idle, caches in sorted(self.rows, key=lambda row: row[1], reverse=True):
                action = "; ".join(self.outcomes.get(path, ["keep"]))
                print(f"{path}\t{size/GIB:.3f}\t{idle/HOUR:.1f}\t{caches/GIB:.3f}\t{action}")
        if save and not self.dry_run:
            try:
                atomic_json(self.state / "last.json", self.summary)
            except Exception as error:
                self.error(self.state / "last.json", error)
        print("janitor: summary " + json.dumps(self.summary, sort_keys=True), flush=True)
        return int(bool(self.summary["errors"]))

    def run(self):
        try:
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
                readings = self.space()
                self.emergency = {disk["device"] for disk in readings.values() if disk["free"] < 60 * GIB}
                self.idle = 12 * HOUR if self.emergency else 2 * DAY
                for name, disk in readings.items():
                    self.log("space", disk["path"], f"{name}; device={disk['device']}; free", disk["free"])
                self.summary["emergency"] = bool(self.emergency)
                self.clean()
                return self.finish()
        except Exception as error:
            self.error(self.root, error)
            return self.finish(save=False)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dry-run", action="store_true", help="print the plan without changing files")
    parser.add_argument("--du", action="store_true", help="show folder sizes, idle time and the read-only cleanup plan")
    args = parser.parse_args(argv)
    return Janitor(dry_run=args.dry_run, du=args.du).run()


if __name__ == "__main__":
    raise SystemExit(main())
