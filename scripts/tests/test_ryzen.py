"""Queue admission and per-worktree uploads without a game or real SSH."""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import select
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
GAME_HELPER = SCRIPT.with_name('ryzen_game.py')


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
        shutil.copyfile(GAME_HELPER, source / 'scripts/ryzen_game.py')
        settings = root / 'home/gothic-ai/game-settings/system'
        settings.mkdir(parents=True)
        for name in ('Gothic.ini', 'SystemPack.ini'):
            (settings / name).touch()
        (settings.parent.parent / 'game-base.json').write_text(json.dumps({'base': 'c' * 64, 'videos': ''}))
        binaries.mkdir()
        shutil.copyfile(SCRIPT.with_name('e2e.sh'), source / 'scripts/e2e.sh')
        shutil.copyfile(SCRIPT.with_name('e2e-services.sh'), source / 'scripts/e2e-services.sh')
        # Base preparation has its own tests; here the base is already ready.
        (source / 'scripts/base-ensure.sh').write_text('exit 0\n')
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
            'set -euo pipefail', function('window_position'), function('e2e_window_position'),
            function('game_settings'), function('remote_run'),
            'ROOT="$TEST_ROOT/source"',
            'toolchain() { :; }',
            'desktop() { RYZEN_SCREEN_SIZE=1920x1080; }',
            'require_prepared() { :; }',
            'export RYZEN_STORYGRAPH_BIN=/usr/bin/true',
            'export RYZEN_GAME_BIN="$TEST_ROOT/bin/game"',
            'remote_run e2e "$TEST_ROOT/state" run',
        ])
        result = subprocess.run(
            ['bash', '-c', harness], capture_output=True, text=True, timeout=20,
            env=dict(os.environ, HOME=str(root / 'home'), TEST_ROOT=str(root),
                     TEST_MODE=mode, SCENARIO='stages', GQ_PROFILE='ryzen',
                     LLM='off', SAVE='', TIMEOUT='240', GAME_EXTRA_ENV='', GOTHIC_TTS_RECORD='0', GOTHIC_SOAK_CONTROL='0',
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
    def run_game(self, action='e2e', prepare=True, build_exit=0, scenario='stages', window=None,
                 *, headless=False, bench_name='smoke', host='fake', jobs='7'):
        directory = self.enterContext(tempfile.TemporaryDirectory(prefix='ryzen-phases-test-'))
        root = Path(directory)
        (root / 'scripts/bench').mkdir(parents=True)
        bench_scenario = root / f'scripts/bench/{bench_name}.bench'
        bench_scenario.write_text(('# @headless\n' if headless else '') + 'quit\n')
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
        harness = SCRIPT.read_text().split('\ntoolchain() {', 1)[0] + r'''
set -euo pipefail
ROOT="$TEST_SESSION" SESSION="$TEST_SESSION" STATE="$TEST_SESSION/state"
OUTPUT="$SESSION/output" RUN=run AI_PORT=''
ACTION="$TEST_ACTION" JOBS="${RYZEN_JOBS:-12}" LOCK_WAIT=23 SCENARIO="$TEST_SCENARIO"
BENCH_SCENARIO="$TEST_BENCH_SCENARIO" RYZEN_MAX=9m WORLD=NEWWORLD.ZEN
TUNNEL_OPEN=0 RUN_STARTED=0 RUN_FINISHED=0 RUN_RC=0
SSH_PID=$$ ENV_ARGS=("RYZEN_JOBS=$JOBS") GQ_ARGS=()
exec 8>"$SESSION/control"
: > "$OUTPUT"
guard_ack() { :; }
preparation_needed() { [[ "$TEST_PREPARE" == 1 ]]; }
'''
        routing = SCRIPT.read_text().split('ACTION="${1:-}"\n', 1)[1].split('\nremote() {', 1)[0]
        harness += '\nset -- "$ACTION"; if [[ "$ACTION" == bench ]]; then set -- "$ACTION" "$BENCH_SCENARIO" "$SESSION/result"; fi\n'
        harness += routing + '\n'
        harness += '\n'.join([function('run_quick'), function('e2e_display_exclusive'), function('bench_window'),
                              function('bench_display_exclusive'), function('game_queue_args'), function('run_game')])
        harness += '\nrun_game env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote "$ACTION" "$STATE" "$RUN" "$SCENARIO"\n'
        env = {name: value for name, value in os.environ.items() if not name.startswith(('RYZEN_', 'BENCH_'))}
        if jobs is not None:
            env['RYZEN_JOBS'] = jobs
        result = subprocess.run(
            ['bash', '-c', harness], capture_output=True, text=True, timeout=10,
            env=dict(env, TEST_SESSION=str(root), TEST_ACTION=action, RYZEN_HOST=host,
                     TEST_PREPARE=str(int(prepare)), TEST_BUILD_EXIT=str(build_exit),
                     TEST_SCENARIO=scenario, TEST_BENCH_SCENARIO=str(bench_scenario), **(window or {})),
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
        command = request[request.index('--') + 1:]
        self.assertIn('RYZEN_RUN_ID=run-game', command)
        self.assertNotIn('RYZEN_BENCH_COLUMN=1', command)
        if action == 'bench':
            # A bench draws offscreen by default: no window slot and no window flags.
            self.assertEqual(options[3:], ['--class', 'quick', '--threads', '4', '--mem', '4G', '--max', '9m',
                                           '--wait-max', '23', '--gpu-mem', '2G', '--name', 'bench smoke'])
            self.assertIn('RYZEN_BENCH_OFFSCREEN=1', command)
            return
        for name, value in [('--class', 'quick'),
                            ('--threads', '4'), ('--mem', '5G'), ('--max', '9m'), ('--gpu-mem', '2G'),
                            ('--wait-max', '23'), ('--display-layout', '2'), ('--name', 'e2e stages')]:
            self.assert_option(options, name, value)
        for option in ('--display', '--display-shared'):
            self.assertIn(option, options)
        for option in ('--display-exclusive', '--display-column'):
            self.assertNotIn(option, options)
        self.assertNotIn('RYZEN_BENCH_OFFSCREEN=1', command)

    def test_preparation_and_game_have_separate_resources_and_names(self):
        for action in ('e2e', 'bench'):
            with self.subTest(action=action):
                result, requests = self.run_game(action)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(requests), 2, requests)
                build = self.queue_options(requests[0])
                for name, value in [('--class', 'quick'), ('--threads', '7'), ('--mem', '10G'),
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
        result, requests = self.run_game('bench', prepare=False, window={
            'RYZEN_BENCH_CLASS': 'batch', 'RYZEN_BENCH_ARCHIVE': '/task/far-fight/chunk',
        })
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        options = self.queue_options(requests[0])
        self.assert_option(options, '--class', 'batch')
        self.assert_option(options, '--archive', '/task/far-fight/chunk')

    def test_headless_bench_uses_batch_without_display_or_gpu_memory(self):
        for settings, expected_class in (({}, 'batch'), ({'RYZEN_BENCH_CLASS': 'quick'}, 'quick')):
            with self.subTest(settings=settings):
                result, requests = self.run_game('bench', headless=True, window=settings)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(requests), 2, requests)
                options = self.queue_options(requests[-1])
                for name, value in (('--class', expected_class), ('--threads', '4'), ('--mem', '6G'),
                                    ('--max', '9m'), ('--name', 'bench smoke')):
                    self.assert_option(options, name, value)
                for option in ('--display', '--display-shared', '--display-exclusive', '--display-column',
                               '--display-layout', '--gpu-mem'):
                    self.assertNotIn(option, options)
                self.assertNotIn('RYZEN_BENCH_COLUMN=1', requests[-1])
                self.assert_option(self.queue_options(requests[0]), '--class', 'quick')

    def test_headless_zero_runs_npc_day_in_the_game_offscreen(self):
        result, requests = self.run_game('bench', prepare=False, headless=True, bench_name='npc-day-parity',
                                         window={'BENCH_HEADLESS': '0'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        options = self.queue_options(requests[0])
        self.assertEqual(options, ['--host', 'fake', 'run', '--class', 'quick', '--threads', '4',
                                  '--mem', '4G', '--max', '9m', '--wait-max', '23',
                                  '--gpu-mem', '2G', '--name', 'bench npc-day-parity'])
        self.assertIn('RYZEN_BENCH_OFFSCREEN=1', requests[0])

    def test_dev_ubuntu_headless_build_and_game_use_host_resources(self):
        result, requests = self.run_game('bench', headless=True, host='dev-ubuntu', jobs=None)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(requests), 2, requests)
        self.assertEqual(self.queue_options(requests[0]), [
            '--host', 'dev-ubuntu', 'run', '--class', 'quick', '--threads', '6', '--mem', '10G',
            '--max', '9m', '--name', 'build bench smoke', '--wait-max', '23',
        ])
        self.assertIn('RYZEN_JOBS=6', requests[0])
        self.assertEqual(self.queue_options(requests[1]), [
            '--host', 'dev-ubuntu', 'run', '--class', 'batch', '--threads', '4', '--mem', '6G',
            '--max', '9m', '--wait-max', '23', '--name', 'bench smoke',
        ])

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
                self.assertEqual('--display-layout' in game, not exclusive)
                self.assertNotIn('--display-column', game)
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
        result, requests = self.run_game(action='bench', scenario='calm', window={'RYZEN_BENCH_WINDOW': '1'})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('--display-exclusive', self.queue_options(requests[-1]))
        self.assertIn('--display-column', self.queue_options(requests[-1]))

    def test_bench_window_conditions_keep_the_window_request(self):
        # Each condition brings back the window of PR #1067: a column, or the whole screen.
        window = {'RYZEN_BENCH_WINDOW': '1'}
        cases = [(window, 'smoke', False), (dict(window, TEMPEST_WINDOW_SIZE='800x600'), 'smoke', False),
                 (dict(window, RYZEN_DISPLAY_EXCLUSIVE='1'), 'smoke', True),
                 (dict(window, TEMPEST_WINDOW_SIZE='960x540'), 'smoke', True),
                 (dict(window, TEMPEST_WINDOW_POS='20,40'), 'smoke', True),
                 (dict(window, TEMPEST_WINDOW_POS=''), 'smoke', True),
                 ({'KEEP': '1'}, 'smoke', True), ({'OPENGOTHIC_BENCH_VISIBLE': '1'}, 'smoke', True),
                 ({'OPENGOTHIC_FPS_LOG': '1'}, 'smoke', False), ({'RYZEN_BENCH_CLASS': 'quiet'}, 'smoke', False),
                 ({}, 'streaming-flythrough', True)]
        for window, bench_name, exclusive in cases:
            with self.subTest(window=window, bench=bench_name):
                result, requests = self.run_game('bench', prepare=False, window=window, bench_name=bench_name)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                options = self.queue_options(requests[0])
                self.assert_option(options, '--class', window.get('RYZEN_BENCH_CLASS', 'quick'))
                self.assert_option(options, '--gpu-mem', '3G')
                self.assertIn('--display', options)
                self.assertEqual('--display-exclusive' in options, exclusive)
                for option in ('--display-shared', '--display-column', '--display-layout'):
                    self.assertEqual(option in options, not exclusive)
                self.assertEqual('RYZEN_BENCH_COLUMN=1' in requests[0], not exclusive)
                self.assertNotIn('RYZEN_BENCH_OFFSCREEN=1', requests[0])

    def test_bench_offscreen_ignores_window_layout_settings(self):
        for window in ({'TEMPEST_WINDOW_SIZE': '960x540'}, {'TEMPEST_WINDOW_POS': '20,40'},
                       {'RYZEN_DISPLAY_EXCLUSIVE': '1'}, {'RYZEN_BENCH_WINDOW': '0', 'KEEP': '0'}):
            with self.subTest(window=window):
                result, requests = self.run_game('bench', prepare=False, window=window)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assert_game_request(requests[0], 'bench')

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

    def test_service_preparation_installs_e2e_dependencies(self):
        e2e = SCRIPT.with_name('e2e.sh').read_text()
        services = e2e[e2e.index('ASR_PID=""'):e2e.index('\ncase "$SCENARIO" in', e2e.index('ASR_PID=""'))]
        for scenario in ('tts', 'svm_lines', 'combat_barks', 'mercy', 'hero_yield',
                         'humiliate', 'surrender_live', 'live_npc', 'live_director',
                         'gossip', 'day', 'group', 'interject', 'programs',
                         'errand', 'errand_fork', 'errand_far', 'errand_give',
                         'fight_reactions_wolf', 'fight_reactions_sight',
                         'fight_reactions_bandits', 'fight_reactions_flee', 'm3', 'm5e'):
            with self.subTest(scenario=scenario), tempfile.TemporaryDirectory(prefix='ryzen-deps-test-') as directory:
                root = Path(directory)
                source = root / 'source'
                state = root / 'state'
                (source / 'scripts').mkdir(parents=True)
                helper = SCRIPT.with_name('e2e-services.sh')
                if helper.exists():
                    shutil.copyfile(helper, source / 'scripts/e2e-services.sh')
                (source / 'server/.venv').mkdir(parents=True)
                (source / 'server/.venv/ready').touch()
                (source / 'tts-service').mkdir()
                (source / 'asr-service').mkdir()
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
if args[0] == 'run':
    sys.exit(0)
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
                startup = subprocess.run(
                    ['bash', '-ec', services + '\nwait'], capture_output=True, text=True, timeout=5,
                    env=dict(os.environ, ROOT=str(source), SCENARIO=scenario, LOG_DIR=directory,
                             TTS='fake', ASR='fake', TTS_PORT='47840', ASR_PORT='47830',
                             VOICE_FILE='/unused.wav', TEST_UV_CALLS=str(root / 'startup.jsonl'),
                             PATH=str(root / 'bin') + os.pathsep + os.environ['PATH']),
                )
                self.assertEqual(startup.returncode, 0, startup.stdout + startup.stderr)
                projects = ('server', *(json.loads(line)['project']
                                        for line in (root / 'startup.jsonl').read_text().splitlines()))
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
                for project in projects:
                    self.assertTrue((source / project / '.venv/ready').exists())
                self.assertEqual([json.loads(line) for line in calls.read_text().splitlines()], [
                    {'project': project, 'args': args}
                    for args in (['sync', '--check', '--frozen'], ['sync', '--frozen'],
                                 ['sync', '--check', '--frozen'])
                    for project in projects
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


    def test_e2e_uploads_declared_save_and_forwards_declared_timeout(self):
        source = SCRIPT.read_text()
        start = source.index("# Resolve the scenario's declared run settings here")
        block = source[start:source.index('echo "Ryzen: upload', start)]
        cases = [
            # scenario, explicit TIMEOUT, expected forwarded arguments, uploaded save
            ('errand', None, ['TIMEOUT=1500', 'SAVE=/remote/state/inputs/run/SAVE'], True),
            ('check_walk', None, ['TIMEOUT=240', 'SAVE='], False),
            ('errand', '600', ['TIMEOUT=600', 'SAVE=/remote/state/inputs/run/SAVE'], True),
        ]
        for scenario, timeout, expected, uploaded in cases:
            with self.subTest(scenario=scenario, timeout=timeout), \
                    tempfile.TemporaryDirectory(prefix='ryzen-run-defaults-') as directory:
                root = Path(directory).resolve()
                (root / 'scripts').mkdir()
                (root / 'server').mkdir()
                shutil.copyfile(SCRIPT.with_name('e2e-services.sh'), root / 'scripts/e2e-services.sh')
                subprocess.run(['git', 'init', '-q', str(root)], check=True)
                (root / 'save_slot_1.sav').write_text('slot 1')
                binaries = root / 'bin'
                binaries.mkdir()
                # A stand-in for RUN: errand is declared with a save, check_walk without one.
                (binaries / 'uv').write_text(
                    '#!/bin/sh\ncat >/dev/null\n'
                    'case "$4" in errand) echo "1500 1" ;; check_walk) echo "240 0" ;; esac\n')
                (binaries / 'rsync').write_text('#!/bin/sh\nprintf \'%s\\n\' "$*" >>"$TEST_ROOT/rsync"\n')
                for executable in binaries.iterdir():
                    executable.chmod(0o755)
                env = {name: value for name, value in os.environ.items()
                       if name not in ('SAVE', 'TIMEOUT', 'VOICE_FILE')}
                env.update(PATH=str(binaries) + os.pathsep + os.environ['PATH'], TEST_ROOT=str(root),
                           ROOT=str(root), SCENARIO=scenario)
                carried = ''
                if timeout is not None:
                    # The carry loop above has already forwarded an explicit TIMEOUT.
                    env['TIMEOUT'] = timeout
                    carried = f'TIMEOUT={timeout}'
                harness = ('set -euo pipefail\nACTION=e2e STATE=/remote/state REMOTE=gothic-ai/e2e/own '
                           f'RUN=run RYZEN_HOST=test ENV_ARGS=({carried})\n'
                           + block + 'printf \'%s\\n\' "${ENV_ARGS[@]}"\n')
                result = subprocess.run(['bash', '-c', harness], env=env,
                                        capture_output=True, text=True, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stdout.splitlines(), expected)
                uploads = (root / 'rsync').read_text().splitlines() if (root / 'rsync').exists() else []
                self.assertEqual(uploads, [f'-a -- {root}/save_slot_1.sav test:gothic-ai/e2e/own/inputs/run/SAVE']
                                 if uploaded else [])


class AutoCleanTests(unittest.TestCase):
    def test_controller_marks_only_own_session_and_preserves_other_data(self):
        with tempfile.TemporaryDirectory(prefix='ryzen-autoclean-test-') as directory:
            home = Path(directory) / 'home'
            worktrees = home / 'gothic-ai/e2e'
            own = worktrees / 'own'
            own.mkdir(parents=True)
            (own / '.last-used').touch()
            stale = worktrees / 'stale'
            (stale / 'build').mkdir(parents=True)
            data = stale / 'result.bin'
            data.write_text('preserve until verified archive')
            (stale / 'build/object.o').write_text('rebuildable')
            for path in [*worktrees.rglob('*'), worktrees]:
                os.utime(path, (time.time() - 3 * 86400,) * 2)
            before = {path: path.stat().st_mtime_ns for path in stale.rglob('*')}
            harness = 'set -euo pipefail\n' + function('auto_clean') + '\nauto_clean\n'
            result = subprocess.run(['bash', '-c', harness], capture_output=True, text=True,
                                    timeout=5, env=dict(os.environ, HOME=str(home),
                                                       WORKTREE_LOCK=str(worktrees / 'own.lock')))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertLess(time.time() - (own / '.last-used').stat().st_mtime, 10)
            self.assertEqual({path: path.stat().st_mtime_ns for path in stale.rglob('*')}, before)
            self.assertEqual(data.read_text(), 'preserve until verified archive')

    def test_controller_rejects_marker_symlink(self):
        with tempfile.TemporaryDirectory(prefix='ryzen-autoclean-test-') as directory:
            home = Path(directory) / 'home'
            own = home / 'gothic-ai/e2e/own'
            own.mkdir(parents=True)
            outside = Path(directory) / 'outside'
            outside.write_text('keep')
            (own / '.last-used').symlink_to(outside)
            harness = 'set -euo pipefail\n' + function('auto_clean') + '\nauto_clean\n'
            result = subprocess.run(['bash', '-c', harness], capture_output=True, text=True,
                                    timeout=5, env=dict(os.environ, HOME=str(home),
                                                       WORKTREE_LOCK=str(own.with_suffix('.lock'))))
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(outside.read_text(), 'keep')


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
        shutil.copyfile(GAME_HELPER, self.source / 'scripts/ryzen_game.py')
        shutil.copyfile(SCRIPT.with_name('ryzen-build.sh'), self.source / 'scripts/ryzen-build.sh')
        shutil.copyfile(SCRIPT.with_name('e2e-services.sh'), self.source / 'scripts/e2e-services.sh')
        (self.source / 'OpenGothic/ai_bridge/storygraph').mkdir(parents=True)
        settings = self.home / 'gothic-ai/game-settings/system'
        settings.mkdir(parents=True)
        for name in ('Gothic.ini', 'SystemPack.ini'):
            (settings / name).touch()
        (settings.parent.parent / 'game-base.json').write_text(json.dumps({'base': 'c' * 64, 'videos': ''}))
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
    targets = args[args.index('--target') + 1:args.index('--parallel')]
    for target in targets:
        output = build / (f'opengothic/{target}' if target in ('Gothic2Notr', 'gothic-body-runner') else 'storygraph')
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text('#!/usr/bin/env python3\nimport os, sys\nfrom pathlib import Path\n'
                          'Path(os.environ["TEST_GAME_PATH"]).write_text(str(Path(sys.argv[0]).resolve()))\n'
                          'if sys.argv[0].endswith("Gothic2Notr"):\n'
                          '    assert Path(sys.argv[0]).with_name("runtime.dat").read_text() == "runtime"\n')
        output.chmod(0o755)
        output.with_name('runtime.dat').write_text('runtime')
        if target == 'Gothic2Notr':
            converter = output.with_name('content-convert')
            converter.write_text('#!/bin/sh\nexit 0\n')
            converter.chmod(0o755)
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
        if kind == 'engine':
            converter = entry / 'content-convert'
            converter.write_text('#!/bin/sh\nexit 0\n')
            converter.chmod(0o755)
        (entry / 'runtime.dat').write_text(marker)
        timestamp = time.time() - age
        os.utime(entry, (timestamp, timestamp))
        return entry

    def test_cache_hit_runs_cached_game_without_a_worktree_build(self):
        cached = self.entry(self.engine_digest)
        storygraph = self.entry(self.story_digest, 'storygraph') / 'storygraph'
        storygraph.write_text(f'#!{sys.executable}\n' + r'''
import os
from pathlib import Path
import sys
assert sys.argv[1:] == [os.environ['GOTHIC_DIR'], '--ou-messages']
fingerprint = os.environ['OPENGOTHIC_BASE_FINGERPRINT']
(Path(os.environ['TEST_ROOT']) / 'selected-base').write_text(fingerprint)
''')
        selected = self.root / 'selected-base'
        (self.source / 'scripts/e2e.sh').write_text(
            'test "$(cat "$TEST_ROOT/selected-base")" = "$OPENGOTHIC_BASE_FINGERPRINT"\n'
            '"$GAME_BIN" --test\n')
        # The pre-server selection must work with both a missing and a stale selection.
        for previous in (None, 'd' * 64):
            if previous is not None:
                selected.write_text(previous)
            result = self.shell(r'''
remote_probe e2e "$TEST_ROOT/worktree" run stages
remote_run e2e "$TEST_ROOT/worktree" run
''')
            self.assertEqual(selected.read_text(), 'c' * 64)
        self.assertIn(f'engine from cache {self.engine_digest[:12]} (0s)', result.stdout)
        self.assertEqual((self.root / 'game-path').read_text(), str(cached / 'Gothic2Notr'))
        self.assertFalse((self.state / 'build').exists())
        self.assertFalse((self.state / 'storygraph-build').exists())
        self.assertFalse((self.root / 'cmake-calls.jsonl').exists())
        self.assertEqual((self.source / 'OpenGothic/ai_bridge/storygraph/build/storygraph').resolve(),
                         self.cache / 'storygraph' / self.story_digest / 'storygraph')

    def test_headless_rebuilds_a_window_only_cache_and_runs_without_desktop(self):
        cached = self.cache / self.engine_digest
        self.env['BENCH_HEADLESS'] = '1'
        self.shell(r'''
printf '%s\n' "''' + 'd' * 64 + r'''" > "$TEST_ROOT/worktree/inputs/run/source.sha256"
remote_prepare bench "$TEST_ROOT/worktree" run smoke
test -x "$TEST_ROOT/worktree/build/opengothic/gothic-body-runner"
printf '%s\n' "''' + self.engine_digest + r'''" > "$TEST_ROOT/worktree/inputs/run/source.sha256"
BENCH_HEADLESS=0 remote_prepare bench "$TEST_ROOT/worktree" run smoke
''')
        self.assertFalse((cached / 'gothic-body-runner').exists())
        self.assertFalse((self.state / 'build/opengothic/gothic-body-runner').exists())
        (self.source / 'scripts/bench.sh').write_text('"$GAME_BIN" --test\n')
        self.shell(r'''
engine_ready "$TEST_ROOT/worktree" "''' + self.engine_digest + r'''"
if remote_probe bench "$TEST_ROOT/worktree" run smoke; then
  echo 'headless accepted a cache without the runner' >&2
  exit 90
fi
remote_prepare bench "$TEST_ROOT/worktree" run smoke
remote_probe bench "$TEST_ROOT/worktree" run smoke
desktop() { echo 'headless looked for a desktop' >&2; return 91; }
command() {
  if [[ "$1" == -v && "$2" == ffmpeg ]]; then
    echo 'headless required ffmpeg' >&2
    return 92
  fi
  builtin command "$@"
}
remote_bench "$TEST_ROOT/worktree" run smoke
''')
        calls = [json.loads(line) for line in (self.root / 'cmake-calls.jsonl').read_text().splitlines()]
        builds = [call for call in calls if '--build' in call]
        self.assertEqual([call[call.index('--target') + 1:call.index('--parallel')] for call in builds],
                         [['Gothic2Notr', 'gothic-body-runner'], ['Gothic2Notr'],
                          ['Gothic2Notr', 'gothic-body-runner']])
        self.assertTrue(os.access(cached / 'gothic-body-runner', os.X_OK))
        self.assertEqual((self.root / 'game-path').read_text(), str(cached / 'Gothic2Notr'))
        self.assertFalse((self.state / 'runs/run/game.sh').exists())

    def test_game_settings_loads_fingerprints_without_installation(self):
        result = self.shell(r'''
game_settings
python3 - <<'PY'
import json
import os
print(json.dumps({key: os.environ[key] for key in
    ('GOTHIC_DIR', 'OPENGOTHIC_BASE_FINGERPRINT', 'OPENGOTHIC_VIDEOS_FINGERPRINT')}))
PY
''')
        self.assertEqual(json.loads(result.stdout), {
            'GOTHIC_DIR': str(self.home / 'gothic-ai/game-settings'),
            'OPENGOTHIC_BASE_FINGERPRINT': 'c' * 64,
            'OPENGOTHIC_VIDEOS_FINGERPRINT': '',
        })
        self.assertFalse((self.home / 'gothic-ai' / 'game').exists())

        stamp = r'''python3 - <<'PY'
import json
import os
from pathlib import Path
Path(os.environ['TEST_ROOT'], 'received-env.json').write_text(json.dumps({key: os.environ[key] for key in
    ('GOTHIC_DIR', 'OPENGOTHIC_BASE_FINGERPRINT', 'OPENGOTHIC_VIDEOS_FINGERPRINT')}))
PY
if [[ -n "${LOG_DIR:-}" ]]; then : >"$LOG_DIR/game.log"; fi
'''
        for filename in ('e2e.sh', 'bench.sh'):
            (self.source / 'scripts' / filename).write_text(stamp)
        self.env['GOTHIC_DIR'] = '/Volumes/x'
        # Exercise the real Mac forwarding list, then enter a fresh remote environment.
        forwarding = SCRIPT.read_text().split('BASE_ENV_ARGS=()\n', 1)[1].split('if [[ "$ACTION" != clean ]]', 1)[0]
        for action in ('e2e', 'bench', 'pytest'):
            with self.subTest(action=action):
                entry = 'remote_bench' if action == 'bench' else f'remote_run {action}'
                self.shell('BASE_ENV_ARGS=()\n' + forwarding + r'''
unset GOTHIC_DIR
for assignment in ${BASE_ENV_ARGS[@]+"${BASE_ENV_ARGS[@]}"}; do export "$assignment"; done
require_prepared() { :; }
build_storygraph() { export RYZEN_STORYGRAPH_BIN=/usr/bin/true; }
export RYZEN_STORYGRAPH_BIN=/usr/bin/true RYZEN_GAME_BIN=/usr/bin/true
ffmpeg() { :; }
uv() { bash "$TEST_ROOT/source/scripts/bench.sh"; }
''' + entry + ' "$TEST_ROOT/worktree" run smoke\n')
                received = json.loads((self.root / 'received-env.json').read_text())
                self.assertEqual(received, {
                    'GOTHIC_DIR': str(self.home / 'gothic-ai/game-settings'),
                    'OPENGOTHIC_BASE_FINGERPRINT': 'c' * 64,
                    'OPENGOTHIC_VIDEOS_FINGERPRINT': '',
                })

    def test_game_settings_rejects_bad_setup_and_preserves_caller_overrides(self):
        import runpy

        environment = runpy.run_path(str(GAME_HELPER))["game_environment"]
        overrides = {
            "GOTHIC_DIR": "/custom/settings",
            "OPENGOTHIC_BASE": "/custom/base",
            "OPENGOTHIC_BASE_FINGERPRINT": "e" * 64,
            "OPENGOTHIC_VIDEOS_FINGERPRINT": "",
            "OPENGOTHIC_BASE_CACHE": "/custom/cache",
            "OPENGOTHIC_BASE_STORE": "",
        }
        self.assertEqual(environment(self.home, overrides), overrides)
        self.assertEqual(environment(self.home, overrides, assets="/custom/assets")["GOTHIC_DIR"],
                         "/custom/assets")
        self.assertEqual(environment(self.home, {"OPENGOTHIC_BASE_FINGERPRINT": ""})[
            "OPENGOTHIC_BASE_FINGERPRINT"], "c" * 64)
        manifest = self.home / "gothic-ai/game-base.json"
        for contents in ('{}', '{"base": "invalid", "videos": ""}'):
            manifest.write_text(contents)
            with self.assertRaisesRegex(ValueError, "Run scripts/ryzen.sh setup first"):
                environment(self.home, {})
        manifest.unlink()
        with self.assertRaisesRegex(ValueError, "Run scripts/ryzen.sh setup first"):
            environment(self.home, {})
        self.assertFalse(manifest.exists())
        self.assertEqual(environment(self.home, overrides), overrides)

    def format_setup(self):
        self.base_cache = self.root / 'base-cache'
        self.base_store = self.root / 'base-store'
        for key in tuple(self.env):
            if key.startswith('OPENGOTHIC_BASE') or key in ('GOTHIC_DIR', 'OPENGOTHIC_VIDEOS_FINGERPRINT'):
                self.env.pop(key)
        self.env.update(OPENGOTHIC_BASE_CACHE=str(self.base_cache), OPENGOTHIC_BASE_STORE=str(self.base_store))
        self.converter = self.bin / 'content-convert'
        self.converter.write_text(
            f'#!{sys.executable}\nimport json, sys\nassert sys.argv[1:] == ["formats"]\n'
            f'print(json.dumps({{"base": {"2" * 64!r}, "videos": {"4" * 64!r}}}))\n')
        self.converter.chmod(0o755)
        self.base_part('c' * 64, '1' * 64)

    def base_part(self, fingerprint, format_sha256, root=None, **changed):
        part = (self.base_cache if root is None else root) / fingerprint
        part.mkdir(parents=True, exist_ok=True)
        identity = {'fingerprint': fingerprint, 'format_sha256': format_sha256,
                    'archives': [{'name': 'GOTHIC.VDF', 'sha256': '5' * 64}],
                    'loose_files': [], 'sources': {'version': 'Gothic II'}}
        identity.update(changed)
        (part / 'base.json').write_text(json.dumps(identity))
        return part

    def video_part(self, fingerprint, format_sha256, root=None, sources=None):
        part = (self.base_cache if root is None else root) / 'videos' / fingerprint
        part.mkdir(parents=True, exist_ok=True)
        identity = {'format_sha256': format_sha256,
                    'sources': [{'name': 'INTRO.BIK', 'sha256': '6' * 64}] if sources is None else sources}
        (part / 'videos.json').write_text(json.dumps({'identity': json.dumps(identity)}))

    def game_result(self, *, converter=True, assets=None, **overrides):
        argv = [sys.executable, str(GAME_HELPER), '--json',
                '--assets=' + str(assets or self.home / 'gothic-ai/game-settings')]
        if converter:
            argv.extend(['--converter', str(self.converter)])
        return subprocess.run(argv, env=dict(self.env, **overrides), capture_output=True, text=True, timeout=10)

    def test_game_settings_replace_base_from_cache_before_store_and_ignore_unrelated_parts(self):
        self.format_setup()
        self.base_part('d' * 64, '2' * 64)
        self.base_part('e' * 64, '2' * 64, self.base_store / 'base')
        for fingerprint in ('.tmp-' + '0' * 64, '0' * 63, '.lock', '.use'):
            self.base_part(fingerprint, '2' * 64)
        self.base_part('0' * 64, '2' * 64).joinpath('base.json').write_text('{broken')
        for fingerprint, changed in (('1' * 64, {'archives': []}),
                                     ('2' * 64, {'loose_files': ['other']}),
                                     ('3' * 64, {'archives': [{'name': 'GOTHIC.VDF', 'sha256': '0' * 64}]})):
            self.base_part(fingerprint, '2' * 64, **changed)
        pointer = self.home / 'gothic-ai/game-base.json'
        previous = pointer.read_bytes()
        result = self.game_result()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['OPENGOTHIC_BASE_FINGERPRINT'], 'd' * 64)
        self.assertEqual(pointer.read_bytes(), previous)

    def test_mac_and_ryzen_select_the_same_installation_across_formats(self):
        self.format_setup()
        shared_cache = self.home / '.cache/gothic-ai/base'
        shared_cache.parent.mkdir(parents=True)
        shutil.move(self.base_cache, shared_cache)
        self.base_cache = shared_cache
        self.env['OPENGOTHIC_BASE_CACHE'] = str(shared_cache)
        game = self.root / 'installation'
        pointers = shared_cache / 'current'
        pointers.mkdir()
        mac_pointer = pointers / (hashlib.sha256(str(game).encode()).hexdigest() + '.json')
        host_pointer = self.home / 'gothic-ai/game-base.json'
        binding = {'path': 'sources/sources.json', 'size': 123, 'sha256': '7' * 64}
        harness = '\n'.join([
            'set -euo pipefail', function('select_mac_base_parts'),
            'ROOT="$TEST_ROOT/source" ACTION=setup',
            'auto_base="$TEST_BASE" auto_videos="$TEST_VIDEOS"',
            'cmake() { if [[ "$1" == -DBASE_ROOT=* ]]; then printf "%s\\n" "$TEST_BASE_FORMAT"; '
            'else printf "%s\\n" "$TEST_VIDEO_FORMAT"; fi; }',
            'select_mac_base_parts',
            'printf "%s\\n" "$base_fingerprint" "$videos_fingerprint"',
        ])
        for kind, field, value, accepted in (
            ('base', 'sources', dict(binding, size=456, sha256='8' * 64), True),
            ('base', 'archives', [{'name': 'OTHER.VDF', 'sha256': '5' * 64}], False),
            ('base', 'loose_files', [{'path': '_work/other.dat', 'sha256': '6' * 64}], False),
            ('videos', 'sources', [{'name': 'OTHER.BIK', 'sha256': '6' * 64}], False),
        ):
            with self.subTest(kind=kind, field=field):
                base = self.base_part('c' * 64, ('1' if kind == 'base' else '2') * 64,
                                      sources=binding)
                if kind == 'base':
                    pointed, candidate = base, self.base_part('d' * 64, '2' * 64, sources=binding)
                    metadata = json.loads((candidate / 'base.json').read_text())
                    metadata[field] = value
                    (candidate / 'base.json').write_text(json.dumps(metadata))
                    old, new = 'c' * 64, 'd' * 64
                else:
                    self.video_part('a' * 64, '3' * 64)
                    self.video_part('b' * 64, '4' * 64, sources=value)
                    pointed = shared_cache / 'videos' / ('a' * 64)
                    old, new = 'a' * 64, 'b' * 64
                mac_pointer.write_text(json.dumps({
                    'version': 1, 'gothic': str(game), kind: {'fingerprint': old, 'path': str(pointed)},
                }))
                host_pointer.write_text(json.dumps({'base': 'c' * 64, 'videos': old if kind == 'videos' else ''}))
                previous = [pointer.read_bytes() for pointer in (mac_pointer, host_pointer)]
                mac = subprocess.run(
                    ['bash', '-c', harness], capture_output=True, text=True, timeout=5,
                    env=dict(self.env, GOTHIC_DIR=str(game), TEST_BASE=str(int(kind == 'base')),
                             TEST_VIDEOS=str(int(kind == 'videos')), TEST_BASE_FORMAT='2' * 64,
                             TEST_VIDEO_FORMAT='4' * 64),
                )
                ryzen = self.game_result()
                self.assertEqual(mac.returncode, 0 if accepted else 1, mac.stdout + mac.stderr)
                if accepted:
                    self.assertEqual(mac.stdout.splitlines()[0 if kind == 'base' else 1], new)
                else:
                    self.assertIn(f'needs_decision: Mac {kind} part {old} has importer format', mac.stderr)
                self.assertEqual(ryzen.returncode, 0 if accepted or kind == 'videos' else 1,
                                 ryzen.stdout + ryzen.stderr)
                if ryzen.returncode == 0:
                    key = 'OPENGOTHIC_BASE_FINGERPRINT' if kind == 'base' else 'OPENGOTHIC_VIDEOS_FINGERPRINT'
                    self.assertEqual(json.loads(ryzen.stdout)[key], new if accepted else old)
                else:
                    self.assertIn('no ready base of the same installation', ryzen.stderr)
                self.assertEqual([pointer.read_bytes() for pointer in (mac_pointer, host_pointer)], previous)

    def test_game_settings_replace_base_from_store_when_pointer_metadata_is_only_in_store(self):
        self.format_setup()
        stored = self.base_store / 'base'
        stored.mkdir(parents=True)
        shutil.move(self.base_cache / ('c' * 64), stored / ('c' * 64))
        self.base_part('e' * 64, '2' * 64, stored)
        result = self.game_result()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['OPENGOTHIC_BASE_FINGERPRINT'], 'e' * 64)

    def test_game_settings_replace_videos_with_same_sources_and_keep_missing_replacement(self):
        self.format_setup()
        self.base_part('c' * 64, '2' * 64)
        (self.home / 'gothic-ai/game-base.json').write_text(json.dumps({'base': 'c' * 64, 'videos': 'a' * 64}))
        self.video_part('a' * 64, '3' * 64)
        self.video_part('0' * 64, '4' * 64, sources=[])
        for root in (self.base_cache, self.base_store / 'base'):
            with self.subTest(root=root):
                self.video_part('b' * 64, '4' * 64, root)
                result = self.game_result()
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)['OPENGOTHIC_VIDEOS_FINGERPRINT'], 'b' * 64)
                shutil.rmtree(root / 'videos' / ('b' * 64))
        result = self.game_result()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['OPENGOTHIC_VIDEOS_FINGERPRINT'], 'a' * 64)

    def test_game_settings_missing_base_format_fails_with_exact_recovery_message(self):
        self.format_setup()
        for store in (str(self.base_store), ''):
            with self.subTest(store=store):
                result = self.game_result(OPENGOTHIC_BASE_STORE=store)
                location = str(self.base_cache) + (f' or {store}/base' if store else '')
                expected = (
                    f'game-base.json names base {"c" * 64} of importer format {"1" * 12}, '
                    f'but this engine needs {"2" * 12}; no ready base of the same installation '
                    f'in that format in {location}. Start the game once on the Mac on this version '
                    '(it publishes the base to the shared store), or run scripts/ryzen.sh setup.\n')
                self.assertEqual(result.returncode, 1)
                self.assertEqual(result.stdout, '')
                self.assertEqual(result.stderr, expected)

    def test_game_settings_keep_pointer_without_usable_converter_or_original_metadata(self):
        self.format_setup()
        self.base_part('d' * 64, '2' * 64)
        working = self.converter.read_text()
        for mode in ('matching', 'no-flag', 'missing', 'hanging', 'invalid-json', 'nonzero', 'missing-base'):
            with self.subTest(mode=mode):
                self.converter.write_text(working)
                if mode == 'matching':
                    self.base_part('c' * 64, '2' * 64)
                else:
                    self.base_part('c' * 64, '1' * 64)
                if mode == 'missing':
                    self.converter.unlink()
                elif mode == 'hanging':
                    self.converter.write_text(f'#!{sys.executable}\nimport time\ntime.sleep(30)\n')
                elif mode == 'invalid-json':
                    self.converter.write_text('#!/bin/sh\necho not-json\n')
                elif mode == 'nonzero':
                    self.converter.write_text('#!/bin/sh\nexit 2\n')
                elif mode == 'missing-base':
                    shutil.rmtree(self.base_cache / ('c' * 64))
                if self.converter.exists():
                    self.converter.chmod(0o755)
                result = self.game_result(converter=mode != 'no-flag')
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout)['OPENGOTHIC_BASE_FINGERPRINT'], 'c' * 64)

    def test_game_settings_keep_explicit_content_choices_and_unrelated_assets(self):
        self.format_setup()
        self.base_part('d' * 64, '2' * 64)
        (self.home / 'gothic-ai/game-base.json').write_text(json.dumps({'base': 'c' * 64, 'videos': 'a' * 64}))
        self.video_part('a' * 64, '3' * 64)
        self.video_part('b' * 64, '4' * 64)
        for key, value in (('OPENGOTHIC_BASE', '/custom/base'), ('OPENGOTHIC_BASE', ''),
                           ('OPENGOTHIC_BASE_FINGERPRINT', 'e' * 64), ('OPENGOTHIC_BASE_FINGERPRINT', ''),
                           ('OPENGOTHIC_VIDEOS_FINGERPRINT', 'f' * 64), ('OPENGOTHIC_VIDEOS_FINGERPRINT', '')):
            with self.subTest(key=key, value=value):
                result = self.game_result(**{key: value})
                self.assertEqual(result.returncode, 0, result.stderr)
                selected = json.loads(result.stdout)
                if key == 'OPENGOTHIC_VIDEOS_FINGERPRINT':
                    self.assertEqual(selected[key], value)
                    self.assertEqual(selected['OPENGOTHIC_BASE_FINGERPRINT'], 'd' * 64)
                else:
                    expected = (value if key == 'OPENGOTHIC_BASE_FINGERPRINT' else 'c' * 64) if value else 'd' * 64
                    self.assertEqual(selected['OPENGOTHIC_BASE_FINGERPRINT'], expected)
                    if key == 'OPENGOTHIC_BASE':
                        self.assertEqual(selected[key], value)
        result = self.game_result(assets='/custom/assets')
        self.assertEqual(result.returncode, 0, result.stderr)
        selected = json.loads(result.stdout)
        self.assertEqual(selected['GOTHIC_DIR'], '/custom/assets')
        self.assertNotIn('OPENGOTHIC_BASE_FINGERPRINT', selected)

    def test_pytest_requires_setup_and_uses_game_settings(self):
        storygraph = self.bin / 'storygraph'
        storygraph.write_text(f'#!{sys.executable}\n' + r'''
import os
from pathlib import Path
import sys
assert sys.argv[1:] == [os.environ['GOTHIC_DIR'], '--ou-messages']
(Path(os.environ['TEST_ROOT']) / 'selected-base').write_text(os.environ['OPENGOTHIC_BASE_FINGERPRINT'])
''')
        storygraph.chmod(0o755)
        self.env.pop('GOTHIC_DIR', None)
        commands = r'''
build_storygraph() { export RYZEN_STORYGRAPH_BIN="$TEST_ROOT/bin/storygraph"; }
uv() {
  test "$(cat "$TEST_ROOT/selected-base")" = "$OPENGOTHIC_BASE_FINGERPRINT"
  printf '%s\n' "$GOTHIC_DIR" "$@" >"$TEST_ROOT/pytest-env"
}
remote_run pytest "$TEST_ROOT/worktree" run -rs tests/test_storygraph.py
'''
        self.shell(commands)
        self.assertEqual((self.root / 'pytest-env').read_text().splitlines(), [
            str(self.home / 'gothic-ai/game-settings'), 'run', 'pytest', '-rs', 'tests/test_storygraph.py',
        ])
        (self.root / 'pytest-env').unlink()
        (self.home / 'gothic-ai/game-base.json').unlink()
        shutil.rmtree(self.home / 'gothic-ai/game-settings')
        with self.assertRaisesRegex(AssertionError, 'Run scripts/ryzen.sh setup first'):
            self.shell(commands)
        self.assertFalse((self.root / 'pytest-env').exists())
        self.assertFalse((self.home / 'gothic-ai/game-base.json').exists())
        self.assertFalse((self.home / 'gothic-ai/game-settings').exists())
        self.assertFalse((self.home / 'gothic-ai' / 'game').exists())

    def test_setup_removes_only_its_temporary_copy_on_failure_or_interrupt(self):
        game = self.root / 'installation'
        game.mkdir()
        part = self.root / 'mac-base'
        part.mkdir()
        (part / 'base.json').write_text(json.dumps({'format_sha256': 'a' * 64}))
        pointers = self.home / '.cache/gothic-ai/base/current'
        pointers.mkdir(parents=True)
        pointer = pointers / (hashlib.sha256(str(game).encode()).hexdigest() + '.json')
        pointer.write_text(json.dumps({'version': 1, 'gothic': str(game),
                                      'base': {'fingerprint': 'c' * 64, 'path': str(part)}}))
        remote_cache = self.root / 'remote/.cache/gothic-ai/base'
        foreign = remote_cache / '.setup-another-session'
        foreign.mkdir(parents=True)
        (foreign / 'keep').touch()
        setup = self.base_sync_source()
        harness = r'''
set -euo pipefail
ACTION=setup RYZEN_HOST=test
ROOT="$TEST_ROOT/source"
cmake() { printf '%064d\n' 0 | tr 0 a; }
toolchain() { :; }
remote() { HOME="$TEST_ROOT/remote" "$@"; }
verify_base_part() { [[ "$4" != full ]] || return 23; }
rsync() {
  case "$TEST_FAILURE" in
    rsync) return 23 ;;
    interrupt) kill -INT "$$" ;;
    verification) return 0 ;;
  esac
}
'''+setup
        for failure, status in (('rsync', 23), ('verification', 23), ('interrupt', 130)):
            with self.subTest(failure=failure):
                result = subprocess.run(
                    ['bash', '-c', harness], capture_output=True, text=True, timeout=5,
                    env=dict(self.env, GOTHIC_DIR=str(game), TEST_FAILURE=failure),
                )
                self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                self.assertEqual(list(remote_cache.glob('.setup-*')), [foreign])
                self.assertTrue((foreign / 'keep').exists())

    def setup_part(self, kind, fingerprint, format_digest, *, path=None, source='installation'):
        cache = self.home / '.cache/gothic-ai/base'
        part = path or cache / ('' if kind == 'base' else 'videos') / fingerprint
        part.mkdir(parents=True)
        identity = {'format_sha256': format_digest}
        if kind == 'base':
            identity.update(archives=[{'name': source}], loose_files=[{'path': source}], sources=[{'name': source}])
            metadata = dict(identity, fingerprint=fingerprint)
        else:
            identity['sources'] = [{'name': source}]
            metadata = {'fingerprint': fingerprint, 'identity': json.dumps(identity)}
        (part / ('base.json' if kind == 'base' else 'videos.json')).write_text(json.dumps(metadata))
        return {'fingerprint': fingerprint, 'path': str(part)}

    @staticmethod
    def base_sync_source():
        return 'select_mac_base_parts() {' + SCRIPT.read_text().split(
            'select_mac_base_parts() {', 1)[1].split('\n# Validate prepared content', 1)[0]

    def run_setup(self, parts, *, host='test', action='setup', overrides=None):
        game = self.root / 'installation'
        (game / 'system').mkdir(parents=True, exist_ok=True)
        for name in ('Gothic.ini', 'SystemPack.ini'):
            (game / 'system' / name).write_text(name)
        pointers = self.home / '.cache/gothic-ai/base/current'
        pointers.mkdir(parents=True, exist_ok=True)
        pointer = pointers / (hashlib.sha256(str(game).encode()).hexdigest() + '.json')
        contents = json.dumps(dict(version=1, gothic=str(game), **parts)) if parts is not None else None
        if contents is None:
            pointer.unlink(missing_ok=True)
        else:
            pointer.write_text(contents)
        (self.root / 'remote').mkdir(exist_ok=True)
        for name in ('remote-calls', 'copies.jsonl', 'requests.jsonl'):
            (self.root / name).unlink(missing_ok=True)
        ssh = self.bin / 'ssh'
        ssh.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
root = Path(os.environ['TEST_ROOT'])
command = sys.argv[-1].replace('/mnt/shared/gothic-ai/base', str(root / 'store'))
with (root / 'remote-calls').open('a') as stream:
    stream.write(json.dumps(shlex.split(command)) + '\n')
environment = {name: value for name, value in os.environ.items()
               if not name.startswith('OPENGOTHIC_') and name != 'GOTHIC_DIR'}
environment['HOME'] = str(root / 'remote')
body = None
if shlex.split(command)[:2] == ['bash', '-s']:
    body = sys.stdin.buffer.read().replace(b'/mnt/shared/gothic-ai/base', str(root / 'store').encode())
sys.exit(subprocess.run(['bash', '-c', command], input=body, env=environment,
                        cwd=root / 'remote').returncode)
''')
        ssh.chmod(0o755)
        rsync = self.bin / 'rsync'
        rsync.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import shutil
import sys
args = sys.argv[1:]
with (Path(os.environ['TEST_ROOT']) / 'copies.jsonl').open('a') as stream:
    stream.write(json.dumps(args) + '\n')
destination = Path(args[-1].removeprefix(os.environ['TEST_HOST'] + ':'))
if not destination.is_absolute():
    destination = Path(os.environ['TEST_ROOT']) / 'remote' / destination
for name in args[2:-1]:
    source = Path(name)
    if source.is_dir():
        shutil.copytree(source, destination, dirs_exist_ok=True)
    else:
        shutil.copyfile(source, destination / source.name)
''')
        rsync.chmod(0o755)
        queue = self.source / 'scripts/gq'
        queue.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import sys
with (Path(os.environ['TEST_ROOT']) / 'requests.jsonl').open('a') as stream:
    stream.write(json.dumps(sys.argv[1:]) + '\n')
''')
        queue.chmod(0o755)
        (self.bin / 'flock').write_text('#!/bin/sh\nexit 0\n')
        (self.bin / 'flock').chmod(0o755)
        setup = self.base_sync_source()
        harness = SCRIPT.read_text().split('\ntoolchain() {', 1)[0] + r'''
set -euo pipefail
ACTION="$TEST_ACTION"
ROOT="$TEST_ROOT/source"
cmake() {
  case "$1 $2 $3" in
    "-DBASE_ROOT=$ROOT/OpenGothic -P $ROOT/OpenGothic/cmake/BaseFormat.cmake") printf '%064d\n' 0 | tr 0 a ;;
    "-DVIDEO_ROOT=$ROOT/OpenGothic -P $ROOT/OpenGothic/cmake/VideoFormat.cmake") printf '%064d\n' 0 | tr 0 b ;;
    *) return 99 ;;
  esac
}
toolchain() { :; }
verify_base_part() { [[ ! -e "$2/not-ready" && ( -f "$2/base.json" || -f "$2/videos.json" ) ]]; }
''' + function('remote') + '\n' + setup
        if action != 'setup':
            preflight = SCRIPT.read_text().split('# Validate prepared content', 1)[1]
            harness += '\n# Validate prepared content' + preflight.split('\nif [[ "$ACTION" == bench ]]; then', 1)[0]
            env_args = next(line for line in SCRIPT.read_text().splitlines() if line.startswith('ENV_ARGS=('))
            harness += r'''
SESSION="$TEST_ROOT" STATE="$TEST_ROOT/remote/state" RUN=run JOBS=7 LOCK_WAIT=3 AI_PORT=''
OUTPUT="$SESSION/output" SSH_PID=$$ TUNNEL_OPEN=0 RUN_STARTED=0 RUN_FINISHED=0 RUN_RC=0
BENCH_SCENARIO=smoke.bench BENCH_HEADLESS=1 GQ_ARGS=(--class quick)
exec 8>"$SESSION/control"
: > "$OUTPUT"
guard_ack() { :; }
preparation_needed() { return 1; }
''' + '\n' + env_args + '\n'
            harness += '\n'.join(function(name) for name in (
                'run_quick', 'e2e_display_exclusive', 'bench_window', 'bench_display_exclusive', 'game_queue_args',
                'run_game'))
            harness += '\n' + ('run_quick' if action == 'pytest' else 'run_game')
            harness += ' env "${ENV_ARGS[@]}" bash "$STATE/source/scripts/ryzen.sh" __remote "$ACTION" "$STATE" "$RUN"\n'
        environment = {name: value for name, value in self.env.items()
                       if not name.startswith(('OPENGOTHIC_', 'RYZEN_', 'BENCH_')) and name != 'GOTHIC_DIR'}
        result = subprocess.run(['bash', '-c', harness], capture_output=True, text=True, timeout=5,
                                env=dict(environment, GOTHIC_DIR=str(game), RYZEN_HOST=host,
                                         TEST_HOST=host, TEST_ACTION=action, **(overrides or {})))
        if contents is not None:
            self.assertEqual(pointer.read_text(), contents)
        copies = self.root / 'copies.jsonl'
        return result, [json.loads(line) for line in copies.read_text().splitlines()] if copies.exists() else []

    def test_dev_ubuntu_setup_reuses_its_local_cache_without_shared_storage(self):
        base = self.setup_part('base', 'c' * 64, 'a' * 64)
        cached = self.root / 'remote/.cache/gothic-ai/base' / ('c' * 64)
        cached.mkdir(parents=True)
        shutil.copyfile(Path(base['path']) / 'base.json', cached / 'base.json')
        result, copies = self.run_setup({'base': base}, host='dev-ubuntu')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('dev-ubuntu has no shared store', result.stdout)
        self.assertIn(f'base ready at {cached}', result.stdout)
        self.assertEqual(len(copies), 1, copies)
        self.assertEqual(copies[0][-1], 'dev-ubuntu:gothic-ai/game-settings/system/')
        self.assertEqual(json.loads((self.root / 'remote/gothic-ai/game-base.json').read_text()),
                         {'base': 'c' * 64, 'videos': ''})

    def test_setup_keeps_pointer_parts_with_checkout_formats(self):
        parts = {
            kind: self.setup_part(kind, fingerprint * 64, digest * 64,
                                  path=self.root / f'pointer-{kind}')
            for kind, fingerprint, digest in (('base', 'c', 'a'), ('videos', 'd', 'b'))
        }
        result, copies = self.run_setup(parts)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([copy[2] for copy in copies[:2]], [part['path'] + '/' for part in parts.values()])
        self.assertEqual(json.loads((self.root / 'remote/gothic-ai/game-base.json').read_text()),
                         {kind: part['fingerprint'] for kind, part in parts.items()})
        self.assertNotIn('Mac pointer has importer format', result.stderr)

    def test_setup_uses_checkout_cache_parts_from_the_same_installation(self):
        parts = {
            kind: self.setup_part(kind, fingerprint * 64, 'f' * 64)
            for kind, fingerprint in (('base', 'c'), ('videos', 'd'))
        }
        expected = {}
        for kind, digest, fingerprint in (('base', 'a', 'e'), ('videos', 'b', 'f')):
            self.setup_part(kind, '0' * 64, digest * 64, source='another installation')
            expected[kind] = self.setup_part(kind, fingerprint * 64, digest * 64)
        result, copies = self.run_setup(parts)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual([copy[2] for copy in copies[:2]], [part['path'] + '/' for part in expected.values()])
        self.assertEqual(json.loads((self.root / 'remote/gothic-ai/game-base.json').read_text()),
                         {kind: part['fingerprint'] for kind, part in expected.items()})
        for kind, digest in (('base', 'a'), ('videos', 'b')):
            self.assertIn(f"Ryzen: Mac pointer has importer format {'f' * 64}, using Mac cache part "
                          f"{expected[kind]['fingerprint']} of format {digest * 64}", result.stderr)

    def test_setup_requires_decision_before_copying_when_checkout_format_is_missing(self):
        for kind, digest in (('base', 'a'), ('videos', 'b')):
            with self.subTest(kind=kind):
                parts = {
                    name: self.setup_part(name, fingerprint * 64,
                                          ('f' if name == kind else format_digest) * 64,
                                          path=self.root / f'{kind}-pointer-{name}')
                    for name, fingerprint, format_digest in (('base', 'c', 'a'), ('videos', 'd', 'b'))
                }
                result, copies = self.run_setup(parts)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn(f"needs_decision: Mac {kind} part {parts[kind]['fingerprint']} has importer "
                              f"format {'f' * 64}, this checkout builds {digest * 64};", result.stderr)
                self.assertIn('~/.cache/gothic-ai/base', result.stderr)
                self.assertEqual(copies, [])
                self.assertFalse((self.root / 'remote-calls').exists())
                self.assertFalse((self.root / 'remote/gothic-ai/game-base.json').exists())

    def remote_game_setup(self, *, base='0' * 64, videos=''):
        settings = self.root / 'remote/gothic-ai/game-settings/system'
        settings.mkdir(parents=True, exist_ok=True)
        for name in ('Gothic.ini', 'SystemPack.ini'):
            (settings / name).write_text('previous settings')
        pointer = settings.parent.parent / 'game-base.json'
        pointer.write_text(json.dumps({'base': base, 'videos': videos}) + '\n')
        return pointer

    def assert_synced_environment(self, expected, *, absent=()):
        calls = [json.loads(line) for line in (self.root / 'remote-calls').read_text().splitlines()]
        check = next(call for call in calls if call[-1] == '--check')
        self.assertEqual(check[0], 'env')
        for name, value in expected.items():
            self.assertIn(f'{name}={value}', check)
        requests = [json.loads(line) for line in (self.root / 'requests.jsonl').read_text().splitlines()]
        self.assertEqual(len(requests), 1, requests)
        for name, value in expected.items():
            self.assertIn(f'{name}={value}', requests[0])
        for name in absent:
            for arguments in (check, requests[0]):
                self.assertFalse(any(argument.startswith(f'{name}=') for argument in arguments), arguments)

    def test_autosync_ready_parts_preserve_host_pointer_and_forward_exact_fingerprints(self):
        parts = {kind: self.setup_part(kind, fingerprint * 64, digest * 64)
                 for kind, fingerprint, digest in (('base', 'c', 'a'), ('videos', 'd', 'b'))}
        pointer = self.remote_game_setup()
        contents = pointer.read_bytes()
        # The base can already be in the shared store, while videos are cached locally.
        for kind, part in parts.items():
            destination = ((self.root / 'store') if kind == 'base' else
                           self.root / 'remote/.cache/gothic-ai/base/videos') / part['fingerprint']
            shutil.copytree(part['path'], destination)
        for action in ('bench', 'e2e', 'pytest'):
            with self.subTest(action=action):
                result, copies = self.run_setup(parts, action=action)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(len(copies), 1, copies)
                self.assertIn('--checksum', copies[0])
                self.assertEqual(copies[0][-1], 'test:gothic-ai/game-settings/system/')
                self.assertIn('test: base cccccccccccc ready', result.stdout)
                self.assertIn('test: videos dddddddddddd ready', result.stdout)
                self.assertEqual(pointer.read_bytes(), contents)
                for name in ('Gothic.ini', 'SystemPack.ini'):
                    self.assertEqual((pointer.parent / 'game-settings/system' / name).read_text(), name)
                self.assert_synced_environment({f'OPENGOTHIC_{kind.upper()}_FINGERPRINT': part['fingerprint']
                                                for kind, part in parts.items()})

    def test_autosync_initializes_dev_ubuntu_without_shared_storage(self):
        parts = {kind: self.setup_part(kind, fingerprint * 64, digest * 64)
                 for kind, fingerprint, digest in (('base', 'c', 'a'), ('videos', 'd', 'b'))}
        result, copies = self.run_setup(parts, host='dev-ubuntu', action='bench')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(copies), 3, copies)
        for kind, part in parts.items():
            self.assertIn(f"dev-ubuntu: {kind} {part['fingerprint'][:12]} copied in ", result.stdout)
            relative = ('' if kind == 'base' else 'videos/') + part['fingerprint']
            cached = self.root / 'remote/.cache/gothic-ai/base' / relative
            self.assertTrue((cached / ('base.json' if kind == 'base' else 'videos.json')).is_file())
        pointer = self.root / 'remote/gothic-ai/game-base.json'
        self.assertEqual(json.loads(pointer.read_text()), {kind: part['fingerprint'] for kind, part in parts.items()})
        for name in ('Gothic.ini', 'SystemPack.ini'):
            self.assertEqual((pointer.parent / 'game-settings/system' / name).read_text(), name)
        self.assert_synced_environment({f'OPENGOTHIC_{kind.upper()}_FINGERPRINT': part['fingerprint']
                                        for kind, part in parts.items()})
        # Missing settings also require first-time setup, even with an existing pointer.
        pointer.write_text(json.dumps({'base': '0' * 64, 'videos': ''}))
        (pointer.parent / 'game-settings/system/Gothic.ini').unlink()
        result, copies = self.run_setup(parts, host='dev-ubuntu', action='bench')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(copies), 1, copies)
        self.assertEqual(json.loads(pointer.read_text()), {kind: part['fingerprint'] for kind, part in parts.items()})
        self.assertFalse((self.root / 'store').exists())

    def test_autosync_missing_checkout_base_fails_game_but_allows_pytest(self):
        parts = {'base': self.setup_part('base', 'c' * 64, 'f' * 64)}
        pointer = self.remote_game_setup()
        contents = pointer.read_bytes()
        self.setup_part('base', '0' * 64, 'f' * 64,
                        path=self.root / 'remote/.cache/gothic-ai/base' / ('0' * 64))
        self.setup_part('base', '1' * 64, 'a' * 64, source='another installation',
                        path=self.root / 'remote/.cache/gothic-ai/base' / ('1' * 64))
        for action in ('bench', 'e2e'):
            with self.subTest(action=action):
                result, copies = self.run_setup(parts, action=action,
                                                 overrides={'OPENGOTHIC_VIDEOS_FINGERPRINT': ''})
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('Start the game once on the Mac on this version', result.stderr)
                self.assertEqual(len(result.stderr.splitlines()), 1, result.stderr)
                self.assertEqual(copies, [])
                self.assertTrue((self.root / 'remote-calls').exists())
                self.assertFalse((self.root / 'requests.jsonl').exists())
        result, copies = self.run_setup(parts, action='pytest', overrides={'OPENGOTHIC_VIDEOS_FINGERPRINT': ''})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(result.stderr.splitlines()), 1, result.stderr)
        self.assertIn('Warning: pytest:', result.stderr)
        self.assertIn('Start the game once on the Mac on this version', result.stderr)
        self.assertEqual(len(copies), 1, copies)
        self.assertEqual(pointer.read_bytes(), contents)
        self.assert_synced_environment({'OPENGOTHIC_VIDEOS_FINGERPRINT': ''},
                                        absent=('OPENGOTHIC_BASE_FINGERPRINT',))

    def test_autosync_uses_compatible_host_parts_when_mac_pointer_parts_were_pruned(self):
        parts = {kind: self.setup_part(kind, fingerprint * 64, 'f' * 64, source='Mac installation')
                 for kind, fingerprint in (('base', 'c'), ('videos', 'd'))}
        for part in parts.values():
            shutil.rmtree(part['path'])
        pointer = self.remote_game_setup(videos='1' * 64)
        contents = pointer.read_bytes()
        host_cache = self.root / 'remote/.cache/gothic-ai/base'
        for kind, anchor, ready, digest in (('base', '0', 'e', 'a'), ('videos', '1', 'f', 'b')):
            directory = host_cache / ('' if kind == 'base' else 'videos')
            self.setup_part(kind, anchor * 64, 'f' * 64, source='host installation',
                            path=directory / (anchor * 64))
            self.setup_part(kind, '2' * 64, digest * 64, source='foreign installation',
                            path=directory / ('2' * 64))
            unready = self.setup_part(kind, '3' * 64, digest * 64, source='host installation',
                                     path=directory / ('3' * 64))
            (Path(unready['path']) / 'not-ready').touch()
            self.setup_part(kind, ready * 64, digest * 64, source='host installation',
                            path=directory / (ready * 64))
        for action, location in (('bench', 'cache'), ('e2e', 'store')):
            with self.subTest(location=location):
                if location == 'store':
                    (self.root / 'store').mkdir()
                    shutil.move(str(host_cache / ('e' * 64)), self.root / 'store' / ('e' * 64))
                result, copies = self.run_setup(parts, action=action)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(result.stderr, '')
                self.assertIn('test: base eeeeeeeeeeee ready (host)', result.stdout)
                self.assertEqual(len(copies), 1, copies)
                self.assertEqual(pointer.read_bytes(), contents)
                self.assert_synced_environment({'OPENGOTHIC_BASE_FINGERPRINT': 'e' * 64,
                                                'OPENGOTHIC_VIDEOS_FINGERPRINT': 'f' * 64})

    def test_autosync_retains_host_video_pointer_without_compatible_videos(self):
        parts = {'base': self.setup_part('base', 'c' * 64, 'a' * 64),
                 'videos': self.setup_part('videos', 'd' * 64, 'f' * 64)}
        pointer = self.remote_game_setup(videos='1' * 64)
        contents = pointer.read_bytes()
        host_cache = self.root / 'remote/.cache/gothic-ai/base'
        shutil.copytree(parts['base']['path'], host_cache / ('c' * 64))
        self.setup_part('videos', '1' * 64, 'f' * 64, path=host_cache / 'videos' / ('1' * 64))
        result, copies = self.run_setup(parts, action='e2e')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(copies), 1, copies)
        self.assertEqual(pointer.read_bytes(), contents)
        self.assert_synced_environment({'OPENGOTHIC_BASE_FINGERPRINT': 'c' * 64},
                                        absent=('OPENGOTHIC_VIDEOS_FINGERPRINT',))

    def test_autosync_respects_explicit_base_and_video_choices(self):
        self.remote_game_setup()
        cases = [
            ({'OPENGOTHIC_BASE_FINGERPRINT': 'e' * 64}, 'base'),
            ({'OPENGOTHIC_BASE': '/remote/custom-base'}, 'base'),
            ({'OPENGOTHIC_VIDEOS_FINGERPRINT': 'f' * 64}, 'videos'),
            ({'OPENGOTHIC_VIDEOS_FINGERPRINT': ''}, 'videos'),
        ]
        for index, (overrides, skipped) in enumerate(cases):
            with self.subTest(overrides=overrides):
                parts = {kind: self.setup_part(kind, fingerprint * 64,
                                              ('0' if kind == skipped else digest) * 64,
                                              path=self.root / f'override-{index}-{kind}')
                         for kind, fingerprint, digest in (('base', 'c', 'a'), ('videos', 'd', 'b'))}
                result, copies = self.run_setup(parts, action='e2e', overrides=overrides)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertNotIn(f'test: {skipped} ', result.stdout)
                self.assertFalse(any(copy[2] == parts[skipped]['path'] + '/' for copy in copies))
                other = 'videos' if skipped == 'base' else 'base'
                self.assert_synced_environment(dict(overrides, **{
                    f'OPENGOTHIC_{other.upper()}_FINGERPRINT': parts[other]['fingerprint']}))
        # Fully explicit content does not need a Mac pointer at all.
        result, copies = self.run_setup(None, action='e2e', overrides={
            'OPENGOTHIC_BASE_FINGERPRINT': 'e' * 64, 'OPENGOTHIC_VIDEOS_FINGERPRINT': '',
        })
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(copies), 1, copies)
        self.assert_synced_environment({'OPENGOTHIC_BASE_FINGERPRINT': 'e' * 64,
                                        'OPENGOTHIC_VIDEOS_FINGERPRINT': ''})

    @unittest.skipUnless(shutil.which('cmake'), 'cmake is not available')
    def test_importer_format_scripts_print_one_sha256(self):
        engine = SCRIPT.parents[1] / 'OpenGothic'
        cmake = shutil.which('cmake')
        for kind in ('Base', 'Video'):
            with self.subTest(kind=kind):
                result = subprocess.run([cmake, f'-D{kind.upper()}_ROOT={engine}',
                                         '-P', str(engine / 'cmake' / f'{kind}Format.cmake')],
                                        capture_output=True, text=True, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertRegex(result.stdout, r'\A[0-9a-f]{64}\n\Z')

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
                                           ('100,100', '1010,100', '100,560')[slot])))
                if explicit.get('TEMPEST_WINDOW_SIZE', '640x360') != '640x360':
                    expected['TEMPEST_WINDOW_POS'] = '0,0'
                expected.update(explicit)
                self.assertEqual(json.loads((self.root / 'e2e-env.json').read_text()), expected)

    def test_bench_keeps_its_window_size_and_ports(self):
        self.entry(self.engine_digest)
        self.entry(self.story_digest, 'storygraph')
        for name in ('PORT', 'ASR_PORT', 'TTS_PORT', 'TEMPEST_WINDOW_POS', 'RYZEN_BENCH_COLUMN'):
            self.env.pop(name, None)
        self.env.update(GQ_DISPLAY_SLOT='2', TEMPEST_WINDOW_SIZE='960x540',
                        RYZEN_BENCH_ARCHIVE=str(self.root/'archive'))
        (self.bin / 'ffmpeg').write_text('#!/bin/sh\nexit 0\n')
        (self.bin / 'ffmpeg').chmod(0o755)
        (self.source / 'scripts/bench.sh').write_text('''
python3 - "$TEST_ROOT/bench-env.json" <<'PY'
import json
import os
import sys
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({key: os.environ.get(key) for key in
    ('PORT', 'ASR_PORT', 'TTS_PORT', 'TEMPEST_WINDOW_SIZE', 'TEMPEST_WINDOW_POS')}))
PY
''')
        self.shell(r'''
remote_probe bench "$TEST_ROOT/worktree" run smoke
remote_bench "$TEST_ROOT/worktree" run smoke
''')
        self.assertEqual(json.loads((self.root / 'bench-env.json').read_text()),
                         {'PORT': None, 'ASR_PORT': None, 'TTS_PORT': None,
                          'TEMPEST_WINDOW_SIZE': '960x540', 'TEMPEST_WINDOW_POS': None})
        self.assertTrue((self.root/'archive/game.sh').is_file())

    def test_offscreen_bench_runs_without_desktop_or_display(self):
        self.entry(self.engine_digest)
        self.entry(self.story_digest, 'storygraph')
        self.env.pop('TEMPEST_WINDOW_SIZE', None)
        self.env.update(RYZEN_BENCH_OFFSCREEN='1', DISPLAY=':0', WAYLAND_DISPLAY='wayland-0',
                        XAUTHORITY='/run/user/1000/.mutter-Xwaylandauth.TEST')
        (self.bin / 'ffmpeg').write_text('#!/bin/sh\nexit 0\n')
        (self.bin / 'ffmpeg').chmod(0o755)
        names = ('DISPLAY', 'WAYLAND_DISPLAY', 'XAUTHORITY', 'TEMPEST_OFFSCREEN', 'TEMPEST_WINDOW_SIZE',
                 'TEMPEST_WINDOW_POS', 'TEST_NVIDIA_VULKAN')
        (self.source / 'scripts/bench.sh').write_text('''
python3 - "$TEST_ROOT/bench-env.json" <<'PY'
import json
import os
import sys
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({key: os.environ.get(key) for key in %r}))
PY
''' % (names,))
        result = self.shell(r'''
desktop() { echo 'offscreen looked for a desktop' >&2; return 91; }
nvidia_vulkan() { export TEST_NVIDIA_VULKAN=1; }
remote_probe bench "$TEST_ROOT/worktree" run smoke
remote_bench "$TEST_ROOT/worktree" run smoke
''')
        self.assertIn('Ryzen: bench offscreen 800x600, no display', result.stdout)
        self.assertEqual(json.loads((self.root / 'bench-env.json').read_text()),
                         dict.fromkeys(names[:3]) | {'TEMPEST_OFFSCREEN': '1', 'TEMPEST_WINDOW_SIZE': '800x600',
                                                     'TEMPEST_WINDOW_POS': None, 'TEST_NVIDIA_VULKAN': '1'})

    def bench_in_column(self, column, screen='1920x1080'):
        for name in ('TEMPEST_WINDOW_SIZE', 'TEMPEST_WINDOW_POS'):
            self.env.pop(name, None)
        self.env.update(GQ_DISPLAY_SLOT=str(column), RYZEN_BENCH_COLUMN='1', TEST_SCREEN_SIZE=screen)
        (self.bin / 'ffmpeg').write_text('#!/bin/sh\nexit 0\n')
        (self.bin / 'ffmpeg').chmod(0o755)
        (self.source / 'scripts/bench.sh').write_text('''
python3 - "$TEST_ROOT/bench-env.json" <<'PY'
import json
import os
import sys
from pathlib import Path
Path(sys.argv[1]).write_text(json.dumps({key: os.environ.get(key) for key in
    ('TEMPEST_WINDOW_SIZE', 'TEMPEST_WINDOW_POS')}))
PY
''')
        return self.shell(r'''
remote_probe bench "$TEST_ROOT/worktree" run smoke
remote_bench "$TEST_ROOT/worktree" run smoke && rc=0 || rc=$?
echo "bench rc=$rc"
''')

    def test_shared_bench_takes_the_window_position_of_its_column(self):
        self.entry(self.engine_digest)
        self.entry(self.story_digest, 'storygraph')
        for column, position in ((0, '100,100'), (1, '1010,100')):
            with self.subTest(column=column):
                result = self.bench_in_column(column)
                self.assertIn('bench rc=0', result.stdout)
                self.assertIn(f'Ryzen: bench column {column}; window 800x600 at {position}; screen 1920x1080',
                              result.stdout)
                self.assertEqual(json.loads((self.root / 'bench-env.json').read_text()),
                                 {'TEMPEST_WINDOW_SIZE': '800x600', 'TEMPEST_WINDOW_POS': position})

    def test_shared_bench_column_that_does_not_fit_the_screen_exits_2(self):
        self.entry(self.engine_digest)
        self.entry(self.story_digest, 'storygraph')
        result = self.bench_in_column(1, screen='1600x900')
        self.assertIn('bench rc=2', result.stdout)
        self.assertIn('Bench window 800x600 in column 1 does not fit screen 1600x900', result.stderr)

    def test_windows_of_every_admitted_combination_fit_without_overlap(self):
        # Ask the scripts for every slot and column, then combine them as gq admits:
        # each column holds one bench or e2e windows in all of its slots.
        result = self.shell(r'''
RYZEN_SCREEN_SIZE=1920x1080
for slot in 0 1 2; do
  (TEMPEST_WINDOW_SIZE=640x360; unset TEMPEST_WINDOW_POS; e2e_window_position "$slot"; echo "slot $slot $TEMPEST_WINDOW_SIZE $TEMPEST_WINDOW_POS")
done
for column in 0 1; do
  (unset TEMPEST_WINDOW_SIZE TEMPEST_WINDOW_POS; GQ_DISPLAY_SLOT=$column bench_window_position >/dev/null
   echo "column $column $TEMPEST_WINDOW_SIZE $TEMPEST_WINDOW_POS")
done
''')
        windows = {}
        for line in result.stdout.splitlines():
            kind, number, size, position = line.split()
            (width, height), (x, y) = (map(int, size.split('x')), map(int, position.split(',')))
            # Mutter's frame and shadow: 640x360 -> 690x447, 800x600 -> 850x687 (measured).
            windows[kind, int(number)] = (x, y, x + width + 50, y + height + 87)
        columns = [[0, 2], [1]]  # gq PROFILES["ryzen"]["display_columns"]
        combinations = []
        for benches in ((), (0,), (1,), (0, 1)):
            frames = [windows['column', column] for column in benches]
            frames += [windows['slot', slot] for column, slots in enumerate(columns)
                       if column not in benches for slot in slots]
            combinations.append(frames)
        self.assertEqual(sorted(map(len, combinations)), [2, 2, 3, 3])
        for frames in combinations:
            for left, top, right, bottom in frames:
                self.assertTrue(0 <= left and 0 <= top and right <= 1920 and bottom <= 1080, frames)
            for index, first in enumerate(frames):
                for second in frames[index + 1:]:
                    overlap = (first[0] < second[2] and second[0] < first[2]
                               and first[1] < second[3] and second[1] < first[3])
                    self.assertFalse(overlap, (first, second))

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
        for call in calls:
            if '--build' not in call:
                for language in ('C', 'CXX'):
                    self.assertIn(f'-DCMAKE_{language}_FLAGS_RELWITHDEBINFO=-O2 -g1 -DNDEBUG', call)
        for entry, name in [(self.cache / self.engine_digest, 'Gothic2Notr'),
                            (self.cache / 'storygraph' / self.story_digest, 'storygraph')]:
            self.assertTrue(os.access(entry / name, os.X_OK))
        self.assertEqual((self.cache / self.engine_digest / 'runtime.dat').read_text(), 'runtime')
        self.assertTrue(os.access(self.cache / self.engine_digest / 'content-convert', os.X_OK))
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
            converter = source / 'content-convert'
            converter.write_text('#!/bin/sh\nexit 0\n')
            converter.chmod(0o755)
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

    def test_storygraph_digest_tracks_engine_and_converter_sources(self):
        # storygraph links base and video import from the engine tree, so it
        # shares the engine digest: OpenGothic plus scripts/convert.
        inputs = ['OpenGothic/ai_bridge/storygraph/main.cpp', 'OpenGothic/ai_bridge/textcodec.cpp',
                  'OpenGothic/lib/ZenKit/src/Archive.cc', 'OpenGothic/common/basestore.cpp',
                  'OpenGothic/game/main.cpp', 'scripts/convert/main.cpp', 'scripts/ryzen-build.sh']
        for relative in inputs:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(relative)

        def digest(function):
            result = subprocess.run(
                ['bash', '-c', definitions() + '\nROOT="$TEST_ROOT/source"\n' + function],
                env=self.env, capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            return result.stdout.strip()

        original = digest('storygraph_digest')
        self.assertEqual(digest('source_digest'), original)
        for relative in inputs:
            with self.subTest(source=relative):
                path = self.source / relative
                path.write_text(relative + ' changed')
                self.assertNotEqual(digest('storygraph_digest'), original)
                path.write_text(relative)
        self.assertEqual(digest('storygraph_digest'), original)
        for relative in ['OpenGothic/ai_bridge/storygraph/build/generated.cpp',
                         'OpenGothic/build/generated.cpp', 'OpenGothic/lib/ZenKit/build/generated.cpp',
                         'scripts/convert/build/generated.cpp', 'scripts/other/tool.py']:
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('generated output')
        self.assertEqual(digest('storygraph_digest'), original)


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

    def test_balance_uploads_with_the_same_run_name_keep_both_snapshots(self):
        source = SCRIPT.with_name('melee-balance-remote.sh').read_text()
        run_series = re.search(r'^run_series\(\) \{\n.*?^\}', source, re.M | re.S).group(0)
        remote = self.root / 'remote'
        remote.mkdir()
        content = self.root / 'content'
        content.mkdir()
        clip = content / 'clip.man'
        (self.binaries / 'rsync').write_text(f'#!{sys.executable}\n' + r'''
import os
from pathlib import Path
import shutil
import sys
host, destination = sys.argv[-1].split(':', 1)
assert host == 'test'
shutil.copytree(sys.argv[-2], Path(os.environ['TEST_ROOT']) / 'remote' / destination, dirs_exist_ok=True)
''')
        harness = r'''
set -euo pipefail
HOST=test TASK=gothic-ai/melee-balance-test RUN_NAME=shared
EPISODES=1 SEED=1 TIMEOUT_MS=1000 SERIES=native-posture-on-1h
ssh() { (cd "$TEST_ROOT/remote" && bash -c "${@: -1}"); }
remote_script() { cat >/dev/null; printf '%s\n' "$8"; }
''' + run_series + '\nrun_series\n'
        snapshots = []
        for value in ('first', 'second'):
            clip.write_text(value)
            result = subprocess.run(['bash', '-c', harness],
                                    env=dict(self.env, OPENGOTHIC_CONTENT=str(content)),
                                    capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            snapshots.append(remote / result.stdout.strip())
        self.assertNotEqual(snapshots[0], snapshots[1])
        self.assertEqual((snapshots[0] / 'clip.man').read_text(), 'first')
        self.assertEqual((snapshots[1] / 'clip.man').read_text(), 'second')

    def test_balance_existing_series_does_not_create_content_snapshot(self):
        source = SCRIPT.with_name('melee-balance-remote.sh').read_text()
        run_series = re.search(r'^run_series\(\) \{\n.*?^\}', source, re.M | re.S).group(0)
        # Keep the NAS existence check inside the same fake remote filesystem.
        run_series = run_series.replace('/mnt/shared/', str(self.root / 'archive') + '/')
        remote = self.root / 'remote'
        remote.mkdir()
        content = self.root / 'content'
        content.mkdir()
        harness = r'''
set -euo pipefail
HOST=test TASK=gothic-ai/melee-balance-test RUN_NAME=shared
EPISODES=1 SEED=1 TIMEOUT_MS=1000 SERIES=native-posture-on-1h,native-vanilla-1h
ssh() { (cd "$TEST_ROOT/remote" && bash -c "${@: -1}"); }
remote_script() { touch "$TEST_ROOT/job-submitted"; }
''' + run_series + '\nrun_series\n'
        for parent in (remote, self.root / 'archive'):
            with self.subTest(parent=parent):
                existing = parent / 'gothic-ai/melee-balance-test/runs/shared/native-vanilla-1h'
                existing.mkdir(parents=True)
                result = subprocess.run(['bash', '-c', harness],
                                        env=dict(self.env, OPENGOTHIC_CONTENT=str(content)),
                                        capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
                self.assertIn('Series exists: native-vanilla-1h; choose a fresh run name', result.stderr)
                self.assertFalse((remote / 'gothic-ai/melee-balance-test/inputs').exists())
                self.assertFalse((self.root / 'rsync.jsonl').exists())
                self.assertFalse((self.root / 'job-submitted').exists())
                existing.rmdir()

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

    def test_bench_routes_content_and_remote_base_paths_to_ryzen(self):
        forwarding = SCRIPT.read_text().split(
            "  # Preserve game options, translating paths into this run's output directory.\n", 1)[1]
        forwarding = forwarding.split('  HD_TEXTURES=', 1)[0]
        result = subprocess.run(
            ['bash', '-ec', 'ENV_ARGS=()\nbench_path() { printf "%s" "$1"; }\n' + forwarding
             + '\nprintf "%s\\n" "${ENV_ARGS[@]}"\n'],
            env=dict(self.env, TIMEOUT='71', BENCH_PROGRESS='13', BENCH_STALL='29', RUNNER_BIN='/Users/x/runner'),
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        for option in ('TIMEOUT=71', 'BENCH_PROGRESS=13', 'BENCH_STALL=29'):
            self.assertIn(option, result.stdout.splitlines())
        self.assertNotIn('RUNNER_BIN=/Users/x/runner', result.stdout.splitlines())
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
                env = dict(self.env, BENCH_HOST='ryzen',
                           OPENGOTHIC_BASE_CACHE='/home/ryzen/gothic-ai/p18d/cache',
                           OPENGOTHIC_BASE_STORE='/home/ryzen/gothic-ai/p18d/store')
                env.pop('KEEP', None)
                if value is not None:
                    env['OPENGOTHIC_CONTENT'] = value
                out = self.root / f'out-{index}'
                result = subprocess.run(['bash', str(scripts / 'bench.sh'), str(scenario), str(out)],
                                        env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(json.loads((out / 'content.json').read_text()), value)
                self.assertEqual((out / 'host.txt').read_text(), 'ryzen\n')

    def bench_game(self, mode, *, marked=False, scenario_name='smoke', report=False,
                   remote_results=None, **settings):
        scripts = self.root / 'scripts'
        scripts.mkdir()
        shutil.copyfile(BENCH_SCRIPT, scripts / 'bench.sh')
        (scripts / 'base-ensure.sh').write_text('exit 0\n')
        (scripts / 'bench_report.py').write_text(
            'from pathlib import Path\nimport sys\n'
            'Path(sys.argv[1], "report-built").touch()\n')
        if report:
            shutil.copyfile(SCRIPT.with_name('bench_report.py'), scripts / 'bench_report.py')
        if remote_results is not None:
            launcher = scripts / 'ryzen.sh'
            launcher.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import sys
root = Path(os.environ['TEST_ROOT'])
host = os.environ.get('RYZEN_HOST', 'ryzen')
with (root / 'routes.jsonl').open('a') as stream:
    stream.write(json.dumps({'host': host, 'wait': os.environ['RYZEN_LOCK_WAIT']}) + '\n')
result = json.loads(os.environ['TEST_REMOTE_RESULTS'])[host]
if result.get('game_log'):
    Path(sys.argv[3], 'game.log').write_text('mock remote game\n')
print(result['message'])
print('Ryzen: queue guard complete mock-game')
print('Ryzen: removed runs/mock and inputs/mock')
print(f'bench: output in {sys.argv[3]}')
sys.exit(result['rc'])
''')
            launcher.chmod(0o755)
        scenario = self.root / f'{scenario_name}.bench'
        scenario.write_text(('# @headless\n' if marked else '') + 'quit\n')
        game = self.binaries / 'game'
        game.write_text(f'#!{sys.executable}\n' + r'''
import json
import os
from pathlib import Path
import signal
import sys
import time
root = Path(os.environ['TEST_ROOT'])
(root / 'game.pid').write_text(str(os.getpid()))
(root / 'invocation.json').write_text(json.dumps({
    'binary': Path(sys.argv[0]).name, 'args': sys.argv[1:], 'cwd': os.getcwd(),
    'bench_class': os.environ.get('RYZEN_BENCH_CLASS'),
    'ini': Path('Gothic.ini').read_text(),
    'save': Path('save_slot_1.sav').read_text() if Path('save_slot_1.sav').exists() else None,
    'env': {key: value for key, value in os.environ.items()
            if key.startswith(('OPENGOTHIC_', 'TEMPEST_'))},
}))
mode = os.environ['TEST_MODE']
if mode in ('done', 'keep'):
    print('[bench] done', flush=True)
    if Path(sys.argv[0]).name != 'game':
        print(json.dumps({'status': 'done'}), flush=True)
if mode == 'stall':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
if mode == 'progress':
    out = Path(os.environ['OPENGOTHIC_BENCH_OUT'])
    heartbeat = {'elapsed_seconds': 1680, 'frame': 123, 'command_index': 5,
                 'command_count': 5, 'command': 'npcday 1440', 'game_time': '18:40',
                 'npcday_elapsed_minutes': 1060.0033, 'npcday_target_minutes': 1440}
    temporary = out / 'heartbeat.tmp'
    temporary.write_text(json.dumps(heartbeat))
    temporary.replace(out / 'heartbeat.json')
if mode not in ('done', 'exit'):
    while not (root / 'finish').exists():
        time.sleep(0.02)
    print('[bench] done', flush=True)
''')
        game.chmod(0o755)
        for name in ('gothic-body-runner', 'custom-runner'):
            runner = self.binaries / name
            shutil.copyfile(game, runner)
            runner.chmod(0o755)
        out = self.root / 'out'
        env = dict(self.env, BENCH_HOST='local', NO_BUILD='1', WORLD='NEWWORLD.ZEN',
                   GOTHIC_DIR=str(self.root / 'data'), GAME_BIN=str(game), TEST_MODE=mode,
                   TIMEOUT='10', BENCH_PROGRESS='0', BENCH_STALL='0', KEEP='')
        if remote_results is not None:
            env['TEST_REMOTE_RESULTS'] = json.dumps(remote_results)
        env.update(settings)
        process = subprocess.Popen(['bash', str(scripts / 'bench.sh'), str(scenario), str(out)],
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

        def cleanup():
            pid_file = self.root / 'game.pid'
            if pid_file.exists():
                try:
                    os.kill(int(pid_file.read_text()), signal.SIGKILL)
                except ProcessLookupError:
                    pass
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=5)

        self.addCleanup(cleanup)
        return process, out

    def test_headless_auto_uses_dev_ubuntu_and_keeps_a_returned_game_failure(self):
        process, out = self.bench_game('done', marked=True, BENCH_HOST='auto',
                                       DEV_UBUNTU_HOST='bench-dev',
                                       GOTHIC_DIR='/Volumes/DevSSD/Dev/games/Gothic II',
                                       remote_results={
                                           'bench-dev': {'rc': 1, 'message': 'game failed', 'game_log': True},
                                       })
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 1, stdout + stderr)
        self.assertEqual([json.loads(line) for line in (self.root / 'routes.jsonl').read_text().splitlines()],
                         [{'host': 'bench-dev', 'wait': '120'}])
        self.assertEqual((out / 'host.txt').read_text(), 'dev-ubuntu\n')
        self.assertNotIn('falling back', stderr)
        self.assertFalse((self.root / 'invocation.json').exists())

    def test_headless_auto_falls_back_through_ryzen_to_mac_with_reasons(self):
        process, out = self.bench_game('done', marked=True, BENCH_HOST='auto',
                                       GOTHIC_DIR='/Volumes/DevSSD/Dev/games/Gothic II',
                                       remote_results={
                                           'dev-ubuntu': {'rc': 255, 'message': 'dev-ubuntu setup unavailable'},
                                           'ryzen': {'rc': 75, 'message': 'Ryzen queue busy'},
                                       })
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        self.assertEqual([json.loads(line) for line in (self.root / 'routes.jsonl').read_text().splitlines()],
                         [{'host': 'dev-ubuntu', 'wait': '120'}, {'host': 'ryzen', 'wait': '120'}])
        fallbacks = [line for line in stderr.splitlines() if 'falling back' in line]
        self.assertEqual(len(fallbacks), 2, stderr)
        self.assertIn('falling back to ryzen', fallbacks[0].lower())
        self.assertIn('dev-ubuntu setup unavailable', fallbacks[0])
        self.assertIn('Mac', fallbacks[1])
        self.assertIn('Ryzen queue busy', fallbacks[1])
        self.assertEqual(json.loads((self.root / 'invocation.json').read_text())['binary'],
                         'gothic-body-runner')
        self.assertEqual((out / 'host.txt').read_text(), 'mac\n')

    def test_window_auto_uses_ryzen_then_mac_without_dev_ubuntu(self):
        process, out = self.bench_game('done', BENCH_HOST='auto',
                                       GOTHIC_DIR='/Volumes/DevSSD/Dev/games/Gothic II',
                                       remote_results={'ryzen': {'rc': 255, 'message': 'Ryzen SSH unavailable'}})
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        self.assertEqual([json.loads(line) for line in (self.root / 'routes.jsonl').read_text().splitlines()],
                         [{'host': 'ryzen', 'wait': '120'}])
        fallbacks = [line for line in stderr.splitlines() if 'falling back' in line]
        self.assertEqual(len(fallbacks), 1, stderr)
        self.assertIn('Ryzen SSH unavailable', fallbacks[0])
        self.assertEqual((out / 'host.txt').read_text(), 'mac\n')

    def test_explicit_dev_ubuntu_rejects_window_scenarios_before_ssh(self):
        ssh = self.binaries / 'ssh'
        ssh.write_text('#!/bin/sh\nprintf called > "$TEST_ROOT/unexpected-transport"\nexit 99\n')
        ssh.chmod(0o755)
        scenario = self.root / 'window.bench'
        for marked, override in ((False, {}), (True, {'BENCH_HEADLESS': '0'})):
            with self.subTest(marked=marked):
                scenario.write_text(('# @headless\n' if marked else '') + 'quit\n')
                result = subprocess.run(
                    ['bash', str(BENCH_SCRIPT), str(scenario), str(self.root / 'out')],
                    env=dict(self.env, BENCH_HOST='dev-ubuntu', **override),
                    capture_output=True, text=True, timeout=5,
                )
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertIn('окна на dev-ubuntu нет', result.stderr)
        self.assertFalse((self.root / 'unexpected-transport').exists())

    def test_dev_ubuntu_archive_is_rejected_before_ssh_or_upload(self):
        for name in ('ssh', 'rsync'):
            command = self.binaries / name
            command.write_text('#!/bin/sh\nprintf called > "$TEST_ROOT/unexpected-transport"\nexit 99\n')
            command.chmod(0o755)
        scenario = self.root / 'headless.bench'
        scenario.write_text('# @headless\nquit\n')
        result = subprocess.run(
            ['bash', str(SCRIPT), 'bench', str(scenario), str(self.root / 'out')],
            env=dict(self.env, RYZEN_HOST='dev-ubuntu', WORLD='NEWWORLD.ZEN',
                     RYZEN_BENCH_ARCHIVE='/task/result'),
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn('RYZEN_BENCH_ARCHIVE', result.stderr)
        self.assertIn('dev-ubuntu', result.stderr)
        self.assertFalse((self.root / 'unexpected-transport').exists())

    def test_dev_ubuntu_du_lists_local_worktrees_without_ryzen_janitor(self):
        task = self.root / 'remote/gothic-ai/e2e/own'
        task.mkdir(parents=True)
        (task / 'result').write_text('local result')
        ssh = self.binaries / 'ssh'
        ssh.write_text(f'#!{sys.executable}\n' + r'''
import os
from pathlib import Path
import shlex
import sys
root = Path(os.environ['TEST_ROOT'])
assert sys.argv[-2] == 'dev-ubuntu', sys.argv
assert shlex.split(sys.argv[-1]) == ['bash', '-s'], sys.argv
os.environ['HOME'] = str(root / 'remote')
os.execvp('bash', ['bash', '-c', sys.argv[-1]])
''')
        ssh.chmod(0o755)
        df = self.binaries / 'df'
        df.write_text('#!/bin/sh\nprintf "Avail\\n100G\\n"\n')
        df.chmod(0o755)
        result = subprocess.run(
            ['bash', str(SCRIPT), 'du'], env=dict(self.env, RYZEN_HOST='dev-ubuntu'),
            capture_output=True, text=True, timeout=5,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('dev-ubuntu has no shared store or Ryzen janitor', result.stdout)
        self.assertIn('disk free 100G', result.stdout)
        self.assertIn('own=', result.stdout)
        self.assertEqual((task / 'result').read_text(), 'local result')

    def test_bench_done_succeeds(self):
        process, out = self.bench_game('done')
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        self.assertTrue((out / 'report-built').exists())
        invocation = json.loads((self.root / 'invocation.json').read_text())
        self.assertEqual(invocation['binary'], 'game')
        self.assertEqual(invocation['args'], ['-g', str(self.root / 'data'), '-window', '-devmode',
                                             '-nomenu', '-w', 'NEWWORLD.ZEN', '-nointro'])

    def test_headless_marker_uses_sibling_runner_and_same_game_environment(self):
        process, out = self.bench_game('done', marked=True, scenario_name='furniture-parts', report=True, MOVABLE_VOBS='1',
                                     OPENGOTHIC_NPC_SIM_TIER='full')
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        invocation = json.loads((self.root / 'invocation.json').read_text())
        self.assertEqual(invocation['binary'], 'gothic-body-runner')
        self.assertEqual(invocation['args'], ['-g', str(self.root / 'data'), '-devmode',
                                             '-nomenu', '--world', 'NEWWORLD.ZEN', '-nointro',
                                             '--game-parity', '--scenario', str(self.root / 'furniture-parts.bench'),
                                             '--output', str(out), '--mode', 'lockstep'])
        self.assertEqual(invocation['cwd'], str((out / 'run').resolve()))
        self.assertIn('[GAME]\nmovable_vobs=1\n', invocation['ini'])
        self.assertEqual(invocation['env']['OPENGOTHIC_BENCH'], str(self.root / 'furniture-parts.bench'))
        self.assertEqual(invocation['env']['OPENGOTHIC_BENCH_OUT'], str(out))
        self.assertEqual(invocation['env']['OPENGOTHIC_NPC_SIM_TIER'], 'full')
        self.assertIsNone(invocation['bench_class'])
        self.assertFalse(any(key.startswith('TEMPEST_') for key in invocation['env']))
        summary = json.loads((out / 'report.json').read_text())
        self.assertEqual(summary['run_mode'], 'headless lockstep')
        self.assertGreaterEqual(summary['game_wall_seconds'], 0)
        self.assertIn('headless lockstep', (out / 'report.md').read_text())
        self.assertRegex(stdout, r'bench: game ran \d+ s')
        self.assertIn('{"status": "done"}', (out / 'game.log').read_text())

    def test_headless_override_uses_custom_runner_and_save(self):
        save = self.root / 'input.sav'
        save.write_text('saved game')
        process, out = self.bench_game('done', BENCH_HEADLESS='1', WORLD='', SAVE=str(save),
                                     RUNNER_BIN=str(self.binaries / 'custom-runner'), RYZEN_BENCH='1',
                                     RYZEN_HOST='dev-ubuntu', report=True)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        invocation = json.loads((self.root / 'invocation.json').read_text())
        self.assertEqual(invocation['binary'], 'custom-runner')
        self.assertEqual(invocation['save'], 'saved game')
        self.assertEqual(invocation['args'], ['-g', str(self.root / 'data'), '-devmode',
                                             '--save', str(out / 'run/save_slot_1.sav'), '-nointro',
                                             '--game-parity', '--scenario', str(self.root / 'smoke.bench'),
                                             '--output', str(out), '--mode', 'lockstep'])
        self.assertEqual((out / 'host.txt').read_text(), 'dev-ubuntu\n')
        self.assertIn('Host: dev-ubuntu', (out / 'report.md').read_text())

    def test_headless_zero_preserves_npc_day_window_arguments(self):
        process, out = self.bench_game('done', marked=True, scenario_name='npc-day-parity',
                                     BENCH_HEADLESS='0', report=True)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        invocation = json.loads((self.root / 'invocation.json').read_text())
        self.assertEqual(invocation['binary'], 'game')
        self.assertEqual(invocation['args'], ['-g', str(self.root / 'data'), '-window', '-devmode',
                                             '-nomenu', '-w', 'NEWWORLD.ZEN', '-nointro'])
        self.assertEqual(json.loads((out / 'report.json').read_text())['run_mode'], 'window')

    def test_headless_exit_without_done_fails(self):
        process, out = self.bench_game('exit', marked=True)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 1, stdout + stderr)
        self.assertIn('without [bench] done', stderr)
        self.assertTrue((out / 'report-built').exists())

    def test_headless_stall_uses_the_game_watchdog(self):
        start = time.monotonic()
        process, out = self.bench_game('stall', marked=True, BENCH_STALL='2', TIMEOUT='30')
        stdout, stderr = process.communicate(timeout=20)
        self.assertEqual(process.returncode, 1, stdout + stderr)
        self.assertIn('bench: game stalled: no output or heartbeat for 2 s, killed', stderr)
        self.assertLess(time.monotonic() - start, 18)
        with self.assertRaises(ProcessLookupError):
            os.kill(int((self.root / 'game.pid').read_text()), 0)
        self.assertTrue((out / 'report-built').exists())

    def test_headless_rejects_keep_before_launch(self):
        process, _ = self.bench_game('done', marked=True, KEEP='1')
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 2, stdout + stderr)
        self.assertIn('KEEP=1', stderr)
        self.assertIn('headless', stderr)
        self.assertFalse((self.root / 'game.pid').exists())

    def test_bench_exit_without_done_fails(self):
        process, out = self.bench_game('exit')
        stdout, stderr = process.communicate(timeout=5)
        self.assertNotIn(process.returncode, (0, 75, 76), stdout + stderr)
        self.assertRegex(stderr, r'bench: scenario did not finish: game exited after \d+ s without \[bench\] done')
        self.assertTrue((out / 'report-built').exists())

    def test_bench_stall_kills_game_and_reports_partial_output(self):
        start = time.monotonic()
        process, out = self.bench_game('stall', BENCH_STALL='2', TIMEOUT='30')
        stdout, stderr = process.communicate(timeout=20)
        self.assertNotIn(process.returncode, (0, 75, 76), stdout + stderr)
        self.assertIn('bench: game stalled: no output or heartbeat for 2 s, killed', stderr)
        self.assertLess(time.monotonic() - start, 18)
        with self.assertRaises(ProcessLookupError):
            os.kill(int((self.root / 'game.pid').read_text()), 0)
        self.assertTrue((out / 'report-built').exists())

    def test_bench_progress_is_streamed_from_heartbeat(self):
        process, _ = self.bench_game('progress', BENCH_PROGRESS='1')
        deadline = time.monotonic() + 5
        output = b''
        expected = b'bench: 0 min, command 5/5 npcday 1440, npcday 1060/1440 game min\n'
        while expected not in output and time.monotonic() < deadline:
            ready, _, _ = select.select([process.stdout], [], [], 0.1)
            if ready:
                block = os.read(process.stdout.fileno(), 4096)
                if not block:
                    break
                output += block
        self.assertIn(expected, output.splitlines(keepends=True))
        self.assertIsNone(process.poll(), output.decode())
        (self.root / 'finish').touch()
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stdout + stderr)

    def test_bench_timeout_without_done_fails(self):
        process, out = self.bench_game('timeout', TIMEOUT='2')
        stdout, stderr = process.communicate(timeout=15)
        self.assertNotIn(process.returncode, (0, 75, 76), stdout + stderr)
        self.assertIn('bench: timeout after 2 s, game killed', stderr)
        with self.assertRaises(ProcessLookupError):
            os.kill(int((self.root / 'game.pid').read_text()), 0)
        self.assertTrue((out / 'report-built').exists())

    def test_bench_keep_timeout_after_done_succeeds(self):
        process, out = self.bench_game('keep', KEEP='1', TIMEOUT='2', BENCH_STALL='1')
        stdout, stderr = process.communicate(timeout=15)
        self.assertEqual(process.returncode, 0, stdout + stderr)
        self.assertNotIn('game stalled', stderr)
        with self.assertRaises(ProcessLookupError):
            os.kill(int((self.root / 'game.pid').read_text()), 0)
        self.assertTrue((out / 'report-built').exists())


if __name__ == '__main__':
    unittest.main()
