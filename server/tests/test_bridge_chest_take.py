"""Compile and execute the bridge's production chest preflight without game assets."""

import shutil
import subprocess
from pathlib import Path

import pytest


def test_native_chest_take_checks_batch_quantity_and_mission_items(tmp_path):
    compiler = shutil.which("c++")
    if compiler is None:
        pytest.skip("needs a C++ compiler for the native chest_take preflight")
    bridge = Path(__file__).resolve().parents[2] / "OpenGothic" / "ai_bridge"
    executable = tmp_path / "chesttake-contract"
    subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wconversion", "-Werror", "-I", str(bridge),
         str(bridge / "tests" / "chesttake_contract.cpp"), "-o", str(executable)],
        check=True, capture_output=True, text=True, timeout=60,
    )
    checked = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=10)
    assert checked.stdout == "Chest take contract: 10 checks passed\n"
