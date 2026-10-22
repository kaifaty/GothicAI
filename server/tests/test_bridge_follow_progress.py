"""Compile and execute follow progress detection without game assets."""

import shutil
import subprocess
from pathlib import Path

import pytest


def test_native_follow_progress_detects_oscillation_without_blocking_running(tmp_path):
    compiler = shutil.which("c++")
    if compiler is None:
        pytest.skip("needs a C++ compiler for native follow progress detection")
    bridge = Path(__file__).resolve().parents[2] / "OpenGothic" / "ai_bridge"
    executable = tmp_path / "followprogress-contract"
    subprocess.run(
        [compiler, "-std=c++20", "-Wall", "-Wconversion", "-Werror", "-I", str(bridge),
         str(bridge / "tests" / "followprogress_contract.cpp"), "-o", str(executable)],
        check=True, capture_output=True, text=True, timeout=60,
    )
    checked = subprocess.run([str(executable)], check=True, capture_output=True, text=True, timeout=10)
    assert checked.stdout == "Follow progress contract: 73 checks passed\n"
