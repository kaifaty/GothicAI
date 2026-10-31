"""Personal install/config and queue boundaries, without SSH or a real game."""
import fcntl
import importlib.machinery
import importlib.util
import io
import json
import os
import shutil
import subprocess
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
GAME_HELPER = (Path(__file__).resolve().parents[1] / "ryzen_game.py").read_text()


def prepared_game(home):
    system = home / "gothic-ai/game-settings/system"
    system.mkdir(parents=True)
    for name in ("Gothic.ini", "SystemPack.ini"):
        (system / name).write_text("[GAME]\n")
    (home / "gothic-ai/game-base.json").write_text(json.dumps({"base": "a" * 64, "videos": ""}))


def payload():
    return {"origin": "origin", "settings": {}, "game_helper": GAME_HELPER}


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
            self.assertEqual(result["game"]["dir"], str(home / "gothic-ai/game-settings"))
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
            self.assertEqual(path.read_text(), "# user's settings\n\n[game]\ndir = "
                             + json.dumps(str(home / "gothic-ai/game-settings")) + "\n")
            path.write_text('[game]\ndir = "/old/install"\ndev_mode = true\n'
                            '[paths]\ngame_home = "/personal/saves"\n')
            install.create_settings(path, source, repo, home)
            self.assertEqual(tomllib.loads(path.read_text()), {
                "game": {"dir": str(home / "gothic-ai/game-settings"), "dev_mode": True},
                "paths": {"game_home": "/personal/saves"},
            })

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

    def test_install_and_update_sync_only_play_content_before_ssh(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            settings = home / ".config/gothic-ai/settings.toml"
            settings.parent.mkdir(parents=True)
            settings.write_text("[game]\n")
            content = home / ".local/share/gothic-ai/content"
            content.mkdir(parents=True)
            for action in ("install", "update"):
                with self.subTest(action=action), patch.object(install.Path, "home", return_value=home), \
                        patch.object(install.subprocess, "check_output", return_value="origin\n"), \
                        patch.object(install, "run") as sync, \
                        patch.object(install, "owned_process") as remote:
                    def after_sync(command, payload):
                        self.assertEqual(sync.call_count, 2)
                        self.assertEqual(command[-2], "ryzen")
                        return 0

                    remote.side_effect = after_sync
                    self.assertEqual(install.main([action]), 0)
                    self.assertIn("--check", sync.call_args_list[0].args[0][-1])
                    command = sync.call_args.args[0]
                    self.assertEqual(command[:4], ["rsync", "-a", "--delete", "--checksum"])
                    self.assertEqual([arg for arg in command if arg.startswith("--include=")],
                                     ["--include=/materials/***", "--include=/meshes/***", "--include=/enabled.json"])
                    self.assertIn("--exclude=*", command)
                    self.assertIn("--delete-excluded", command)
                    self.assertEqual(command[command.index("-e") + 1],
                                     "ssh -o BatchMode=yes -o ConnectTimeout=10")
                    self.assertEqual(command[-2], f"{content}/")
                    self.assertEqual(command[-1], "ryzen:.local/share/gothic-ai/play-content.incoming/")
                    self.assertNotIn(".local/share/gothic-ai/content", command[-1])
                    self.assertNotIn(".local/share/gothic-ai/content",
                                     next(arg for arg in command if arg.startswith("--rsync-path=")))

    @unittest.skipUnless(shutil.which("rsync"), "needs rsync")
    def test_content_sync_filters_deletes_checksums_and_handles_missing_source(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            content = home / ".local/share/gothic-ai/content"
            materials = content / "materials"
            materials.mkdir(parents=True)
            (materials / "materials.json").write_text("{}")
            texture = materials / "albedo.dds"
            texture.write_bytes(b"first")
            (content / "meshes").mkdir()
            (content / "meshes/meshes.json").write_text("{}")
            (content / "enabled.json").write_text("{}")
            (content / "staging").mkdir()
            (content / "staging/private.log").write_text("not active content")
            (content / "build.log").touch()
            incoming = home / "remote/play-content.incoming"
            (incoming / "materials").mkdir(parents=True)
            (incoming / "materials/removed.dds").touch()
            (incoming / "stale.log").touch()

            def local_sync(command):
                # The remote mkdir command is not a local rsync executable.
                local = [arg for arg in command[:-1] if not arg.startswith("--rsync-path=")]
                result = subprocess.run([*local, f"{incoming}/"], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

            with patch.object(install.Path, "home", return_value=home), \
                    patch.object(install, "run", side_effect=local_sync):
                install.sync_content()
                self.assertEqual({str(path.relative_to(incoming)) for path in incoming.rglob("*") if path.is_file()},
                                 {"materials/materials.json", "materials/albedo.dds", "meshes/meshes.json", "enabled.json"})
                previous = texture.stat()
                texture.write_bytes(b"other")
                os.utime(texture, ns=(previous.st_atime_ns, previous.st_mtime_ns))
                install.sync_content()
                self.assertEqual((incoming / "materials/albedo.dds").read_bytes(), b"other")
                shutil.rmtree(materials)
                shutil.rmtree(content / "meshes")
                (content / "enabled.json").unlink()
                install.sync_content()
                self.assertEqual(list(incoming.iterdir()), [])
                (incoming / "stale.log").touch()
                shutil.rmtree(content)
                install.sync_content()
                self.assertEqual(list(incoming.iterdir()), [])

    def test_remote_install_preserves_shared_queue_and_profile(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            repo = home / "gothic-ai/play"
            repo.mkdir(parents=True)
            binary = repo / "launcher/app/src-tauri/target/release/gothic-launcher"
            binary.parent.mkdir(parents=True)
            binary.touch(mode=0o755)
            prepared_game(home)
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
            content = home / ".local/share/gothic-ai/play-content"
            content.mkdir(parents=True)
            default = content.with_name("content")
            default.mkdir()
            (default / "other-session").write_text("untouched")
            incoming = content.with_name("play-content.incoming")
            old = content.with_name("play-content.old")
            old.mkdir()
            (old / "interrupted-update").touch()
            rename = Path.rename

            def rename_under_lock(path, target):
                with (home / "gothic-ai/play.install.lock").open("a") as lock:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return rename(path, target)

            for action, result in (("install", 0), ("update", 0), ("update", 3)):
                (content / "old-file").touch()
                incoming.mkdir()
                if result == 0:
                    (incoming / "enabled.json").write_text(action)
                with self.subTest(action=action, result=result), patch.dict(install.os.environ), \
                        patch.object(install.Path, "home", return_value=home), \
                        patch.object(install.Path, "rename", autospec=True, side_effect=rename_under_lock) as swap, \
                        patch.object(install.os, "chdir"), \
                        patch.object(install, "run") as run, \
                        patch.object(install.subprocess, "check_output", side_effect=["main\n", "origin\n", ""]), \
                        patch.object(install, "owned_process", return_value=result) as build:
                    self.assertEqual(install.remote_install(action, payload(), "source"), result)
                    run.assert_called_once_with(["git", "-C", repo, "pull", "--ff-only"])
                    build.assert_called_once()
                    entry = (home / ".local/share/applications/gothic-ai.desktop").read_text()
                    self.assertEqual("Exec=ptyxis" in entry, result == 3)
                    self.assertEqual([call.args for call in swap.call_args_list], [(content, old), (incoming, content)])
                self.assertEqual(list(content.iterdir()), [content / "enabled.json"] if result == 0 else [])
                if result == 0:
                    self.assertEqual((content / "enabled.json").read_text(), action)
                self.assertFalse(incoming.exists())
                self.assertFalse(old.exists())
                self.assertEqual((default / "other-session").read_text(), "untouched")
                self.assertEqual([(path.read_bytes(), path.stat().st_mtime_ns) for path in (queue, profile)], before)

    def test_remote_install_requires_interactive_queue_before_git(self):
        for queue_source in (None, "#!/bin/sh\nprintf '%s\\n' '--display'\n", "#!/bin/sh\nexit 1\n"):
            with self.subTest(queue_source=queue_source), tempfile.TemporaryDirectory() as directory:
                home = Path(directory)
                prepared_game(home)
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
                        install.remote_install("install", payload(), "source")
                    run.assert_not_called()
                    metadata.assert_not_called()
                    build.assert_not_called()
                self.assertFalse((home / "gothic-ai/play").exists())

    def test_update_refuses_active_session_before_touching_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            home = Path(directory)
            prepared_game(home)
            lock_path = home / "gothic-ai/play.install.lock"
            lock_path.parent.mkdir(parents=True, exist_ok=True)
            content = home / ".local/share/gothic-ai/play-content"
            content.mkdir(parents=True)
            (content / "old-file").write_text("still playing")
            incoming = content.with_name("play-content.incoming")
            incoming.mkdir()
            (incoming / "new-file").write_text("waiting")
            with lock_path.open("w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                with patch.object(install.Path, "home", return_value=home), \
                        patch.object(install, "run") as run, \
                        patch.object(install.subprocess, "check_output") as metadata, \
                        patch.object(install, "owned_process") as build:
                    with self.assertRaisesRegex(ValueError, "playing"):
                        install.remote_install("update", payload(), "source")
                    run.assert_not_called()
                    metadata.assert_not_called()
                    build.assert_not_called()
            self.assertEqual((content / "old-file").read_text(), "still playing")
            self.assertEqual(list(content.iterdir()), [content / "old-file"])
            self.assertEqual((incoming / "new-file").read_text(), "waiting")
            self.assertFalse(content.with_name("play-content.old").exists())

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
