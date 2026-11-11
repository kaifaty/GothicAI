#!/usr/bin/env python3
"""Census of the engine's layers: which #include edges cross the proposed boundary.

Every source file of OpenGothic/common, OpenGothic/game and OpenGothic/ai_bridge gets a
layer from LAYER_RULES (first matching rule wins). An include edge is a violation when the
included file's layer is not in ALLOWED[layer of the including file].

    python3 reports/engine/2026-11-10-engine-layers/census.py            # markdown
    python3 reports/engine/2026-11-10-engine-layers/census.py --json     # numbers

The layer map is a proposal (README.md next to this file), not a decision.
"""
from __future__ import annotations

import argparse
import fnmatch
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
ENGINE = ROOT / "OpenGothic"
SCAN_DIRS = ("common", "game", "ai_bridge")
EXTS = {".h", ".cpp", ".c", ".mm"}
INCLUDE_ROOTS = ("common", ".", "ai_bridge")

# Layers, bottom to top.
LAYERS = ("platform", "rpg", "gothic", "import", "bridge", "tools")
TITLES = {
    "platform": "Платформа",
    "rpg": "Каркас RPG",
    "gothic": "Готика",
    "import": "Импорт Готики",
    "bridge": "Мост ИИ",
    "tools": "Стенды и инструменты",
}
ALLOWED = {
    "platform": {"platform"},
    "rpg": {"platform", "rpg"},
    "gothic": {"platform", "rpg", "gothic"},
    "import": {"platform", "gothic", "import"},
    "bridge": {"platform", "rpg", "gothic", "bridge"},
    "tools": set(LAYERS),
}

# (glob relative to OpenGothic/, layer); first match wins. Paths without extension
# match every extension of that name.
LAYER_RULES: list[tuple[str, str]] = [
    # Import of the original install into our formats (ZenKit, DirectMusic, Bink).
    ("common/asset/*fromzenkit*", "import"),
    ("common/zenkitlog", "import"),
    ("common/baseimport", "import"),
    ("common/basestoreimport", "import"),
    ("common/basesources", "import"),
    ("common/baseidentity", "import"),
    ("common/musicnativeimport", "import"),
    ("common/videoimport", "import"),
    ("common/contentcensus", "import"),
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
    ("game/main", "gothic"),
    ("common/gamemusic", "gothic"),
    ("common/dmusic/*", "gothic"),
    ("common/programdata", "gothic"),
    ("common/outputunits", "gothic"),
    ("common/gothicglb", "gothic"),
    ("common/worldbase", "gothic"),
    # An action-RPG frame any such game needs: NPCs, items, fights, navigation, saves.
    ("common/game/*", "rpg"),
    ("common/world/*", "rpg"),
    ("common/nav*", "rpg"),
    # The platform: window-free core, resources of our formats, render, physics, sound, nets.
    ("common/asset/*", "platform"),
    ("common/graphics/*", "platform"),
    ("common/physics/*", "platform"),
    ("common/sound/*", "platform"),
    ("common/nets/*", "platform"),
    ("common/utils/*", "platform"),
    ("common/*", "platform"),
]

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]', re.M)
SCRIPT_VM = "common/daedalus/"


def rel(path: Path) -> str:
    return path.relative_to(ENGINE).as_posix()


def layer_of(path: str) -> str:
    stem = re.sub(r"\.(h|cpp|c|mm)$", "", path)
    for pattern, layer in LAYER_RULES:
        if fnmatch.fnmatch(path, pattern) or fnmatch.fnmatch(stem, pattern):
            return layer
    raise SystemExit(f"no layer rule for {path}")


def sources() -> list[Path]:
    out = []
    for d in SCAN_DIRS:
        out += [p for p in (ENGINE / d).rglob("*") if p.suffix in EXTS and p.is_file()]
    return sorted(out)


def resolve(src: Path, name: str, known: set[str]) -> str | None:
    candidates = [src.parent / name] + [ENGINE / r / name for r in INCLUDE_ROOTS]
    for c in candidates:
        try:
            r = rel(c.resolve())
        except ValueError:
            continue
        if r in known:
            return r
    return None


def census() -> dict:
    files = sources()
    known = {rel(p) for p in files}
    layer = {f: layer_of(f) for f in known}
    lines = {rel(p): p.read_text(errors="replace").count("\n") for p in files}
    edges: dict[str, set[str]] = defaultdict(set)
    zenkit: set[str] = set()
    for p in files:
        f = rel(p)
        for kind, name in INCLUDE_RE.findall(p.read_text(errors="replace")):
            if name.startswith("zenkit/") or name.startswith("phoenix/"):
                zenkit.add(f)
            if kind != '"' and not name.startswith(("game/", "world/", "graphics/", "daedalus/")):
                continue
            target = resolve(p, name, known)
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

    per_layer = {
        l: {"files": 0, "lines": 0, "vm_direct": 0, "vm_transitive": 0, "zenkit": 0} for l in LAYERS
    }
    for f in known:
        s = per_layer[layer[f]]
        s["files"] += 1
        s["lines"] += lines[f]
        if any(t.startswith(SCRIPT_VM) for t in edges.get(f, ())):
            s["vm_direct"] += 1
        if f in reach_vm:
            s["vm_transitive"] += 1
        if f in zenkit:
            s["zenkit"] += 1

    matrix: Counter = Counter()
    violations: list[tuple[str, str]] = []
    for f, targets in edges.items():
        for t in targets:
            matrix[(layer[f], layer[t])] += 1
            if layer[t] not in ALLOWED[layer[f]]:
                violations.append((f, t))

    hubs: dict[str, Counter] = defaultdict(Counter)
    offenders: dict[str, Counter] = defaultdict(Counter)
    for f, t in violations:
        hubs[layer[f]][t] += 1
        offenders[layer[f]][f] += 1

    return {
        "per_layer": per_layer,
        "matrix": {f"{a}->{b}": n for (a, b), n in sorted(matrix.items())},
        "violations": len(violations),
        "violations_by_layer": {l: sum(offenders[l].values()) for l in LAYERS},
        "violating_files_by_layer": {l: len(offenders[l]) for l in LAYERS},
        "hubs": {l: hubs[l].most_common() for l in LAYERS if hubs[l]},
        "offenders": {l: offenders[l].most_common() for l in LAYERS if offenders[l]},
        "zenkit_outside_import": sorted(f for f in zenkit if layer[f] not in ("import", "tools")),
        "layer_of": layer,
    }


def markdown(c: dict, top: int) -> str:
    out = ["# Перепись слоёв движка", "", "Создано `census.py`; правила слоёв — в нём же.", ""]
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--json", action="store_true", help="print the numbers as JSON")
    ap.add_argument("--top", type=int, default=15, help="rows per hub/offender table")
    args = ap.parse_args()
    c = census()
    if args.json:
        c = {k: v for k, v in c.items() if k != "layer_of"}
        json.dump(c, sys.stdout, ensure_ascii=False, indent=1)
        print()
    else:
        print(markdown(c, args.top))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
