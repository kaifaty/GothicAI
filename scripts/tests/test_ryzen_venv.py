"""Task environments use one torch pin without replacing older experiments."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

import pytest


HELPER = Path(__file__).resolve().parents[1] / "ryzen-venv.sh"
FAKE_UV = r'''
import json
import os
from pathlib import Path
import shlex
import sys

args = sys.argv[1:]
with open(os.environ["UV_CALLS"], "a") as output:
    output.write(json.dumps(args) + "\n")
if args[0] == "venv":
    venv = Path(args[-1])
    (venv / "bin").mkdir(parents=True)
    (venv / "packages").mkdir()
    python = venv / "bin/python"
    python.write_text("#!/bin/sh\nexport PYTHONPATH=" + shlex.quote(str(venv / "packages"))
                      + "\nexec " + shlex.quote(sys.executable) + ' -S "$@"\n')
    python.chmod(0o755)
elif args[:2] == ["pip", "install"]:
    assert "--index-url" not in args
else:
    raise AssertionError(args)
'''


@pytest.fixture
def host(tmp_path):
    binary = tmp_path / "uv"
    binary.write_text(f"#!{sys.executable}\n" + FAKE_UV)
    binary.chmod(0o755)
    return {**os.environ, "PATH": str(tmp_path) + os.pathsep + os.environ["PATH"],
            "UV_CALLS": str(tmp_path / "calls.jsonl")}


def run(host, venv, *packages):
    result = subprocess.run(
        ["bash", "-c", 'set -euo pipefail; source "$1"; shift; ryzen_task_venv "$@"',
         "test-ryzen-venv", str(HELPER), str(venv), *packages],
        env=host, capture_output=True, text=True, timeout=10)
    assert result.returncode == 0, result.stderr
    return result


def calls(host):
    return [json.loads(line) for line in Path(host["UV_CALLS"]).read_text().splitlines()]


def install_metadata(venv, name, version):
    dist = venv / "packages" / f"{name}-{version}.dist-info"
    dist.mkdir()
    (dist / "METADATA").write_text(f"Name: {name}\nVersion: {version}\n")


def test_new_environment_pins_torch_and_passes_extra_packages(host, tmp_path):
    venv = tmp_path / "task env"
    result = run(host, venv, "scipy", "tqdm==4.67.1")
    assert result.stderr == ""
    assert calls(host) == [
        ["venv", "--python", "3.12", str(venv)],
        ["pip", "install", "--python", str(venv / "bin/python"),
         "torch==2.14.1", "numpy", "pytest", "scipy", "tqdm==4.67.1"],
    ]


@pytest.mark.parametrize("version", ["2.9.1+cu128", "2.14.1+cu130"])
def test_existing_environment_preserves_installed_torch(host, tmp_path, version):
    venv = tmp_path / "old task"
    run(host, venv)
    install_metadata(venv, "torch", version)
    install_metadata(venv, "tqdm", "4.66.0")
    Path(host["UV_CALLS"]).write_text("")
    result = run(host, venv, "tqdm==4.67.1", "scipy")
    if version == "2.9.1+cu128":
        assert result.stderr.count("Warning:") == 1
        assert all(value in result.stderr for value in (str(venv), version, "2.14.1"))
        assert calls(host) == [
            ["pip", "install", "--python", str(venv / "bin/python"),
             "torch==2.9.1+cu128", "scipy"],
        ]
        Path(host["UV_CALLS"]).write_text("")
        assert run(host, venv).stderr.count("Warning:") == 1
        assert calls(host) == []
    else:
        assert result.stderr == ""
        assert calls(host) == [
            ["pip", "install", "--python", str(venv / "bin/python"),
             "torch==2.14.1", "numpy", "pytest", "tqdm==4.67.1", "scipy"],
        ]
