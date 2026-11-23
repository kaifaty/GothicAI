#!/usr/bin/env python3
"""What the RPG layer takes from Gothic: the numbers of the step-7 note (README.md next to this file).

Run from the repository root: python3 reports/engine/2026-11-14-rpg-records/census.py
Layers and the include graph come from scripts/engine_layers.py; only the standard library is used.
"""
import collections
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "scripts"))
import engine_layers as el  # noqa: E402

ENGINE = ROOT / "OpenGothic"
UPPER = ("gothic", "tools", "import", "bridge")


def rel(path: Path) -> str:
    return path.relative_to(ENGINE).as_posix()


FILES = el.sources(ENGINE)
KNOWN = {rel(p) for p in FILES}
LAYER = {f: el.layer_of(f) for f in KNOWN}
RAW = {rel(p): p.read_text(errors="replace") for p in FILES}
CODE = {f: el.HIDDEN_RE.sub("", t) for f, t in RAW.items()}  # comments and raw strings removed
RPG = sorted(f for f in KNOWN if LAYER[f] == "rpg")


def include_edges() -> dict[str, set[str]]:
    def resolve(src: str, name: str) -> str | None:
        for c in ((ENGINE / src).parent / name, *(ENGINE / r / name for r in el.INCLUDE_ROOTS)):
            try:
                r = rel(c.resolve())
            except ValueError:
                continue
            if r in KNOWN:
                return r
        return None

    edges: dict[str, set[str]] = collections.defaultdict(set)
    for f, t in RAW.items():
        for kind, name in el.INCLUDE_RE.findall(t):
            if kind != '"' and not name.startswith(el.OWN_ANGLE):
                continue
            target = resolve(f, name)
            if target and target != f:
                edges[f].add(target)
    return edges


def reach(edges: dict[str, set[str]], pred) -> set[str]:
    included_by: dict[str, set[str]] = collections.defaultdict(set)
    for f, targets in edges.items():
        for t in targets:
            included_by[t].add(f)
    seen = {f for f in KNOWN if pred(f)}
    queue = list(seen)
    while queue:
        for f in included_by.get(queue.pop(), ()):
            if f not in seen:
                seen.add(f)
                queue.append(f)
    return seen


def greedy_cuts() -> None:
    """Which Gothic and bench headers to cut from the RPG layer first: the biggest drop each step."""
    edges = include_edges()

    def score(e):
        vm = reach(e, lambda f: f.startswith(el.SCRIPT_VM))
        up = reach(e, lambda f: LAYER[f] in UPPER)
        return sum(f in vm for f in RPG), sum(f in up for f in RPG)

    cur = {f: set(t) for f, t in edges.items()}
    print(f"\n## Include cuts, greedy ({len(RPG)} RPG files)\n")
    print("| Cut from the RPG layer | RPG files that include it | reach the VM | reach an upper layer |")
    print("|---|---:|---:|---:|")
    print("| nothing | | %d | %d |" % score(cur))
    targets = sorted({t for f in RPG for t in cur.get(f, ()) if LAYER[t] in UPPER})
    while targets:
        best = None
        for t in targets:
            e = {f: (ts - {t} if LAYER[f] == "rpg" else ts) for f, ts in cur.items()}
            s = score(e)
            if best is None or s < best[0]:
                best = (s, t, e)
        s, t, cur = best
        targets.remove(t)
        n = sum(1 for f in RPG if t in edges.get(f, ()))
        print(f"| `{t}` | {n} | {s[0]} | {s[1]} |")


def vm_types() -> None:
    by_type: collections.Counter = collections.Counter()
    files: dict[str, set[str]] = collections.defaultdict(set)
    for f in RPG:
        for m in re.finditer(r"daedalus::\s*(\w+)", CODE[f]):
            by_type[m.group(1)] += 1
            files[m.group(1)].add(f)
    print(f"\n## VM types named in the RPG layer: {len(set().union(*files.values()))} files, {sum(by_type.values())} mentions\n")
    for k, v in by_type.most_common():
        print(f"- `daedalus::{k}`: {v} in {len(files[k])} files")


def field_access() -> None:
    """Direct reads and writes of script-instance fields through hnpc->, hitem->, handle()., handlePtr()->."""
    rx = re.compile(r"(?:\bhnpc\s*->|\bhitem\s*->|\bhandle\(\)\s*\.|\bhandlePtr\(\)\s*->)\s*(\w+)"
                    r"(\s*(?:\[[^\]]*\])?\s*(?:=(?!=)|\+=|-=|\|=|&=|\+\+|--))?")
    print("\n## Direct access to script-instance fields\n")
    print("| Layer | files | reads | writes |")
    print("|---|---:|---:|---:|")
    fields: dict[str, list[int]] = collections.defaultdict(lambda: [0, 0])
    for layer in el.LAYERS:
        r = w = 0
        touched = set()
        for f in KNOWN:
            if LAYER[f] != layer:
                continue
            for m in rx.finditer(CODE[f]):
                touched.add(f)
                r += not m.group(2)
                w += bool(m.group(2))
                if layer == "rpg":
                    fields[m.group(1)][1 if m.group(2) else 0] += 1
        if touched:
            print(f"| `{layer}` | {len(touched)} | {r} | {w} |")
    top = sorted(fields.items(), key=lambda x: -sum(x[1]))
    print("\nRPG layer, field reads/writes: " + ", ".join(f"`{f}` {a}/{b}" for f, (a, b) in top))


def calls() -> None:
    rx = {
        "GameScript through script()": re.compile(r"\bscript\(\)\s*(?:\.|->)\s*(\w+)"),
        "Gothic:: singleton": re.compile(r"\bGothic::\s*(?:inst\(\)\s*\.\s*)?(\w+)"),
    }
    print("\n## Calls from the RPG layer\n")
    for title, x in rx.items():
        c: collections.Counter = collections.Counter()
        touched = set()
        for f in RPG:
            for m in x.finditer(CODE[f]):
                c[m.group(1)] += 1
                touched.add(f)
        print(f"- **{title}**: {sum(c.values())} calls, {len(c)} methods, {len(touched)} files — "
              + ", ".join(f"`{a}` {b}" for a, b in c.most_common(12)))


def constants() -> None:
    text = CODE["common/game/constants.h"]
    owner = {}
    for m in re.finditer(r"enum\s+(?:class\s+)?(\w+)?[^{;]*\{([^}]*)\}", text):
        for e in re.findall(r"^\s*(\w+)\s*(?:=|,|$)", m.group(2), re.M):
            owner[e] = m.group(1) or "(unnamed)"
    use: collections.Counter = collections.Counter()
    files: dict[str, set[str]] = collections.defaultdict(set)
    for f in RPG:
        for w in re.findall(r"\b[A-Za-z_]\w+\b", CODE[f]):
            if w in owner and len(w) > 3:
                use[owner[w]] += 1
                files[owner[w]].add(f)
    print("\n## Enumerators of game/constants.h used in the RPG layer\n")
    for k, v in use.most_common():
        print(f"- `{k}`: {v} uses in {len(files[k])} files")


if __name__ == "__main__":
    print(f"# RPG layer and Gothic: census ({len(RPG)} files, {sum(RAW[f].count(chr(10)) for f in RPG)} lines)")
    greedy_cuts()
    vm_types()
    field_access()
    calls()
    constants()
