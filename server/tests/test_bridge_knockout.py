"""Compile and execute the production knockout.h preflight without game assets."""

import shutil
import subprocess
from pathlib import Path

import pytest


def test_native_knockout_checks_both_participants_and_plans_nonlethal_loss(tmp_path):
    compiler = shutil.which("c++")
    if compiler is None:
        pytest.skip("needs a C++ compiler for the native knockout preflight")
    bridge = Path(__file__).resolve().parents[2] / "OpenGothic" / "ai_bridge"
    executable = tmp_path / "knockout-contract"
    subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wconversion", "-Werror", "-I", str(bridge),
         str(bridge / "tests" / "knockout_contract.cpp"), "-o", str(executable)],
        check=True, capture_output=True, text=True, timeout=60,
    )
    checked = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=10)
    assert checked.stdout == "Knockout contract: 46 checks passed\n"
