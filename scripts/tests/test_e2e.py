"""Run-local settings, startup guards, and fake-game process lifecycle."""
import configparser
import os
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[1] / "e2e.sh"


class GameExitTests(unittest.TestCase):
    def run_game(self, mode, *, checks_started=False, server_finishes=False):
        source = SCRIPT.read_text()
        lifecycle = source[source.index("report_game_exit() {"):source.index("trap cleanup EXIT")]
        monitor = source[source.index('for _ in $(seq 1 "$TIMEOUT"); do'):
                         source.index('if [[ "$rc" == 0 && "${GOTHIC_TTS_RECORD:-0}" == 1 ]]')]
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "server.log").write_text(
                "INFO scenario m0: checks started\n" if checks_started else "")
            game = root / "game.py"
            game.write_text('''
import os
from pathlib import Path
import signal
import sys
import time

root = Path(os.environ["LOG_DIR"])
mode = sys.argv[1]
if mode == "cleanup":
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
(root / "game.ready").touch()
while not (root / "game.go").exists():
    time.sleep(0.01)
if mode == "kill":
    os.kill(os.getpid(), signal.SIGKILL)
elif mode == "term":
    os.kill(os.getpid(), signal.SIGTERM)
elif mode == "failure":
    sys.exit(3)
elif mode == "exited":
    sys.exit(0)
while True:
    time.sleep(1)
''')
            harness = '\n'.join([
                "set -euo pipefail",
                "GAME= ASR_PID= TTS_PID= SCENARIO=m0 TIMEOUT=20",
                # Keep the real lifecycle, with a shorter bounded cleanup wait.
                "sleep() { command sleep 0.01; }",
                lifecycle,
                "trap cleanup EXIT",
                f"{shlex.quote(sys.executable)} {shlex.quote(str(game))} {shlex.quote(mode)} &",
                "GAME=$!",
                'until [[ -f "$LOG_DIR/game.ready" ]]; do sleep 0.01; done',
                'touch "$LOG_DIR/game.go"',
            ])
            if mode != "cleanup":
                harness += '\nuntil ! kill -0 "$GAME" 2>/dev/null; do sleep 0.01; done'
            if server_finishes:
                harness += '\n(command exit 23) & SRV=$!\nuntil ! kill -0 "$SRV" 2>/dev/null; do sleep 0.01; done'
            else:
                harness += '\ncommand sleep 10 & SRV=$!'
            done = subprocess.run(
                ["bash", "-c", harness + '\n' + monitor + '\nexit "$rc"'],
                env=dict(os.environ, LOG_DIR=str(root)), check=False,
                capture_output=True, text=True, timeout=5)
            return done

    def test_sigkill_before_checks_records_one_utc_time(self):
        done = self.run_game("kill")
        self.assertEqual(done.returncode, 1, done.stderr)
        killed = [line for line in done.stdout.splitlines()
                  if line.startswith("game killed before checks at ")]
        self.assertEqual(len(killed), 1, done.stdout)
        self.assertRegex(killed[0], r"^game killed before checks at \d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertIn("signal SIGKILL (9), status 137 (unexpected)", done.stdout)

    def test_sigkill_after_checks_and_other_early_failures_are_not_retriable(self):
        for mode, started in (("kill", True), ("failure", False), ("term", False)):
            with self.subTest(mode=mode, checks_started=started):
                done = self.run_game(mode, checks_started=started)
                self.assertEqual(done.returncode, 1, done.stderr)
                self.assertNotIn("game killed before checks", done.stdout)

    def test_server_and_game_exit_between_polls_still_records_early_sigkill(self):
        done = self.run_game("kill", server_finishes=True)
        self.assertEqual(done.returncode, 23, done.stderr)
        self.assertEqual(done.stdout.count("game killed before checks at "), 1, done.stdout)
        self.assertIn("status 137 (exited)", done.stdout)

    def test_game_already_exited_is_reported_without_unexpected(self):
        done = self.run_game("exited", server_finishes=True)
        self.assertEqual(done.returncode, 23, done.stderr)
        self.assertIn("game exited: code 0 (exited)", done.stdout)
        self.assertNotIn("unexpected", done.stdout)

    def test_cleanup_sigkill_is_not_retriable_and_preserves_server_status(self):
        done = self.run_game("cleanup", server_finishes=True)
        self.assertEqual(done.returncode, 23, done.stderr)
        self.assertIn("signal SIGKILL (9), status 137 (cleanup)", done.stdout)
        self.assertNotIn("game killed before checks", done.stdout)


class LocalCheckWaitTests(unittest.TestCase):
    def test_only_mac_waits_for_existing_pytest_or_game(self):
        source = SCRIPT.read_text()
        guard = source[source.index("# Recheck Mac contention"):source.index("GAME_ENV=(")]
        harness = r'''
uname() { printf '%s\n' "$TEST_PLATFORM"; }
pgrep() {
  echo pgrep >> "$CALLS"
  if [ ! -e "$SEEN" ]; then
    : > "$SEEN"
    printf '%s\n' "$BUSY_PROCESS" | grep -E "$2"
  else
    return 1
  fi
}
sleep() { printf 'sleep %s\n' "$1" >> "$CALLS"; }
'''
        for platform in ("Darwin", "Linux"):
            for process in (
                "/usr/bin/python3 /tmp/server/.venv/bin/pytest tests/test_protocol.py",
                "/tmp/build/Gothic2Notr -g /tmp/gothic",
            ):
                with self.subTest(platform=platform, process=process), tempfile.TemporaryDirectory() as temp:
                    calls = Path(temp) / "calls"
                    done = subprocess.run(
                        ["bash", "-ec", harness + guard + "echo ready"],
                        check=False,
                        env=dict(os.environ, TEST_PLATFORM=platform, BUSY_PROCESS=process,
                                 CALLS=str(calls), SEEN=str(Path(temp) / "seen")),
                        capture_output=True, text=True, timeout=5,
                    )
                    self.assertEqual(done.returncode, 0, done.stderr)
                    self.assertEqual(done.stdout, "ready\n")
                    observed = calls.read_text().splitlines() if calls.exists() else []
                    self.assertEqual(observed, ["pgrep", "sleep 5", "pgrep"] if platform == "Darwin" else [])


class SoakControlTests(unittest.TestCase):
    def test_control_selects_native_body_without_relaxing_other_launch_requirements(self):
        source = SCRIPT.read_text()
        selection = source[source.index('SCENARIO="${SCENARIO:-m0}"'):source.index('ASR="${ASR:-fake}"')]
        cases = [
            ("neural-soak", "1", "", "", "fake", 0, "--no-neural-body"),
            ("neural-soak", "1", "tcp://policy:47835", "", "fake", 0, "--no-neural-body"),
            ("neural-soak", "0", "tcp://policy:47835", "", "fake", 0, "--neural-body"),
            ("neural-soak", "0", "", "", "fake", 2, None),
            ("neural-soak", "1", "", "/tmp/save.sav", "fake", 2, None),
            ("neural-soak", "1", "", "", "chatgpt", 2, None),
            ("neural-load", "1", "tcp://policy:47835", "", "fake", 2, None),
        ]
        for scenario, control, url, save, llm, status, body_flag in cases:
            with self.subTest(scenario=scenario, control=control, url=url, save=save, llm=llm):
                env = dict(os.environ, SCENARIO=scenario, GOTHIC_SOAK_CONTROL=control,
                           GOTHIC_BODY_URL=url, SAVE=save, LLM=llm, GOTHIC_TTS_RECORD="0")
                result = subprocess.run(
                    ["bash", "-euc", selection + '\nprintf "%s" "${BODY_ARGS[@]}"'],
                    check=False,
                    capture_output=True, text=True, timeout=5, env=env,
                )
                self.assertEqual(result.returncode, status, result.stdout + result.stderr)
                if body_flag is not None:
                    self.assertEqual(result.stdout, body_flag)


class SoakVideoTests(unittest.TestCase):
    def video_environment(self, *, cached=False, executable=True, profile="ryzen", cadence=None):
        source = SCRIPT.read_text()
        block = source[source.index("GAME_ENV=("):source.index('if [[ -n "${GAME_EXTRA_ENV:-}" ]]')]
        with tempfile.TemporaryDirectory() as temp:
            home = Path(temp)
            root = home / "task/source"
            run = home / "task/runs/run"
            root.mkdir(parents=True)
            run.mkdir(parents=True)
            if cached:
                runtime = home / "gothic-ai/engine-cache" / ("a" * 64) / "Gothic2Notr"
                runtime.parent.mkdir(parents=True)
                runtime.write_text("#!/bin/sh\nexit 0\n")
                runtime.chmod(0o755 if executable else 0o644)
                game = run / "game.sh"
                game.write_text('#!/bin/bash\nexec "$RYZEN_GAME_BIN" "$@" -g2 -window\n')
            else:
                runtime = None
                game = root / "OpenGothic/build/opengothic/Gothic2Notr"
                game.parent.mkdir(parents=True)
                if cadence is not None:
                    (game.parent.parent / "CMakeCache.txt").write_text(
                        f"OPENGOTHIC_ENABLE_APP_CADENCE:BOOL={cadence}\n")
            env = dict(os.environ, HOME=str(home), ROOT=str(root), LOG_DIR=str(run),
                       GAME_BIN=str(game), SCENARIO="neural-soak", PORT="47817",
                       GQ_PROFILE=profile)
            env.pop("RYZEN_GAME_BIN", None)
            if runtime is not None:
                env["RYZEN_GAME_BIN"] = str(runtime)
            done = subprocess.run(
                ["bash", "-euc", block + '\nprintf "%s\\n" "${GAME_ENV[@]}"'],
                check=False, env=env, capture_output=True, text=True, timeout=5)
            self.assertEqual(done.returncode, 0, done.stderr)
            return done.stdout.splitlines()

    def test_ryzen_cache_hit_records_without_a_worktree_build(self):
        observed = self.video_environment(cached=True)
        self.assertIn("OPENGOTHIC_BENCH_REALTIME=1", observed)
        self.assertTrue(any(line.endswith("/scripts/bench/neural-soak-e2e.bench")
                            for line in observed))
        self.assertFalse(any("video unavailable" in line for line in observed))

    def test_cache_hint_requires_ryzen_and_an_executable_runtime(self):
        for options in ({"profile": "local"}, {"executable": False}):
            with self.subTest(options=options):
                observed = self.video_environment(cached=True, **options)
                self.assertNotIn("OPENGOTHIC_BENCH_REALTIME=1", observed)
                self.assertTrue(any("video unavailable" in line for line in observed))

    def test_local_build_still_requires_cadence(self):
        for cadence in (None, "OFF", "ON"):
            with self.subTest(cadence=cadence):
                observed = self.video_environment(profile="local", cadence=cadence)
                self.assertEqual("OPENGOTHIC_BENCH_REALTIME=1" in observed, cadence == "ON")


class RunSettingsTests(unittest.TestCase):
    def run_settings(self, *, options=None, existing_ini="", scenario="stages",
                     exception=None, systempack=None):
        source = SCRIPT.read_text()
        block = source[source.index("# Per-scenario defaults"):source.index("START_ARGS=(-nomenu)")]
        if exception is not None:
            block = block.replace("E2E_FPS_EXCEPTIONS=(", f"E2E_FPS_EXCEPTIONS=(\n  {exception}", 1)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            log_dir = root / "run"
            game_dir = root / "game"
            log_dir.mkdir()
            game_dir.mkdir()
            ini_path = log_dir / "Gothic.ini"
            if existing_ini:
                ini_path.write_text(existing_ini)
            systempack_path = None
            if systempack is not None:
                relative_path, content = systempack
                systempack_path = game_dir / relative_path
                systempack_path.parent.mkdir(parents=True)
                systempack_bytes = content if isinstance(content, bytes) else content.encode()
                systempack_path.write_bytes(systempack_bytes)
            env = dict(os.environ)
            for name in ("E2E_MAX_FPS", "CHARACTER_VIRTUAL", "NPC_CHARACTER_VIRTUAL",
                         "MONSTER_CHARACTER_VIRTUAL", "GOTHIC_TTS_RECORD", "OPENGOTHIC_FPS_LOG"):
                env.pop(name, None)
            env.update(LOG_DIR=str(log_dir), GOTHIC_DIR=str(game_dir), SCENARIO=scenario)
            env.update(options or {})
            done = subprocess.run(["bash", "-euc", block], check=False, env=env, capture_output=True,
                                  text=True, timeout=5)
            ini = configparser.ConfigParser(interpolation=None)
            ini.read(ini_path)
            if systempack_path is not None:
                self.assertEqual(systempack_path.read_bytes(), systempack_bytes)
            return done, ini

    def test_default_limit_is_30(self):
        done, ini = self.run_settings()
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(ini.getint("ENGINE", "zMaxFps"), 30)
        self.assertIn("zMaxFps=30", done.stdout)
        self.assertIn("OPENGOTHIC_FPS_LOG=1", done.stdout)

    def test_explicit_limit_overrides_existing_ini(self):
        for value in ("0", "60"):
            with self.subTest(value=value):
                done, ini = self.run_settings(options={"E2E_MAX_FPS": value},
                                              existing_ini="[ENGINE]\nZMAXFPS=120\n")
                self.assertEqual(done.returncode, 0, done.stderr)
                # An explicit zero also overrides a cap inherited from the base game ini.
                self.assertEqual(ini.getint("ENGINE", "zMaxFps"), int(value))
                self.assertIn(f"zMaxFps={value}", done.stdout)

    def test_character_virtual_and_tts_settings_keep_engine_and_existing_sections(self):
        existing_ini = ("[ENGINE]\nviewDistance=2\n[GAME]\nragdoll=0\n"
                        "[INTERNAL]\nexistingInternal=yes\nvidResIndex=4\n"
                        "[SOUND]\nexistingSound=yes\nsoundVolume=0\nmusicVolume=1\n")
        done, ini = self.run_settings(
            scenario="tts", existing_ini=existing_ini,
            options={"CHARACTER_VIRTUAL": "1", "NPC_CHARACTER_VIRTUAL": "0",
                     "MONSTER_CHARACTER_VIRTUAL": "1", "GOTHIC_TTS_RECORD": "1"})
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(ini.getint("ENGINE", "zMaxFps"), 30)
        self.assertEqual(ini.get("ENGINE", "viewDistance"), "2")
        self.assertEqual(dict(ini["GAME"]), {
            "ragdoll": "0", "character_virtual": "1", "npc_character_virtual": "0",
            "monster_character_virtual": "1"})
        self.assertEqual(dict(ini["INTERNAL"]), {"existinginternal": "yes", "vidresindex": "0"})
        self.assertEqual(dict(ini["SOUND"]), {
            "existingsound": "yes", "soundvolume": "1", "musicvolume": "0"})

    def test_scenario_exception_and_explicit_override(self):
        for exception_value in ("0", "60"):
            for explicit_value in (None, "0", "30"):
                with self.subTest(exception=exception_value, explicit=explicit_value):
                    options = {} if explicit_value is None else {"E2E_MAX_FPS": explicit_value}
                    done, ini = self.run_settings(
                        scenario="fps-test-fixture", exception=f"fps-test-fixture:{exception_value}",
                        options=options)
                    self.assertEqual(done.returncode, 0, done.stderr)
                    self.assertEqual(ini.getint("ENGINE", "zMaxFps"),
                                     int(explicit_value if explicit_value is not None else exception_value))
        done, ini = self.run_settings(exception="fps-test-fixture:60")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(ini.getint("ENGINE", "zMaxFps"), 30)

    def test_positive_systempack_limit_warns_with_case_insensitive_system_path(self):
        content = "[OTHER]\r\nFPS_Limit=100\r\n[PaRaMeTeRs]\r\n  fPs_LiMiT = 75\r\n"
        for folder in ("System", "system", "SyStEm"):
            with self.subTest(folder=folder):
                done, ini = self.run_settings(systempack=(f"{folder}/SyStEmPaCk.InI", content))
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertEqual(ini.getint("ENGINE", "zMaxFps"), 30)
                self.assertIn("WARNING:", done.stderr)
                self.assertIn("FPS_Limit=75 overrides e2e zMaxFps=30", done.stderr)

    def test_zero_systempack_limit_does_not_warn(self):
        done, _ = self.run_settings(systempack=(
            "System/SystemPack.ini", "[OTHER]\nFPS_Limit=100\n[PARAMETERS]\nFPS_Limit=0\n"))
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("WARNING:", done.stderr)

    def test_systempack_cp1251_comments_do_not_break_utf8_locale(self):
        # Real game installations have Cyrillic comments in Windows-1251.
        comment = "; Предел кадров\r\n".encode("cp1251")
        for value in (0, 75):
            with self.subTest(value=value):
                content = comment + f"[PARAMETERS]\r\nFPS_Limit={value}\r\n".encode("ascii")
                done, ini = self.run_settings(
                    options={"LC_ALL": "en_US.UTF-8"},
                    systempack=("System/SystemPack.ini", content))
                self.assertEqual(done.returncode, 0, done.stderr)
                self.assertEqual(ini.getint("ENGINE", "zMaxFps"), 30)
                self.assertIn("zMaxFps=30", done.stdout)
                if value:
                    self.assertIn("FPS_Limit=75 overrides e2e zMaxFps=30", done.stderr)
                else:
                    self.assertNotIn("WARNING:", done.stderr)

    def test_invalid_limit_is_rejected(self):
        for value in ("", "-1", "30.5", "no-limit"):
            with self.subTest(value=value):
                done, _ = self.run_settings(options={"E2E_MAX_FPS": value})
                self.assertEqual(done.returncode, 2, done.stderr)
                self.assertIn("E2E_MAX_FPS must be a nonnegative integer", done.stderr)

    def test_invalid_character_virtual_keeps_usage_error_status(self):
        for name in ("CHARACTER_VIRTUAL", "NPC_CHARACTER_VIRTUAL", "MONSTER_CHARACTER_VIRTUAL"):
            with self.subTest(name=name):
                done, _ = self.run_settings(options={name: "invalid"})
                self.assertEqual(done.returncode, 2, done.stderr)
                self.assertIn(f"{name} must be 0 or 1", done.stderr)


class RyzenOptionForwardingTests(unittest.TestCase):
    def test_explicit_zero_limit_is_forwarded_to_ryzen(self):
        source = SCRIPT.with_name("ryzen.sh").read_text()
        block = source[source.index("# Carry e2e options"):source.index('if [[ "$ACTION" == e2e &&')]
        done = subprocess.run(
            ["bash", "-euc", 'ENV_ARGS=()\n' + block + 'printf \'%s\\n\' "${ENV_ARGS[@]}"'],
            check=False, env={"PATH": os.environ["PATH"], "E2E_MAX_FPS": "0"},
            capture_output=True, text=True, timeout=5)
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.splitlines(), ["E2E_MAX_FPS=0"])


if __name__ == "__main__":
    unittest.main()
