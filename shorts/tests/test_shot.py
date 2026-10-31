from pathlib import Path
from unittest.mock import AsyncMock, Mock

import pytest
from gothic_launcher.commands import Layout, Ports
from gothic_launcher.settings import Config

from gothic_shorts import film
from gothic_shorts.shot import Shot, ShotError, load

SHOT = '''
title = "Проба"
hook = "Зацепка"
npc = "NONE_100_XARDAS"
music = 0.2
script = """
cam shoulder {npc}   # over the hero's shoulder
rec main
hero Ксардас, привет.
waitreply
hero Пока.
stop
"""
'''


def test_hero_lines_become_speak_with_their_recording(tmp_path):
    path = tmp_path / "probe.toml"
    path.write_text(SHOT)
    shot = load(path)
    assert (shot.name, shot.title, shot.music) == ("probe", "Проба", 0.2)
    assert shot.hero_lines == ["Ксардас, привет.", "Пока."]
    wav = tmp_path / "a.wav"
    scenario = shot.scenario({"Ксардас, привет.": wav, "Пока.": wav})
    assert scenario.splitlines() == [
        "cam shoulder NONE_100_XARDAS",
        "rec main",
        f"ptt {wav.resolve()} Ксардас, привет.",
        "waitreply",
        f"ptt {wav.resolve()} Пока.",
        "stop",
    ]


def test_a_shot_has_to_record(tmp_path):
    path = tmp_path / "bad.toml"
    path.write_text('script = "hero Привет"')
    with pytest.raises(ShotError, match="rec"):
        load(path)


def test_game_music_defaults_off(tmp_path):
    path = tmp_path / "probe.toml"
    path.write_text('script = "rec main"')
    assert load(path).music == Shot("probe", "Проба", "rec main").music == 0.0


def test_music_bed_defaults_and_shot_override(tmp_path):
    path = tmp_path / "probe.toml"
    path.write_text('script = "rec main"')
    assert load(path).bed == Shot("probe", "Проба", "rec main").bed == "khorinis-countryside"
    assert load(path).bed_start is None
    path.write_text('bed = "xardas-tower"\nbed_start = 12.5\nscript = "rec main"')
    shot = load(path)
    assert (shot.bed, shot.bed_start) == ("xardas-tower", 12.5)
    assert "bed" not in shot.extra and "bed_start" not in shot.extra


@pytest.mark.parametrize("value", ["-1", "nan", "inf", '"later"'])
def test_bed_start_must_be_nonnegative_seconds(tmp_path, value):
    path = tmp_path / "probe.toml"
    path.write_text(f'bed_start = {value}\nscript = "rec main"')
    with pytest.raises(ShotError, match="bed_start"):
        load(path)


def test_film_records_effects_and_separate_voices_without_game_music(tmp_path, monkeypatch):
    save = tmp_path / "source.sav"
    save.write_bytes(b"save")
    shot = Shot("probe", "Проба", "rec main", save=save, music=0.35, sound=0.7)
    layout = Layout(tmp_path, Path("uv"), Path("game-bin"), tmp_path / "game", tmp_path)
    monkeypatch.setattr(film, "voice_lines", Mock(return_value={}))
    monkeypatch.setattr(film.Ports, "free", lambda: Ports(10001, 10002, 10003))
    supervisor = Mock(return_value=Mock(run=AsyncMock(return_value=0)))
    monkeypatch.setattr(film, "Supervisor", supervisor)

    config = Config.load(tmp_path / "config")
    take = film.film(shot, config, layout)

    assert (take / "run" / "Gothic.ini").read_text() == (
        "[INTERNAL]\nvidResIndex=0\n[SOUND]\nsoundVolume=0.7\nmusicVolume=0\nmusicEnabled=0\n"
        "worldSounds=0\n"
    )
    game = supervisor.call_args.args[1]
    assert game.env["OPENGOTHIC_BENCH_VOICE_STEM"] == "1"
    assert game.env["TEMPEST_AUDIO_LOOPBACK"] == "48000"
    tts = next(proc for proc in supervisor.call_args.args[0] if proc.name == "tts")
    assert "--no-original-voices" in tts.argv
    assert config.settings.tts.original_voices is True


@pytest.mark.parametrize(
    "setting, expected", [("", True), ("brand = true\n", True), ("brand = false\n", False)]
)
def test_brand_defaults_on_and_reads_toml(tmp_path, setting, expected):
    path = tmp_path / "probe.toml"
    path.write_text(setting + SHOT)
    shot = load(path)
    assert shot.brand is expected
    assert "brand" not in shot.extra


def test_the_shots_in_the_repository_load():
    for path in (Path(__file__).parents[1] / "shots").glob("*.toml"):
        shot = load(path)
        assert shot.hero_lines
        assert shot.bed == "xardas-tower"
