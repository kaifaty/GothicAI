"""Link census of the engine platform: nm parsing, the hole rule and object-to-layer mapping."""

import importlib.util
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
        assert "common_var" in s.defined and not any(n.startswith(("ltmp", "_ZL", "-[")) for n in s.defined)
    assert "_Z9hidden_fnv" in census.parse_nm(MACHO, "macho").defined


def unit(source, layer, undefined=(), strong=(), weak=(), library="OpenGothic"):
    defined = {n: False for n in strong} | {n: True for n in weak}
    return census.Unit(source + ".o", library, source, layer, census.Symbols(set(undefined), defined))


def test_hole_rule():
    units = [
        unit("common/graphics/a.cpp", "platform", undefined={"inl", "jolt", "upper", "libc", "strong"}),
        unit("common/graphics/b.cpp", "platform", weak={"inl"}),
        unit("common/world/w.cpp", "rpg", strong={"upper", "inl", "jolt", "strong"}),
        unit("lib/JoltPhysics/x.cpp", None, strong={"jolt"}, library="Jolt"),
        unit("common/ui/m.cpp", "gothic", weak={"strong"}),
    ]
    r = census.report(units, "c++filt")
    assert [h["mangled"] for h in r["holes"]] == ["strong", "upper"]
    assert r["holes"][0]["defined_in"] == [{"file": "common/world/w.cpp", "layer": "rpg", "weak": False},
                                           {"file": "common/ui/m.cpp", "layer": "gothic", "weak": True}]
    assert (r["resolved_in_platform"], r["third_party_references"], r["system_symbols"]) == (1, 1, 1)
    assert r["third_party"] == [{"library": "Jolt", "symbols": 1, "files": [{"file": "common/graphics/a.cpp", "symbols": 1}]}]
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
    units = [census.Unit("plat.o", "x", "common/plat.cpp", "platform", plat),
             census.Unit("up.o", "x", "common/world/up.cpp", "rpg", up)]
    r = census.report(units, "c++filt")
    assert [(h["symbol"], h["defined_in"][0]["layer"]) for h in r["holes"]] == [("up()", "rpg")]
