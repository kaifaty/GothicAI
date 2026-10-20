"""Personal install/config and queue boundaries, without SSH or a real game."""
import ctypes
import fcntl
import importlib.machinery
import importlib.util
import io
import tempfile
import tomllib
import unittest
from pathlib import Path
from unittest.mock import patch


def load(name, filename):
    path = Path(__file__).resolve().parents[1] / filename
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


install = load("ryzen_play", "ryzen-play.sh")
session = load("ryzen_play_session", "ryzen-play-session.sh")


class RyzenPlayTests(unittest.TestCase):
    def test_arguments(self):
        self.assertEqual(install.arguments(["install"]).action, "install")
        self.assertEqual(install.arguments(["update"]).action, "update")
        self.assertFalse(session.arguments([]).smoke)
        self.assertTrue(session.arguments(["--smoke"]).smoke)
        with patch("sys.stderr"):
            for arguments in ([], ["setup"], ["install", "unexpected"]):
                with self.assertRaises(SystemExit) as error:
                    install.arguments(arguments)
                self.assertEqual(error.exception.code, 2)

    def test_settings_copy_without_secrets_or_mac_paths_and_without_overwrite(self):
        source = {
            "game": {"dir": "/Mac/Game", "dev_mode": False, "chat_scale": 1.25},
            "paths": {"repo": "/Mac/Repo", "uv": "/Mac/uv", "game_bin": "/Mac/Game.app",
                      "game_home": "/Mac/Saves"},
            "llm": {"backend": "chatgpt", "reasoning_effort": "medium", "decider_url": "ws://models:1",
                    "body_url": "tcp://body:2", "quick": True},
            "voice": {"provider": "voxtral", "voxtral_url": "http://models:3",
                      "mic_device": "MacBook Pro Microphone"},
            "tts": {"provider": "inworld", "voxcpm_url": "ws://models:4"},
            "secrets": {"openai_api_key": "must-not-copy"},
        }
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            path = home / ".config/gothic-ai/settings.toml"
            repo = home / "gothic-ai/play"
            self.assertTrue(install.create_settings(path, source, repo, home))
            content = path.read_text()
            result = tomllib.loads(content)
            self.assertEqual(result["game"]["dir"], str(home / "gothic-ai/game"))
            self.assertEqual(result["paths"], {"repo": str(repo), "uv": str(home / ".local/bin/uv")})
            for section in ("llm", "tts"):
                self.assertEqual(result[section], source[section])
            self.assertEqual(result["voice"], {"provider": "voxtral", "voxtral_url": "http://models:3"})
            self.assertNotIn("mic_device", result["voice"])
            self.assertEqual(source["voice"]["mic_device"], "MacBook Pro Microphone")
            self.assertNotIn("must-not-copy", content)
            self.assertNotIn("/Mac/", content)
            self.assertEqual(list(path.parent.iterdir()), [path])
            path.write_text("# user's settings\n")
            self.assertFalse(install.create_settings(path, source, repo, home))
            self.assertEqual(path.read_text(), "# user's settings\n")

    def test_prepare_keeps_build_and_all_uv_sync_inside_the_queue(self):
        repo = Path("/tmp/play")
        command = install.build_command("prepare-source")
        separator = command.index("--")
        options = command[:separator]
        self.assertEqual(options[options.index("--class") + 1], "quick")
        self.assertNotIn("--display-exclusive", options)
        self.assertEqual(command[separator + 1:], [install.sys.executable, "-c", "prepare-source", "__prepare"])
        with patch.dict(install.os.environ), \
                patch.object(install.Path, "cwd", return_value=repo), \
                patch.object(install.shutil, "which", return_value="/tool"), \
                patch.object(install.Path, "is_file", return_value=True), \
                patch.object(install, "run") as run:
            install.prepare()
        calls = [call.args[0] for call in run.call_args_list]
        self.assertIn("-DCMAKE_BUILD_TYPE=RelWithDebInfo", calls[0])
        self.assertEqual(calls[1][1:3], ["--build", repo / "OpenGothic/build"])
        self.assertIn("storygraph", calls[3])
        self.assertEqual([call[-1] for call in calls[4:]],
                         [repo / name for name in ("server", "asr-service", "tts-service", "launcher/core")])
        self.assertTrue(all("--locked" in call for call in calls[4:]))

    def test_remote_install_preserves_shared_queue_and_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            repo = home / "gothic-ai/play"
            repo.mkdir(parents=True)
            (home / "gothic-ai/game").mkdir()
            uv = home / ".local/bin/uv"
            uv.parent.mkdir(parents=True)
            uv.touch()
            queue = home / "gothic-ai/bin/gq"
            queue.parent.mkdir()
            queue.write_text("#!/bin/sh\n# marker from another session\nprintf '%s\\n' '--interactive'\n")
            queue.chmod(0o755)
            profile = queue.parent / "gq.profile"
            profile.write_bytes(b"ryzen\n")
            before = [(path.read_bytes(), path.stat().st_mtime_ns) for path in (queue, profile)]
            for action in ("install", "update"):
                with self.subTest(action=action), patch.dict(install.os.environ), \
                        patch.object(install.Path, "home", return_value=home), \
                        patch.object(install.os, "chdir"), \
                        patch.object(install, "run") as run, \
                        patch.object(install.subprocess, "check_output", side_effect=["main\n", "origin\n", ""]), \
                        patch.object(install, "owned_process", return_value=0) as build:
                    self.assertEqual(install.remote_install(action, {"origin": "origin", "settings": {}}, "source"), 0)
                    run.assert_called_once_with(["git", "-C", repo, "pull", "--ff-only"])
                    build.assert_called_once()
                self.assertEqual([(path.read_bytes(), path.stat().st_mtime_ns) for path in (queue, profile)], before)

    def test_remote_install_requires_interactive_queue_before_git(self):
        for queue_source in (None, "#!/bin/sh\nprintf '%s\\n' '--display'\n", "#!/bin/sh\nexit 1\n"):
            with self.subTest(queue_source=queue_source), tempfile.TemporaryDirectory() as directory:
                home = Path(directory)
                (home / "gothic-ai/game").mkdir(parents=True)
                uv = home / ".local/bin/uv"
                uv.parent.mkdir(parents=True)
                uv.touch()
                queue = home / "gothic-ai/bin/gq"
                if queue_source is not None:
                    queue.parent.mkdir()
                    queue.write_text(queue_source)
                    queue.chmod(0o755)
                with patch.object(install.Path, "home", return_value=home), \
                        patch.object(install, "run") as run, \
                        patch.object(install.subprocess, "check_output") as metadata, \
                        patch.object(install, "owned_process") as build:
                    with self.assertRaisesRegex(ValueError, "scripts/gq install ryzen"):
                        install.remote_install("install", {"origin": "origin", "settings": {}}, "source")
                    run.assert_not_called()
                    metadata.assert_not_called()
                    build.assert_not_called()
                self.assertFalse((home / "gothic-ai/play").exists())

    def test_session_holds_clone_lock_until_game_exits(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            lock_path = home / "gothic-ai/play.install.lock"
            lock_path.parent.mkdir(parents=True)

            def while_game_runs():
                with self.assertRaisesRegex(ValueError, "playing"):
                    install.remote_install("update", {"origin": "origin", "settings": {}}, "source")
                run.assert_not_called()
                metadata.assert_not_called()
                build.assert_not_called()
                return 0

            with patch.dict(session.os.environ, DISPLAY=":0"), \
                    patch.object(session.Path, "home", return_value=home), \
                    patch.object(session.Path, "is_file", return_value=True), \
                    patch.object(session.os, "chdir"), \
                    patch.object(session, "portaudio_available", return_value=True), \
                    patch.object(session.subprocess, "Popen") as start, \
                    patch.object(install, "run") as run, \
                    patch.object(install.subprocess, "check_output") as metadata, \
                    patch.object(install, "owned_process") as build:
                start.return_value.wait.side_effect = while_game_runs
                self.assertEqual(session.main([]), 0)
                start.assert_called_once()
            with lock_path.open("r+") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_session_refuses_active_install(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            lock_path = home / "gothic-ai/play.install.lock"
            lock_path.parent.mkdir(parents=True)
            with lock_path.open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with patch.dict(session.os.environ, DISPLAY=":0"), \
                        patch.object(session.Path, "home", return_value=home), \
                        patch.object(session.Path, "is_file", return_value=True), \
                        patch.object(session.os, "chdir"), \
                        patch.object(session, "portaudio_available", return_value=True), \
                        patch.object(session.subprocess, "Popen") as start, \
                        patch.object(session.sys.stdin, "isatty", return_value=False), \
                        patch.object(session.sys, "stderr", io.StringIO()):
                    self.assertEqual(session.main([]), 2)
                    start.assert_not_called()

    def test_play_exclusive_six_hour_live_request(self):
        command = session.session_command(Path("/repo"), microphone=False)
        for flag, value in (("--class", "quick"), ("--threads", "8"), ("--mem", "8G"),
                            ("--gpu-mem", "3G"), ("--max", "6h")):
            self.assertEqual(command[command.index(flag) + 1], value)
        self.assertIn("--interactive", command)
        self.assertIn("--display-exclusive", command)
        self.assertIn("--continue", command)
        self.assertIn("--no-voice", command)
        self.assertNotIn("--smoke", command)
        smoke = session.session_command(Path("/repo"), smoke=True)
        self.assertIn("--smoke", smoke)
        self.assertIn("--new", smoke)
        self.assertNotIn("--continue", smoke)

    def test_portaudio_checks_loadable_library_not_only_a_filename(self):
        with patch.object(ctypes, "CDLL") as load_library:
            self.assertTrue(session.portaudio_available())
            load_library.assert_called_once_with("libportaudio.so.2")
        with patch.object(ctypes, "CDLL", side_effect=OSError("not installed")):
            self.assertFalse(session.portaudio_available())

    def test_missing_portaudio_prints_install_command_and_starts_without_microphone(self):
        output = io.StringIO()
        with tempfile.TemporaryDirectory() as directory, \
                patch.dict(session.os.environ, DISPLAY=":0", ALSOFT_DRIVERS="null"), \
                patch.object(session.Path, "home", return_value=Path(directory)), \
                patch.object(session.Path, "is_file", return_value=True), \
                patch.object(session.os, "chdir"), \
                patch.object(session, "portaudio_available", return_value=False), \
                patch.object(session.subprocess, "Popen") as start, \
                patch.object(session.sys, "stdout", output):
            (Path(directory) / "gothic-ai").mkdir()
            start.return_value.wait.return_value = 0
            self.assertEqual(session.main([]), 0)
            self.assertIn("--no-voice", start.call_args.args[0])
            self.assertNotIn("ALSOFT_DRIVERS", session.os.environ)
        self.assertIn("sudo apt install libportaudio2", output.getvalue())

    def test_desktop_opens_visible_terminal_with_game_icon(self):
        entry = install.desktop_entry(Path("/home/player/gothic-ai/play"))
        self.assertIn("Name=Gothic AI\n", entry)
        self.assertIn("Exec=ptyxis --new-window", entry)
        self.assertIn("scripts/ryzen-play-session.sh", entry)
        self.assertIn("Icon=/home/player/gothic-ai/play/launcher/app/src-tauri/icons/128x128.png", entry)


if __name__ == "__main__":
    unittest.main()
