#!/usr/bin/env python3
"""Copy finished task directories to CIFS, verify contents, then remove the source."""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import subprocess
import sys


INCOMPLETE_MARKER = ".janitor-incomplete.json"


class ArchiveError(Exception):
    def __init__(self, message, *, bytes=0):
        super().__init__(message)
        self.bytes = bytes


def local_root():
    return Path(os.environ.get("GOTHIC_LOCAL_ROOT", "~/gothic-ai")).expanduser().resolve()


def validate_source(src, *, require_absolute=False):
    path = Path(src)
    if require_absolute and not path.is_absolute():
        raise ArchiveError("source must be an absolute path inside the local root")
    if path.expanduser().is_symlink():
        raise ArchiveError("source must be a directory, not a symbolic link")
    path = path.expanduser().resolve()
    root = local_root()
    if path == root or not path.is_relative_to(root):
        raise ArchiveError(f"source must be below the local root: {root}")
    return path


def nas_mounted(root):
    # Opening the directory first makes autofs mount CIFS before /proc/mounts is read.
    with os.scandir(root):
        pass
    mounts = Path(os.environ.get("GOTHIC_NAS_MOUNTS", "/proc/mounts"))
    for line in mounts.read_text().splitlines():
        fields = line.split()
        if len(fields) < 3 or fields[2] != "cifs":
            continue
        mount = Path(re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), fields[1]))
        if root == mount or root.is_relative_to(mount):
            return True
    return False


def inventory(root, *, exclude_pytest=True):
    files, directories, links = {}, set(), {}

    def failed(error):
        raise error

    for directory, dirnames, filenames in os.walk(root, followlinks=False, onerror=failed):
        for name in sorted(dirnames + filenames):
            path = Path(directory) / name
            if exclude_pytest and name.startswith("pytest-"):
                if name in dirnames:
                    dirnames.remove(name)
                continue
            relative = path.relative_to(root).as_posix()
            info = path.lstat()
            if stat.S_ISLNK(info.st_mode):
                links[relative] = os.readlink(path)
                if name in dirnames:
                    dirnames.remove(name)
            elif stat.S_ISDIR(info.st_mode):
                directories.add(relative)
            elif stat.S_ISREG(info.st_mode):
                files[relative] = (info.st_size, info.st_mtime_ns)
            else:
                raise ArchiveError(f"unsupported file type: {path}")
    return files, directories, links


def symlink_table(links):
    output = io.StringIO(newline="")
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    writer.writerows(sorted(links.items()))
    return output.getvalue()


def digest(path):
    with path.open("rb") as source:
        return hashlib.file_digest(source, "sha256").digest()


def verify(src, dest, snapshot, table, *, exclude_pytest=True):
    if inventory(src, exclude_pytest=exclude_pytest) != snapshot:
        raise ArchiveError("source changed during copying")
    files, directories, links = snapshot
    for relative in directories:
        path = dest / relative
        if path.is_symlink() or not path.is_dir():
            raise ArchiveError(f"directory mismatch: {relative}")
    for relative, (size, _mtime) in files.items():
        path = dest / relative
        if (path.is_symlink() or not path.is_file() or path.stat().st_size != size
                or digest(src / relative) != digest(path)):
            raise ArchiveError(f"checksum mismatch: {relative}")
    if table is not None and (dest / ".symlinks.tsv").read_text() != table:
        raise ArchiveError("checksum mismatch: .symlinks.tsv")
    # Detect files created, removed or modified while checksums were being read.
    if inventory(src, exclude_pytest=exclude_pytest) != snapshot:
        raise ArchiveError("source changed during verification")


def check_attempt(destination, attempt):
    """Only an interrupted copy with the same source and token may reuse a path."""
    if not destination.exists():
        return
    marker = destination / INCOMPLETE_MARKER
    try:
        matches = (destination.is_dir() and not marker.is_symlink()
                   and json.loads(marker.read_text()) == attempt)
    except (OSError, ValueError):
        matches = False
    if not matches:
        raise ArchiveError(f"destination already exists without a matching unfinished attempt: {destination}")


def move(src, *, keep_local=False, dry_run=False, cancelled=None,
         destination_relative=None, attempt_id=None):
    """Return verified data bytes; never remove local data before complete verification."""
    size = 0
    try:
        source = validate_source(src)
        if not source.is_dir():
            raise ArchiveError(f"source directory does not exist: {source}")
        if (source / ".symlinks.tsv").exists() or (source / ".symlinks.tsv").is_symlink():
            raise ArchiveError("source contains reserved file .symlinks.tsv")
        root = Path(os.environ.get("GOTHIC_NAS_ROOT", "/mnt/shared/gothic-ai")).expanduser().absolute()
        attempt = None
        if destination_relative is None:
            if attempt_id is not None:
                raise ArchiveError("attempt_id requires destination_relative")
            destination = root / source.relative_to(local_root())
        else:
            relative = Path(destination_relative)
            if relative.is_absolute() or not relative.parts or ".." in relative.parts:
                raise ArchiveError("destination_relative must be a nonempty relative path without '..'")
            if not isinstance(attempt_id, str) or not attempt_id:
                raise ArchiveError("destination_relative requires a nonempty attempt_id")
            destination = root / relative
            attempt = {"source": str(source), "attempt_id": attempt_id}
            marker = source / INCOMPLETE_MARKER
            if marker.exists() or marker.is_symlink():
                raise ArchiveError(f"source contains reserved file {INCOMPLETE_MARKER}")
        if root.resolve().is_relative_to(local_root()) or local_root().is_relative_to(root.resolve()):
            raise ArchiveError("local and NAS roots must not overlap")
        # CIFS cannot store links. Refuse a misleading destination on other filesystems too.
        for path in (destination, *destination.parents):
            if path.is_symlink():
                raise ArchiveError(f"destination contains a symbolic link: {path}")
        if attempt is not None:
            check_attempt(destination, attempt)
        exclude_pytest = attempt is None
        snapshot = inventory(source, exclude_pytest=exclude_pytest)
        files, directories, links = snapshot
        size = sum(info[0] for info in files.values())
        for relative, target in sorted(links.items()):
            print(f"nas_archive: symlink {relative!r} -> {target!r}", flush=True)
        if dry_run:
            print(f"nas_archive: would move {source} -> {destination} ({size} bytes)", flush=True)
            return size
        if not nas_mounted(root):
            raise ArchiveError(f"NAS is not mounted as cifs: {root}")
        table = symlink_table(links) if links else None
        required = size + (len(table.encode()) if table is not None else 0)
        # Already copied files consume NAS space; rsync can reuse those bytes on retry.
        existing = 0
        for relative in (*files, *directories, ".symlinks.tsv"):
            path = destination / relative
            if path.is_symlink():
                raise ArchiveError(f"destination contains a symbolic link: {path}")
            if relative in files and path.is_file():
                existing += min(path.stat().st_size, files[relative][0])
        if shutil.disk_usage(root).free < required - existing:
            raise ArchiveError(f"not enough NAS space: need {required - existing} bytes")
        if cancelled is not None and cancelled():
            raise ArchiveError("archive cancelled")
        if attempt is None:
            destination.mkdir(parents=True, exist_ok=True)
        else:
            try:
                destination.mkdir(parents=True)
            except FileExistsError:
                check_attempt(destination, attempt)
            else:
                with (destination / INCOMPLETE_MARKER).open("x") as marker:
                    json.dump(attempt, marker)
        if attempt is not None:
            copied_files, copied_directories, copied_links = inventory(destination, exclude_pytest=False)
            expected = set(files) | directories | set(links) | {INCOMPLETE_MARKER}
            if table is not None:
                expected.add(".symlinks.tsv")
            stale = (set(copied_files) | copied_directories | set(copied_links)) - expected
            check_attempt(destination, attempt)
            if not nas_mounted(root):
                raise ArchiveError(f"NAS is no longer mounted as cifs: {root}")
            # Only this attempt owns these paths; never follow stale directory links.
            for relative in sorted(stale, key=lambda name: name.count("/"), reverse=True):
                path = destination / relative
                if path.is_symlink() or not path.is_dir():
                    path.unlink()
                else:
                    path.rmdir()
        # Repair interrupted copies even when their size and mtime match the source,
        # without a pre-copy checksum read from NAS. SHA-256 verifies the result below.
        result = subprocess.run(
            ["rsync", "--recursive", "--times", "--ignore-times", "--no-links", "--partial",
             *(["--exclude=pytest-*"] if exclude_pytest else []),
             "--", str(source) + "/", str(destination) + "/"],
            capture_output=True, text=True,
        )
        if result.returncode:
            detail = " ".join((result.stderr or result.stdout).split())
            raise ArchiveError(f"rsync failed ({result.returncode}): {detail}")
        if table is not None:
            (destination / ".symlinks.tsv").write_text(table)
        verify(source, destination, snapshot, table, exclude_pytest=exclude_pytest)
        if not nas_mounted(root):
            raise ArchiveError(f"NAS is no longer mounted as cifs: {root}")
        if cancelled is not None and cancelled():
            raise ArchiveError("archive cancelled")
        if attempt is not None:
            # Seal verified data before cleanup can remove or partially remove the
            # source. A later task at the same path must never resume this copy.
            (destination / INCOMPLETE_MARKER).unlink()
        if not keep_local:
            try:
                shutil.rmtree(source)
            except OSError as error:
                raise ArchiveError(f"verified on NAS; local cleanup failed: {error}") from None
        return size
    except ArchiveError as error:
        error.bytes = size
        raise
    except (OSError, subprocess.SubprocessError, KeyboardInterrupt) as error:
        raise ArchiveError(str(error) or "interrupted", bytes=size) from None


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("move")
    command.add_argument("src")
    command.add_argument("--keep-local", action="store_true")
    command.add_argument("--dry-run", action="store_true")
    command.add_argument("--destination-relative")
    command.add_argument("--attempt-id")
    args = parser.parse_args(argv)
    try:
        size = move(args.src, keep_local=args.keep_local, dry_run=args.dry_run,
                    destination_relative=args.destination_relative, attempt_id=args.attempt_id)
        if not args.dry_run:
            action = "copied and verified" if args.keep_local else "moved and verified"
            print(f"nas_archive: {action} {args.src} ({size} bytes)")
        return 0
    except ArchiveError as error:
        detail = " ".join(str(error).split())
        print(f"nas_archive: {detail} ({error.bytes} bytes)", file=sys.stderr)
        return 1


if __name__ == "__main__":
    def interrupted(signum, _frame):
        raise ArchiveError(f"interrupted by {signal.Signals(signum).name}")

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    sys.exit(main())
