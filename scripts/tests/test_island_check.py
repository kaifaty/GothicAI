"""The island gate must reject missing motion and every attempted private file read."""

import gzip
import importlib.util
import json
import os
import subprocess
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location('island_check', Path(__file__).resolve().parents[1] / 'island_check.py')
check = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(check)


def evidence(folder):
    context = dict(run_root='/task/run', binary_dir='/task/build', uid=1000, program_exit=0)
    (folder / 'report.json').write_text(json.dumps(context))
    def event(kind, ms, **fields):
        return dict(event=kind, ms=ms, x=1., y=1., z=1., grounded=True, **fields)
    events = [event('start', 0)]
    events += [event('position', ms) for ms in range(100, 30001, 100)]
    events += [event('residency', 100, id='-3,0', **{'from': 'Far', 'to': 'Near'}),
               event('residency', 200, id='2,0', **{'from': 'Near', 'to': 'Far'}),
               event('texture', 200, name='sand', source='/task/run/data/sand.dds'),
               event('jump', 550), event('land', 750)]
    for e in events:
        if 550 <= e['ms'] < 750:
            e['grounded'] = False
    for at, before, after, x, z in [(1000, '0,0', '1,0', 65, 1), (2000, '1,0', '1,1', 65, 65),
                                   (3000, '1,1', '2,1', 129, 65)]:
        e = event('chunk', at, **{'from': before, 'to': after})
        e.update(x=x, z=z)
        events.append(e)
    for at, name in [(1100, 'beach'), (2100, 'rocks'), (3100, 'hill')]:
        (folder / (name+'.png')).write_bytes(b'fixture')
        events.append(event('shot', at, name=name, file=f'/task/run/{name}.png'))
    events.append(event('census', 30000, content={}))
    events.sort(key=lambda e: e['ms'])
    events.append(event('end', 30000, completed=True, exit_code=0, frames=900,
                        frame_median_ms=25, frame_p95_ms=30))
    (folder / 'census.json').write_text('{}')
    (folder / 'clip.mp4').write_bytes(b'fixture')
    with gzip.open(folder / 'strace.log.gz', 'wt') as trace:
        trace.write('100 execve("/task/build/engine-island", ["/task/build/engine-island", "--script", "/not-a-file-operand"], 0x0) = 0\n'
                    '100 openat(AT_FDCWD, "data/sand.dds", O_RDONLY) = 3</task/run/data/sand.dds>\n'
                    '100 openat(AT_FDCWD, "/usr/lib/libc.so", O_RDONLY) = 4</usr/lib/libc.so>\n'
                    '100 +++ exited with 0 +++\n')
    return events


def save(folder, events):
    (folder / 'events.jsonl').write_text(''.join(json.dumps(e)+'\n' for e in events))


def test_pass(tmp_path):
    save(tmp_path, evidence(tmp_path))
    report = check.verify(tmp_path)
    assert report['passed'] and report['crossings'] == 3
    assert report['jumps'] == report['landings'] == 1 and report['forbidden_paths'] == []


@pytest.mark.parametrize('defect,reason', [
    ('exit', 'exit 0'), ('short', '30–60'), ('long', '30–60'),
    ('crossings', 'only 1'), ('two_crossings', 'only 2'), ('axis', 'both X and Z'),
    ('load', 'streaming missing'), ('unload', 'streaming missing'), ('jump', 'jump/landing'),
    ('land', 'incomplete'), ('ground', 'off ground'), ('shot', 'missing shots'),
    ('samples', 'position samples'), ('sparse_samples', 'position samples'), ('census', 'content census'), ('texture', 'texture loads'),
])
def test_motion_failures(tmp_path, defect, reason):
    events = evidence(tmp_path)
    if defect == 'exit':
        events[-1]['completed'] = False
    elif defect == 'short':
        events[-1]['ms'] = 20000
    elif defect == 'long':
        events[-1]['ms'] = 61000
        sample = next(e for e in events if e['event'] == 'position' and e['ms'] == 30000)
        events[-1:-1] = [dict(sample, ms=ms) for ms in range(30100, 61001, 100)]
    elif defect in ('crossings', 'two_crossings'):
        crossings = [e for e in events if e['event'] == 'chunk']
        keep = 1 if defect == 'crossings' else 2
        events = [e for e in events if e not in crossings[keep:]]
    elif defect == 'axis':
        for e in events:
            if e['event'] == 'chunk':
                e.update(z=1., **{'from': '0,0', 'to': '1,0'})
    elif defect in ('load', 'unload', 'jump', 'land', 'census', 'texture'):
        if defect in ('load', 'unload'):
            source = 'Far' if defect == 'load' else 'Near'
            events = [e for e in events if e['event'] != 'residency' or e['from'] != source]
        else:
            events = [e for e in events if e['event'] != defect]
        if defect == 'jump':
            events = [e for e in events if e['event'] != 'land']
            for e in events:
                e['grounded'] = True
    elif defect == 'ground':
        events[0]['grounded'] = False
    elif defect == 'shot':
        (tmp_path / 'hill.png').unlink()
    elif defect == 'samples':
        events = [e for e in events if e['event'] != 'position']
    elif defect == 'sparse_samples':
        events = [e for e in events if e['event'] != 'position' or e['ms'] % 200 == 0]
    save(tmp_path, events)
    report = check.verify(tmp_path)
    assert not report['passed']
    assert any(reason in failure for failure in report['failures']), report


@pytest.mark.parametrize('syscall', [
    'openat(AT_FDCWD, "/home/player/game/system/Gothic.ini", O_RDONLY) = 3</home/player/game/system/Gothic.ini>',
    'openat(AT_FDCWD, "/home/player/game/system/Gothic.ini", O_RDONLY) = -1 ENOENT (No such file or directory)',
    'openat(7</task/run/data>, "../../../home/player/Gothic.ini", O_RDONLY) = -1 ENOENT',
    'openat(7</home/player/game>, "system/Gothic.ini", O_RDONLY) = -1 ENOENT',
    'openat(AT_FDCWD, "../../game/Gothic.ini", O_RDONLY) = -1 ENOENT',
    'openat(AT_FDCWD, "/task/run/symlink", O_RDONLY) = 3</home/player/game/Gothic.ini>',
    'openat(AT_FDCWD, "/usr-private/Gothic.ini", O_RDONLY) = -1 ENOENT',
])
def test_private_paths_including_failed_opens(tmp_path, syscall):
    save(tmp_path, evidence(tmp_path))
    with gzip.open(tmp_path / 'strace.log.gz', 'at') as trace:
        trace.write('100 '+syscall+'\n')
    report = check.verify(tmp_path)
    assert not report['passed'] and report['forbidden_paths']


def test_child_cwd_and_resumed_call():
    paths, errors = check.trace_paths(
        '100 symlink("../nvidiactl", "/dev/char/195:255") = -1 EEXIST\n'
        '100 chdir("data") = 0\n'
        '100 clone(child_stack=NULL, flags=CLONE_VM|SIGCHLD <unfinished ...>\n'
        '100 <... clone resumed>) = 101\n'
        '101 openat(AT_FDCWD, "sand.dds", O_RDONLY <unfinished ...>\n'
        '101 <... openat resumed>) = 3</task/run/data/sand.dds>\n'
        '101 +++ exited with 0 +++\n', '/task/run')
    assert not errors and '/task/run/data/sand.dds' in paths
    assert '/dev/nvidiactl' in paths and '/task/nvidiactl' not in paths
    assert check.trace_paths('', '/task/run')[1]
    assert check.trace_paths('100 openat(AT_FDCWD, "data", O_RDONLY <unfinished ...>\n', '/task/run')[1]


def test_startup_publications_do_not_count(tmp_path):
    events = evidence(tmp_path)
    for e in events:
        if e['event'] in ('residency', 'texture'):
            e['ms'] = 0
    events.sort(key=lambda e: e['ms'])
    save(tmp_path, events)
    report = check.verify(tmp_path)
    assert not report['passed'] and report['loads'] == report['unloads'] == report['texture_loads'] == 0


def test_crash_before_events_has_verdict(tmp_path):
    (tmp_path / 'report.json').write_text(json.dumps(dict(
        run_root='/task/run', binary_dir='/task/build', uid=1000, program_exit=134)))
    report = check.verify(tmp_path)
    assert not report['passed'] and report['failures'] == ['program exited with code 134']
    assert json.loads((tmp_path / 'report.json').read_text()) == report


@pytest.mark.parametrize('failure,expected', [('configure', 76), ('build', 1), ('contract', 1)])
def test_remote_build_exit_codes(tmp_path, failure, expected):
    state = tmp_path / 'state'
    tools = tmp_path / 'bin'
    tools.mkdir()
    (state / 'source/scripts').mkdir(parents=True)
    (state / 'source/scripts/ryzen-build.sh').write_text('RYZEN_CMAKE_FLAGS=(-DTEST=ON)\n')
    binary = state / 'build/opengothic/gothic-island-program-test'
    binary.parent.mkdir(parents=True)
    binary.write_text('#!/bin/sh\nexit 9\n' if failure == 'contract' else '#!/bin/sh\nexit 0\n')
    binary.chmod(0o755)
    cmake = tools / 'cmake'
    cmake.write_text('#!/bin/sh\ncase "$1:$ISLAND_TEST_FAILURE" in\n'
                     '  -S:configure|--build:build) exit 9 ;;\nesac\nexit 0\n')
    cmake.chmod(0o755)
    # These preflight tools are not executed by the fake CMake.
    for name in ('ninja', 'gcc', 'g++', 'glslangValidator'):
        (tools / name).symlink_to(cmake)
    environment = dict(os.environ, HOME=str(tmp_path), PATH=str(tools)+':'+os.environ['PATH'],
                       ISLAND_TEST_FAILURE=failure)
    result = subprocess.run(['bash', str(Path(__file__).resolve().parents[1] / 'island-remote.sh'),
                             'build', str(state), str(state / 'run')], env=environment,
                            capture_output=True, text=True, check=False)
    assert result.returncode == expected, result.stdout + result.stderr
