#!/usr/bin/env python3
"""Engine layers: includes and member definitions across the engine/Gothic boundary.

Every source file of OpenGothic/common, game, ai_bridge and island gets a
layer from LAYER_RULES (first matching rule wins). An include edge is a violation when the
included file's layer is not in ALLOWED[layer of the including file].

    python3 scripts/engine_layers.py                       # markdown report of the working tree
    python3 scripts/engine_layers.py report --json         # the same numbers as JSON
    python3 scripts/engine_layers.py check --base origin/main

`check` compares the committed HEAD with its merge-base and warns about every forbidden
include or foreign member definition that HEAD adds. It only warns and always exits 0. The layer map is a proposal:
reports/engine/2026-11-10-engine-layers/README.md.
"""
from __future__ import annotations

import argparse
import fnmatch
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import tempfile
from collections import Counter, defaultdict
from pathlib import Path

SCAN_DIRS = ("common", "game", "ai_bridge", "island")
EXTS = {".h", ".cpp", ".c", ".mm"}
INCLUDE_ROOTS = ("common", ".", "ai_bridge")

# Layers, bottom to top.
LAYERS = ("platform", "rpg", "gothic", "import", "bridge", "island", "tools")
TITLES = {
    "platform": "Платформа",
    "rpg": "Каркас RPG",
    "gothic": "Готика",
    "import": "Импорт Готики",
    "bridge": "Мост ИИ",
    "island": "Остров",
    "tools": "Стенды и инструменты",
}
ALLOWED = {
    "platform": {"platform"},
    "rpg": {"platform", "rpg"},
    "gothic": {"platform", "rpg", "gothic"},
    "import": {"platform", "gothic", "import"},
    "bridge": {"platform", "rpg", "gothic", "bridge"},
    "island": {"platform", "island"},
    "tools": set(LAYERS),
}

# (glob relative to OpenGothic/, layer); first match wins. Paths without extension
# match every extension of that name.
# OpenGothic/cmake/EngineLayerGuard.cmake reads this list line by line: keep one
# ("glob", "layer") tuple per line, globs of letters, digits, `_`, `-`, `.`, `/` and `*`.
LAYER_RULES: list[tuple[str, str]] = [
    # Import of the original install into our formats (ZenKit, DirectMusic, Bink).
    ("common/asset/*fromzenkit*", "import"),
    ("common/zenkitlog", "import"),
    ("common/baseimport", "import"),
    ("common/basestoreimport", "import"),
    ("common/basesources", "import"),
    ("common/musicnativeimport", "import"),
    ("common/videoimport", "import"),
    ("common/dmusic/directmusicimport", "import"),
    ("common/bink/*", "import"),
    ("ai_bridge/storygraph/*", "tools"),
    # Benches, training tools and side executables.
    ("common/testbench*", "tools"),
    ("common/navbench", "tools"),
    ("common/navdemo*", "tools"),
    ("common/navkeyteacher", "tools"),
    ("common/navmemoryteacher", "tools"),
    ("common/navlive*", "tools"),
    ("common/navmemorycosttrace", "tools"),
    ("common/navreward", "tools"),
    ("common/navsimpackage", "tools"),
    ("common/npcdayjournal", "tools"),
    ("common/game/*bench", "tools"),
    ("common/game/bodyarena*", "tools"),
    ("common/game/bodyreplay", "tools"),
    ("common/game/bodytrajectory", "tools"),
    ("game/bodyrunner", "tools"),
    ("game/society*", "tools"),
    # The AI bridge and the society core it feeds.
    ("ai_bridge/*", "bridge"),
    ("common/society/*", "bridge"),
    # Gothic itself: the script VM and its bindings, G2 rules, Gothic's app shell and menus,
    # Gothic's media formats at runtime.
    ("common/daedalus/*", "gothic"),
    ("common/game/gamescript*", "gothic"),
    ("common/game/definitions/*", "gothic"),
    # Pure mesh packing over our asset types, independent of Gothic.
    ("common/game/compatibility/phoenix", "platform"),
    ("common/game/compatibility/*", "gothic"),
    ("common/game/constants", "gothic"),
    ("common/game/ownsymbols", "gothic"),
    ("common/game/fightalgo", "gothic"),
    ("common/game/damagecalculator", "gothic"),
    ("common/game/aistate", "gothic"),
    ("common/game/aiouputpipe", "gothic"),
    ("common/game/questlog", "gothic"),
    ("common/game/globaleffects", "gothic"),
    ("common/game/savegameheader", "gothic"),
    ("common/game/chronicle", "gothic"),
    ("common/world/triggers/*", "gothic"),
    ("common/ui/*", "gothic"),
    ("common/gothic", "gothic"),
    ("common/marvin", "gothic"),
    ("common/mainwindow", "gothic"),
    # The entry point assembles all layers, including the bench and AI bridge.
    ("game/main", "tools"),
    ("common/gamemusic", "gothic"),
    ("common/dmusic/*", "gothic"),
    ("common/programdata", "gothic"),
    ("common/outputunits", "gothic"),
    # Our own GLB format reader and writer, independent of Gothic rules.
    ("common/gothicglb", "platform"),
    # Our world format and object layers belong to the RPG framework.
    ("common/worldbase", "rpg"),
    # An action-RPG frame any such game needs: NPCs, items, fights, navigation, saves.
    # Game-object visuals depend on NPCs, interactives and damage despite living in graphics/.
    ("common/graphics/mdlvisual*", "rpg"),
    ("common/graphics/objvisual*", "rpg"),
    ("common/graphics/effect", "rpg"),
    # Pure camera math, independent of game objects despite living in game/.
    ("common/game/aimcamera", "platform"),
    # Pure impact math and transform validation, independent of game objects.
    ("common/game/meleeimpact", "platform"),
    ("common/game/physicssave", "platform"),
    ("common/game/*", "rpg"),
    # Pure chunk residency and lookahead logic, independent of game objects.
    ("common/world/chunkresidency", "platform"),
    ("common/world/*", "rpg"),
    ("common/nav*", "rpg"),
    # The platform: resources of our formats, render, animation, physics, sound, nets.
    # Installation identity and ready-base validation use file I/O only.
    ("common/baseidentity", "platform"),
    ("common/asset/*", "platform"),
    ("common/graphics/*", "platform"),
    ("common/physics/*", "platform"),
    ("common/sound/*", "platform"),
    ("common/nets/*", "platform"),
    ("common/utils/*", "platform"),
    ("common/*", "platform"),
    # A new side executable is a tool until it gets its own rule.
    ("game/*", "tools"),
    # The empty island (reports/engine/2026-11-13-empty-island/README.md): a program of the platform alone.
    ("island/*", "island"),
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
SCRIPT_VM = "common/daedalus/"
OWN_ANGLE = ("game/", "world/", "graphics/", "daedalus/")

# Keep line numbers while hiding tokens that cannot introduce C++ scopes.
HIDDEN_RE = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"'
                       r'|"(?:\\[\s\S]|[^"\\])*"|(?<!\w)\'(?:\\[\s\S]|[^\'\\])*\''
                       r'|^[ \t]*\#(?:\\\n|[^\n])*', re.M)
NAME = r"[A-Za-z_]\w*"
QUALIFIED = rf"(?:::)?{NAME}(?:\s*::\s*{NAME})*"
NAMESPACE_RE = re.compile(rf"^\s*(?:inline\s+)?namespace\s*({QUALIFIED})?\s*$")
CLASS_RE = re.compile(rf"(?:^|:)\s*(?:class|struct|union)\s+({QUALIFIED})\b[^{{}};()]*$")
MEMBER_RE = re.compile(rf"(?P<owner>{QUALIFIED})\s*::\s*"
                       rf"(?P<member>operator\s*(?:\(\)|\[\]|[^\w\s(]+|(?:{QUALIFIED}[\s*&]*)+)"
                       rf"|~?{NAME})\s*\(")


def cpp_declarations(text: str) -> tuple[list[str], list[tuple[tuple[str, ...], str, str, int]]]:
    """Class bodies and out-of-class member bodies; no preprocessing or template parsing."""
    code = HIDDEN_RE.sub(lambda m: " " + "\n" * m.group().count("\n"), text)
    classes = []
    definitions = []
    scope: tuple[str, ...] = ()
    stack: list[tuple[str, tuple[str, ...]]] = []
    skipped = 0
    start = 0
    for token in re.finditer(r"[{};]", code):
        mark = token.group()
        if skipped:
            skipped += (mark == "{") - (mark == "}")
            start = token.end()
            continue
        if mark == "{":
            prefix = code[start:token.start()]
            namespace = NAMESPACE_RE.fullmatch(prefix)
            cls = CLASS_RE.search(prefix)
            if namespace or cls:
                stack.append(("namespace" if namespace else "class", scope))
                name = (namespace or cls).group(1) or "(anonymous)"
                parts = tuple(re.sub(r"\s", "", name).split("::"))
                scope = parts[1:] if parts[0] == "" else scope + parts
                if cls:
                    classes.append("::".join(scope))
            else:
                if not any(kind == "class" for kind, _ in stack):
                    member = MEMBER_RE.search(prefix) if "::" in prefix else None
                    # Calls in namespace-scope initializers are not definitions.
                    if member and not re.search(r"[=(]", prefix[:member.start()]):
                        owner = re.sub(r"\s", "", member.group("owner"))
                        name = re.sub(r"\s+", " ", member.group("member").strip())
                        line = code.count("\n", 0, start + member.start()) + 1
                        definitions.append((scope, owner, name, line))
                skipped = 1
        elif mark == "}" and stack:
            _, scope = stack.pop()
        start = token.end()
    return classes, definitions


def foreign_definitions(text: dict[str, str], edges: dict[str, set[str]], layer: dict[str, str]) -> list[dict]:
    classes: dict[str, set[str]] = defaultdict(set)
    definitions = {}
    for file, source in text.items():
        declared, definitions[file] = cpp_declarations(source)
        # Forward declarations do not establish ownership of a class.
        for name in declared:
            classes[name].add(file)

    result = []
    for file, members in definitions.items():
        if not members:
            continue
        visible = {file}
        queue = [file]
        while queue:
            for target in edges.get(queue.pop(), ()):
                if target not in visible:
                    visible.add(target)
                    queue.append(target)
        for scope, owner, member, line in members:
            parts = tuple(owner.lstrip(":").split("::"))
            prefixes = [()] if owner.startswith("::") else [scope[:n] for n in range(len(scope), -1, -1)]
            for prefix in prefixes:
                full = "::".join(prefix + parts)
                headers = classes.get(full, set()) & visible
                if not headers:
                    continue
                for header in sorted(headers):
                    if layer[file] != layer[header] and layer[file] not in ALLOWED[layer[header]]:
                        result.append({"file": file, "line": line, "member": full + "::" + member, "header": header})
                break
    return sorted(result, key=lambda d: (d["file"], d["line"], d["member"], d["header"]))


def repo_root() -> Path:
    try:
        out = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True, check=True)
        return Path(out.stdout.strip())
    except (OSError, subprocess.CalledProcessError):
        return Path(__file__).resolve().parents[1]


def layer_of(path: str) -> str:
    stem = re.sub(r"\.(h|cpp|c|mm)$", "", path)
    for pattern, layer in LAYER_RULES:
        if fnmatch.fnmatch(path, pattern) or fnmatch.fnmatch(stem, pattern):
            return layer
    raise SystemExit(f"no layer rule for {path}")


def sources(engine: Path) -> list[Path]:
    out = []
    for d in SCAN_DIRS:
        if (engine / d).is_dir():
            out += [p for p in (engine / d).rglob("*") if p.suffix in EXTS and p.is_file()]
    return sorted(out)


def census(engine: Path) -> dict:
    """Layers, include edges and violations of the engine tree at `engine` (an OpenGothic dir)."""
    engine = engine.resolve()

    def rel(path: Path) -> str:
        return path.relative_to(engine).as_posix()

    files = sources(engine)
    known = {rel(p) for p in files}
    layer = {f: layer_of(f) for f in known}
    text = {rel(p): p.read_text(errors="replace") for p in files}
    lines = {f: t.count("\n") for f, t in text.items()}

    def resolve(src: str, name: str) -> str | None:
        candidates = ((engine / src).parent / name, *(engine / r / name for r in INCLUDE_ROOTS))
        for c in candidates:
            try:
                r = rel(c.resolve())
            except ValueError:
                continue
            if r in known:
                return r
        return None

    edges: dict[str, set[str]] = defaultdict(set)
    zenkit: set[str] = set()
    for f, t in text.items():
        for kind, name in INCLUDE_RE.findall(t):
            if name.startswith(("zenkit/", "phoenix/")):
                zenkit.add(f)
            if kind != '"' and not name.startswith(OWN_ANGLE):
                continue
            target = resolve(f, name)
            if target and target != f:
                edges[f].add(target)

    # Transitive reach of the script VM: walk the include graph backwards from its files.
    included_by: dict[str, set[str]] = defaultdict(set)
    for f, targets in edges.items():
        for t in targets:
            included_by[t].add(f)
    reach_vm = {f for f in known if f.startswith(SCRIPT_VM)}
    queue = list(reach_vm)
    while queue:
        for f in included_by.get(queue.pop(), ()):
            if f not in reach_vm:
                reach_vm.add(f)
                queue.append(f)

    per_layer = {l: {"files": 0, "lines": 0, "vm_direct": 0, "vm_transitive": 0, "zenkit": 0} for l in LAYERS}
    for f in known:
        s = per_layer[layer[f]]
        s["files"] += 1
        s["lines"] += lines[f]
        s["vm_direct"] += any(t.startswith(SCRIPT_VM) for t in edges.get(f, ()))
        s["vm_transitive"] += f in reach_vm
        s["zenkit"] += f in zenkit

    matrix: Counter = Counter()
    violations: list[tuple[str, str]] = []
    for f, targets in edges.items():
        for t in targets:
            matrix[(layer[f], layer[t])] += 1
            if layer[t] not in ALLOWED[layer[f]]:
                violations.append((f, t))
    violations.sort()
    foreign = foreign_definitions(text, edges, layer)

    hubs: dict[str, Counter] = defaultdict(Counter)
    offenders: dict[str, Counter] = defaultdict(Counter)
    for f, t in violations:
        hubs[layer[f]][t] += 1
        offenders[layer[f]][f] += 1

    return {
        "per_layer": per_layer,
        "matrix": {f"{a}->{b}": n for (a, b), n in sorted(matrix.items())},
        "violations": len(violations),
        "foreign_definitions": len(foreign),
        "foreign_definitions_by_layer": {l: sum(layer[d["header"]] == l for d in foreign) for l in LAYERS},
        "foreign_definition_list": foreign,
        "violations_by_layer": {l: sum(offenders[l].values()) for l in LAYERS},
        "violating_files_by_layer": {l: len(offenders[l]) for l in LAYERS},
        "hubs": {l: hubs[l].most_common() for l in LAYERS if hubs[l]},
        "offenders": {l: offenders[l].most_common() for l in LAYERS if offenders[l]},
        "zenkit_outside_import": sorted(f for f in zenkit if layer[f] not in ("import", "tools")),
        "violation_edges": [list(v) for v in violations],
        "layer_of": layer,
    }


def markdown(c: dict, top: int) -> str:
    out = ["# Перепись слоёв движка", "", "Создано `scripts/engine_layers.py`; правила слоёв — в нём же.", ""]
    out += ["## Слои", "", "| Слой | Файлов | Строк | Включают VM скриптов прямо | Достают до VM через включения | ZenKit |",
            "|---|---:|---:|---:|---:|---:|"]
    for l in LAYERS:
        s = c["per_layer"][l]
        out.append(f"| {TITLES[l]} (`{l}`) | {s['files']} | {s['lines']} | {s['vm_direct']} | {s['vm_transitive']} | {s['zenkit']} |")
    out += ["", "## Связи между слоями", "", "Строка — кто включает, столбец — кого. Жирное — запрещено правилом.", "",
            "| | " + " | ".join(f"`{l}`" for l in LAYERS) + " |", "|---|" + "---:|" * len(LAYERS)]
    for a in LAYERS:
        row = []
        for b in LAYERS:
            n = c["matrix"].get(f"{a}->{b}", 0)
            row.append(f"**{n}**" if n and b not in ALLOWED[a] else str(n))
        out.append(f"| `{a}` | " + " | ".join(row) + " |")
    out += ["", f"Всего нарушений: **{c['violations']}**.", ""]
    out += ["## Чужие определения членов классов", "", f"Всего: **{c['foreign_definitions']}**.", "",
            "| Файл | Строка | Член | Заголовок класса |", "|---|---:|---|---|"]
    for d in c["foreign_definition_list"]:
        out.append(f"| `{d['file']}` | {d['line']} | `{d['member']}` | `{d['header']}` |")
    out.append("")
    for l in LAYERS:
        if l not in c["hubs"]:
            continue
        lay = c["layer_of"]
        out += [f"## {TITLES[l]}: куда тянутся нарушения", "",
                f"{c['violations_by_layer'][l]} включений из {c['violating_files_by_layer'][l]} файлов.", "",
                "| Включаемый файл | Его слой | Сколько файлов слоя его включают |", "|---|---|---:|"]
        for t, n in c["hubs"][l][:top]:
            out.append(f"| `{t}` | `{lay[t]}` | {n} |")
        out += ["", "| Файл слоя с нарушениями | Запрещённых включений |", "|---|---:|"]
        for f, n in c["offenders"][l][:top]:
            out.append(f"| `{f}` | {n} |")
        out.append("")
    if c["zenkit_outside_import"]:
        out += ["## ZenKit вне импорта", ""] + [f"- `{f}`" for f in c["zenkit_outside_import"]] + [""]
    return "\n".join(out)


def tree_at(root: Path, revision: str, dest: Path) -> Path:
    """Extract the scanned engine dirs of `revision` into `dest`; return its OpenGothic dir."""
    paths = [f"OpenGothic/{d}" for d in SCAN_DIRS]
    present = subprocess.run(["git", "-C", str(root), "ls-tree", "--name-only", revision, *paths],
                             capture_output=True, text=True, check=True).stdout.split()
    if present:
        blob = subprocess.run(["git", "-C", str(root), "archive", "--format=tar", revision, *present],
                              capture_output=True, check=True).stdout
        with tarfile.open(fileobj=io.BytesIO(blob)) as tar:
            tar.extractall(dest, filter="data")
    (dest / "OpenGothic").mkdir(exist_ok=True)
    return dest / "OpenGothic"


def escape(text: str, *, prop: bool = False) -> str:
    text = text.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    return text.replace(":", "%3A").replace(",", "%2C") if prop else text


def check(root: Path, base: str) -> int:
    merge_base = subprocess.run(["git", "-C", str(root), "merge-base", base, "HEAD"],
                                capture_output=True, text=True, check=True).stdout.strip()
    with tempfile.TemporaryDirectory() as tmp:
        before = census(tree_at(root, merge_base, Path(tmp) / "base"))
        after = census(tree_at(root, "HEAD", Path(tmp) / "head"))
    old = {tuple(e) for e in before["violation_edges"]}
    new = {tuple(e) for e in after["violation_edges"]}
    added, removed = sorted(new - old), sorted(old - new)
    lay = after["layer_of"]
    title = "Слои движка: новое включение через границу"
    for f, t in added:
        message = (f"{TITLES[lay[f]]} (`{lay[f]}`) включает `{t}` из слоя {TITLES[lay[t]]} (`{lay[t]}`). "
                   "Правила слоёв — scripts/engine_layers.py, замысел — reports/engine/2026-11-10-engine-layers. "
                   "Пока это только предупреждение.")
        print(f"::warning file={escape('OpenGothic/' + f, prop=True)},title={escape(title, prop=True)}::{escape(message)}")
    # Ignore line shifts, but count overloads of the same member in each file.
    old_members = Counter((d["file"], d["member"]) for d in before["foreign_definition_list"])
    new_members = Counter((d["file"], d["member"]) for d in after["foreign_definition_list"])
    added_members = new_members - old_members
    pending = added_members.copy()
    for d in after["foreign_definition_list"]:
        key = (d["file"], d["member"])
        if not pending[key]:
            continue
        pending[key] -= 1
        title = "Слои движка: новое чужое определение"
        message = (f"`{d['member']}` объявлен в `{d['header']}` (`{lay[d['header']]}`), "
                   f"но определён в слое `{lay[d['file']]}`. "
                   "Правила слоёв — scripts/engine_layers.py, замысел — reports/engine/2026-11-10-engine-layers. "
                   "Пока это только предупреждение.")
        print(f"::warning file={escape('OpenGothic/' + d['file'], prop=True)},line={d['line']},"
              f"title={escape(title, prop=True)}::{escape(message)}")
    totals = f"{before['violations']} → {after['violations']}"
    vm_before = before["per_layer"]["platform"]["vm_transitive"]
    vm_after = after["per_layer"]["platform"]["vm_transitive"]
    lines = ["### Слои движка", "",
             f"Включений против слоёв: {totals} (новых {len(added)}, убранных {len(removed)}). "
             f"Файлов платформы, достающих до VM скриптов: {vm_before} → {vm_after}.", "",
             f"Чужих определений: {before['foreign_definitions']} → {after['foreign_definitions']} "
             f"(новых {sum(added_members.values())}, убранных {sum((old_members - new_members).values())}).", ""]
    if added:
        lines += ["| Файл | Включает | Слои |", "|---|---|---|"]
        lines += [f"| `{f}` | `{t}` | `{lay[f]}` → `{lay[t]}` |" for f, t in added]
        lines += ["", "Пока это только предупреждение."]
    summary = "\n".join(lines)
    print(summary)
    if path := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(path, "a", encoding="utf-8") as stream:
            stream.write(summary + "\n")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="command")
    rep = sub.add_parser("report", help="markdown or JSON census of the working tree (default)")
    rep.add_argument("--json", action="store_true", help="print the numbers as JSON")
    rep.add_argument("--top", type=int, default=15, help="rows per hub/offender table")
    chk = sub.add_parser("check", help="warn about forbidden includes and foreign definitions HEAD adds over its merge-base")
    chk.add_argument("--base", required=True, help="compare with the merge-base of this revision and HEAD")
    args = ap.parse_args(argv)
    root = repo_root()
    if args.command == "check":
        return check(root, args.base)
    c = census(root / "OpenGothic")
    if getattr(args, "json", False):
        json.dump({k: v for k, v in c.items() if k not in ("layer_of", "violation_edges")},
                  sys.stdout, ensure_ascii=False, indent=1)
        print()
    else:
        print(markdown(c, getattr(args, "top", 15)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
