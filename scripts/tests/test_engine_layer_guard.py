"""OpenGothic/cmake/EngineLayerGuard.cmake in `cmake -P` mode on a tiny synthetic engine tree."""

import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
GUARD = ROOT / "OpenGothic/cmake/EngineLayerGuard.cmake"
RULES = ROOT / "scripts/engine_layers.py"
SPEC = importlib.util.spec_from_file_location("gothic_engine_layers_guard", RULES)
layers = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = layers
SPEC.loader.exec_module(layers)

pytestmark = pytest.mark.skipif(shutil.which("cmake") is None, reason="no cmake")

TREE = {
    "common/daedalus/vm.h": "#pragma once\n",
    "common/game/gamescript.h": '#pragma once\n#include "daedalus/vm.h"\n',
    "common/world/world.h": '#include "game/gamescript.h"\n',
    "common/world/world.cpp": '#include "world.h"\n#include <graphics/mesh.h>\n',
    "common/graphics/mesh.h": "#pragma once\n#include <vector>\n",
    "common/graphics/mesh.cpp": '#include "mesh.h"\n',
}


def guard(tmp_path: Path, tree: dict[str, str], rules: Path = RULES) -> subprocess.CompletedProcess:
    engine = tmp_path / "with space" / "OpenGothic"
    for path, text in tree.items():
        (engine / path).parent.mkdir(parents=True, exist_ok=True)
        (engine / path).write_text(text)
    sources = ";".join(str(engine / path) for path in tree)
    inputs = tmp_path / "inputs.cmake"
    inputs.write_text(f"set(ENGINE_LAYER_GUARD_ROOT [==[{engine}]==])\n"
                      f"set(ENGINE_LAYER_GUARD_RULES [==[{rules}]==])\n"
                      f"set(ENGINE_LAYER_GUARD_SOURCES [==[{sources}]==])\n")
    return subprocess.run(["cmake", f"-DGUARD_INPUTS={inputs}", f"-DGUARD_STAMP={tmp_path / 'with space/guard.stamp'}",
                           "-P", str(GUARD)], capture_output=True, text=True)


def test_clean_tree_and_rpg_reaching_the_vm_pass(tmp_path):
    result = guard(tmp_path, TREE)
    assert result.returncode == 0, result.stderr
    assert "2 platform files of 6 files checked, none reach the script VM" in result.stdout
    depfile = (tmp_path / "with space/guard.stamp.d").read_text()
    assert "with\\ space/OpenGothic/common/world/world.cpp" in depfile


def test_platform_reaching_the_vm_through_rpg_fails_with_the_chain(tmp_path):
    tree = dict(TREE)
    tree["common/graphics/mesh.cpp"] = '#include "mesh.h"\n// draws the world\n#include "world/world.h"\n'
    result = guard(tmp_path, tree)
    assert result.returncode != 0
    message = " ".join(result.stderr.split())
    assert "1 of 2 platform files reach the script VM" in message
    assert ("common/graphics/mesh.cpp:3 -> common/world/world.h:1 -> common/game/gamescript.h:2"
            " -> common/daedalus/vm.h") in message
    assert "scripts/engine_layers.py" in message


def test_rules_read_by_the_guard_are_layer_rules(tmp_path):
    script = tmp_path / "rules.cmake"
    script.write_text(f'include("{GUARD.as_posix()}")\n'
                      f'engine_layer_guard_read_rules("{RULES.as_posix()}" patterns layers regexes)\n'
                      'foreach(pattern layer IN ZIP_LISTS patterns layers)\n'
                      '  message(STATUS "rule ${pattern} ${layer}")\n'
                      'endforeach()\n')
    result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    rules = [tuple(line.split()[2:]) for line in result.stdout.splitlines() if line.startswith("-- rule ")]
    assert rules == layers.LAYER_RULES


def test_unsupported_glob_fails(tmp_path):
    rules = tmp_path / "engine_layers.py"
    rules.write_text('LAYER_RULES: list[tuple[str, str]] = [\n    ("common/game/?ench", "tools"),\n'
                     '    ("common/*", "platform"),\n]\n')
    result = guard(tmp_path, TREE, rules)
    assert result.returncode != 0
    assert 'unsupported glob "common/game/?ench"' in " ".join(result.stderr.split())
