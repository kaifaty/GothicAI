"""Backup failures must propagate; remote verification uses the raw PNG bytes."""

import json
import os
from pathlib import Path
import subprocess

import pytest

from test_registry import source_recipe, write_registry


SCRIPT = Path(__file__).with_name('backup-sources.sh')


@pytest.fixture
def prepared_backup(tmp_path):
    entry, source_root, source = source_recipe(tmp_path)
    registry = write_registry(tmp_path, [entry])
    remote_home = tmp_path / 'remote-home'
    remote_home.mkdir()
    bin_dir = tmp_path / 'bin'
    bin_dir.mkdir()
    log = tmp_path / 'commands.jsonl'
    for name, body in {
        'ssh': '''
import json, os, subprocess, sys
from pathlib import Path
with Path(os.environ['BACKUP_TEST_LOG']).open('a') as stream:
    stream.write(json.dumps(sys.argv) + '\\n')
assert sys.argv[1] == 'dev-ubuntu'
env = dict(os.environ, HOME=os.environ['BACKUP_TEST_REMOTE_HOME'])
raise SystemExit(subprocess.run(['bash', '-c', ' '.join(sys.argv[2:])], env=env).returncode)
''',
        'rsync': '''
import json, os, shutil, sys
from pathlib import Path
with Path(os.environ['BACKUP_TEST_LOG']).open('a') as stream:
    stream.write(json.dumps(sys.argv) + '\\n')
assert sys.argv[1:4] == ['-a', '--checksum', '--']
assert sys.argv[-1] == 'dev-ubuntu:gothic-ai/content-backup/sources/'
if os.environ.get('BACKUP_TEST_RSYNC_FAIL'):
    raise SystemExit(23)
target = Path(os.environ['BACKUP_TEST_REMOTE_HOME']) / 'gothic-ai/content-backup/sources'
shutil.copytree(sys.argv[-2], target, dirs_exist_ok=True, symlinks=True)
if os.environ.get('BACKUP_TEST_CORRUPT'):
    next(target.rglob('*.png')).write_bytes(b'corrupt remote copy')
''',
    }.items():
        tool = bin_dir / name
        tool.write_text('#!/usr/bin/env python3\n' + body)
        tool.chmod(0o755)
    env = dict(os.environ, PATH=str(bin_dir) + os.pathsep + os.environ['PATH'],
        BACKUP_TEST_LOG=str(log), BACKUP_TEST_REMOTE_HOME=str(remote_home))
    return registry, source_root, source, remote_home, log, env


def run_backup(prepared):
    registry, source_root, _, _, _, env = prepared
    return subprocess.run([str(SCRIPT), '--registry', str(registry),
        '--source-root', str(source_root)], env=env, capture_output=True, text=True)


def test_backup_copies_only_source_store_and_verifies_remote(prepared_backup):
    registry, source_root, source, remote_home, log, env = prepared_backup
    result = run_backup(prepared_backup)
    assert result.returncode == 0, result.stderr
    assert 'Source backup verified: 1 PNGs on dev-ubuntu' in result.stdout
    remote_source = remote_home / 'gothic-ai/content-backup/sources' / source.relative_to(source_root)
    assert remote_source.read_bytes() == source.read_bytes()
    commands = [json.loads(line) for line in log.read_text().splitlines()]
    assert [Path(command[0]).name for command in commands] == ['ssh', 'rsync', 'ssh']
    assert commands[1][-2] == str(source_root) + '/'
    assert '--delete' not in commands[1]
    assert not (remote_home / 'registry.jsonl').exists()


def test_backup_stops_before_network_when_local_source_differs(prepared_backup):
    _, _, source, _, log, _ = prepared_backup
    source.write_bytes(b'changed local source')
    result = run_backup(prepared_backup)
    assert result.returncode != 0
    assert 'source_sha256: mismatch' in result.stderr
    assert not log.exists()


def test_remote_corruption_returns_nonzero(prepared_backup):
    prepared_backup[-1]['BACKUP_TEST_CORRUPT'] = '1'
    result = run_backup(prepared_backup)
    assert result.returncode != 0
    assert 'source_sha256: mismatch' in result.stderr
    assert 'Source backup verified' not in result.stdout


def test_transfer_failure_never_claims_verified_backup(prepared_backup):
    prepared_backup[-1]['BACKUP_TEST_RSYNC_FAIL'] = '1'
    result = run_backup(prepared_backup)
    assert result.returncode == 23
    assert 'Source backup verified' not in result.stdout
    log = prepared_backup[-2]
    assert len(log.read_text().splitlines()) == 2
