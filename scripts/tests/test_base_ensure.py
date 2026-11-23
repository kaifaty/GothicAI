"""Installation preparation uses the converter beside the selected game."""
import os
from pathlib import Path
import subprocess

import pytest


SCRIPT = Path(__file__).resolve().parents[1] / "base-ensure.sh"


@pytest.mark.parametrize("selection", [None, "OPENGOTHIC_BASE", "OPENGOTHIC_BASE_FINGERPRINT"])
def test_resolve_and_ready_base_skip(tmp_path, selection):
    binary = tmp_path / "engine with spaces" / "Gothic2Notr"
    binary.parent.mkdir()
    converter = binary.with_name("content-convert")
    converter.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >"$CALLS"\n')
    converter.chmod(0o755)
    calls = tmp_path / "calls"
    env = {key: value for key, value in os.environ.items()
           if key not in ("OPENGOTHIC_BASE", "OPENGOTHIC_BASE_FINGERPRINT", "CONTENT_CONVERT")}
    env.update(GAME_BIN=str(binary), CALLS=str(calls))
    if selection:
        env[selection] = "selected"
    result = subprocess.run(["bash", str(SCRIPT), str(tmp_path / "Gothic II")], env=env, check=False)
    assert result.returncode == 0
    if selection:
        assert not calls.exists()
    else:
        assert calls.read_text().splitlines() == ["resolve", str(tmp_path / "Gothic II")]


def test_converter_override_propagates_failure(tmp_path):
    converter = tmp_path / "converter"
    converter.write_text('#!/bin/sh\nexit 23\n')
    converter.chmod(0o755)
    env = {key: value for key, value in os.environ.items()
           if key not in ("OPENGOTHIC_BASE", "OPENGOTHIC_BASE_FINGERPRINT")}
    env.update(CONTENT_CONVERT=str(converter), GAME_BIN="/unused/Gothic2Notr")
    result = subprocess.run(["bash", str(SCRIPT), str(tmp_path)], env=env, check=False)
    assert result.returncode == 23
