"""Archive only temporary assets; verification failures must preserve the last copy."""

import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
import tarfile
import time

import pytest

import archive_nas as archive


@pytest.fixture
def prepared(tmp_path, monkeypatch):
    source, dest, state = (tmp_path / name for name in ('source', 'nas', 'state'))
    source.mkdir()
    monkeypatch.setenv('HOME', str(tmp_path / 'home'))
    monkeypatch.setattr(Path, 'home', classmethod(lambda cls: tmp_path / 'home'))
    monkeypatch.setattr(archive, 'destination_available', lambda path: True)
    monkeypatch.setattr(archive, 'game_running', lambda: False)

    def no_subprocess(*args, **kwargs):
        pytest.fail(f'Unexpected external command: {args}')

    monkeypatch.setattr(archive.subprocess, 'run', no_subprocess)
    return source, dest, state


def invoke(prepared, command='run', *extra):
    source, dest, state = prepared
    arguments = [command, '--source-root', str(source), '--dest', str(dest)]
    if command != 'status':
        arguments += ['--state-dir', str(state), '--rsync-host', '']
    return archive.main([*arguments, *extra])


def asset(prepared, path='content-staging/set-a/texture.bin', data=b'original'):
    target = prepared[0] / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    old = time.time() - 3600
    os.utime(target, (old, old))
    return target


def manifest(prepared, root='content-staging'):
    return json.loads((prepared[1] / root / 'manifest.json').read_text())


def report(prepared):
    return json.loads((prepared[2] / 'last-run.json').read_text())


def snapshot(dest):
    if not dest.exists():
        return None
    return {str(path.relative_to(dest)): (path.stat().st_ino, path.stat().st_mtime_ns,
        path.read_bytes() if path.is_file() else None)
        for path in [dest, *dest.rglob('*')]}


@pytest.mark.parametrize('root, unit', [
    ('content-staging', 'set-a'), ('remaster', 'frames'),
    ('world-edit', 'harbour'), ('content', ''),
])
def test_new_unit_is_readable_and_manifest_hash_matches(prepared, root, unit):
    relative = '/'.join(part for part in (root, unit, 'texture.bin') if part)
    source = asset(prepared, relative, b'x' * (1024 * 1024) + b'y')
    assert invoke(prepared) == 0
    name = unit or root
    target = prepared[1] / root / f'{name}.tar'
    with tarfile.open(target) as stream:
        assert stream.extractfile(f'{name}/texture.bin').read() == source.read_bytes()
    entry = manifest(prepared, root)[name]
    assert entry['sha256'] == hashlib.sha256(target.read_bytes()).hexdigest()
    assert len(entry['fingerprint']) == 64
    assert entry['size'] == target.stat().st_size
    assert entry['files'] == 1
    assert entry['date']
    assert report(prepared)['roots'][root]['copied'] == 1


def test_unchanged_run_does_not_write_destination(prepared):
    asset(prepared)
    assert invoke(prepared) == 0
    before = snapshot(prepared[1])
    assert invoke(prepared) == 0
    assert snapshot(prepared[1]) == before
    assert report(prepared)['roots']['content-staging']['skipped'] == 1


def test_changed_unit_replaces_tar_and_manifest(prepared):
    source = asset(prepared)
    assert invoke(prepared) == 0
    old_entry = manifest(prepared)['set-a']
    old_tar = (prepared[1] / 'content-staging/set-a.tar').read_bytes()
    old = source.stat().st_mtime_ns + 100 * 1_000_000_000
    source.write_bytes(b'updated!')
    os.utime(source, ns=(old, old))
    assert invoke(prepared) == 0
    target = prepared[1] / 'content-staging/set-a.tar'
    assert target.read_bytes() != old_tar
    entry = manifest(prepared)['set-a']
    assert entry['fingerprint'] != old_entry['fingerprint']
    assert entry['sha256'] == hashlib.sha256(target.read_bytes()).hexdigest()
    with tarfile.open(target) as stream:
        assert stream.extractfile('set-a/texture.bin').read() == b'updated!'


def test_bad_readback_preserves_old_tar_and_manifest(prepared, monkeypatch):
    asset(prepared)
    assert invoke(prepared) == 0
    target = prepared[1] / 'content-staging/set-a.tar'
    old_tar = target.read_bytes()
    old_manifest = (target.parent / 'manifest.json').read_bytes()
    asset(prepared, data=b'changed asset')
    read_hash = archive.file_sha256
    monkeypatch.setattr(archive, 'file_sha256',
        lambda path: '0' * 64 if str(path).endswith('.partial') else read_hash(path))
    assert invoke(prepared) == 1
    assert target.read_bytes() == old_tar
    assert (target.parent / 'manifest.json').read_bytes() == old_manifest
    assert not target.with_name('set-a.tar.partial').exists()
    assert report(prepared)['roots']['content-staging']['errors'] == 1


def test_interrupted_partial_is_removed_even_when_unit_is_unchanged(prepared):
    asset(prepared)
    partial = prepared[1] / 'content-staging/set-a.tar.partial'
    partial.parent.mkdir(parents=True)
    partial.write_bytes(b'interrupted')
    assert invoke(prepared) == 0
    assert not partial.exists()
    partial.write_bytes(b'another interrupted write')
    assert invoke(prepared) == 0
    assert not partial.exists()


def test_tar_keeps_symlink_without_dereferencing(prepared):
    source = asset(prepared)
    (source.parent / 'texture-link.bin').symlink_to('texture.bin')
    assert invoke(prepared) == 0
    with tarfile.open(prepared[1] / 'content-staging/set-a.tar') as stream:
        link = stream.getmember('set-a/texture-link.bin')
        assert link.issym()
        assert link.linkname == 'texture.bin'


def test_fresh_unit_waits_until_files_settle(prepared, monkeypatch):
    source = asset(prepared)
    now = time.time_ns()
    monkeypatch.setattr(archive.time, 'time_ns', lambda: now)
    cutoff = now - 30 * 60 * 1_000_000_000
    os.utime(source, ns=(cutoff + 1, cutoff + 1))
    assert invoke(prepared) == 0
    assert not (prepared[1] / 'content-staging/set-a.tar').exists()
    stats = report(prepared)['roots']['content-staging']
    assert stats['skipped'] == 1
    assert stats['skipped_reasons']['set-a'] == 'still_settling'
    os.utime(source, ns=(cutoff - 1, cutoff - 1))
    assert invoke(prepared) == 0
    assert (prepared[1] / 'content-staging/set-a.tar').is_file()


def test_files_mode_copies_new_file_and_verifies_hash(prepared, monkeypatch):
    source = asset(prepared, 'content-sources/imagegen/source.png', b'x' * (1024 * 1024) + b'y')
    assert invoke(prepared) == 0
    target = prepared[1] / 'content-sources/imagegen/source.png'
    assert target.read_bytes() == source.read_bytes()
    entry = manifest(prepared, 'content-sources')['imagegen/source.png']
    assert entry['sha256'] == hashlib.sha256(target.read_bytes()).hexdigest()
    assert entry['size'] == source.stat().st_size
    assert entry['mtime_ns'] == source.stat().st_mtime_ns
    assert not target.with_name('source.png.partial').exists()
    asset(prepared, 'content-sources/imagegen/bad.png')
    read_hash = archive.file_sha256
    monkeypatch.setattr(archive, 'file_sha256', lambda path:
        '0' * 64 if Path(path).name == 'bad.png.partial' else read_hash(path))
    assert invoke(prepared) == 1
    assert not target.with_name('bad.png').exists()
    assert not target.with_name('bad.png.partial').exists()
    assert 'imagegen/bad.png' not in manifest(prepared, 'content-sources')


def test_cached_matching_file_skips_destination_read(prepared, monkeypatch):
    asset(prepared, 'content-sources/imagegen/source.png')
    assert invoke(prepared) == 0
    before = snapshot(prepared[1])
    read_hash = archive.file_sha256

    def no_nas_hash(path):
        assert not Path(path).is_relative_to(prepared[1]), 'Cached NAS file was read again'
        return read_hash(path)

    monkeypatch.setattr(archive, 'file_sha256', no_nas_hash)
    assert invoke(prepared) == 0
    assert snapshot(prepared[1]) == before
    assert report(prepared)['roots']['content-sources']['skipped'] == 1


def test_existing_equal_file_is_verified_then_cached(prepared):
    source = asset(prepared, 'content-sources/source.png')
    target = prepared[1] / 'content-sources/source.png'
    target.parent.mkdir(parents=True)
    target.write_bytes(source.read_bytes())
    before = target.stat().st_mtime_ns
    assert invoke(prepared) == 0
    assert target.stat().st_mtime_ns == before
    assert manifest(prepared, 'content-sources')['source.png']['sha256'] == (
        hashlib.sha256(source.read_bytes()).hexdigest())
    assert report(prepared)['roots']['content-sources']['skipped'] == 1


def test_conflicting_source_file_is_not_overwritten(prepared):
    source = asset(prepared, 'content-sources/source.png', b'local asset')
    target = prepared[1] / 'content-sources/source.png'
    target.parent.mkdir(parents=True)
    target.write_bytes(b'other asset')
    before = snapshot(prepared[1])
    assert invoke(prepared) == 1
    assert target.read_bytes() == b'other asset'
    assert snapshot(prepared[1]) == before
    assert any('source.png' in error for error in report(prepared)['errors'])
    assert report(prepared)['roots']['content-sources']['errors'] == 1
    target.write_bytes(source.read_bytes())
    assert invoke(prepared) == 0
    before = snapshot(prepared[1])
    old = source.stat().st_mtime_ns + 100 * 1_000_000_000
    source.write_bytes(b'other asset')
    os.utime(source, ns=(old, old))
    assert invoke(prepared) == 1
    assert target.read_bytes() == b'local asset'
    assert snapshot(prepared[1]) == before
    assert report(prepared)['roots']['content-sources']['errors'] == 1


def test_deleted_source_file_stays_on_destination_and_in_manifest(prepared):
    source = asset(prepared, 'content-sources/source.png')
    assert invoke(prepared) == 0
    before = snapshot(prepared[1])
    source.unlink()
    assert invoke(prepared) == 0
    assert snapshot(prepared[1]) == before
    assert 'source.png' in manifest(prepared, 'content-sources')


def test_unavailable_destination_skips_whole_run(prepared, monkeypatch):
    asset(prepared)
    monkeypatch.setattr(archive, 'destination_available', lambda path: False)
    assert invoke(prepared) == 0
    assert report(prepared)['skipped_reason'] == 'destination_unavailable'
    assert not prepared[1].exists()


def test_running_game_skips_whole_run(prepared, monkeypatch):
    asset(prepared)
    monkeypatch.setattr(archive, 'game_running', lambda: True)
    assert invoke(prepared) == 0
    assert report(prepared)['skipped_reason'] == 'game_running'
    assert not prepared[1].exists()


def test_held_flock_skips_whole_run(prepared):
    asset(prepared)
    lock = archive.acquire_lock(prepared[2])
    assert lock is not None
    try:
        assert invoke(prepared) == 0
    finally:
        lock.close()
    assert report(prepared)['skipped_reason'] == 'already_running'
    assert not prepared[1].exists()


def test_one_failed_unit_does_not_block_next_unit(prepared, monkeypatch):
    asset(prepared, 'content-staging/a-bad/texture.bin')
    asset(prepared, 'content-staging/b-good/texture.bin')
    read_hash = archive.file_sha256
    monkeypatch.setattr(archive, 'file_sha256', lambda path:
        '0' * 64 if Path(path).name == 'a-bad.tar.partial' else read_hash(path))
    assert invoke(prepared) == 1
    assert not (prepared[1] / 'content-staging/a-bad.tar').exists()
    with tarfile.open(prepared[1] / 'content-staging/b-good.tar') as stream:
        assert stream.extractfile('b-good/texture.bin').read() == b'original'
    assert set(manifest(prepared)) == {'b-good'}
    stats = report(prepared)['roots']['content-staging']
    assert (stats['copied'], stats['errors']) == (1, 1)


def test_unreadable_source_metadata_does_not_block_next_file(prepared, monkeypatch):
    bad = asset(prepared, 'content-sources/a-bad.png')
    good = asset(prepared, 'content-sources/b-good.png')
    read_stat = Path.lstat

    def unreadable_metadata(path, *args, **kwargs):
        if path == bad:
            raise PermissionError(f'Cannot stat source: {path}')
        return read_stat(path, *args, **kwargs)

    monkeypatch.setattr(Path, 'lstat', unreadable_metadata)
    assert invoke(prepared) == 1
    assert not (prepared[1] / 'content-sources/a-bad.png').exists()
    assert (prepared[1] / 'content-sources/b-good.png').read_bytes() == good.read_bytes()
    assert set(manifest(prepared, 'content-sources')) == {'b-good.png'}
    stats = report(prepared)['roots']['content-sources']
    assert (stats['copied'], stats['errors']) == (1, 1)


def test_dry_run_does_not_write_destination_or_start_rsync(prepared):
    asset(prepared)
    source = asset(prepared, 'content-sources/source.png')
    assert invoke(prepared, 'run', '--dry-run', '--rsync-host', 'test-host') == 0
    assert not prepared[1].exists()
    target = prepared[1] / 'content-sources/source.png'
    target.parent.mkdir(parents=True)
    target.write_bytes(source.read_bytes())
    before = snapshot(prepared[1])
    assert invoke(prepared, 'run', '--dry-run', '--rsync-host', 'test-host') == 0
    assert snapshot(prepared[1]) == before


def test_status_reports_missing_and_changed_without_destination_writes(prepared, capsys):
    asset(prepared)
    assert invoke(prepared, 'status') == 0
    assert 'content-staging: missing=1 changed=0 bytes=8' in capsys.readouterr().out
    assert not prepared[1].exists()
    assert invoke(prepared) == 0
    capsys.readouterr()
    asset(prepared, data=b'updated asset')
    before = snapshot(prepared[1])
    assert invoke(prepared, 'status') == 0
    assert 'content-staging: missing=0 changed=1 bytes=13' in capsys.readouterr().out
    assert snapshot(prepared[1]) == before


def test_rsync_failure_is_reported_after_nas_archive(prepared, monkeypatch):
    asset(prepared)
    asset(prepared, 'content-sources/source.png')
    commands = []

    def failed_rsync(command, **kwargs):
        commands.append(command)
        assert (prepared[1] / 'content-staging/set-a.tar').is_file()
        assert (prepared[1] / 'content-sources/source.png').is_file()
        assert command[0] == 'rsync'
        assert '-a' in command
        assert '--delete' not in command
        assert command[-2] == str(prepared[0] / 'content-sources') + '/'
        assert command[-1] == 'test-host:gothic-ai/content-backup/sources/'
        if kwargs.get('check'):
            raise subprocess.CalledProcessError(23, command, stderr='transfer refused')
        return subprocess.CompletedProcess(command, 23, stdout='', stderr='transfer refused')

    monkeypatch.setattr(archive.subprocess, 'run', failed_rsync)
    assert invoke(prepared, 'run', '--rsync-host', 'test-host') == 1
    assert len(commands) == 1
    assert (prepared[1] / 'content-staging/set-a.tar').is_file()
    assert (prepared[1] / 'content-sources/source.png').is_file()
    assert any('rsync' in error.lower() for error in report(prepared)['errors'])


def test_plist_is_parseable_with_label_schedule_and_absolute_paths(prepared, capsys):
    assert invoke(prepared, 'plist', '--hour', '2', '--minute', '17') == 0
    config = plistlib.loads(capsys.readouterr().out.encode())
    assert config['Label'] == 'local.gothic-ai.content-archive'
    assert config['StartCalendarInterval'] == {'Hour': 2, 'Minute': 17}
    assert config['ProcessType'] == 'Background'
    assert config['LowPriorityIO'] is True
    arguments = config['ProgramArguments']
    assert Path(arguments[0]).is_absolute()
    assert Path(arguments[1]).is_absolute()
    assert Path(arguments[1]).name == 'archive_nas.py'
    assert 'run' in arguments
    log = str(Path.home() / 'Library/Logs/gothic-ai/content-archive.log')
    assert config['StandardOutPath'] == config['StandardErrorPath'] == log
    assert invoke(prepared, 'plist') == 0
    assert plistlib.loads(capsys.readouterr().out.encode())['StartCalendarInterval'] == {
        'Hour': 4, 'Minute': 30}
