"""Engine layer census and its PR ratchet on a tiny synthetic engine tree."""

import importlib.util
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gothic_engine_layers", ROOT / "scripts/engine_layers.py")
layers = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = layers
SPEC.loader.exec_module(layers)


def write(root: Path, path: str, text: str) -> None:
    target = root / "OpenGothic" / path
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(text)


def git(root: Path, *args: str) -> str:
    return subprocess.run(["git", "-C", str(root), *args], capture_output=True, text=True, check=True).stdout


@pytest.fixture
def repo(tmp_path):
    git(tmp_path, "init", "-q")
    git(tmp_path, "config", "user.email", "agent@example.invalid")
    git(tmp_path, "config", "user.name", "agent")
    write(tmp_path, "common/daedalus/vm.h", "#pragma once\n")
    write(tmp_path, "common/game/gamescript.h", '#pragma once\n#include "daedalus/vm.h"\n')
    write(tmp_path, "common/graphics/mesh.h", "#pragma once\n")
    write(tmp_path, "common/graphics/mesh.cpp", '#include "mesh.h"\n')
    write(tmp_path, "common/world/world.h", '#pragma once\n#include "graphics/mesh.h"\n#include "game/gamescript.h"\n')
    git(tmp_path, "add", "-A")
    git(tmp_path, "commit", "-qm", "base")
    return tmp_path


def test_layers_follow_rules():
    assert layers.layer_of("common/graphics/renderer.cpp") == "platform"
    assert layers.layer_of("common/world/objects/npc.h") == "rpg"
    assert layers.layer_of("common/game/gamescript.h") == "gothic"
    assert layers.layer_of("common/asset/worldfromzenkit.cpp") == "import"
    assert layers.layer_of("ai_bridge/aibridge.cpp") == "bridge"
    assert layers.layer_of("common/testbench_water.cpp") == "tools"
    assert layers.layer_of("game/newtool.cpp") == "tools"


def test_census_counts_violations_and_vm_reach(repo):
    write(repo, "common/graphics/mesh.cpp", '#include "mesh.h"\n#include "world/world.h"\n')
    c = layers.census(repo / "OpenGothic")
    assert c["violation_edges"] == [["common/graphics/mesh.cpp", "common/world/world.h"],
                                    ["common/world/world.h", "common/game/gamescript.h"]]
    assert c["per_layer"]["platform"]["vm_transitive"] == 1
    assert c["per_layer"]["platform"]["vm_direct"] == 0


def test_check_warns_only_about_added_edges(repo, monkeypatch, capsys):
    monkeypatch.chdir(repo)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    git(repo, "checkout", "-qb", "pr")
    write(repo, "common/graphics/mesh.cpp", '#include "mesh.h"\n#include "world/world.h"\n')
    git(repo, "commit", "-qam", "cross the boundary")
    base = git(repo, "rev-parse", "HEAD~1").strip()

    assert layers.check(repo, base) == 0
    out = capsys.readouterr().out
    warnings = [line for line in out.splitlines() if line.startswith("::warning")]
    assert len(warnings) == 1
    assert "file=OpenGothic/common/graphics/mesh.cpp" in warnings[0]
    assert "common/world/world.h" in warnings[0]
    assert "1 → 2 (новых 1, убранных 0)" in out
    assert "до VM скриптов: 0 → 1" in out


def test_check_is_quiet_when_nothing_is_added(repo, monkeypatch, capsys):
    monkeypatch.chdir(repo)
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    base = git(repo, "rev-parse", "HEAD").strip()
    assert layers.check(repo, base) == 0
    assert "::warning" not in capsys.readouterr().out


def test_foreign_definitions_resolve_scopes_and_visible_headers(repo):
    write(repo, "common/graphics/foo.h", """
class Foo { public: void m(); };
namespace X {
class Foo {
 public:
  Foo(); ~Foo(); void m(); int operator()(int); operator bool() const;
  struct Nested { void m(); };
};
}
""")
    write(repo, "common/game/foo.h", """
#include "graphics/foo.h"
namespace Other { class Foo { public: void m(); }; }
inline void X::Foo::Nested::m() {}
""")
    write(repo, "common/game/foo.cpp", """
#include "foo.h"
void X::Foo::m() {}
void Other::Foo::m() {}
namespace X {
Foo::Foo() {}
Foo::~Foo() {}
int Foo::operator()(int v) { return v; }
Foo::operator bool() const { return true; }
namespace Deep { void Foo::m() {} }
}
namespace Other { void Foo::m() {} }
void declaration(X::Foo&);
// void X::Foo::comment() {}
const char* text = "void X::Foo::string() {}";
const char* raw = R"code(void X::Foo::raw() {})code";
void function() { X::Foo::m(); struct Local { void m() {} }; }
struct Local { void m() { X::Foo::m(); } };
""")
    # The same short name without an include path must not bind to platform Foo.
    write(repo, "common/world/unrelated.cpp", "void Foo::m() {}\nvoid X::Foo::m() {}\n")
    c = layers.census(repo / "OpenGothic")
    definitions = c["foreign_definition_list"]
    assert [d["member"] for d in definitions] == [
        "X::Foo::m", "X::Foo::Foo", "X::Foo::~Foo", "X::Foo::operator()", "X::Foo::operator bool",
        "X::Foo::m", "X::Foo::Nested::m",
    ]
    assert all(d["header"] == "common/graphics/foo.h" for d in definitions)
    assert definitions[0]["file"] == "common/game/foo.cpp"
    assert definitions[0]["line"] == 3
    assert c["foreign_definitions"] == 7
    assert c["foreign_definitions_by_layer"] == dict.fromkeys(layers.LAYERS, 0) | {"platform": 7}
    assert "`X::Foo::Nested::m`" in layers.markdown(c, 15)


def test_check_counts_new_foreign_overloads_without_line_shift_warnings(repo, monkeypatch, capsys):
    monkeypatch.delenv("GITHUB_STEP_SUMMARY", raising=False)
    write(repo, "common/graphics/foo.h", "class Foo { public: void m(); void m(int); void n(); };\n")
    original = '#include "graphics/foo.h"\nvoid Foo::m() {}\n'
    write(repo, "common/game/foo.cpp", original)
    git(repo, "add", "-A")
    git(repo, "commit", "-qm", "existing foreign definition")
    base = git(repo, "rev-parse", "HEAD").strip()
    write(repo, "common/game/foo.cpp", "\n\n" + original)
    git(repo, "commit", "-qam", "shift lines")
    assert layers.check(repo, base) == 0
    assert "::warning" not in capsys.readouterr().out

    write(repo, "common/game/foo.cpp", "\n\n" + original + "void Foo::m(int) {}\nvoid Foo::n() {}\n")
    git(repo, "commit", "-qam", "add foreign members")
    assert layers.check(repo, base) == 0
    warnings = [line for line in capsys.readouterr().out.splitlines() if line.startswith("::warning")]
    assert len(warnings) == 2
    assert all("file=OpenGothic/common/game/foo.cpp" in warning for warning in warnings)
    assert all("common/graphics/foo.h" in warning for warning in warnings)
    assert sum("Foo::m" in warning for warning in warnings) == 1
    assert sum("Foo::n" in warning for warning in warnings) == 1
