import stat
import sys
import tomllib
from pathlib import Path

import pytest

from gothic_launcher.settings import Config, SettingsError


def test_missing_files_mean_defaults(tmp_path):
    config = Config.load(tmp_path)
    assert config.settings.llm.backend == "chatgpt"
    assert config.settings.llm.body_policy == "engine" and config.settings.llm.body_net is None
    assert config.settings.voice.provider == "inworld" and config.settings.tts.provider == "inworld"
    assert config.settings.voice.fallback == "chatgpt"
    assert config.settings.tts.original_voices is True
    assert config.secrets.inworld_api_key is None


@pytest.mark.skipif(sys.platform == "win32", reason="file modes and a POSIX path; Windows: test_windows.py")
def test_saved_settings_load_back_and_keys_stay_in_their_own_file(tmp_path):
    config = Config.load(tmp_path)
    config.set("game.dir", "/games/Gothic II")
    config.set("tts.original_voices", "false")
    config.set("llm.body_policy", "service")
    config.set("llm.body_net", "/nets/p30.bodynet")
    config.set("secrets.inworld_api_key", "iw-secret")
    config.save()

    again = Config.load(tmp_path)
    assert str(again.settings.game.dir) == "/games/Gothic II"
    assert again.settings.tts.original_voices is False
    assert again.settings.llm.body_policy == "service"
    assert str(again.settings.llm.body_net) == "/nets/p30.bodynet"
    assert again.secrets.inworld_api_key == "iw-secret"
    assert "iw-secret" not in config.settings_path.read_text()
    assert stat.S_IMODE(config.secrets_path.stat().st_mode) == 0o600
    assert tomllib.loads(config.settings_path.read_text())["game"]["dir"] == "/games/Gothic II"
    assert "iw-secret" not in again.show() and "<set>" in again.show()


def test_values_are_converted_and_checked(tmp_path):
    config = Config.load(tmp_path)
    config.set("game.dev_mode", "true")
    config.set("tts.loudness", "-16")
    assert config.settings.game.dev_mode is True and config.settings.tts.loudness == -16.0
    config.set("tts.loudness", "none")
    assert config.settings.tts.loudness == "none"
    with pytest.raises(SettingsError, match="llm.backend"):
        config.set("llm.backend", "claude.ai")
    with pytest.raises(SettingsError, match="llm.body_policy"):
        config.set("llm.body_policy", "other")
    with pytest.raises(SettingsError, match="unknown setting"):
        config.set("llm.temperature", "1")
    with pytest.raises(SettingsError, match="game.chat_scale"):
        config.set("game.chat_scale", "0")


def test_unset_goes_back_to_the_default(tmp_path):
    config = Config.load(tmp_path)
    config.set("voice.provider", "chatgpt")
    config.set("voice.provider", None)
    assert config.settings.voice.provider == "inworld"


def test_import_from_env(tmp_path):
    env = tmp_path / ".env"
    env.write_text(
        "# comment\n"
        "GOTHIC_VOXTRAL_URL=http://gpu:47830\n"
        "GOTHIC_BODY_POLICY=service\n"
        "OPENGOTHIC_BODY_NET=/nets/p30.bodynet\n"
        "export GOTHIC_TTS_PROVIDER='voxcpm'\n"
        'INWORLD_API_KEY="iw"\n'
        "OPENAI_API_KEY=\n"
        "GOTHIC_GPU_SSH=me@gpu\n"
    )
    config = Config.load(tmp_path)
    imported, skipped = config.import_env(env)
    assert imported == [
        "voice.voxtral_url", "llm.body_policy", "llm.body_net", "tts.provider", "secrets.inworld_api_key"
    ]
    assert skipped == ["GOTHIC_GPU_SSH"]
    assert config.settings.voice.voxtral_url == "http://gpu:47830"
    assert config.settings.llm.body_policy == "service"
    assert config.settings.llm.body_net == Path("/nets/p30.bodynet")
    assert config.settings.tts.provider == "voxcpm" and config.secrets.inworld_api_key == "iw"
    assert config.secrets.openai_api_key is None  # empty values are not settings


def test_a_broken_file_names_itself(tmp_path):
    (tmp_path / "settings.toml").write_text("[game\n")
    with pytest.raises(SettingsError, match="settings.toml"):
        Config.load(tmp_path)
    (tmp_path / "settings.toml").write_text("[llm]\nbackend = 'nope'\n")
    with pytest.raises(SettingsError, match="llm.backend"):
        Config.load(tmp_path)
