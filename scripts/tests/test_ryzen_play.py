"""Personal install/config and queue boundaries, without SSH or a real game."""
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
        self.assertEqual([call[-1] for call in calls[4:8]],
                         [repo / name for name in ("server", "asr-service", "tts-service", "launcher/core")])
        self.assertTrue(all("--locked" in call for call in calls[4:8]))
        self.assertEqual(calls[8], [repo / "scripts/build-launcher.sh", "--no-install"])

    def test_remote_install_preserves_shared_queue_and_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            repo = home / "gothic-ai/play"
            repo.mkdir(parents=True)
            binary = repo / "launcher/app/src-tauri/target/release/gothic-launcher"
            binary.parent.mkdir(parents=True)
            binary.touch(mode=0o755)
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
            for action, result in (("install", 0), ("update", 0), ("update", 3)):
                with self.subTest(action=action, result=result), patch.dict(install.os.environ), \
                        patch.object(install.Path, "home", return_value=home), \
                        patch.object(install.os, "chdir"), \
                        patch.object(install, "run") as run, \
                        patch.object(install.subprocess, "check_output", side_effect=["main\n", "origin\n", ""]), \
                        patch.object(install, "owned_process", return_value=result) as build:
                    self.assertEqual(install.remote_install(action, {"origin": "origin", "settings": {}}, "source"), result)
                    run.assert_called_once_with(["git", "-C", repo, "pull", "--ff-only"])
                    build.assert_called_once()
                    entry = (home / ".local/share/applications/gothic-ai.desktop").read_text()
                    self.assertEqual("Exec=ptyxis" in entry, result == 3)
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

    def test_update_refuses_active_session_before_touching_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            lock_path = home / "gothic-ai/play.install.lock"
            lock_path.parent.mkdir(parents=True)
            with lock_path.open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with patch.object(install.Path, "home", return_value=home), \
                        patch.object(install, "run") as run, \
                        patch.object(install.subprocess, "check_output") as metadata, \
                        patch.object(install, "owned_process") as build:
                    with self.assertRaisesRegex(ValueError, "playing"):
                        install.remote_install("update", {"origin": "origin", "settings": {}}, "source")
                    run.assert_not_called()
                    metadata.assert_not_called()
                    build.assert_not_called()

    def test_terminal_wrapper_delegates_to_the_same_linux_core_session(self):
        with patch.object(session.os, "execv") as execute:
            self.assertIsNone(session.main(["--smoke"]))
        command = execute.call_args.args[1]
        self.assertEqual(command[-3:], ["gothic-launcher", "play", "--smoke"])
        self.assertIn("--locked", command)
        self.assertEqual(Path(command[command.index("--project") + 1]).name, "core")

    def test_terminal_wrapper_keeps_startup_errors_visible(self):
        with patch.object(session.os, "execv", side_effect=OSError("uv not found")), \
                patch.object(session.sys.stdin, "isatty", return_value=True), \
                patch("builtins.input") as wait_for_enter, \
                patch.object(session.sys, "stderr", new_callable=io.StringIO) as error:
            self.assertEqual(session.main([]), 2)
            self.assertIn("uv not found", error.getvalue())
            wait_for_enter.assert_called_once_with("Press Enter to close this terminal.")

    def test_bun_checksum_mismatch_does_not_install_or_replace_binary(self):
        script = Path(__file__).resolve().parents[1] / "build-launcher.sh"
        bootstrap = script.read_text().split("python3 - <<'PY'\n", 1)[1].split("\nPY", 1)[0]
        for existing in (False, True):
            with self.subTest(existing=existing), tempfile.TemporaryDirectory() as directory:
                home = Path(directory)
                binary = home / ".local/bin/bun"
                if existing:
                    binary.parent.mkdir(parents=True)
                    binary.write_bytes(b"existing bun")
                response = io.BytesIO(b"corrupt download")
                with patch("urllib.request.urlopen", return_value=response) as download, \
                        patch.object(Path, "home", return_value=home):
                    with self.assertRaisesRegex(SystemExit, "SHA-256 verification; nothing was installed"):
                        exec(compile(bootstrap, str(script), "exec"), {})
                download.assert_called_once_with(
                    "https://github.com/oven-sh/bun/releases/download/bun-v1.4.3/bun-linux-x64.zip")
                if existing:
                    self.assertEqual(binary.read_bytes(), b"existing bun")
                    self.assertEqual(list(binary.parent.iterdir()), [binary])
                else:
                    self.assertFalse((home / ".local").exists())

    def test_desktop_opens_visible_terminal_with_game_icon(self):
        entry = install.desktop_entry(Path("/home/player/gothic-ai/play"))
        self.assertIn("Name=Gothic AI\n", entry)
        self.assertIn("Exec=ptyxis --new-window", entry)
        self.assertIn("scripts/ryzen-play-session.sh", entry)
        self.assertIn("Icon=/home/player/gothic-ai/play/launcher/app/src-tauri/icons/128x128.png", entry)

    def test_desktop_uses_window_only_after_a_successful_executable_build(self):
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory) / "play"
            binary = repo / "launcher/app/src-tauri/target/release/gothic-launcher"
            binary.parent.mkdir(parents=True)
            binary.touch()
            self.assertIn("Exec=ptyxis", install.desktop_entry(repo))
            binary.chmod(0o755)
            entry = install.desktop_entry(repo)
            self.assertIn(f'Exec="{binary}"\n', entry)
            self.assertNotIn("ptyxis", entry)
            self.assertIn("Exec=ptyxis", install.desktop_entry(repo, launcher_ready=False))


if __name__ == "__main__":
    unittest.main()
