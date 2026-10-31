"""The base-game profile beside a modded Gothic II (scripts/windows/game_profile.py), on synthetic archives."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "windows" / "game_profile.py"
_spec = importlib.util.spec_from_file_location("game_profile", SCRIPT)
profile = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(profile)


def archive(game: Path, name: str, flags: int = 0x50) -> None:
    """A VDF header only: the comment, its signature and the flags the profile reads."""
    data = bytearray(296)
    data[256:272] = b"PSVDSC_V2.00\r\n\r\n"
    data[292:296] = flags.to_bytes(4, "little")
    (game / "Data" / name).write_bytes(data)


@pytest.fixture
def game(tmp_path):
    folder = tmp_path / "Steam Gothic II"
    (folder / "Data").mkdir(parents=True)
    (folder / "_work").mkdir()
    for name in profile.REQUIRED:
        archive(folder, name)
    return folder


def test_a_clean_installation_is_used_as_it_is(game, tmp_path):
    assert profile.prepare(game, tmp_path / "profiles") == game.resolve()
    assert not (tmp_path / "profiles").exists()


def test_mods_stay_out_and_nothing_of_the_game_is_copied_or_changed(game, tmp_path):
    archive(game, "Union.vdf", 0xA0)
    archive(game, "AB_Scripts.vdf")
    before = {p.name: p.read_bytes() for p in (game / "Data").iterdir()}
    result = profile.prepare(game, tmp_path / "profiles")
    assert {p.name for p in (result / "Data").iterdir()} == set(profile.REQUIRED)
    for name in profile.REQUIRED:
        assert os.path.samefile(result / "Data" / name, game / "Data" / name)  # links, not copies
    assert (result / "_work").resolve() == (game / "_work").resolve()
    assert before == {p.name: p.read_bytes() for p in (game / "Data").iterdir()}
    assert profile.prepare(game, tmp_path / "profiles") == result  # reused


def test_a_missing_original_archive_makes_no_profile(game, tmp_path):
    (game / "Data" / "Worlds.vdf").unlink()
    with pytest.raises(ValueError, match="Worlds.vdf"):
        profile.prepare(game, tmp_path / "profiles")
    assert not (tmp_path / "profiles").exists()


def test_a_compressed_original_archive_is_refused(game, tmp_path):
    archive(game, "Worlds.vdf", 0xA0)
    with pytest.raises(ValueError, match="compressed"):
        profile.prepare(game, tmp_path / "profiles")


def test_set_config_selects_the_result_through_the_launcher(game, tmp_path):
    fake = tmp_path / "fake-launcher" / "gothic_launcher"
    fake.mkdir(parents=True)
    (fake / "__init__.py").write_text("", encoding="utf-8")
    (fake / "cli.py").write_text(
        "import json, os, sys\nfrom pathlib import Path\n"
        "def main():\n"
        "    Path(os.environ['GOTHIC_TEST_CONFIG_OUT']).write_text(json.dumps(sys.argv[1:]), encoding='utf-8')\n",
        encoding="utf-8",
    )
    output = tmp_path / "selected-game.json"
    env = dict(os.environ, PYTHONPATH=str(fake.parent), PYTHONUTF8="1", GOTHIC_TEST_CONFIG_OUT=str(output))
    result = subprocess.run(
        [sys.executable, str(SCRIPT), str(game), "--set-config"], env=env, check=True, capture_output=True
    )
    assert json.loads(output.read_text(encoding="utf-8")) == ["config", "set", "game.dir", str(game.resolve())]
    assert json.loads(result.stdout)["game_dir"] == str(game.resolve())


def test_a_refused_installation_is_a_short_error(game, tmp_path):
    (game / "Data" / "Worlds.vdf").unlink()
    result = subprocess.run([sys.executable, str(SCRIPT), str(game)], capture_output=True, text=True)
    assert result.returncode == 1
    assert "Worlds.vdf" in result.stderr and "Traceback" not in result.stderr
