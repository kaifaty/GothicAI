#!/usr/bin/env python3
"""What Gothic's script program holds about NPCs: the numbers of the note next to this file.

Reads the main Daedalus program from a ready base (our own container of symbols and bytecode,
docs/asset-migration.md, П12). Only the standard library is used.

    python3 reports/engine/2026-11-14-npc-data/census.py [<base dir>]

Without an argument it takes OPENGOTHIC_BASE or the newest base in ~/.cache/gothic-ai/base.
"""
import collections
import json
import os
import statistics
import struct
import sys
from pathlib import Path

WIDE = {61: 4, 62: 4, 64: 4, 65: 4, 67: 4, 75: 4, 76: 4, 80: 4, 245: 5}  # opcode -> operand bytes
BL, BE, PUSHI, PUSHV, PUSHVI, B, BZ, RSR = 61, 62, 64, 65, 67, 75, 76, 60
ASSIGN = {9, 19, 20, 21, 22, 70, 71, 72, 73, 74}
T_CLASS, T_FUNC, T_PROTO, T_INST = 4, 5, 6, 7
EXTERNAL = 1 << 3
CONST = 1 << 0


def base_dir() -> Path:
    if len(sys.argv) > 1:
        return Path(sys.argv[1])
    if os.environ.get("OPENGOTHIC_BASE"):
        return Path(os.environ["OPENGOTHIC_BASE"])
    cache = Path.home() / ".cache/gothic-ai/base"
    bases = [p for p in cache.iterdir() if (p / "programs").is_dir()]
    return max(bases, key=lambda p: p.stat().st_mtime)


def read_program(path: Path):
    meta = json.loads(path.with_suffix(".json").read_text())["data"]
    raw = path.read_bytes()
    at = 0

    def u32():
        nonlocal at
        v = struct.unpack_from("<I", raw, at)[0]
        at += 4
        return v

    def text():
        nonlocal at
        n = u32()
        s = raw[at:at + n].decode("latin-1")
        at += n
        return s

    symbols = []
    for _ in range(meta["symbols"]["count"]):
        name = text()
        at += 1  # generated flag
        f = [u32() for _ in range(14)]
        ints = [u32() for _ in range(u32())]
        floats = u32()
        at += 4 * floats
        strings = [text() for _ in range(u32())]
        signed = lambda v: v - (1 << 32) if v >= 1 << 31 else v
        symbols.append({"name": name.upper(), "type": f[0], "flags": f[2],
                        "address": signed(f[3]), "parent": signed(f[4]), "ints": ints, "strings": strings})
    code = raw[meta["bytecode"]["offset"]:meta["bytecode"]["offset"] + meta["bytecode"]["length"]]
    return symbols, code


def main() -> None:
    programs = base_dir() / "programs"
    path = max(programs.glob("*.bin"), key=lambda p: p.stat().st_size)
    symbols, code = read_program(path)
    print(f"# Main script program: {len(symbols)} symbols, {len(code)} bytes of bytecode\n")

    def root(i: int) -> str:
        seen = 0
        while 0 <= i < len(symbols) and symbols[i]["type"] != T_CLASS and seen < 8:
            i = symbols[i]["parent"]
            seen += 1
        return symbols[i]["name"] if 0 <= i < len(symbols) else "?"

    coded = [(s["address"], n) for n, s in enumerate(symbols)
             if s["type"] in (T_FUNC, T_PROTO, T_INST) and not s["flags"] & EXTERNAL and 0 <= s["address"] < len(code)
             # an instance variable (`var C_NPC x`) has the instance type but no constructor: it is not CONST
             and (s["type"] != T_INST or (s["flags"] & CONST and s["address"] > 0 and "." not in s["name"]))]
    coded.sort()
    at_address = {a: n for a, n in coded}
    end = {n: (coded[k + 1][0] if k + 1 < len(coded) else len(code)) for k, (a, n) in enumerate(coded)}

    def ops(n: int):
        at = symbols[n]["address"]
        while at < end[n]:
            op = code[at]
            width = WIDE.get(op, 0)
            arg = struct.unpack_from("<i", code, at + 1)[0] if width else None
            yield op, arg
            at += 1 + width

    def profile(n: int):
        calls, externals, branches, assigns = [], [], 0, 0
        for op, arg in ops(n):
            if op == BL and arg in at_address:
                calls.append(symbols[at_address[arg]]["name"])
            elif op == BE and 0 <= arg < len(symbols):
                externals.append(symbols[arg]["name"])
            elif op in (B, BZ):
                branches += 1
            elif op in ASSIGN:
                assigns += 1
        return calls, externals, branches, assigns

    size = lambda n: end[n] - symbols[n]["address"]
    functions = [n for a, n in coded if symbols[n]["type"] == T_FUNC]
    instances = collections.defaultdict(list)
    for a, n in coded:
        if symbols[n]["type"] == T_INST:
            instances[root(symbols[n]["parent"])].append(n)

    print("## Instances by class\n")
    for cls, items in sorted(instances.items(), key=lambda x: -len(x[1])):
        print(f"- `{cls}`: {len(items)} instances, {sum(map(size, items))} bytes")

    def constructors(cls: str, title: str):
        items = instances.get(cls, [])
        if not items:
            return
        prof = [profile(n) for n in items]
        plain = sum(1 for p in prof if p[2] == 0)
        ext = collections.Counter(e for p in prof for e in set(p[1]))
        fn = collections.Counter(c for p in prof for c in set(p[0]))
        print(f"\n## {title}: {len(items)} constructors\n")
        print(f"- without a branch: {plain} ({100 * plain // len(items)}%)")
        print(f"- assignments per constructor, median: {statistics.median(p[3] for p in prof):.0f}")
        print("- engine functions called (constructors that call them): "
              + ", ".join(f"`{k}` {v}" for k, v in ext.most_common(14)))
        print("- script functions called: " + ", ".join(f"`{k}` {v}" for k, v in fn.most_common(14)))
        print(f"- distinct script functions called: {len(fn)}, distinct engine functions: {len(ext)}")

    constructors("C_NPC", "NPC")
    constructors("C_ITEM", "Items")
    constructors("C_INFO", "Dialogue topics")

    routines = [n for n in functions if symbols[n]["name"].startswith("RTN_")]
    prof = {n: profile(n) for n in routines}
    entries = [sum(c.startswith("TA_") for c in prof[n][0]) for n in routines]
    other = collections.Counter(c for n in routines for c in prof[n][0] if not c.startswith("TA_"))
    activities = collections.Counter(c for n in routines for c in prof[n][0] if c.startswith("TA_"))
    print(f"\n## Daily routines: {len(routines)} functions\n")
    print(f"- without a branch: {sum(1 for n in routines if prof[n][2] == 0)}")
    print(f"- entries in all: {sum(entries)}; per routine median {statistics.median(entries):.0f}, max {max(entries)}")
    print(f"- routines that call anything but an activity: {sum(1 for n in routines if any(not c.startswith('TA_') for c in prof[n][0]) or prof[n][1])}"
          + (" (" + ", ".join(f"`{k}` {v}" for k, v in other.most_common(6)) + ")" if other else ""))
    print(f"- distinct activities: {len(activities)}; most used: "
          + ", ".join(f"`{k}` {v}" for k, v in activities.most_common(16)))

    def family(name: str) -> str:
        for prefix, title in (("DIA_", "dialogues (DIA_)"), ("ZS_", "AI states (ZS_)"), ("B_", "shared behaviour (B_)"),
                              ("C_", "shared checks (C_)"), ("RTN_", "routines (RTN_)"), ("TA_", "activities (TA_)"),
                              ("PC_", "hero dialogues (PC_)"), ("STARTUP_", "world start (STARTUP_)"),
                              ("INIT_", "world start (STARTUP_)"), ("USE", "item use"), ("EQUIP_", "item use"),
                              ("UNEQUIP_", "item use"), ("SPELL_", "spells"), ("EVT_", "world events (EVT_)")):
            if name.startswith(prefix):
                return title
        return "other functions"

    share = collections.Counter()
    count = collections.Counter()
    for a, n in coded:
        t = symbols[n]["type"]
        key = family(symbols[n]["name"]) if t == T_FUNC else {T_INST: "instance constructors", T_PROTO: "prototypes"}[t]
        share[key] += size(n)
        count[key] += 1
    print("\n## Where the bytecode goes\n")
    print("| Family | functions | bytes | share |")
    print("|---|---:|---:|---:|")
    for k, v in share.most_common():
        print(f"| {k} | {count[k]} | {v} | {100 * v / len(code):.1f}% |")

    states = collections.defaultdict(int)
    for n in functions:
        name = symbols[n]["name"]
        if name.startswith("ZS_"):
            for tail in ("_LOOP", "_END"):
                if name.endswith(tail):
                    name = name[:-len(tail)]
            states[name] += size(n)
    print(f"\n## AI states: {len(states)}\n")
    print("- largest, bytes: " + ", ".join(f"`{k}` {v}" for k, v in sorted(states.items(), key=lambda x: -x[1])[:14]))
    ai = [n for n in functions if symbols[n]["name"].startswith(("ZS_", "B_", "C_"))]
    ext = collections.Counter(e for n in ai for e in profile(n)[1])
    prefix = collections.Counter(k.split("_")[0] for k in ext)
    print(f"- AI code (ZS_, B_, C_): {len(ai)} functions, {sum(map(size, ai))} bytes, "
          f"{sum(profile(n)[2] for n in ai)} branches")
    print(f"- distinct engine functions the AI code calls: {len(ext)} ("
          + ", ".join(f"{k}_ {v}" for k, v in prefix.most_common(8)) + ")")
    print("- most called: " + ", ".join(f"`{k}` {v}" for k, v in ext.most_common(16)))
    by_name = {symbols[n]["name"]: n for n in functions}

    def closure(seeds):
        seen, queue = set(seeds), list(seeds)
        while queue:
            for c in profile(queue.pop())[0]:
                n = by_name.get(c)
                if n is not None and n not in seen:
                    seen.add(n)
                    queue.append(n)
        return seen

    zs = [n for n in functions if symbols[n]["name"].startswith("ZS_")]
    reach = closure(zs)
    print(f"- reachable from the states by direct calls: {len(reach)} functions, {sum(map(size, reach))} bytes "
          f"({100 * sum(map(size, reach)) / len(code):.1f}% of the bytecode)")
    routine_states = closure([n for n in functions if symbols[n]["name"].startswith("TA_")])
    used = sorted({e for n in routine_states for e in profile(n)[1]})
    print(f"- activities (TA_) reach {len(routine_states)} functions; engine functions they call: " + ", ".join(f"`{e}`" for e in used))

    print("\n## Other families\n")
    for title, pick in (("world start (STARTUP_, INIT_)", lambda s: s.startswith(("STARTUP_", "INIT_"))),
                        ("item use (USE*, EQUIP_, UNEQUIP_)", lambda s: s.startswith(("USE", "EQUIP_", "UNEQUIP_"))),
                        ("spells (SPELL_)", lambda s: s.startswith("SPELL_"))):
        group = [n for n in functions if pick(symbols[n]["name"])]
        prof = [profile(n) for n in group]
        ext = collections.Counter(e for p in prof for e in p[1])
        print(f"- **{title}**: {len(group)} functions, {sum(1 for p in prof if p[2] == 0)} without a branch; engine calls: "
              + ", ".join(f"`{k}` {v}" for k, v in ext.most_common(8)))
    for name in ("B_CREATEAMBIENTINV", "B_SETATTRIBUTESTOCHAPTER", "B_SETNPCVISUAL", "B_SETFIGHTSKILLS", "B_GIVENPCTALENTS"):
        if name in by_name:
            group = closure([by_name[name]])
            ext = sorted({e for n in group for e in profile(n)[1]})
            print(f"- `{name}`: {sum(profile(n)[2] for n in group)} branches; engine calls: " + (", ".join(f"`{e}`" for e in ext) or "none"))
    declared = sum(1 for s in symbols if s["type"] == T_FUNC and s["flags"] & EXTERNAL)
    print(f"- engine functions declared for scripts in all: {declared}")


if __name__ == "__main__":
    main()
