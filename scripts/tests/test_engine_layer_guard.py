"""Engine layer include and link-map guards in `cmake -P` mode."""

import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
GUARD = ROOT / "OpenGothic/cmake/EngineLayerGuard.cmake"
PLATFORM_MAP = ROOT / "OpenGothic/cmake/EnginePlatformMap.cmake"
RULES = ROOT / "scripts/engine_layers.py"
SPEC = importlib.util.spec_from_file_location("gothic_engine_layers_guard", RULES)
layers = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = layers
SPEC.loader.exec_module(layers)

pytestmark = pytest.mark.skipif(shutil.which("cmake") is None, reason="no cmake")

TREE = {
    "common/daedalus/vm.h": "#pragma once\n",
    "common/game/gamerules.h": "#pragma once\n",
    "common/game/gamescript.h": '#pragma once\n#include "daedalus/vm.h"\n#include "game/gamerules.h"\n',
    "common/game/gamescript.cpp": '#include "gamescript.h"\n#include "world/world.h"\n',
    "common/world/world.h": '#include "game/gamerules.h"\n',
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


def test_clean_tree_and_gothic_reaching_the_vm_pass(tmp_path):
    result = guard(tmp_path, TREE)
    assert result.returncode == 0, result.stderr
    assert "2 platform and 3 RPG files of 8 files checked, none reach the script VM" in result.stdout
    stamp = tmp_path / "with space/guard.stamp"
    assert stamp.is_file()
    depfile = (tmp_path / "with space/guard.stamp.d").read_text()
    assert depfile.split(": ", 1)[0] == str(stamp).replace(" ", "\\ ")
    assert "with\\ space/OpenGothic/common/world/world.cpp" in depfile


def test_platform_reaching_the_vm_fails_with_the_chain(tmp_path):
    tree = dict(TREE)
    tree["common/graphics/mesh.cpp"] = '#include "mesh.h"\n// draws the script\n#include "game/gamescript.h"\n'
    tree["common/graphics/light.cpp"] = '#include "daedalus/vm.h"\n'
    result = guard(tmp_path, tree)
    assert result.returncode != 0
    message = " ".join(result.stderr.split())
    assert "2 of 6 platform and RPG files reach the script VM" in message
    assert "[platform] common/graphics/light.cpp:1 -> common/daedalus/vm.h" in message
    assert "[platform] common/graphics/mesh.cpp:3 -> common/game/gamescript.h:2 -> common/daedalus/vm.h" in message
    assert "[rpg]" not in message
    assert "scripts/engine_layers.py" in message


def test_rpg_reaching_the_vm_through_a_gothic_header_fails_with_the_chain(tmp_path):
    tree = dict(TREE)
    tree["common/world/world.h"] = '#include "game/gamerules.h"\n#include "game/gamescript.h"\n'
    result = guard(tmp_path, tree)
    assert result.returncode != 0
    message = " ".join(result.stderr.split())
    assert "2 of 5 platform and RPG files reach the script VM" in message
    assert ("[rpg] common/world/world.cpp:1 -> common/world/world.h:2 -> common/game/gamescript.h:2"
            " -> common/daedalus/vm.h") in message
    assert "[rpg] common/world/world.h:2 -> common/game/gamescript.h:2 -> common/daedalus/vm.h" in message
    assert "[platform]" not in message
    assert "game/gamerules.h" in message


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


@pytest.mark.parametrize("link_map,error", [
    ("libgothic-base-runtime.a(sha256.cpp.o)\n", ""),
    ("[ 9] /build/libgothic-base-runtime.a[5](sha256.cpp.o)\n", ""),
    ("libgothic-base-runtime.a(programdata.cpp.o)\n",
     "gothic-base-runtime(programdata.cpp.o) from common/programdata.cpp belongs to gothic, not platform"),
    ("# Object files:\n[ 1] unrelated.cpp.o\n", "no mixed runtime objects found"),
], ids=["gnu-platform", "ld64-platform", "gothic-object", "no-runtime-objects"])
def test_platform_link_map(tmp_path, link_map, error):
    directory = tmp_path / "with space"
    directory.mkdir()
    map_file = directory / "link.map"
    map_file.write_text(link_map)
    inputs = directory / "inputs.cmake"
    inputs.write_text(f"set(PLATFORM_RULES [==[{RULES}]==])\n"
                      "set(PLATFORM_MEMBERS [==[gothic-base-runtime|sha256.cpp.o|common/sha256.cpp;"
                      "gothic-base-runtime|programdata.cpp.o|common/programdata.cpp]==])\n")
    result = subprocess.run(["cmake", f"-DPLATFORM_MAP={map_file}", f"-DPLATFORM_INPUTS={inputs}",
                             "-P", str(PLATFORM_MAP)], capture_output=True, text=True)
    if error:
        assert result.returncode != 0
        assert error in " ".join(result.stderr.split())
    else:
        assert result.returncode == 0, result.stderr
        assert "1 mixed runtime objects checked, all platform" in result.stdout


@pytest.mark.parametrize("link_map,error", [
    ("libgothic-base-runtime.a(sha256.cpp.o)\n", ""),
    ("libgothic-base-runtime.a(programdata.cpp.o)\n",
     "engine-island: gothic-base-runtime(programdata.cpp.o) from common/programdata.cpp belongs to gothic, "
     "not platform or island"),
], ids=["platform", "gothic-object"])
def test_island_link_map(tmp_path, link_map, error):
    """engine-island's map (cmake/Island.cmake): the island besides the platform, named in the messages."""
    map_file = tmp_path / "link.map"
    map_file.write_text(link_map)
    inputs = tmp_path / "inputs.cmake"
    inputs.write_text(f"set(PLATFORM_RULES [==[{RULES}]==])\n"
                      "set(PLATFORM_MEMBERS [==[gothic-base-runtime|sha256.cpp.o|common/sha256.cpp;"
                      "gothic-base-runtime|programdata.cpp.o|common/programdata.cpp]==])\n")
    result = subprocess.run(["cmake", f"-DPLATFORM_MAP={map_file}", f"-DPLATFORM_INPUTS={inputs}",
                             "-DPLATFORM_TARGET=engine-island", "-DPLATFORM_LAYERS=platform,island",
                             "-P", str(PLATFORM_MAP)], capture_output=True, text=True)
    if error:
        assert result.returncode != 0
        assert error in " ".join(result.stderr.split())
    else:
        assert result.returncode == 0, result.stderr
        assert "engine-island: 1 mixed runtime objects checked, all platform or island" in result.stdout


@pytest.mark.parametrize("target,allowed", [
    ("engine-platform-link", "platform"), ("engine-island", "platform,island"),
])
@pytest.mark.parametrize("record,error", [
    ("Loaded C:\\build with space\\gothic-base-runtime.lib(common\\sha256.cpp.obj)", ""),
    ("Loaded gothic-base-runtime.lib(programdata.cpp.obj)",
     "gothic-base-runtime(programdata.cpp.obj) from common/programdata.cpp belongs to gothic, not platform"),
    ("Loaded gothic-video-runtime.lib(unknown.cpp.obj)", "unknown mixed runtime object"),
    ("", "no mixed runtime objects found"),
], ids=["platform", "gothic-object", "unknown-object", "no-runtime-objects"])
def test_msvc_link_map(tmp_path, target, allowed, record, error):
    # /MAP symbols, references, discarded/folded COMDATs and searched libraries
    # cannot stand in for extraction records, even when they name a known member.
    trace = ("Searching C:\\build with space\\gothic-base-runtime.lib:\r\n"
             "  Found ?readIndex@ProgramData@@\r\n"
             "    Referenced in gothic-base-runtime.lib(programdata.cpp.obj)\r\n"
             " 0001:00000000 ?sha256@@ 0000000140001000 f gothic-base-runtime:sha256.cpp.obj\r\n"
             "  Discarded ?sha256@@ from gothic-base-runtime.lib(sha256.cpp.obj)\r\n"
             "  Folded ?sha256@@ from gothic-base-runtime.lib(sha256.cpp.obj)\r\n")
    # A loaded object remains subject to the guard even if it has no public symbols.
    trace += f"    {record}\r\n    {record}\r\n"
    map_file = tmp_path / "link.log"
    map_file.write_bytes(trace.encode() if record else b"")
    inputs = tmp_path / "inputs.cmake"
    inputs.write_text(f"set(PLATFORM_RULES [==[{RULES}]==])\n"
                      "set(PLATFORM_MEMBERS [==[gothic-base-runtime|sha256.cpp.obj|common/sha256.cpp;"
                      "gothic-base-runtime|programdata.cpp.obj|common/programdata.cpp]==])\n")
    result = subprocess.run(["cmake", f"-DPLATFORM_MAP={map_file}", f"-DPLATFORM_INPUTS={inputs}",
                             "-DPLATFORM_MAP_FORMAT=MSVC", f"-DPLATFORM_TARGET={target}",
                             f"-DPLATFORM_LAYERS={allowed}", "-P", str(PLATFORM_MAP)],
                            capture_output=True, text=True)
    if error:
        assert result.returncode != 0
        assert error in " ".join(result.stderr.split())
        if "unknown" in error:
            assert "unknown.cpp.obj" in result.stderr
    else:
        assert result.returncode == 0, result.stderr
        assert f"{target}: 1 mixed runtime objects checked, all {allowed.replace(',', ' or ')}" in result.stdout
        assert "mixed runtime object gothic-base-runtime(sha256.cpp.obj)" in result.stdout


@pytest.mark.parametrize("generator,exit_code", [
    ("Ninja", 0), ("Ninja", 7), ("Visual Studio 17 2022", 0),
])
@pytest.mark.skipif(shutil.which("ninja") is None, reason="no ninja")
def test_msvc_link_output_capture(tmp_path, generator, exit_code):
    directory = tmp_path / "with space"
    directory.mkdir()
    build = directory / "build"
    log = build / "engine-platform-link-Release.map.link.log"
    linker = directory / "linker.py"
    linker.write_text("import os, sys\n"
                      "from pathlib import Path\n"
                      "assert os.environ['VERBOSE'] == '1'\n"
                      "Path(sys.argv[1]).touch()\n"
                      "if '/VERBOSE' in sys.argv:\n"
                      "    print('    Loaded gothic-base-runtime.lib(sha256.cpp.obj)')\n"
                      "else:\n"
                      "    print('    Searching gothic-base-runtime.lib:')\n"
                      "print('linker diagnostic', file=sys.stderr)\n"
                      f"sys.exit({exit_code})\n")
    # Run the actual target configuration: testing capture alone missed /VERBOSE:LIB,
    # which native MSVC 14.44 prints without any Loaded records (release run 43744732687).
    platform = GUARD.with_name("EnginePlatform.cmake").read_text()
    start = platform.index("function(engine_link_map_check ")
    end = platform.index("\nendfunction()", start) + len("\nendfunction()")
    shutil.copy(GUARD, directory / GUARD.name)
    (directory / "main.obj").touch()
    (directory / "inputs.cmake").write_text(
        f"set(PLATFORM_RULES [==[{RULES}]==])\n"
        "set(PLATFORM_MEMBERS gothic-base-runtime|sha256.cpp.obj|common/sha256.cpp)\n")
    (directory / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\nproject(capture NONE)\nset(MSVC TRUE)\n"
        f'set(guard_rules "{RULES.as_posix()}")\n'
        f'set(ENGINE_LINK_MAP_CHECK "{PLATFORM_MAP.as_posix()}")\n'
        'set(ENGINE_LINK_MAP_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/inputs.cmake")\n'
        f'set(CMAKE_CXX_LINK_EXECUTABLE "\\\"{Path(sys.executable).as_posix()}\\\" '
        f'\\\"{linker.as_posix()}\\\" <TARGET> <LINK_FLAGS>")\n'
        f"{platform[start:end]}\n"
        "add_executable(engine-platform-link main.obj)\n"
        "set_target_properties(engine-platform-link PROPERTIES LINKER_LANGUAGE CXX)\n"
        # Exercise the Visual Studio branch without needing its toolchain on this host.
        f'set(CMAKE_GENERATOR "{generator}")\n'
        "engine_link_map_check(engine-platform-link platform)\n"
        "get_target_property(launcher engine-platform-link RULE_LAUNCH_LINK)\n"
        'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/launcher.txt" "${launcher}")\n')
    configured = subprocess.run(["cmake", "-S", str(directory), "-B", str(build), "-G", "Ninja",
                                 "-DCMAKE_BUILD_TYPE=Release"], capture_output=True, text=True)
    assert configured.returncode == 0, configured.stdout + configured.stderr
    launcher = (build / "launcher.txt").read_text()
    if generator != "Ninja":
        assert "mixed-runtime map check disabled for Visual Studio" in " ".join(configured.stderr.split())
        assert launcher == "launcher-NOTFOUND"
        return
    assert "$<" not in launcher
    assert log.as_posix() in launcher
    log.write_text("stale output")
    result = subprocess.run(["cmake", "--build", str(build)],
                            capture_output=True, text=True)
    assert (result.returncode == 0) == (exit_code == 0)
    assert "Loaded gothic-base-runtime.lib(sha256.cpp.obj)" in log.read_text()
    assert "linker diagnostic" in log.read_text()
    assert "stale output" not in log.read_text()
    output = result.stdout + result.stderr
    assert "linker diagnostic" in output
    if exit_code:
        assert f"MSVC link failed ({exit_code})" in output
    else:
        assert "1 mixed runtime objects checked, all platform" in output
