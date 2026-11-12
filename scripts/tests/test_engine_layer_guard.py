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
    stamp = tmp_path / "with space/guard.stamp"
    assert stamp.is_file()
    depfile = (tmp_path / "with space/guard.stamp.d").read_text()
    assert depfile.split(": ", 1)[0] == str(stamp).replace(" ", "\\ ")
    assert "with\\ space/OpenGothic/common/world/world.cpp" in depfile


def test_platform_reaching_the_vm_through_rpg_fails_with_the_chain(tmp_path):
    tree = dict(TREE)
    tree["common/graphics/mesh.cpp"] = '#include "mesh.h"\n// draws the world\n#include "world/world.h"\n'
    tree["common/graphics/light.cpp"] = '#include "game/gamescript.h"\n'
    result = guard(tmp_path, tree)
    assert result.returncode != 0
    message = " ".join(result.stderr.split())
    assert "2 of 3 platform files reach the script VM" in message
    assert ("common/graphics/light.cpp:1 -> common/game/gamescript.h:2 -> common/daedalus/vm.h") in message
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


def test_guard_layers_match_layer_of_on_the_real_tree(tmp_path):
    engine = ROOT / "OpenGothic"
    paths = sorted(p.relative_to(engine).as_posix() for d in layers.SCAN_DIRS
                   for p in (engine / d).rglob("*") if p.suffix in layers.EXTS and p.is_file())
    listing = tmp_path / "paths.txt"
    listing.write_text("\n".join(paths) + "\n")
    script = tmp_path / "layers.cmake"
    script.write_text(f'include("{GUARD.as_posix()}")\n'
                      f'engine_layer_guard_read_rules("{RULES.as_posix()}" patterns layers regexes)\n'
                      f'file(STRINGS "{listing.as_posix()}" paths)\n'
                      'set(out "")\n'
                      'foreach(path IN LISTS paths)\n'
                      '  engine_layer_guard_layer_of("${path}" "${regexes}" "${layers}" layer)\n'
                      '  string(APPEND out "${path} ${layer}\\n")\n'
                      'endforeach()\n'
                      f'file(WRITE "{(tmp_path / "layers.txt").as_posix()}" "${{out}}")\n')
    result = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    guard_layers = dict(line.split(" ") for line in (tmp_path / "layers.txt").read_text().splitlines())
    assert len(paths) > 500
    assert guard_layers == {path: layers.layer_of(path) for path in paths}


@pytest.mark.parametrize("glob", ["common/game/?ench", "common/game/[bn]ench"])
def test_unsupported_glob_fails(tmp_path, glob):
    rules = tmp_path / "engine_layers.py"
    rules.write_text(f'LAYER_RULES: list[tuple[str, str]] = [\n    ("{glob}", "tools"),\n'
                     '    ("common/*", "platform"),\n]\n')
    result = guard(tmp_path, TREE, rules)
    assert result.returncode != 0
    assert f'unsupported glob "{glob}"' in " ".join(result.stderr.split())
