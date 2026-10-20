import os
import time
from dataclasses import replace
from pathlib import Path

import pytest

from gothic_launcher import cli, commands
from gothic_launcher.commands import (
    LaunchError,
    Layout,
    Ports,
    child_env,
    default_game_bin,
    default_hd_textures,
    default_log_root,
    find_layout,
    game,
    latest_save,
    services,
)
from gothic_launcher.settings import Config
from gothic_launcher.state import SessionFile

PORTS = Ports(server=50001, asr=50002, tts=50003)
LAYOUT = Layout(
    Path("/repo"), Path("/bin/uv"), Path("/repo/Gothic2Notr"), Path("/games/G2"), Path("/home/game")
)


@pytest.fixture
def config(tmp_path):
    return Config.load(tmp_path)


def by_name(procs):
    return {p.name: p for p in procs}


def test_services_get_their_settings_and_find_each_other(config, tmp_path):
    config.set("voice.voxtral_url", "http://gpu:47830")
    config.set("tts.provider", "voxcpm")
    config.set("tts.fallback", "none")
    config.set("tts.loudness", "-16")
    config.set("llm.model", "gpt-6.1-sol")
    config.set("llm.decider_url", "ws://gpu:47832")
    config.set("llm.quick_url", "http://gpu:47834/v1")
    config.set("llm.body_url", "tcp://gpu:47835")
    config.set("secrets.inworld_api_key", "iw-secret")
    config.set("secrets.openai_api_key", "sk-secret")

    procs = services(config, LAYOUT, PORTS, tmp_path)

    assert [p.name for p in procs] == ["asr", "tts", "server"]
    asr, tts, server = by_name(procs).values()
    assert asr.argv[:5] == ["/bin/uv", "run", "--project", "/repo/asr-service", "gothic-asr"]
    assert asr.argv[5:] == ["serve", "--port", "50002", "--provider", "voxtral", "--fallback", "chatgpt"]
    assert asr.env["GOTHIC_VOXTRAL_URL"] == "http://gpu:47830"
    assert tts.argv[5:] == [
        "serve",
        "--port",
        "50003",
        "--provider",
        "voxcpm",
        "--fallback",
        "none",
        "--loudness=-16",
    ]
    assert tts.env["INWORLD_API_KEY"] == "iw-secret"
    assert server.health == "http://127.0.0.1:50001/health" and server.cwd == tmp_path
    args = server.argv
    assert args[args.index("--asr-url") + 1] == "ws://127.0.0.1:50002"
    assert args[args.index("--tts-url") + 1] == "ws://127.0.0.1:50003"
    assert args[args.index("--model") + 1] == "gpt-6.1-sol" and "--no-console" in args
    assert args[args.index("--decider-url") + 1] == "ws://gpu:47832"
    assert args[args.index("--quick-url") + 1] == "http://gpu:47834/v1" and "--no-quick" not in args
    assert args[args.index("--body-url") + 1] == "tcp://gpu:47835"
    assert args[args.index("--game-dir") + 1] == "/games/G2"
    for p in procs:  # keys never in argv: they would end up in logs and `ps`
        assert not any("secret" in a for a in p.argv)


def test_voice_and_npc_voices_can_be_off(config, tmp_path):
    config.set("voice.enabled", "false")
    config.set("tts.enabled", "false")
    procs = services(config, LAYOUT, PORTS, tmp_path)
    assert [p.name for p in procs] == ["server"]
    assert "--no-voice" in procs[0].argv and "--no-tts" in procs[0].argv


def test_the_quick_model_is_on_unless_turned_off(config, tmp_path):
    assert "--no-quick" not in by_name(services(config, LAYOUT, PORTS, tmp_path))["server"].argv
    config.set("llm.quick", "false")
    assert "--no-quick" in by_name(services(config, LAYOUT, PORTS, tmp_path))["server"].argv


def test_the_game_runs_in_its_home_and_finds_the_server(config):
    config.set("game.chat_scale", "1.5")
    g = game(config, LAYOUT, PORTS)
    assert g.argv == ["/repo/Gothic2Notr", "-g", "/games/G2"]
    assert g.cwd == Path("/home/game") and not g.restart and g.health is None
    assert g.env == {"GOTHIC_AI_URL": "ws://127.0.0.1:50001", "GOTHIC_AI_CHAT_SCALE": "1.5"}
    assert game(config, LAYOUT, PORTS, save="3").argv[-2:] == ["-save", "3"]
    assert game(config, LAYOUT, PORTS, new=True).argv[-1] == "-nomenu"
    config.set("game.dev_mode", "true")
    assert game(config, LAYOUT, PORTS).argv[-1] == "-devmode"


def test_the_game_gets_the_sharper_textures_when_they_are_made(config, tmp_path):
    assert "OPENGOTHIC_TEXTURES" not in game(config, LAYOUT, PORTS).env
    with_hd = Layout(LAYOUT.repo, LAYOUT.uv, LAYOUT.game_bin, LAYOUT.game_dir, LAYOUT.game_home, tmp_path)
    assert game(config, with_hd, PORTS).env["OPENGOTHIC_TEXTURES"] == str(tmp_path)


def test_the_game_gets_the_fracture_cache_when_made(config, tmp_path, monkeypatch):
    monkeypatch.delenv("OPENGOTHIC_FRACTURE", raising=False)
    assert "OPENGOTHIC_FRACTURE" not in game(config, LAYOUT, PORTS).env
    layout = replace(LAYOUT, fracture_cache=tmp_path)
    assert game(config, layout, PORTS).env["OPENGOTHIC_FRACTURE"] == str(tmp_path)


@pytest.mark.parametrize("override", ["/another/fracture", ""])
def test_the_game_preserves_an_explicit_fracture_override(config, tmp_path, monkeypatch, override):
    monkeypatch.setenv("OPENGOTHIC_FRACTURE", override)
    layout = replace(LAYOUT, fracture_cache=tmp_path)
    assert game(config, layout, PORTS).env["OPENGOTHIC_FRACTURE"] == override


def test_latest_save(tmp_path):
    assert latest_save(tmp_path) is None
    for i, slot in enumerate(["1", "0", "12"]):
        p = tmp_path / f"save_slot_{slot}.sav"
        p.write_bytes(b"")
        os.utime(p, (time.time() + i, time.time() + i))
    (tmp_path / "save_slot_x.sav").write_bytes(b"")
    assert latest_save(tmp_path) == "12"


def test_the_inherited_environment_cannot_override_the_settings(monkeypatch):
    monkeypatch.setenv("GOTHIC_TTS_PROVIDER", "codex")
    monkeypatch.setenv("INWORLD_API_KEY", "from-the-shell")
    monkeypatch.setenv("HOME_TEST_VALUE", "kept")
    monkeypatch.setenv("VIRTUAL_ENV", "/launcher/.venv")
    env = child_env({"GOTHIC_AI_URL": "ws://127.0.0.1:1"})
    assert "GOTHIC_TTS_PROVIDER" not in env and "INWORLD_API_KEY" not in env and "VIRTUAL_ENV" not in env
    assert env["HOME_TEST_VALUE"] == "kept" and env["GOTHIC_AI_URL"] == "ws://127.0.0.1:1"
    assert env["PYTHONUNBUFFERED"] == "1"


def test_find_layout_lists_everything_missing(config, tmp_path, monkeypatch):
    repo = tmp_path / "repo"
    (repo / "server").mkdir(parents=True)
    (repo / "server" / "pyproject.toml").write_text("")
    config.set("paths.repo", str(repo))
    config.set("paths.uv", "/bin/sh")
    with pytest.raises(LaunchError) as err:
        find_layout(config)
    assert "not built" in str(err.value) and "game folder is not set" in str(err.value)

    game_dir = tmp_path / "Gothic II"
    (game_dir / "DATA").mkdir(parents=True)  # case differs between installs
    binary = repo / "OpenGothic" / "build" / "opengothic" / "Gothic2Notr"
    binary.parent.mkdir(parents=True)
    binary.write_text("#!/bin/sh\n")
    binary.chmod(0o755)
    config.set("game.dir", str(game_dir))
    layout = find_layout(config)
    assert layout.game_bin == binary and layout.game_dir == game_dir and layout.uv == Path("/bin/sh")

    bundled = binary.parent / "OpenGothic Dev.app" / "Contents" / "MacOS" / "Gothic2Notr"
    bundled.parent.mkdir(parents=True)
    bundled.symlink_to("../../../Gothic2Notr")  # as scripts/make-dev-app.sh makes it
    monkeypatch.setattr(commands.sys, "platform", "darwin")
    assert find_layout(config).game_bin == bundled  # the game gets its own name and icon in the Dock
    monkeypatch.setattr(commands.sys, "platform", "linux")
    assert default_game_bin(repo) == binary  # Linux never chooses a stray macOS bundle


@pytest.mark.parametrize("platform", ["linux", "darwin"])
def test_platform_log_and_texture_paths(monkeypatch, platform):
    monkeypatch.setattr(commands.sys, "platform", platform)
    home = Path.home()
    if platform == "linux":
        assert default_log_root() == home / ".local/state/gothic-ai/logs"
        assert default_hd_textures() == home / "gothic-ai/hd-textures/textures"
    else:
        assert default_log_root() == home / "Library/Logs/GothicAI"
        assert default_hd_textures() == home / ".local/share/gothic-ai/hd-textures/textures"


def test_linux_missing_credentials_is_a_short_cli_error(config, tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(cli.sys, "platform", "linux")
    monkeypatch.setattr(cli.sys, "argv", ["gothic-launcher", "up", "--continue"])
    monkeypatch.setattr(cli, "_config", lambda: config)
    monkeypatch.setattr(cli, "CHATGPT_TOKENS", tmp_path / "chatgpt.json")
    monkeypatch.setattr(cli, "SessionFile", lambda: SessionFile(tmp_path / "launcher.json"))
    with pytest.raises(SystemExit) as err:
        cli.main()
    assert err.value.code == 2
    error = capsys.readouterr().err
    assert "gothic-launcher login" in error
    assert "gothic-launcher config set secrets.inworld_api_key" in error
    assert "Traceback" not in error


def test_openai_credentials_are_checked_only_when_selected(config, tmp_path, monkeypatch):
    monkeypatch.setattr(cli, "CHATGPT_TOKENS", tmp_path / "chatgpt.json")
    config.set("llm.backend", "openai")
    config.set("voice.enabled", False)
    config.set("tts.enabled", False)
    with pytest.raises(LaunchError, match="config set secrets.openai_api_key"):
        cli._require_credentials(config)
    config.set("secrets.openai_api_key", "test-key")
    cli._require_credentials(config)


def test_smoke_overrides_never_spend_model_quota_or_change_saved_settings(config, tmp_path, monkeypatch):
    config.set("llm.decider_url", "ws://gpu:47832")
    config.set("llm.quick_url", "http://gpu:47834/v1")
    config.set("llm.body_url", "tcp://gpu:47835")
    config.save()
    saved = config.settings_path.read_text()
    cli._smoke_settings(config)
    monkeypatch.setattr(cli, "CHATGPT_TOKENS", tmp_path / "chatgpt.json")
    cli._require_credentials(config)
    [server] = services(config, LAYOUT, PORTS, tmp_path)
    assert server.name == "server"
    assert server.argv[server.argv.index("--llm") + 1] == "fake"
    assert {"--no-quick", "--no-voice", "--no-tts"}.issubset(server.argv)
    assert not {"--decider-url", "--quick-url", "--body-url"}.intersection(server.argv)
    assert config.settings_path.read_text() == saved


def test_free_ports_differ():
    ports = Ports.free()
    assert len({ports.server, ports.asr, ports.tts}) == 3
