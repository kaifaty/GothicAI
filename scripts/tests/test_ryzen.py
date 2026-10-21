"""Queue admission and per-worktree uploads without a game or real SSH."""
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / 'ryzen.sh'
BENCH_SCRIPT = SCRIPT.with_name('bench.sh')


def function(name):
    match = re.search(r'^' + name + r'\(\) \{\n.*?^\}', SCRIPT.read_text(), re.M | re.S)
    assert match is not None, name
    return match.group(0)


def definitions():
    return SCRIPT.read_text().split('# Internal entry point', 1)[0]


def wait_for(path):
    deadline = time.monotonic() + 5
    while not path.exists():
        if time.monotonic() >= deadline:
            raise AssertionError(f'no marker: {path}')
        time.sleep(0.02)


class KilledGameRetryTests(unittest.TestCase):
    def run_e2e(self, mode):
        root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix='ryzen-kill-retry-'))).resolve()
        source = root / 'source'
        binaries = root / 'bin'
        (source / 'scripts').mkdir(parents=True)
        (source / 'server').mkdir()
        (root / 'home/gothic-ai/game/Data').mkdir(parents=True)
        binaries.mkdir()
        shutil.copyfile(SCRIPT.with_name('e2e.sh'), source / 'scripts/e2e.sh')
        # Keep the production shell monitor, but replace its services and game.
        uv = binaries / 'uv'
        uv.write_text(f'#!{sys.executable}\n' + r'''
import os
from pathlib import Path
import time
run = Path(os.environ['LOG_DIR'])
print('fake server started', flush=True)
while not (run / 'begin-checks').exists():
    time.sleep(0.01)
print('scenario stages: checks started', flush=True)
(run / 'checks-ready').touch()
if os.environ['TEST_MODE'] == 'kill-after':
    while True:
        time.sleep(0.01)
time.sleep(0.05)
''')
        game = binaries / 'game'
        game.write_text(f'#!{sys.executable}\n' + r'''
import os
from pathlib import Path
import signal
import sys
import time
starts = Path(os.environ['TEST_ROOT']) / 'starts'
number = int(starts.read_text()) + 1 if starts.exists() else 1
starts.write_text(str(number))
print(f'fake game run {number}', flush=True)
mode = os.environ['TEST_MODE']
if mode == 'kill-always' or (mode == 'kill-once' and number == 1):
    os.kill(os.getpid(), signal.SIGKILL)
if mode == 'exit-before':
    sys.exit(3)
run = Path(os.environ['LOG_DIR'])
(run / 'begin-checks').touch()
while not (run / 'checks-ready').exists():
    time.sleep(0.01)
if mode == 'kill-after':
    os.kill(os.getpid(), signal.SIGKILL)
while True:
    time.sleep(0.01)
''')
        (binaries / 'sleep').write_text('#!/bin/sh\nexec /bin/sleep 0.02\n')
        (binaries / 'uname').write_text('#!/bin/sh\necho Linux\n')
        for executable in binaries.iterdir():
            executable.chmod(0o755)
        harness = '\n'.join([
            'set -euo pipefail', function('e2e_window_position'), function('remote_run'),
            'ROOT="$TEST_ROOT/source"',
            'toolchain() { :; }',
            'desktop() { RYZEN_SCREEN_SIZE=1920x1080; }',
            'require_prepared() { :; }',
            'export RYZEN_GAME_BIN="$TEST_ROOT/bin/game"',
            'remote_run e2e "$TEST_ROOT/state" run',
        ])
        result = subprocess.run(
            ['bash', '-c', harness], capture_output=True, text=True, timeout=20,
            env=dict(os.environ, HOME=str(root / 'home'), TEST_ROOT=str(root),
                     TEST_MODE=mode, SCENARIO='stages', GQ_PROFILE='ryzen',
                     LLM='fake', SAVE='', GAME_EXTRA_ENV='', GOTHIC_TTS_RECORD='0', GOTHIC_SOAK_CONTROL='0',
                     PATH=str(binaries) + os.pathsep + os.environ['PATH']),
        )
        return result, root / 'state/runs/run', int((root / 'starts').read_text())

    def assert_kills(self, result, count):
        times = re.findall(
            r'^Ryzen: game killed before checks at (\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z), run (\d)/4$',
            result.stdout, re.M,
        )
        self.assertEqual(len(times), count, result.stdout + result.stderr)
        self.assertEqual([int(number) for _, number in times], list(range(1, count + 1)))
        return [timestamp for timestamp, _ in times]

    def test_four_sigkills_before_checks_stop_with_76_and_preserve_every_run(self):
        result, logs, starts = self.run_e2e('kill-always')
        self.assertEqual(result.returncode, 76, result.stdout + result.stderr)
        self.assertEqual(starts, 4)
        times = self.assert_kills(result, 4)
        self.assertIn('Ryzen: game killed before checks 4 times; times: ' + ' '.join(times), result.stdout)
        for number, directory in enumerate([logs] + [logs / f'run-{n}' for n in range(2, 5)], 1):
            self.assertIn(f'fake game run {number}', (directory / 'game.log').read_text())
            self.assertIn('fake server started', (directory / 'server.log').read_text())
            self.assertNotIn('checks started', (directory / 'server.log').read_text())
            self.assertIn('signal SIGKILL (9), status 137', (directory / 'e2e.log').read_text())
            self.assertIn(f'game killed before checks at {times[number - 1]}',
                          (directory / 'e2e.log').read_text())

    def test_one_sigkill_before_checks_retries_and_passes(self):
        result, logs, starts = self.run_e2e('kill-once')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(starts, 2)
        self.assert_kills(result, 1)
        self.assertIn('fake game run 1', (logs / 'game.log').read_text())
        self.assertIn('fake game run 2', (logs / 'run-2/game.log').read_text())
        self.assertIn('scenario stages: checks started', (logs / 'run-2/server.log').read_text())
        self.assertNotIn('game killed before checks at', (logs / 'run-2/e2e.log').read_text())
        self.assertFalse((logs / 'run-3').exists())

    def test_sigkill_after_checks_keeps_failure_without_retry(self):
        result, logs, starts = self.run_e2e('kill-after')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(starts, 1)
        self.assert_kills(result, 0)
        self.assertIn('scenario stages: checks started', (logs / 'server.log').read_text())
        self.assertIn('signal SIGKILL (9), status 137', (logs / 'e2e.log').read_text())
        self.assertFalse((logs / 'run-2').exists())

    def test_other_startup_failure_does_not_retry(self):
        result, logs, starts = self.run_e2e('exit-before')
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(starts, 1)
        self.assert_kills(result, 0)
        self.assertIn('game exited: code 3 (unexpected)', (logs / 'e2e.log').read_text())
        self.assertFalse((logs / 'run-2').exists())


class QueueSplitTests(unittest.TestCase):
    def run_game(self, action='e2e', prepare=True, build_exit=0, scenario='stages', window=None):
        directory = self.enterContext(tempfile.TemporaryDirectory(prefix='ryzen-phases-test-'))
        root = Path(directory)
        (root / 'scripts').mkdir()
        queue = root / 'scripts/gq'
        queue.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import sys
root = Path(os.environ['TEST_SESSION'])
with (root / 'requests.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\n')
name = sys.argv[sys.argv.index('--name') + 1]
if name.startswith('build ') and int(os.environ['TEST_BUILD_EXIT']):
    print('compile failure: build log tail', flush=True)
    sys.exit(int(os.environ['TEST_BUILD_EXIT']))
''')
        queue.chmod(0o755)
        harness = r'''
set -euo pipefail
ROOT="$TEST_SESSION" SESSION="$TEST_SESSION" STATE="$TEST_SESSION/state"
OUTPUT="$SESSION/output" RUN=run RYZEN_HOST=fake AI_PORT=''
ACTION="$TEST_ACTION" JOBS=7 LOCK_WAIT=23 SCENARIO="$TEST_SCENARIO"
BENCH_SCENARIO=/benchmarks/smoke.bench RYZEN_MAX=9m
if [[ "$ACTION" == bench ]]; then BENCH_CLASS=quiet; else unset BENCH_CLASS; fi
TUNNEL_OPEN=0 RUN_STARTED=0 RUN_FINISHED=0 RUN_RC=0
SSH_PID=$$ ENV_ARGS=(RYZEN_JOBS=7) GQ_ARGS=()
exec 8>"$SESSION/control"
: > "$OUTPUT"
guard_ack() { :; }
preparation_needed() { [[ "$TEST_PREPARE" == 1 ]]; }
'''
        harness += '\n'.join([function('run_quick'), function('e2e_display_exclusive'),
                              function('game_queue_args'), function('run_game')])
        harness += '\nrun_game env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote "$ACTION" "$STATE" "$RUN" "$SCENARIO"\n'
        result = subprocess.run(
            ['bash', '-c', harness], capture_output=True, text=True, timeout=10,
            env=dict(os.environ, TEST_SESSION=str(root), TEST_ACTION=action,
                     TEST_PREPARE=str(int(prepare)), TEST_BUILD_EXIT=str(build_exit),
                     TEST_SCENARIO=scenario, **(window or {})),
        )
        requests = root / 'requests.jsonl'
        return result, [json.loads(line) for line in requests.read_text().splitlines()] if requests.exists() else []

    @staticmethod
    def queue_options(request):
        return request[:request.index('--')]

    def assert_option(self, options, name, expected):
        self.assertIn(name, options)
        self.assertEqual(options[options.index(name) + 1], expected)

    def assert_game_request(self, request, action):
        options = self.queue_options(request)
        for name, value in [('--class', 'quiet' if action == 'bench' else 'quick'),
                            ('--threads', '4'), ('--mem', '5G'), ('--max', '9m'),
                            ('--gpu-mem', '4G' if action == 'bench' else '2G'),
                            ('--wait-max', '23'),
                            ('--name', 'bench smoke' if action == 'bench' else 'e2e stages')]:
            self.assert_option(options, name, value)
        self.assertIn('--display', options)
        self.assertEqual('--display-shared' in options, action == 'e2e')
        self.assertIn('RYZEN_RUN_ID=run-game', request[request.index('--') + 1:])

    def test_preparation_and_game_have_separate_resources_and_names(self):
        for action in ('e2e', 'bench'):
            with self.subTest(action=action):
                result, requests = self.run_game(action)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(requests), 2, requests)
                build = self.queue_options(requests[0])
                for name, value in [('--class', 'quick'), ('--threads', '7'), ('--mem', '12G'),
                                    ('--max', '9m'),
                                    ('--name', 'build bench smoke' if action == 'bench' else 'build e2e stages')]:
                    self.assert_option(build, name, value)
                for option in ('--display', '--gpu-mem', '--wait-max'):
                    self.assertNotIn(option, build)
                command = requests[0][requests[0].index('--') + 1:]
                self.assertIn('RYZEN_RUN_ID=run-build', command)
                self.assertEqual(command[command.index('__remote') + 1:][:2], ['prepare', action])
                self.assert_game_request(requests[1], action)

    def test_ready_snapshot_submits_only_game(self):
        for action in ('e2e', 'bench'):
            with self.subTest(action=action):
                result, requests = self.run_game(action, prepare=False)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(requests), 1, requests)
                self.assert_game_request(requests[0], action)

    def test_build_failure_returns_its_status_without_game_submission(self):
        result, requests = self.run_game(build_exit=42)
        self.assertEqual(result.returncode, 42, result.stdout + result.stderr)
        self.assertEqual(len(requests), 1, requests)
        self.assert_option(self.queue_options(requests[0]), '--name', 'build e2e stages')
        self.assertIn('compile failure: build log tail', result.stdout)

    def test_exclusive_scenario_only_changes_the_game_request(self):
        for scenario, exclusive in (('fight_reactions_bandits', False),
                                    ('calm', False), ('orders', False), ('stages', False),
                                    ('fight_reactions_wolf', True), ('calm_companion', True),
                                    ('neural-companion', True), ('wolf', True),
                                    ('rescue_self', True), ('companion', True),
                                    ('crimes', False), ('quest_law', False)):
            with self.subTest(scenario=scenario):
                result, requests = self.run_game(scenario=scenario)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(requests), 2, requests)
                build = self.queue_options(requests[0])
                self.assertNotIn('--display', build)
                self.assertNotIn('--display-exclusive', build)
                game = self.queue_options(requests[1])
                self.assertIn('--display', game)
                self.assertEqual('--display-exclusive' in game, exclusive)
                self.assertEqual('--display-shared' in game, not exclusive)
                self.assert_option(game, '--name', f'e2e {scenario}')

    def test_forced_exclusive_overrides_shared_scenario(self):
        result, requests = self.run_game(scenario='stages', window={'RYZEN_DISPLAY_EXCLUSIVE': '1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        options = self.queue_options(requests[-1])
        self.assertIn('--display-exclusive', options)
        self.assertNotIn('--display-shared', options)

    def test_forced_shared_overrides_listed_scenario(self):
        result, requests = self.run_game(scenario='fight_reactions_wolf',
                                         window={'RYZEN_DISPLAY_EXCLUSIVE': '0'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        options = self.queue_options(requests[-1])
        self.assertIn('--display-shared', options)
        self.assertNotIn('--display-exclusive', options)

    def test_bench_does_not_use_e2e_exclusive_scenarios(self):
        result, requests = self.run_game(action='bench', scenario='calm')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('--display-exclusive', self.queue_options(requests[-1]))
        self.assertNotIn('--display-shared', self.queue_options(requests[-1]))

    def test_custom_window_does_not_overlap_shared_slot_layout(self):
        for window, shared in (({'TEMPEST_WINDOW_SIZE': '640x360'}, True),
                               ({'TEMPEST_WINDOW_SIZE': '320x180'}, False),
                               ({'TEMPEST_WINDOW_SIZE': '1280x720'}, False),
                               ({'TEMPEST_WINDOW_POS': '20,40'}, False),
                               ({'TEMPEST_WINDOW_POS': ''}, False)):
            with self.subTest(window=window):
                result, requests = self.run_game(scenario='stages', window=window)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                options = self.queue_options(requests[-1])
                self.assertEqual('--display-shared' in options, shared)
                self.assertEqual('--display-exclusive' in options, not shared)

    def test_tts_preparation_installs_missing_dependencies(self):
        for scenario in ('errand', 'errand_fork', 'errand_far', 'errand_give',
                         'fight_reactions_wolf', 'fight_reactions_sight',
                         'fight_reactions_bandits', 'fight_reactions_flee'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory(prefix='ryzen-deps-test-') as directory:
                root = Path(directory)
                source = root / 'source'
                state = root / 'state'
                (source / 'server/.venv').mkdir(parents=True)
                (source / 'server/.venv/ready').touch()
                (source / 'tts-service').mkdir()
                (state / 'inputs/run').mkdir(parents=True)
                (root / 'bin').mkdir()
                uv = root / 'bin/uv'
                uv.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import sys
args = sys.argv[1:]
with Path(os.environ['TEST_UV_CALLS']).open('a') as stream:
    stream.write(json.dumps({'project': Path.cwd().name, 'args': args}) + '\n')
marker = Path('.venv/ready')
if args == ['sync', '--check', '--frozen']:
    sys.exit(0 if marker.exists() else 1)
if args == ['sync', '--frozen']:
    marker.parent.mkdir(exist_ok=True)
    marker.touch()
    sys.exit(0)
sys.exit('unexpected uv arguments: ' + repr(args))
''')
                uv.chmod(0o755)
                harness = r'''
set -euo pipefail
ROOT="$TEST_ROOT/source"
toolchain() { :; }
source_digest() { echo snapshot-digest; }
storygraph_digest() { echo storygraph-digest; }
check_protocol() { :; }
engine_ready() { export RYZEN_GAME_BIN="$HOME/gothic-ai/engine-cache/$2/Gothic2Notr"; }
storygraph_ready() { export RYZEN_STORYGRAPH_BIN="$HOME/gothic-ai/engine-cache/storygraph/$2/storygraph"; }
build_engine() { :; }
build_storygraph() { :; }
'''
                harness += '\n'.join(function(name) for name in (
                    'dependency_projects', 'remote_probe', 'remote_prepare'))
                harness += r'''
if remote_probe e2e "$TEST_ROOT/state" run "$TEST_SCENARIO"; then
  echo 'initial probe unexpectedly ready' >&2
  exit 90
fi
remote_prepare e2e "$TEST_ROOT/state" run "$TEST_SCENARIO"
remote_probe e2e "$TEST_ROOT/state" run "$TEST_SCENARIO"
'''
                calls = root / 'uv-calls.jsonl'
                result = subprocess.run(
                    ['bash', '-c', harness], capture_output=True, text=True, timeout=5,
                    env=dict(os.environ, TEST_ROOT=directory, TEST_SCENARIO=scenario,
                             TEST_UV_CALLS=str(calls), PATH=str(root / 'bin') + os.pathsep + os.environ['PATH']),
                )
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((source / 'tts-service/.venv/ready').exists())
                self.assertEqual([json.loads(line) for line in calls.read_text().splitlines()], [
                    {'project': project, 'args': args}
                    for args in (['sync', '--check', '--frozen'], ['sync', '--frozen'],
                                 ['sync', '--check', '--frozen'])
                    for project in ('server', 'tts-service')
                ])

    def test_game_rejects_stale_engine_without_building(self):
        with tempfile.TemporaryDirectory(prefix='ryzen-stale-test-') as directory:
            state = Path(directory)
            (state / 'inputs/run').mkdir(parents=True)
            (state / 'inputs/run/source.sha256').write_text('a' * 64 + '\n')
            (state / 'build/opengothic').mkdir(parents=True)
            binary = state / 'build/opengothic/Gothic2Notr'
            binary.touch()
            binary.chmod(0o755)
            (state / 'build/source.sha256').write_text('c' * 64 + '\n')
            (state / 'build/CMakeCache.txt').write_text('OPENGOTHIC_ENABLE_APP_CADENCE:BOOL=ON\n')
            harness = '\n'.join([
                definitions(),
                'ROOT="$TEST_STATE/source" HOME="$TEST_STATE/home"',
                'build_engine() { touch "$TEST_STATE/build-was-called"; }',
                'source_digest() { echo "unexpected source scan" >&2; exit 99; }',
                'require_prepared bench "$TEST_STATE" run stages',
            ])
            result = subprocess.run(['bash', '-c', harness], capture_output=True, text=True, timeout=5,
                                    env=dict(os.environ, TEST_STATE=directory))
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('source.sha256', result.stdout + result.stderr)
            self.assertIn('refusing to build', (result.stdout + result.stderr).lower())
            self.assertFalse((state / 'build-was-called').exists())


class AutoCleanTests(unittest.TestCase):
    gib = 1024 ** 3

    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix='ryzen-autoclean-test-'))).resolve()
        self.home = self.root / 'home'
        self.worktrees = self.home / 'gothic-ai/e2e'
        self.worktrees.mkdir(parents=True)
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        self.free_space = self.root / 'free-bytes'
        self.free_space.write_text(str(100 * self.gib))
        self.calls = self.root / 'uv-calls.jsonl'
        self.env = dict(os.environ, HOME=str(self.home), CLEAN_IDLE_HOURS='48',
                        WORKTREE_LOCK=str(self.worktrees / 'own.lock'),
                        TEST_FREE_SPACE=str(self.free_space), TEST_UV_CALLS=str(self.calls),
                        PATH=str(self.bin) + os.pathsep + os.environ['PATH'])
        # Run the real heredoc with an isolated disk size, without depending on
        # the machine's free space or installing Linux flock on macOS.
        python = self.bin / 'python3'
        python.write_text(f'#!{sys.executable}\n' + r'''
import collections
import os
from pathlib import Path
import shutil
import sys
usage = collections.namedtuple('usage', 'total used free')
def disk_usage(path):
    free = int(Path(os.environ['TEST_FREE_SPACE']).read_text())
    return usage(200 * 1024 ** 3, 200 * 1024 ** 3 - free, free)
shutil.disk_usage = disk_usage
sys.argv = sys.argv[1:]
assert sys.argv[0] == '-', sys.argv
exec(compile(sys.stdin.read(), '<auto-clean-test>', 'exec'), {'__name__': '__main__'})
''')
        python.chmod(0o755)
        flock = self.bin / 'flock'
        flock.write_text(f'#!{sys.executable}\n' + r'''
import fcntl
import os
from pathlib import Path
import sys
import time
if sys.argv[1] == '-n':
    try:
        fcntl.flock(int(sys.argv[2]), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        sys.exit(1)
else:
    assert sys.argv[1] == '-w', sys.argv
    # Delay the legacy wrapper after exec 9>lock but before its flock call.
    Path(os.environ['TEST_FLOCK_OPENED']).touch()
    while not Path(os.environ['TEST_FLOCK_CONTINUE']).exists():
        time.sleep(0.02)
    deadline = time.monotonic() + float(sys.argv[2])
    while True:
        try:
            fcntl.flock(int(sys.argv[3]), fcntl.LOCK_EX | fcntl.LOCK_NB)
            break
        except BlockingIOError:
            Path(os.environ['TEST_FLOCK_WAITING']).touch()
            if time.monotonic() >= deadline:
                sys.exit(1)
            time.sleep(0.02)
''')
        flock.chmod(0o755)
        uv = self.home / '.local/bin/uv'
        uv.parent.mkdir(parents=True)
        uv.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import sys
import time
with Path(os.environ['TEST_UV_CALLS']).open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\n')
if os.environ.get('TEST_UV_STARTED'):
    Path(os.environ['TEST_UV_STARTED']).touch()
time.sleep(float(os.environ.get('TEST_UV_DELAY', '0')))
print('fake uv progress')
print('fake uv diagnostics', file=sys.stderr)
if int(os.environ.get('TEST_UV_EXIT', '0')):
    sys.exit(int(os.environ['TEST_UV_EXIT']))
space = Path(os.environ['TEST_FREE_SPACE'])
space.write_text(str(int(space.read_text()) + 2 * 1024 ** 3))
''')
        uv.chmod(0o755)

    def shell(self, *, popen=False):
        harness = 'set -euo pipefail\n' + function('auto_clean') + '\nauto_clean\n'
        if popen:
            return subprocess.Popen(['bash', '-c', harness], env=self.env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        result = subprocess.run(['bash', '-c', harness], env=self.env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    @staticmethod
    def age(path, hours):
        timestamp = time.time() - hours * 3600
        os.utime(path, (timestamp, timestamp))

    def worktree(self, name, *, hours=49, marker=True):
        state = self.worktrees / name
        (state / 'source').mkdir(parents=True)
        (state / 'source/code.py').write_text('source')
        if marker:
            (state / '.last-used').touch()
        for path in [*state.rglob('*'), state]:
            self.age(path, hours)
        (self.worktrees / (name + '.lock')).touch()
        return state

    def uv_calls(self):
        return [json.loads(line) for line in self.calls.read_text().splitlines()] if self.calls.exists() else []

    def test_removes_idle_unlocked_worktree_but_preserves_lock_inode(self):
        stale = self.worktree('stale')
        stale_lock = self.worktrees / 'stale.lock'
        lock_inode = stale_lock.stat().st_ino
        fresh = self.worktree('fresh', hours=1)
        own = self.worktree('own')
        locked = self.worktree('locked')
        with (self.worktrees / 'locked.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            result = self.shell()
        self.assertFalse(stale.exists())
        self.assertEqual(stale_lock.stat().st_ino, lock_inode)
        for state in (fresh, own, locked):
            self.assertTrue(state.exists(), state)
            self.assertTrue(state.with_suffix('.lock').exists(), state)
        self.assertLess(time.time() - (own / '.last-used').stat().st_mtime, 10)
        self.assertEqual(len(result.stdout.splitlines()), 1, result.stdout)
        self.assertIn('stale', result.stdout)
        self.assertEqual(self.uv_calls(), [])

    def test_cleanup_preserves_lock_for_legacy_waiter_that_already_opened_it(self):
        stale = self.worktree('legacy')
        lock_path = self.worktrees / 'legacy.lock'
        inode = lock_path.stat().st_ino
        opened = self.root / 'legacy-opened'
        proceed = self.root / 'legacy-continue'
        waiting = self.root / 'legacy-waiting'
        acquired = self.root / 'legacy-acquired'
        finish = self.root / 'legacy-finish'
        # Keep the old take_lock implementation here: older worktrees have no
        # inode check and must still serialize with every subsequent wrapper.
        harness = r'''
set -euo pipefail
LOCK_WAIT=5
take_lock() {
  mkdir -p "$(dirname "$WORKTREE_LOCK")"
  exec 9>"$WORKTREE_LOCK"
  local started=$SECONDS monitor waiter status
  echo "Ryzen: waiting for worktree lock $WORKTREE_LOCK (up to ${LOCK_WAIT}s)"
  (IFS= read -r unused || kill -TERM "$$") <&0 &
  monitor=$!
  flock -w "$LOCK_WAIT" 9 &
  waiter=$!
  trap 'kill "$monitor" "$waiter" 2>/dev/null || true; exit 143' HUP INT TERM
  wait "$waiter" && status=0 || status=$?
  kill "$monitor" 2>/dev/null || true
  wait "$monitor" 2>/dev/null || true
  trap - HUP INT TERM
  [[ "$status" == 0 ]] || { echo 'Ryzen занят' >&2; exit 75; }
  echo "Ryzen: lock acquired in $((SECONDS - started))s"
}
take_lock
touch "$TEST_ACQUIRED"
while [[ ! -e "$TEST_FINISH" ]]; do sleep 0.02; done
'''
        legacy = subprocess.Popen(
            ['bash', '-c', harness], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, start_new_session=True,
            env=dict(self.env, WORKTREE_LOCK=str(lock_path),
                     TEST_FLOCK_OPENED=str(opened), TEST_FLOCK_CONTINUE=str(proceed),
                     TEST_FLOCK_WAITING=str(waiting), TEST_ACQUIRED=str(acquired),
                     TEST_FINISH=str(finish)),
        )
        try:
            wait_for(opened)
            self.shell()
            self.assertFalse(stale.exists())
            self.assertEqual(lock_path.stat().st_ino, inode)
            # The next wrapper can recreate this directory, but its lock must
            # conflict with the fd that the legacy wrapper opened before GC.
            stale.mkdir()
            with lock_path.open('a') as contender:
                fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
                proceed.touch()
                wait_for(waiting)
                self.assertFalse(acquired.exists())
                fcntl.flock(contender, fcntl.LOCK_UN)
                wait_for(acquired)
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(contender, fcntl.LOCK_EX | fcntl.LOCK_NB)
            finish.touch()
            stdout, stderr = legacy.communicate(timeout=5)
            self.assertEqual(legacy.returncode, 0, stdout + stderr)
            self.assertIn('lock acquired', stdout)
        finally:
            if legacy.poll() is None:
                os.killpg(legacy.pid, signal.SIGKILL)
                legacy.communicate(timeout=5)

    def test_missing_marker_uses_newest_mtime_through_two_levels(self):
        old = self.worktree('old-without-marker', marker=False)
        fresh = self.worktree('fresh-file', marker=False)
        self.age(fresh / 'source/code.py', 1)
        fresh_directory = self.worktree('fresh-directory', marker=False)
        self.age(fresh_directory / 'source', 1)
        self.shell()
        self.assertFalse(old.exists())
        self.assertTrue(fresh.exists())
        self.assertTrue(fresh_directory.exists())

    def test_missing_marker_ignores_files_below_two_levels(self):
        old = self.worktree('deep-file', marker=False)
        deeper = old / 'source/nested'
        deeper.mkdir()
        (deeper / 'fresh.py').write_text('fresh, but below the fallback depth')
        self.age(deeper, 49)
        self.age(deeper.parent, 49)
        self.shell()
        self.assertFalse(old.exists())

    def test_marker_takes_precedence_over_newer_source_files(self):
        old = self.worktree('old-marker')
        self.age(old / 'source/code.py', 1)
        self.shell()
        self.assertFalse(old.exists())

    def test_configured_idle_hours_changes_cutoff(self):
        state = self.worktree('two-days-old')
        self.env['CLEAN_IDLE_HOURS'] = '72'
        self.shell()
        self.assertTrue(state.exists())
        self.env['CLEAN_IDLE_HOURS'] = '24'
        self.shell()
        self.assertFalse(state.exists())

    def test_zero_idle_hours_disables_worktree_cleanup_but_allows_prune(self):
        old = self.worktree('disabled')
        self.env['CLEAN_IDLE_HOURS'] = '0'
        self.free_space.write_text(str(59 * self.gib))
        self.shell()
        self.assertTrue(old.exists())
        self.assertTrue((self.worktrees / 'disabled.lock').exists())
        self.assertEqual(self.uv_calls(), [['cache', 'prune']])

    def test_symlinks_and_other_task_directories_are_preserved(self):
        sentinels = []
        for name in ('engine-cache', 'body-training', 'nav-probe'):
            directory = self.home / 'gothic-ai' / name
            directory.mkdir()
            sentinel = directory / 'keep'
            sentinel.write_text(name)
            self.age(sentinel, 100)
            self.age(directory, 100)
            sentinels.append(sentinel)
        outside = self.root / 'outside'
        outside.mkdir()
        (outside / 'keep').write_text('outside')
        self.age(outside, 100)
        symlink = self.worktrees / 'outside-link'
        symlink.symlink_to(outside, target_is_directory=True)
        self.shell()
        self.assertTrue(symlink.is_symlink())
        self.assertEqual((outside / 'keep').read_text(), 'outside')
        for sentinel in sentinels:
            self.assertEqual(sentinel.read_text(), sentinel.parent.name)

    def test_prune_only_below_sixty_gib_and_reports_one_line(self):
        for free in (61 * self.gib, 60 * self.gib):
            self.free_space.write_text(str(free))
            self.shell()
            self.assertEqual(self.uv_calls(), [])
            self.assertFalse((self.worktrees / '.uv-prune').exists())
        self.free_space.write_text(str(60 * self.gib - 1))
        result = self.shell()
        self.assertEqual(self.uv_calls(), [['cache', 'prune']])
        self.assertTrue((self.worktrees / '.uv-prune').is_file())
        self.assertEqual(len(result.stdout.splitlines()), 2, result.stdout)
        self.assertNotIn('fake uv', result.stdout + result.stderr)

    def test_prune_waits_twelve_hours_after_success(self):
        marker = self.worktrees / '.uv-prune'
        marker.touch()
        self.free_space.write_text(str(10 * self.gib))
        self.age(marker, 11.9)
        self.shell()
        self.assertEqual(self.uv_calls(), [])
        self.age(marker, 12.1)
        self.shell()
        self.assertEqual(self.uv_calls(), [['cache', 'prune']])
        self.shell()
        self.assertEqual(self.uv_calls(), [['cache', 'prune']])

    def test_failed_prune_also_waits_twelve_hours_before_retry(self):
        self.free_space.write_text(str(10 * self.gib))
        self.env['TEST_UV_EXIT'] = '2'
        result = self.shell()
        self.assertIn('failed', result.stdout)
        marker = self.worktrees / '.uv-prune'
        self.assertTrue(marker.exists())
        self.env['TEST_UV_EXIT'] = '0'
        self.shell()
        self.assertEqual(self.uv_calls(), [['cache', 'prune']])
        self.age(marker, 12.1)
        self.shell()
        self.assertEqual(self.uv_calls(), [['cache', 'prune'], ['cache', 'prune']])
        self.assertTrue(marker.exists())

    def test_simultaneous_low_disk_checks_prune_once(self):
        self.free_space.write_text(str(10 * self.gib))
        started = self.root / 'uv-started'
        self.env.update(TEST_UV_STARTED=str(started), TEST_UV_DELAY='0.5')
        first = self.shell(popen=True)
        try:
            wait_for(started)
            self.shell()
            stdout, stderr = first.communicate(timeout=10)
            self.assertEqual(first.returncode, 0, stdout + stderr)
        finally:
            if first.poll() is None:
                first.kill()
                first.communicate(timeout=5)
        self.assertEqual(self.uv_calls(), [['cache', 'prune']])


class CacheTests(unittest.TestCase):
    engine_digest = 'a' * 64
    story_digest = 'b' * 64

    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix='ryzen-cache-test-'))).resolve()
        self.source = self.root / 'source'
        self.home = self.root / 'home'
        self.state = self.root / 'worktree'
        self.cache = self.home / 'gothic-ai/engine-cache'
        self.bin = self.root / 'bin'
        self.bin.mkdir()
        (self.source / 'scripts').mkdir(parents=True)
        (self.source / 'server').mkdir()
        (self.source / 'OpenGothic/ai_bridge/storygraph').mkdir(parents=True)
        (self.home / 'gothic-ai/game/Data').mkdir(parents=True)
        (self.state / 'inputs/run').mkdir(parents=True)
        (self.source / 'scripts/e2e.sh').write_text('"$GAME_BIN" --test\n')
        self.env = dict(os.environ, HOME=str(self.home), TEST_ROOT=str(self.root),
                        TEST_GAME_PATH=str(self.root / 'game-path'),
                        TEST_CMAKE_CALLS=str(self.root / 'cmake-calls.jsonl'),
                        VULKAN_SDK='/unused-test-sdk', SCENARIO='stages',
                        PATH=str(self.bin) + os.pathsep + os.environ['PATH'])
        cmake = self.bin / 'cmake'
        cmake.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import sys
args = sys.argv[1:]
with Path(os.environ['TEST_CMAKE_CALLS']).open('a') as stream:
    stream.write(json.dumps(args) + '\n')
if '--build' in args:
    build = Path(args[args.index('--build') + 1])
    target = args[args.index('--target') + 1]
    output = build / ('opengothic/Gothic2Notr' if target == 'Gothic2Notr' else 'storygraph')
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text('#!/usr/bin/env python3\nimport os, sys\nfrom pathlib import Path\n'
                      'Path(os.environ["TEST_GAME_PATH"]).write_text(str(Path(sys.argv[0]).resolve()))\n'
                      'assert Path(sys.argv[0]).with_name("runtime.dat").read_text() == "runtime"\n')
    output.chmod(0o755)
    output.with_name('runtime.dat').write_text('runtime')
else:
    build = Path(args[args.index('-B') + 1])
    build.mkdir(parents=True, exist_ok=True)
    (build / 'CMakeCache.txt').write_text('OPENGOTHIC_ENABLE_APP_CADENCE:BOOL=ON\n')
print('fake cmake: OK')
''')
        cmake.chmod(0o755)

    def shell(self, commands, *, popen=False):
        harness = definitions() + r'''
ROOT="$TEST_ROOT/source"
toolchain() { :; }
desktop() { RYZEN_SCREEN_SIZE="${TEST_SCREEN_SIZE:-1920x1080}"; }
check_protocol() { :; }
uv() { :; }
source_digest() { printf '%s\n' "'''+self.engine_digest+r'''"; }
storygraph_digest() { printf '%s\n' "'''+self.story_digest+r'''"; }
'''+commands
        if popen:
            return subprocess.Popen(['bash', '-c', harness], env=self.env,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        result = subprocess.run(['bash', '-c', harness], env=self.env,
                                capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result

    def entry(self, digest, kind='engine', *, age=0, marker='runtime'):
        entry = self.cache / ('' if kind == 'engine' else 'storygraph') / digest
        entry.mkdir(parents=True)
        binary = entry / ('Gothic2Notr' if kind == 'engine' else 'storygraph')
        binary.write_text(f'#!{sys.executable}\n' + r'''
import os
from pathlib import Path
import sys
Path(os.environ['TEST_GAME_PATH']).write_text(str(Path(sys.argv[0]).resolve()))
assert Path(sys.argv[0]).with_name('runtime.dat').read_text() == 'runtime'
''')
        binary.chmod(0o755)
        (entry / 'runtime.dat').write_text(marker)
        timestamp = time.time() - age
        os.utime(entry, (timestamp, timestamp))
        return entry

    def test_cache_hit_runs_cached_game_without_a_worktree_build(self):
        cached = self.entry(self.engine_digest)
        self.entry(self.story_digest, 'storygraph')
        result = self.shell(r'''
remote_probe e2e "$TEST_ROOT/worktree" run stages
remote_run e2e "$TEST_ROOT/worktree" run
''')
        self.assertIn(f'engine from cache {self.engine_digest[:12]} (0s)', result.stdout)
        self.assertEqual((self.root / 'game-path').read_text(), str(cached / 'Gothic2Notr'))
        self.assertFalse((self.state / 'build').exists())
        self.assertFalse((self.state / 'storygraph-build').exists())
        self.assertFalse((self.root / 'cmake-calls.jsonl').exists())
        self.assertEqual((self.source / 'OpenGothic/ai_bridge/storygraph/build/storygraph').resolve(),
                         self.cache / 'storygraph' / self.story_digest / 'storygraph')

    def test_e2e_ports_size_and_position_follow_the_assigned_slot(self):
        self.entry(self.engine_digest)
        self.entry(self.story_digest, 'storygraph')
        names = ('PORT', 'ASR_PORT', 'TTS_PORT', 'TEMPEST_WINDOW_SIZE', 'TEMPEST_WINDOW_POS')
        for name in names:
            self.env.pop(name, None)
        (self.source / 'scripts/e2e.sh').write_text('''
python3 - "$TEST_ROOT/e2e-env.json" <<'PY'
import json
import os
import sys
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({key: os.environ[key] for key in
    ('PORT', 'ASR_PORT', 'TTS_PORT', 'TEMPEST_WINDOW_SIZE', 'TEMPEST_WINDOW_POS')}))
PY
''')
        cases = [(slot, {}) for slot in range(3)] + [
            (1, {'PORT': '51000'}),
            (0, {'TEMPEST_WINDOW_SIZE': '1280x720'}),
            (2, dict(zip(names, ('51001', '51002', '51003', '1280x720', '17,29')))),
        ]
        for slot, explicit in cases:
            with self.subTest(slot=slot, explicit=explicit):
                for name in names:
                    self.env.pop(name, None)
                self.env.update(explicit, GQ_DISPLAY_SLOT=str(slot))
                self.shell(r'''
remote_probe e2e "$TEST_ROOT/worktree" run stages
remote_run e2e "$TEST_ROOT/worktree" run
''')
                expected = dict(zip(names, (str(47827 + slot * 100),
                                           str(47829 + slot * 100),
                                           str(47840 + slot * 100), '640x360',
                                           ('100,100', '800,100', '100,560')[slot])))
                if explicit.get('TEMPEST_WINDOW_SIZE', '640x360') != '640x360':
                    expected['TEMPEST_WINDOW_POS'] = '0,0'
                expected.update(explicit)
                self.assertEqual(json.loads((self.root / 'e2e-env.json').read_text()), expected)

    def test_bench_keeps_its_window_size_and_ports(self):
        self.entry(self.engine_digest)
        self.entry(self.story_digest, 'storygraph')
        for name in ('PORT', 'ASR_PORT', 'TTS_PORT'):
            self.env.pop(name, None)
        self.env.update(GQ_DISPLAY_SLOT='2', TEMPEST_WINDOW_SIZE='960x540')
        (self.bin / 'ffmpeg').write_text('#!/bin/sh\nexit 0\n')
        (self.bin / 'ffmpeg').chmod(0o755)
        (self.source / 'scripts/bench.sh').write_text('''
python3 - "$TEST_ROOT/bench-env.json" <<'PY'
import json
import os
import sys
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({key: os.environ.get(key) for key in
    ('PORT', 'ASR_PORT', 'TTS_PORT', 'TEMPEST_WINDOW_SIZE')}))
PY
''')
        self.shell(r'''
remote_probe bench "$TEST_ROOT/worktree" run smoke
remote_bench "$TEST_ROOT/worktree" run smoke
''')
        self.assertEqual(json.loads((self.root / 'bench-env.json').read_text()),
                         {'PORT': None, 'ASR_PORT': None, 'TTS_PORT': None,
                          'TEMPEST_WINDOW_SIZE': '960x540'})

    def test_e2e_retains_rebuilt_graph_in_worktree_cache(self):
        self.entry(self.engine_digest)
        self.entry(self.story_digest, 'storygraph')
        (self.state / 'data').mkdir()
        graph = self.state / 'data/story.json'
        (self.source / 'scripts/e2e.sh').write_text('''
printf '%s' "$GOTHIC_AI_DATA" > "$TEST_ROOT/data-path"
if [[ ! -f "$GOTHIC_AI_DATA/story.json" ]]; then
  printf 'rebuilt graph' > "$GOTHIC_AI_DATA/story.tmp"
  mv "$GOTHIC_AI_DATA/story.tmp" "$GOTHIC_AI_DATA/story.json"
fi
''')
        self.shell(r'''
remote_probe e2e "$TEST_ROOT/worktree" run stages
remote_run e2e "$TEST_ROOT/worktree" run
''')
        self.assertEqual((self.root / 'data-path').read_text(), str(self.state / 'data'))
        self.assertEqual(graph.read_text(), 'rebuilt graph')
        graph_time = time.time() - 60
        os.utime(graph, (graph_time, graph_time))
        self.shell(r'''
remote_probe e2e "$TEST_ROOT/worktree" run stages
remote_run e2e "$TEST_ROOT/worktree" run
''')
        self.assertEqual(graph.read_text(), 'rebuilt graph')
        self.assertAlmostEqual(graph.stat().st_mtime, graph_time, places=5)
        self.assertFalse((self.state / 'runs/run/data').exists())

    def test_miss_builds_and_publishes_engine_and_storygraph(self):
        self.shell(r'''
build_engine "$TEST_ROOT/worktree" "'''+self.engine_digest+r'''"
build_storygraph "$TEST_ROOT/worktree" "'''+self.story_digest+r'''"
''')
        calls = [json.loads(line) for line in (self.root / 'cmake-calls.jsonl').read_text().splitlines()]
        self.assertEqual(len(calls), 4)
        self.assertEqual([call[call.index('--target') + 1] for call in calls if '--build' in call],
                         ['Gothic2Notr', 'storygraph'])
        for entry, name in [(self.cache / self.engine_digest, 'Gothic2Notr'),
                            (self.cache / 'storygraph' / self.story_digest, 'storygraph')]:
            self.assertTrue(os.access(entry / name, os.X_OK))
        self.assertEqual((self.cache / self.engine_digest / 'runtime.dat').read_text(), 'runtime')
        self.assertTrue((self.state / 'build/opengothic/Gothic2Notr').exists())
        self.assertTrue((self.state / 'storygraph-build/storygraph').exists())

    def test_evicted_cache_republishes_ready_local_artifacts_without_rebuilding(self):
        self.shell(r'''
build_engine "$TEST_ROOT/worktree" "'''+self.engine_digest+r'''"
build_storygraph "$TEST_ROOT/worktree" "'''+self.story_digest+r'''"
''')
        engine = self.cache / self.engine_digest
        storygraph = self.cache / 'storygraph' / self.story_digest
        calls = self.root / 'cmake-calls.jsonl'
        game_path = self.root / 'game-path'
        for evicted in ('engine', 'storygraph', 'both'):
            with self.subTest(evicted=evicted):
                for entry in ([engine] if evicted == 'engine' else
                              [storygraph] if evicted == 'storygraph' else
                              [engine, storygraph]):
                    shutil.rmtree(entry)
                calls.unlink(missing_ok=True)
                game_path.unlink(missing_ok=True)
                result = self.shell(r'''
if remote_probe e2e "$TEST_ROOT/worktree" run stages; then
  echo 'local-only artifacts unexpectedly skipped cache publication' >&2
  exit 90
fi
remote_prepare e2e "$TEST_ROOT/worktree" run stages
remote_probe e2e "$TEST_ROOT/worktree" run stages
remote_run e2e "$TEST_ROOT/worktree" run
''')
                self.assertIn(f'engine from cache {self.engine_digest[:12]} (0s)', result.stdout)
                self.assertEqual(game_path.read_text(), str(engine / 'Gothic2Notr'))
                self.assertEqual((engine / 'runtime.dat').read_text(), 'runtime')
                self.assertTrue(os.access(storygraph / 'storygraph', os.X_OK))
                self.assertEqual((self.source / 'OpenGothic/ai_bridge/storygraph/build/storygraph').resolve(),
                                 storygraph / 'storygraph')
                self.assertFalse(calls.exists(), 'publishing ready local artifacts must not invoke cmake')

    def test_simultaneous_publications_leave_one_complete_entry(self):
        for name in ('one', 'two'):
            source = self.root / name
            source.mkdir()
            binary = source / 'Gothic2Notr'
            binary.write_bytes((name.encode() + b'\n') * 200000)
            binary.chmod(0o755)
            (source / 'runtime.dat').write_text(name)
        processes = []
        try:
            for name in ('one', 'two') * 3:
                processes.append(self.shell(
                    f'cache_artifact publish engine {self.engine_digest} "$TEST_ROOT/{name}"\n',
                    popen=True))
            for process in processes:
                stdout, stderr = process.communicate(timeout=10)
                self.assertEqual(process.returncode, 0, stdout + stderr)
        finally:
            for process in processes:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
        entry = self.cache / self.engine_digest
        name = (entry / 'runtime.dat').read_text()
        self.assertIn(name, ('one', 'two'))
        self.assertEqual((entry / 'Gothic2Notr').read_bytes(), (name.encode() + b'\n') * 200000)
        self.assertEqual([path.name for path in self.cache.iterdir() if path.is_dir()], [self.engine_digest])

    def test_eviction_uses_last_access_and_preserves_recent_entries(self):
        entries = [self.entry(f'{index:064x}', age=(20 - index) * 3600) for index in range(10)]
        fresh = [self.entry(f'{index:064x}', age=60) for index in range(10, 12)]
        oldest = entries[0]
        self.shell(f'cache_artifact get engine {oldest.name}\ncache_artifact prune engine unused\n')
        retained = [path for path in self.cache.iterdir() if path.is_dir()]
        self.assertEqual(len(retained), 8)
        self.assertTrue(oldest.exists())
        self.assertGreater(oldest.stat().st_mtime, time.time() - 10)
        self.assertTrue(all(path.exists() for path in fresh + entries[5:]))
        self.assertTrue(all(not path.exists() for path in entries[1:5]))

    def test_more_than_eight_recent_entries_are_protected(self):
        entries = [self.entry(f'{index:064x}', age=3600) for index in range(10)]
        self.shell('cache_artifact prune engine unused\n')
        self.assertTrue(all(path.exists() for path in entries))

    def test_clean_cache_removes_old_entries_and_protects_recent_ones(self):
        old_engine = self.entry('c' * 64, age=3 * 3600)
        old_storygraph = self.entry('d' * 64, 'storygraph', age=3 * 3600)
        recent_engine = self.entry('e' * 64, age=3600)
        recent_storygraph = self.entry('f' * 64, 'storygraph', age=3600)
        with self.cache.with_suffix('.lock').open('a') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            process = self.shell('cache_artifact clean engine unused\n', popen=True)
            try:
                time.sleep(0.15)
                self.assertIsNone(process.poll(), 'cache cleanup ignored the shared cache lock')
                self.assertTrue(old_engine.exists())
                fcntl.flock(lock, fcntl.LOCK_UN)
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stdout + stderr)
            finally:
                if process.poll() is None:
                    process.kill()
                    process.communicate(timeout=5)
        self.assertFalse(old_engine.exists())
        self.assertFalse(old_storygraph.exists())
        self.assertTrue(recent_engine.exists())
        self.assertTrue(recent_storygraph.exists())

    def test_clean_removes_only_its_worktree(self):
        engine = self.entry(self.engine_digest, age=3 * 3600)
        storygraph = self.entry(self.story_digest, 'storygraph', age=3 * 3600)
        (self.state / 'owned-file').touch()
        clean = re.search(r'^if \[\[ "\$ACTION" == clean \]\]; then\n.*?^fi',
                          SCRIPT.read_text(), re.M | re.S)
        self.assertIsNotNone(clean)
        self.shell(r'''
ACTION=clean STATE="$TEST_ROOT/worktree" REMOTE=gothic-ai/e2e/test
remote() { "$@"; }
'''+clean.group(0))
        self.assertFalse(self.state.exists())
        self.assertTrue((engine / 'Gothic2Notr').exists())
        self.assertTrue((storygraph / 'storygraph').exists())

    def test_storygraph_digest_tracks_dependencies_independently(self):
        inputs = ['ai_bridge/storygraph/main.cpp', 'ai_bridge/textcodec.cpp',
                  'ai_bridge/textcodec.h', 'lib/ZenKit/src/Archive.cc',
                  'lib/rapidjson/include/rapidjson/document.h']
        for relative in inputs + ['game/main.cpp']:
            path = self.source / 'OpenGothic' / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(relative)

        def digest(kind=''):
            result = subprocess.run(
                ['bash', '-c', definitions() + '\nROOT="$TEST_ROOT/source"\nsource_digest ' + kind],
                env=self.env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout.strip()

        original = digest('storygraph')
        for relative in inputs:
            with self.subTest(source=relative):
                path = self.source / 'OpenGothic' / relative
                path.write_text(relative + ' changed')
                self.assertNotEqual(digest('storygraph'), original)
                path.write_text(relative)
        engine_before = digest()
        (self.source / 'OpenGothic/game/main.cpp').write_text('engine-only edit')
        self.assertEqual(digest('storygraph'), original)
        self.assertNotEqual(digest(), engine_before)
        engine_before = digest()
        for relative in ['ai_bridge/storygraph/build/generated.cpp', 'build/generated.cpp',
                         'lib/ZenKit/build/generated.cpp']:
            path = self.source / 'OpenGothic' / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('generated output')
        self.assertEqual(digest('storygraph'), original)
        self.assertEqual(digest(), engine_before)


class TunnelTests(unittest.TestCase):
    def run_job(self, directory, mode='run', forward_fail=False, reserved_port=0):
        root = Path(directory)
        (root / 'scripts').mkdir()
        (root / 'inputs/run').mkdir(parents=True)
        gq = root / 'scripts/gq'
        gq.write_text(f'#!{sys.executable}\n' + r'''
import os
from pathlib import Path
import sys
import time
root = Path(os.environ['TEST_SESSION'])
(root / 'submitted').touch()
(root / 'job-pid').write_text(str(os.getpid()))
if os.environ['TEST_MODE'] == 'busy':
    print('Ryzen занят', file=sys.stderr, flush=True)
    sys.exit(75)
while not (root / 'admitted').exists():
    time.sleep(0.02)
print('Ryzen: waiting for AI tunnel', flush=True)
while not (root / 'inputs/run/tunnel-ready').exists():
    time.sleep(0.02)
(root / 'job-ran').touch()
''')
        gq.chmod(0o755)
        harness = r'''
set -euo pipefail
ROOT="$TEST_SESSION" SESSION="$TEST_SESSION" STATE="$TEST_SESSION"
OUTPUT="$SESSION/output" RUN=run RYZEN_HOST=test AI_PORT=47827
TUNNEL_OPEN=0 RUN_STARTED=0 RUN_FINISHED=0 RUN_RC=0
SSH_PID=$$
GQ_ARGS=(--name test)
exec 8>"$SESSION/control"
: > "$OUTPUT"
ssh() {
  printf '%s\n' "$*" >> "$SESSION/forward"
  [[ -f "$SESSION/admitted" && "$TEST_FORWARD_FAIL" == 0 ]] || return 255
  [[ "$*" != *'-O cancel'* ]] || return 0
  if [[ "$TEST_RESERVED_PORT" != 0 && ! -f "$SESSION/retried" ]]; then
    touch "$SESSION/retried"
    printf '%s\n' "$TEST_RESERVED_PORT"
    return 0
  fi
  printf '51234\n'
}
remote() { "$@"; }
guard_ack() { :; }
'''
        env = dict(os.environ, TEST_SESSION=str(root), TEST_MODE=mode,
                   TEST_FORWARD_FAIL=str(int(forward_fail)), TEST_RESERVED_PORT=str(int(reserved_port)))
        process = subprocess.Popen(
            ['bash', '-c', harness + function('start_tunnel') + '\n' + function('run_quick') + '\nrun_quick true'],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            start_new_session=True,
        )
        self.addCleanup(self.stop_job, process, root)
        return root, process

    @staticmethod
    def stop_job(process, root):
        if process.poll() is None:
            process.kill()
        if (root / 'job-pid').exists() and not (root / 'job-ran').exists():
            try:
                os.kill(int((root / 'job-pid').read_text()), signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.communicate(timeout=5)

    def test_tunnel_opens_only_after_job_admission(self):
        directory = self.enterContext(tempfile.TemporaryDirectory())
        root, process = self.run_job(directory)
        wait_for(root / 'submitted')
        time.sleep(0.15)
        self.assertFalse((root / 'forward').exists())
        (root / 'admitted').touch()
        output, _ = process.communicate(timeout=8)
        self.assertEqual(process.returncode, 0, output)
        self.assertTrue((root / 'job-ran').exists())
        forwards = (root / 'forward').read_text().splitlines()
        self.assertEqual(len(forwards), 1)
        self.assertIn('-O forward', forwards[0])
        self.assertIn(str(root / 'control'), forwards[0])
        self.assertIn('127.0.0.1:0:127.0.0.1:47827', forwards[0])
        self.assertEqual((root / 'inputs/run/tunnel-ready').read_text(), '51234\n')

    def test_queue_timeout_does_not_open_tunnel(self):
        directory = self.enterContext(tempfile.TemporaryDirectory())
        root, process = self.run_job(directory, mode='busy')
        output, _ = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 75, output)
        self.assertIn('Ryzen занят', output)
        self.assertFalse((root / 'forward').exists())
        self.assertFalse((root / 'job-ran').exists())

    def test_failed_forward_stops_job_before_game(self):
        directory = self.enterContext(tempfile.TemporaryDirectory())
        root, process = self.run_job(directory, forward_fail=True)
        wait_for(root / 'submitted')
        (root / 'admitted').touch()
        output, _ = process.communicate(timeout=8)
        self.assertEqual(process.returncode, 1, output)
        self.assertFalse((root / 'inputs/run/tunnel-ready').exists())
        self.assertFalse((root / 'job-ran').exists())

    def test_e2e_port_is_cancelled_and_reallocated(self):
        for slot in range(3):
            for base in (47827, 47829, 47840):
                with self.subTest(port=base + slot * 100):
                    directory = self.enterContext(tempfile.TemporaryDirectory())
                    root, process = self.run_job(directory, reserved_port=base + slot * 100)
                    wait_for(root / 'submitted')
                    (root / 'admitted').touch()
                    output, _ = process.communicate(timeout=8)
                    self.assertEqual(process.returncode, 0, output)
                    calls = (root / 'forward').read_text().splitlines()
                    self.assertEqual(len(calls), 3)
                    self.assertIn('-O cancel', calls[1])
                    self.assertIn('127.0.0.1:0:127.0.0.1:47827', calls[1])
                    self.assertEqual((root / 'inputs/run/tunnel-ready').read_text(), '51234\n')

    def test_remote_bench_waits_then_preserves_url_components(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'inputs/run').mkdir(parents=True)
            harness = function('remote_bench') + r'''
require_prepared() { :; }
toolchain() { printf '%s' "$GOTHIC_AI_URL" > "$TEST_SESSION/rewritten"; exit 0; }
remote_bench "$TEST_SESSION" run unused
'''
            # Exit in toolchain after observing the URL; no game tools are used.
            process = subprocess.Popen(['bash', '-ec', harness], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                       text=True, env=dict(os.environ, TEST_SESSION=str(root), RYZEN_AI_TUNNEL='1',
                                                           GOTHIC_AI_URL='ws://user:secret@localhost:47827/ws?q=x#fragment'))
            try:
                self.assertEqual(process.stdout.readline().strip(), 'Ryzen: game admitted')
                self.assertEqual(process.stdout.readline().strip(), 'Ryzen: waiting for AI tunnel')
                self.assertFalse((root / 'rewritten').exists())
                (root / 'inputs/run/tunnel-ready').write_text('51234\n')
                stdout, stderr = process.communicate(timeout=5)
                self.assertEqual(process.returncode, 0, stdout + stderr)
                self.assertEqual((root / 'rewritten').read_text(), 'ws://user:secret@localhost:51234/ws?q=x#fragment')
            finally:
                if process.poll() is None:
                    process.kill()
                process.communicate(timeout=5)


@unittest.skipUnless(shutil.which('rsync'), 'needs rsync')
class TextureTests(unittest.TestCase):
    def test_two_worktree_uploads_do_not_replace_readers_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binaries = root / 'bin'
            binaries.mkdir()
            wrapper = binaries / 'rsync'
            wrapper.write_text(f'#!{sys.executable}\n' + f'''
import os
from pathlib import Path
import subprocess
import sys
args = sys.argv[1:]
if ':' in args[-1]:
    host, path = args[-1].split(':', 1)
    assert host == 'test'
    args[-1] = str(Path(os.environ['TEST_REMOTE_HOME']) / path) + '/'
sys.exit(subprocess.call([{shutil.which('rsync')!r}, *args]))
''')
            wrapper.chmod(0o755)
            remote_home = root / 'remote'
            expected = []
            with (root / 'A.lock').open('w') as lock_a, (root / 'B.lock').open('w') as lock_b:
                fcntl.flock(lock_a, fcntl.LOCK_EX)
                fcntl.flock(lock_b, fcntl.LOCK_EX)
                for name in ('A', 'B'):
                    textures = root / name
                    textures.mkdir()
                    (textures / f'{name}.dds').write_text(name)
                    remote = f'gothic-ai/e2e/{name}'
                    state = remote_home / remote
                    harness = '\n'.join([
                        'set -euo pipefail',
                        f'STATE={shlex.quote(str(state))}',
                        f'REMOTE={shlex.quote(remote)}',
                        f'HD_TEXTURES={shlex.quote(str(textures))}',
                        'RYZEN_HOST=test', 'ENV_ARGS=()', 'remote() { "$@"; }',
                        function('sync_hd_textures'), 'sync_hd_textures',
                        'printf "%s\\n" "${ENV_ARGS[@]}"',
                    ])
                    result = subprocess.run(['bash', '-c', harness], capture_output=True, text=True,
                                            env=dict(os.environ, PATH=str(binaries) + os.pathsep + os.environ['PATH'],
                                                     TEST_REMOTE_HOME=str(remote_home)), timeout=5)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    target = state / f'hd-textures/textures/{name}.dds'
                    self.assertEqual(result.stdout.strip(), f'OPENGOTHIC_TEXTURES={target.parent}')
                    expected.append(target)
                for path in expected:
                    self.assertEqual(path.read_text(), path.stem)


class ContentTests(unittest.TestCase):
    def setUp(self):
        self.root = Path(self.enterContext(tempfile.TemporaryDirectory(prefix='ryzen-content-test-')))
        self.env = {name: value for name, value in os.environ.items()
                    if not name.startswith(('OPENGOTHIC_', 'GOTHIC_', 'TEMPEST_', 'BENCH_', 'RYZEN_'))}
        self.env['TEST_ROOT'] = str(self.root)
        self.binaries = self.root / 'bin'
        self.binaries.mkdir()
        self.env['PATH'] = str(self.binaries) + os.pathsep + os.environ['PATH']
        rsync = self.binaries / 'rsync'
        rsync.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import shutil
import sys
root = Path(os.environ['TEST_ROOT'])
with (root / 'rsync.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\n')
assert sys.argv[1:3] == ['-a', '--delete']
host, destination = sys.argv[-1].split(':', 1)
assert host == 'test'
shutil.copytree(sys.argv[-2], root / 'remote' / destination, dirs_exist_ok=True)
''')
        rsync.chmod(0o755)

    def sync(self, *, value=None, run='one'):
        env = dict(self.env, TEST_RUN=run)
        if value is not None:
            env['OPENGOTHIC_CONTENT'] = value
        harness = r'''
set -euo pipefail
STATE="$TEST_ROOT/remote/gothic-ai/e2e/own" REMOTE=gothic-ai/e2e/own
RUN="$TEST_RUN" RYZEN_HOST=test ENV_ARGS=()
remote() { "$@"; }
'''+function('sync_content')+r'''
sync_content
printf '%s\n' "${ENV_ARGS[@]-}"
'''
        return subprocess.run(['bash', '-c', harness], env=env,
                              capture_output=True, text=True, timeout=5)

    def test_upload_is_per_run_and_passes_remote_path(self):
        content = self.root / 'content with spaces'
        meshes = content / 'meshes'
        meshes.mkdir(parents=True)
        index = meshes / 'meshes.json'
        index.write_text('{"FIRST":{}}')
        for run, text in (('one', '{"FIRST":{}}'), ('two', '{"SECOND":{}}')):
            index.write_text(text)
            result = self.sync(value=str(content), run=run)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            remote = self.root / f'remote/gothic-ai/e2e/own/inputs/{run}/content'
            self.assertEqual(result.stdout.strip(), f'OPENGOTHIC_CONTENT={remote}')
            self.assertEqual((remote / 'meshes/meshes.json').read_text(), text)
        self.assertEqual((self.root / 'remote/gothic-ai/e2e/own/inputs/one/content/meshes/meshes.json').read_text(),
                         '{"FIRST":{}}')
        calls = [json.loads(line) for line in (self.root / 'rsync.jsonl').read_text().splitlines()]
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0][-2], f'{content.resolve()}/')

    def test_empty_value_disables_remote_default_without_upload(self):
        result = self.sync(value='')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), 'OPENGOTHIC_CONTENT=')
        self.assertFalse((self.root / 'rsync.jsonl').exists())
        self.assertFalse((self.root / 'remote').exists())

    def test_unset_value_preserves_remote_default_without_upload(self):
        result = self.sync()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(result.stdout.strip(), '')
        self.assertFalse((self.root / 'rsync.jsonl').exists())
        self.assertFalse((self.root / 'remote').exists())

    def test_missing_explicit_directory_fails_before_upload(self):
        result = self.sync(value=str(self.root / 'missing'))
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('Missing OPENGOTHIC_CONTENT directory:', result.stderr)
        self.assertFalse((self.root / 'rsync.jsonl').exists())

    def test_bench_routes_supplied_content_to_ryzen(self):
        scripts = self.root / 'scripts'
        scripts.mkdir()
        shutil.copyfile(BENCH_SCRIPT, scripts / 'bench.sh')
        launcher = scripts / 'ryzen.sh'
        launcher.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import sys
assert sys.argv[1] == 'bench'
out = Path(sys.argv[3])
(out / 'content.json').write_text(json.dumps(os.environ.get('OPENGOTHIC_CONTENT')))
(out / 'game.log').write_text('mock remote game\n')
''')
        launcher.chmod(0o755)
        scenario = self.root / 'smoke.bench'
        scenario.write_text('quit\n')
        content = self.root / 'content outside output'
        content.mkdir()
        for index, value in enumerate((None, '', str(content))):
            with self.subTest(content=value):
                env = dict(self.env, BENCH_HOST='ryzen')
                env.pop('KEEP', None)
                if value is not None:
                    env['OPENGOTHIC_CONTENT'] = value
                out = self.root / f'out-{index}'
                result = subprocess.run(['bash', str(scripts / 'bench.sh'), str(scenario), str(out)],
                                        env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(json.loads((out / 'content.json').read_text()), value)
                self.assertEqual((out / 'host.txt').read_text(), 'ryzen\n')


if __name__ == '__main__':
    unittest.main()
