"""Link census of the engine platform: nm parsing, the hole rule and object-to-layer mapping."""

import importlib.util
import json
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gothic_engine_link_census", ROOT / "scripts/engine_link_census.py")
census = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = census
SPEC.loader.exec_module(census)

ELF = """\
                 U _Z3barv
0000000000000000 T _Z3foov
0000000000000000 W _Z3inlv
0000000000000000 V _ZZ3inlvE1x
0000000000000000 t _ZL8local_fnv
0000000000000004 C common_var
                 w weakref
0000000000000000 B _ZN1A1sE
"""

MACHO = """\
                 (undefined) external __Z3barv
0000000000000010 (__TEXT,__text) external __Z3foov
0000000000000040 (__TEXT,__text) weak private external __Z3inlv
0000000000000064 (__DATA,__data) weak external __ZZ3inlvE1x
0000000000000000 (__TEXT,__text) private external __Z9hidden_fnv
0000000000000070 (__DATA,__bss) external __ZN1A1sE
0000000000000008 (__TEXT,__text) non-external ltmp0
0000000000000108 (common) (alignment 2^2) external _common_var
                 (undefined) weak external _weakref
0000000000000020 (__TEXT,__text) non-external -[View drawRect:]
"""


def test_parse_nm_both_formats():
    for fmt, text in (("elf", ELF), ("macho", MACHO)):
        s = census.parse_nm(text, fmt)
        assert s.undefined == {"_Z3barv", "weakref"}, fmt
        assert s.defined["_Z3foov"] is False and s.defined["_Z3inlv"] is True and s.defined["_ZZ3inlvE1x"] is True
        assert "common_var" in s.defined and s.defined["_ZN1A1sE"] is False and not any(n.startswith(("ltmp", "_ZL", "-[")) for n in s.defined)
    assert "_Z9hidden_fnv" in census.parse_nm(MACHO, "macho").defined


def unit(source, layer, undefined=(), strong=(), weak=(), library="OpenGothic"):
    defined = {n: False for n in strong} | {n: True for n in weak}
    return census.Unit(f"CMakeFiles/{library}.dir/{source}.o", library, source, layer,
                       census.Symbols(set(undefined), defined))


def test_hole_rule():
    units = [
        unit("common/graphics/a.cpp", "platform", undefined={"inl", "jolt", "upper", "libc", "strong", "bfc"}),
        unit("common/graphics/b.cpp", "platform", weak={"inl"}),
        unit("common/world/w.cpp", "rpg", strong={"upper", "inl", "jolt", "strong"}),
        unit("lib/JoltPhysics/x.cpp", None, strong={"jolt"}, weak={"bfc"}, library="Jolt"),
        unit("lib/Tempest/y.cpp", None, weak={"jolt", "bfc"}, library="Tempest"),
        unit("common/ui/m.cpp", "gothic", weak={"strong"}),
    ]
    r = census.report(units, "c++filt")
    assert [(h["file"], h["mangled"]) for h in r["holes"]] == [("common/graphics/a.cpp", "strong"),
                                                                ("common/graphics/a.cpp", "upper")]
    assert r["holes"][0]["defined_in"] == [{"file": "common/world/w.cpp", "layer": "rpg", "weak": False},
                                           {"file": "common/ui/m.cpp", "layer": "gothic", "weak": True}]
    assert (r["resolved_in_platform"], r["third_party_references"], r["system_symbols"]) == (1, 2, 1)
    # A strong definition wins over weak copies; a weak-only symbol goes to every library holding a copy.
    assert [(t["library"], t["symbols"], t["weak_only_symbols"]) for t in r["third_party"]] == [("Jolt", 2, 1), ("Tempest", 1, 1)]
    assert r["hole_symbols_by_layer"] == {"rpg": 2, "gothic": 1}


def test_object_mapping(tmp_path):
    engine, build = tmp_path / "OpenGothic", tmp_path / "OpenGothic/build"
    assert census.classify(str(engine / "common/graphics/renderer.cpp"), engine, build) == ("common/graphics/renderer.cpp", "platform")
    assert census.classify(str(engine / "common/ui/inventorymenu.cpp"), engine, build) == ("common/ui/inventorymenu.cpp", "gothic")
    assert census.classify(str(engine / "lib/Tempest/Engine/x.cpp"), engine, build)[1] is None
    assert census.classify(str(build / "shader/sprv/shader.cpp"), engine, build)[1] is None
    assert census.classify(None, engine, build) == ("", None)
    rule, inputs = census.query_inputs(
        "opengothic/Gothic2Notr:\n  input: CXX_EXECUTABLE_LINKER__Gothic2Notr\n    main.cpp.o\n    | libOpenGothic.a\n"
        "    || content-convert\n  outputs:\n    all\n")
    assert (rule, inputs) == ("CXX_EXECUTABLE_LINKER__Gothic2Notr", ["main.cpp.o", "libOpenGothic.a"])
    assert census.archive_name("lib/JoltPhysics/Build/libJolt.a") == "Jolt"


def test_collect_maps_objects_to_sources(tmp_path, monkeypatch):
    engine = tmp_path / "OpenGothic"
    build = engine / "build"
    objects = {"CMakeFiles/OpenGothic.dir/common/graphics/a.cpp.o": "common/graphics/a.cpp",
               "CMakeFiles/OpenGothic.dir/common/world/w.cpp.o": "common/world/w.cpp",
               "lib/JoltPhysics/Build/CMakeFiles/Jolt.dir/Jolt/x.cpp.o": "lib/JoltPhysics/Jolt/x.cpp"}
    compdb = [{"directory": str(build), "command": "c++", "file": str(engine / src), "output": obj}
              for obj, src in objects.items()]
    queries = {
        "Gothic2Notr": "Gothic2Notr:\n  input: phony\n    opengothic/Gothic2Notr\n  outputs:\n",
        "opengothic/Gothic2Notr": "opengothic/Gothic2Notr:\n  input: LINK\n    | libOpenGothic.a\n"
                                  "    | lib/JoltPhysics/Build/libJolt.a\n    || content-convert\n",
        "libOpenGothic.a": "libOpenGothic.a:\n  input: AR\n    CMakeFiles/OpenGothic.dir/common/graphics/a.cpp.o\n"
                           "    CMakeFiles/OpenGothic.dir/common/world/w.cpp.o\n",
        "lib/JoltPhysics/Build/libJolt.a": "libJolt.a:\n  input: AR\n    lib/JoltPhysics/Build/CMakeFiles/Jolt.dir/Jolt/x.cpp.o\n",
    }

    def ninja(_build, *args):
        if args == ("-t", "compdb"):
            return json.dumps(compdb)
        if args[:2] == ("-t", "query") and args[2] in queries:
            return queries[args[2]]
        raise subprocess.CalledProcessError(1, args)

    symbols = {"a.cpp.o": census.Symbols({"upper", "jolt"}, {}),
               "w.cpp.o": census.Symbols(set(), {"upper": False}),
               "x.cpp.o": census.Symbols(set(), {"jolt": False})}
    monkeypatch.setattr(census, "ninja", ninja)
    monkeypatch.setattr(census, "run_nm", lambda nm, fmt, obj: symbols[obj.name])
    units = census.collect(build, "Gothic2Notr", engine, "nm", "elf", 2)
    assert [(u.source, u.layer, u.library) for u in units] == [
        ("common/graphics/a.cpp", "platform", "OpenGothic"), ("common/world/w.cpp", "rpg", "OpenGothic"),
        (str(engine / "lib/JoltPhysics/Jolt/x.cpp"), None, "Jolt")]
    r = census.report(units, "c++filt")
    assert [(h["file"], h["mangled"], h["defined_in"][0]["file"]) for h in r["holes"]] == [
        ("common/graphics/a.cpp", "upper", "common/world/w.cpp")]
    assert r["third_party"][0]["files"] == [{"file": "common/graphics/a.cpp", "symbols": 1}]


@pytest.mark.skipif(not (shutil.which("c++") and shutil.which("nm")), reason="needs a C++ compiler and nm")
def test_real_objects(tmp_path):
    (tmp_path / "plat.cpp").write_text("int up(); inline int inl() { static int x; return ++x; }\n"
                                       "int plat() { return up() + inl(); }\n")
    (tmp_path / "up.cpp").write_text("inline int inl() { static int x; return ++x; }\nint up() { return inl(); }\n")
    for name in ("plat", "up"):
        subprocess.run(["c++", "-O0", "-c", f"{name}.cpp", "-o", f"{name}.o"], cwd=tmp_path, check=True)
    fmt = "macho" if sys.platform == "darwin" else "elf"
    plat, up = (census.run_nm("nm", fmt, tmp_path / f"{n}.o") for n in ("plat", "up"))
    assert "_Z2upv" in plat.undefined and plat.defined["_Z3inlv"] is True and up.defined["_Z2upv"] is False
    # The static `x` of the inline function is a definition in both objects (weak, or GNU unique on Linux).
    assert "_ZZ3inlvE1x" in plat.defined and "_ZZ3inlvE1x" in up.defined
    units = [census.Unit("CMakeFiles/x.dir/common/plat.cpp.o", "x", "common/plat.cpp", "platform", plat),
             census.Unit("CMakeFiles/x.dir/common/world/up.cpp.o", "x", "common/world/up.cpp", "rpg", up)]
    r = census.report(units, "c++filt")
    assert [(h["file"], h["symbol"], h["defined_in"][0]["file"], h["defined_in"][0]["layer"]) for h in r["holes"]] == [
        ("common/plat.cpp", "up()", "common/world/up.cpp", "rpg")]
