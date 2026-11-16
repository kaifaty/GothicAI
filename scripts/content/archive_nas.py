#!/usr/bin/env python3
"""Incrementally archive Mac content to the mounted NAS; never prune backups."""

import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time


# Order matters: irreplaceable source images go first.
ROOTS = (
    ('content-sources', 'files', False),
    ('content-staging', 'tar', False),
    ('remaster', 'tar', False),
    ('world-edit', 'tar', False),
    ('content', 'tar', True),
)
DEFAULT_SOURCE = Path('~/.local/share/gothic-ai').expanduser()
DEFAULT_DEST = Path('/Volumes/shared/gothic-ai/content-archive-mac')
DEFAULT_STATE = DEFAULT_SOURCE / 'content-archive'


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def destination_available(dest):
    return os.path.ismount('/Volumes/shared') and dest.parent.is_dir()


def game_running():
    result = subprocess.run(['pgrep', '-x', 'Gothic2Notr'], capture_output=True)
    if result.returncode not in (0, 1):
        raise RuntimeError(f'pgrep failed: exit {result.returncode}')
    return result.returncode == 0


def acquire_lock(state_dir):
    state_dir.mkdir(parents=True, exist_ok=True)
    stream = (state_dir / 'run.lock').open('a')
    try:
        fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        stream.close()
        return None
    return stream


def atomic_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8',
                dir=path.parent, prefix=path.name + '.', suffix='.partial', delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, ensure_ascii=True, indent=2, sort_keys=True)
            stream.write('\n')
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def load_manifest(directory):
    path = directory / 'manifest.json'
    if not path.exists():
        return {}
    with path.open(encoding='utf-8') as stream:
        return json.load(stream)


def tree_entries(root):
    """Use lstat throughout: even directory symlinks stay inside the archive."""
    entries = [(root, root.lstat())]
    if stat.S_ISDIR(entries[0][1].st_mode):
        for child in sorted(root.iterdir()):
            entries.extend(tree_entries(child))
    return entries


def snapshot(unit):
    rows = []
    size = files = 0
    newest = 0
    for path, info in sorted(tree_entries(unit), key=lambda entry: entry[0].relative_to(unit).as_posix()):
        target = ''
        if stat.S_ISLNK(info.st_mode):
            kind, target = 'symlink', os.readlink(path)
        elif stat.S_ISDIR(info.st_mode):
            kind = 'directory'
        elif stat.S_ISREG(info.st_mode):
            kind = 'file'
            size += info.st_size
            files += 1
            newest = max(newest, info.st_mtime_ns)
        else:
            raise ValueError(f'Unsupported file type: {path}')
        rows.append((path.relative_to(unit).as_posix(), kind, info.st_size, info.st_mtime_ns, target))
    fingerprint = hashlib.sha256(json.dumps(rows, ensure_ascii=True).encode()).hexdigest()
    return dict(fingerprint=fingerprint, size=size, files=files, newest=newest)


def tar_units(root, whole):
    if not root.exists():
        return []
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f'Expected source directory: {root}')
    if whole:
        return [root]
    return sorted(child for child in root.iterdir() if child.is_dir() and not child.is_symlink())


def file_sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


class HashingWriter:
    def __init__(self, stream):
        self.stream = stream
        self.digest = hashlib.sha256()

    def write(self, data):
        written = self.stream.write(data)
        self.digest.update(data[:written])
        return written


def write_verified(source, target, *, archive):
    target.parent.mkdir(parents=True, exist_ok=True)
    partial = target.with_name(target.name + '.partial')
    try:
        with partial.open('wb') as stream:
            writer = HashingWriter(stream)
            if archive:
                with tarfile.open(fileobj=writer, mode='w|', dereference=False) as bundle:
                    bundle.add(source, arcname=source.name)
            else:
                with source.open('rb') as incoming:
                    shutil.copyfileobj(incoming, writer, length=1024 * 1024)
        checksum = writer.digest.hexdigest()
        if file_sha256(partial) != checksum:
            raise ValueError(f'Readback SHA-256 mismatch: {target}')
        size = partial.stat().st_size
        os.replace(partial, target)
        return checksum, size
    finally:
        partial.unlink(missing_ok=True)


def cache_matches(entry, source_info, target):
    if not entry or target.is_symlink() or not target.is_file():
        return False
    target_info = target.stat()
    return (entry.get('size') == source_info.st_size == target_info.st_size
            and entry.get('mtime_ns') == source_info.st_mtime_ns
            and entry.get('dest_mtime_ns') == target_info.st_mtime_ns)


def archive_tar(unit, directory, manifest, cutoff, dry_run, counts):
    info = snapshot(unit)
    target = directory / (unit.name + '.tar')
    if not dry_run:
        target.with_name(target.name + '.partial').unlink(missing_ok=True)
    if info['newest'] > cutoff:
        counts['skipped'] += 1
        counts['skipped_reasons'][unit.name] = 'still_settling'
        return
    previous = manifest.get(unit.name, {})
    if previous.get('fingerprint') == info['fingerprint'] and target.is_file():
        counts['skipped'] += 1
        return
    if dry_run:
        counts['copied'] += 1
        counts['bytes'] += info['size']
        return
    checksum, size = write_verified(unit, target, archive=True)
    manifest[unit.name] = dict(fingerprint=info['fingerprint'], sha256=checksum,
        size=size, files=info['files'], date=utc_now())
    atomic_json(directory / 'manifest.json', manifest)
    counts['copied'] += 1
    counts['bytes'] += size


def archive_file(source, relative, source_info, directory, manifest, cutoff, dry_run, counts):
    if not stat.S_ISREG(source_info.st_mode):
        raise ValueError(f'Expected regular source file: {source}')
    target = directory / relative
    if not dry_run:
        target.with_name(target.name + '.partial').unlink(missing_ok=True)
    if source_info.st_mtime_ns > cutoff:
        counts['skipped'] += 1
        counts['skipped_reasons'][relative] = 'still_settling'
        return
    if cache_matches(manifest.get(relative), source_info, target):
        counts['skipped'] += 1
        return
    exists = target.exists() or target.is_symlink()
    if exists:
        if (target.is_symlink() or not target.is_file()
                or target.stat().st_size != source_info.st_size):
            raise ValueError(f'Conflicting destination file: {target}')
        checksum = file_sha256(source)
        if checksum != file_sha256(target):
            raise ValueError(f'Conflicting destination file: {target}')
        size = source_info.st_size
        counts['skipped'] += 1
    else:
        if dry_run:
            counts['copied'] += 1
            counts['bytes'] += source_info.st_size
            return
        checksum, size = write_verified(source, target, archive=False)
        counts['copied'] += 1
        counts['bytes'] += size
    if not dry_run:
        manifest[relative] = dict(size=size, mtime_ns=source_info.st_mtime_ns,
            sha256=checksum, dest_mtime_ns=target.stat().st_mtime_ns, date=utc_now())
        atomic_json(directory / 'manifest.json', manifest)


def rsync_sources(source, host):
    result = subprocess.run(['rsync', '-a', '--', str(source) + '/',
        host + ':gothic-ai/content-backup/sources/'], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'rsync failed: exit {result.returncode}: {result.stderr.strip()}')


def selected_roots(only):
    return [row for row in ROOTS if only is None or row[0] == only]


def archive_root(args, name, mode, whole, counts, errors):
    root, directory = args.source_root / name, args.dest / name
    if not root.exists():
        return
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f'Expected source directory: {root}')
    manifest = load_manifest(directory)
    cutoff = time.time_ns() - int(args.settle_minutes * 60 * 1_000_000_000)
    if mode == 'tar':
        units = tar_units(root, whole)
    else:
        def walk_error(error):
            counts['errors'] += 1
            errors.append(f'{root}: {error}')

        units = []
        for parent, directories, filenames in os.walk(root, onerror=walk_error, followlinks=False):
            directories.sort()
            links = [child for child in directories if (Path(parent) / child).is_symlink()]
            units.extend(Path(parent) / child for child in sorted(filenames + links))
    for unit in units:
        try:
            if mode == 'tar':
                archive_tar(unit, directory, manifest, cutoff, args.dry_run, counts)
            else:
                archive_file(unit, unit.relative_to(root).as_posix(), unit.lstat(),
                    directory, manifest, cutoff, args.dry_run, counts)
        except (OSError, ValueError, tarfile.TarError) as error:
            counts['errors'] += 1
            errors.append(f'{unit}: {error}')


def run(args):
    report = dict(date=utc_now(), dry_run=args.dry_run, skipped_reason=None, roots={}, errors=[])
    started = time.monotonic()
    lock = None
    try:
        if not destination_available(args.dest):
            report['skipped_reason'] = 'destination_unavailable'
        elif game_running():
            report['skipped_reason'] = 'game_running'
        else:
            lock = acquire_lock(args.state_dir)
            if lock is None:
                report['skipped_reason'] = 'already_running'
        if report['skipped_reason'] is None:
            for name, mode, whole in selected_roots(args.only):
                counts = dict(copied=0, skipped=0, errors=0, bytes=0, seconds=0, skipped_reasons={})
                report['roots'][name] = counts
                root_started = time.monotonic()
                try:
                    archive_root(args, name, mode, whole, counts, report['errors'])
                except (OSError, ValueError) as error:
                    counts['errors'] += 1
                    report['errors'].append(f'{name}: {error}')
                counts['seconds'] = round(time.monotonic() - root_started, 3)
                print(f'{name}: copied={counts["copied"]} skipped={counts["skipped"]} '
                    f'errors={counts["errors"]} bytes={counts["bytes"]} seconds={counts["seconds"]}')
            sources = args.source_root / 'content-sources'
            if (not args.dry_run and args.rsync_host and sources.is_dir()
                    and args.only in (None, 'content-sources')):
                try:
                    rsync_sources(sources, args.rsync_host)
                except (OSError, RuntimeError) as error:
                    report['errors'].append(str(error))
    except (OSError, RuntimeError) as error:
        report['errors'].append(str(error))
    finally:
        report['seconds'] = round(time.monotonic() - started, 3)
        if report['skipped_reason']:
            print(f'Run skipped: {report["skipped_reason"]}')
        for error in report['errors']:
            print(f'Error: {error}')
        try:
            atomic_json(args.state_dir / 'last-run.json', report)
        finally:
            if lock is not None:
                lock.close()
    return int(bool(report['errors']))


def status(args):
    if not destination_available(args.dest):
        print('Status unavailable: destination_unavailable')
        return 0
    errors = []
    for name, mode, whole in ROOTS:
        missing = changed = size = 0
        root, directory = args.source_root / name, args.dest / name
        try:
            manifest = load_manifest(directory)
            if mode == 'tar':
                for unit in tar_units(root, whole):
                    info = snapshot(unit)
                    if unit.name not in manifest or not (directory / (unit.name + '.tar')).is_file():
                        missing += 1
                        size += info['size']
                    elif manifest[unit.name]['fingerprint'] != info['fingerprint']:
                        changed += 1
                        size += info['size']
            elif root.exists():
                for path, info in tree_entries(root):
                    if stat.S_ISDIR(info.st_mode):
                        continue
                    relative = path.relative_to(root).as_posix()
                    target = directory / relative
                    if not target.exists() and not target.is_symlink():
                        missing += 1
                        size += info.st_size
                    elif not cache_matches(manifest.get(relative), info, target):
                        if (target.is_symlink() or not target.is_file()
                                or target.stat().st_size != info.st_size
                                or file_sha256(path) != file_sha256(target)):
                            changed += 1
                            size += info.st_size
        except (OSError, ValueError) as error:
            errors.append(f'{name}: {error}')
        print(f'{name}: missing={missing} changed={changed} bytes={size}')
    for error in errors:
        print(f'Error: {error}')
    return int(bool(errors))


def print_plist(args):
    log = str(Path('~/Library/Logs/gothic-ai/content-archive.log').expanduser())
    value = dict(Label='local.gothic-ai.content-archive',
        ProgramArguments=[str(Path(sys.executable).absolute()), str(Path(__file__).resolve()), 'run',
            '--source-root', str(args.source_root), '--dest', str(args.dest),
            '--state-dir', str(args.state_dir), '--settle-minutes', str(args.settle_minutes),
            '--rsync-host', args.rsync_host],
        StartCalendarInterval=dict(Hour=args.hour, Minute=args.minute),
        ProcessType='Background', LowPriorityIO=True, StandardOutPath=log, StandardErrorPath=log)
    print(plistlib.dumps(value).decode(), end='')
    return 0


def main(argv=None):
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument('--source-root', type=Path, default=DEFAULT_SOURCE)
    common.add_argument('--dest', type=Path, default=DEFAULT_DEST)
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    run_parser = commands.add_parser('run', parents=[common])
    run_parser.add_argument('--dry-run', action='store_true')
    run_parser.add_argument('--only', choices=[row[0] for row in ROOTS])
    commands.add_parser('status', parents=[common])
    plist_parser = commands.add_parser('plist', parents=[common])
    for subparser in (run_parser, plist_parser):
        subparser.add_argument('--state-dir', type=Path, default=DEFAULT_STATE)
        subparser.add_argument('--settle-minutes', type=float, default=30)
        subparser.add_argument('--rsync-host', default='dev-ubuntu')
    plist_parser.add_argument('--hour', type=int, choices=range(24), default=4)
    plist_parser.add_argument('--minute', type=int, choices=range(60), default=30)
    args = parser.parse_args(argv)
    for name in ('source_root', 'dest'):
        setattr(args, name, getattr(args, name).expanduser().absolute())
    if args.command != 'status':
        args.state_dir = args.state_dir.expanduser().absolute()
    return {'run': run, 'status': status, 'plist': print_plist}[args.command](args)


if __name__ == '__main__':
    raise SystemExit(main())
