#!/usr/bin/env python3
"""What Gothic's script program holds about NPCs: the numbers of the note next to this file.

Reads the main Daedalus program from a ready base (our own container of symbols and bytecode,
docs/asset-migration.md, П12). Only the standard library is used.

    python3 reports/engine/2026-11-14-npc-data/census.py [<base dir>]

Without an argument it takes OPENGOTHIC_BASE or the newest base in ~/.cache/gothic-ai/base.
"""
import collections
import hashlib
import json
import os
import re
import statistics
import struct
import sys
from functools import cache
from pathlib import Path

WIDE = {61: 4, 62: 4, 64: 4, 65: 4, 67: 4, 75: 4, 76: 4, 80: 4, 245: 5}  # opcode -> operand bytes
BL, BE, PUSHI, PUSHV, PUSHVI, B, BZ, RSR = 61, 62, 64, 65, 67, 75, 76, 60
ASSIGN = {9, 19, 20, 21, 22, 70, 71, 72, 73, 74}
T_CLASS, T_FUNC, T_PROTO, T_INST = 4, 5, 6, 7
EXTERNAL = 1 << 3
CONST = 1 << 0
GAME = Path(__file__).resolve().parents[3] / "OpenGothic/common/game"


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


def phase_routes(base, program):
    """Match the native dispatcher and this base's captured activity records."""
    records_source = (GAME / "definitions/gameplaydefinitions.h").read_text()
    stock_sha = re.search(r'stockProgram\s*=\s*"([0-9a-f]{64})"', records_source)[1]
    if program != stock_sha:
        return set(), set(), "program is outside the verified NotR route"

    phases = (GAME / "gamescript_activities.cpp").read_text()
    names = re.findall(r'\{("ZS_[A-Z0-9_]+"|\w+),\s*&GameScript::\w+', phases)
    if not names:
        raise ValueError("Native state registrations not found")
    header = (GAME / "gamescript.h").read_text()
    constants = dict(re.findall(r'\b(\w+)\s*=\s*"(ZS_[A-Z0-9_]+)"', header))
    native = {name.strip('"') if name.startswith('"') else constants[name] for name in names}

    path = base / "definitions" / program / "activities.json"
    if not path.is_file():
        return native, set(), "activity records are absent"
    section = json.loads(path.read_text())
    if section["program_sha256"] != program:
        raise ValueError("Activity records do not match the selected program")
    captured = {row["symbol"] for row in section["records"] if row["status"] == "captured"}
    return native, captured, f"activity format v{section['version']}"


def perception_sites(coded, symbols, ops):
    """Find literal function arguments of the stock NPC_PERCENABLE external."""
    callbacks = collections.defaultdict(set)
    sites = matched = 0
    for _, n in coded:
        instructions = list(ops(n))
        for i, (op, arg) in enumerate(instructions):
            if op != BE or not 0 <= arg < len(symbols) or symbols[arg]["name"] != "NPC_PERCENABLE":
                continue
            sites += 1
            previous = instructions[max(0, i - 3):i]
            if tuple(p[0] for p in previous) != (PUSHVI, PUSHV, PUSHI):
                continue
            _, perception_id = previous[1]
            _, target_id = previous[2]
            if not (0 <= perception_id < len(symbols) and 0 <= target_id < len(symbols)):
                continue
            perception = symbols[perception_id]["name"]
            target = symbols[target_id]
            if perception.startswith("PERC_") and target["type"] == T_FUNC:
                callbacks[target["name"]].add((perception, symbols[n]["name"]))
                matched += 1
    return callbacks, sites, matched


def main() -> None:
    base = base_dir()
    programs = base / "programs"
    path = max(programs.glob("*.bin"), key=lambda p: p.stat().st_size)
    symbols, code = read_program(path)
    program = hashlib.sha256(path.with_suffix(".json").read_bytes() + path.read_bytes()).hexdigest()
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

    @cache
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
    by_name = {symbols[n]["name"]: n for _, n in coded}

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
    print(f"- reachable from the states by direct calls: {len(reach)} bytecode symbols, {sum(map(size, reach))} bytes "
          f"({100 * sum(map(size, reach)) / len(code):.1f}% of the bytecode)")
    routine_states = closure([n for n in functions if symbols[n]["name"].startswith("TA_")])
    used = sorted({e for n in routine_states for e in profile(n)[1]})
    print(f"- activities (TA_) reach {len(routine_states)} functions; engine functions they call: " + ", ".join(f"`{e}`" for e in used))

    native, captured, route_scope = phase_routes(base, program)
    missing = (native | captured) - states.keys()
    if missing:
        raise ValueError("Registered states absent from the program: " + ", ".join(sorted(missing)))
    routes = dict.fromkeys(captured, "captured template")
    routes.update(dict.fromkeys(native, "native dispatcher"))
    print(f"\n## NPC entry inventory\n\nProgram `{program}` ({route_scope}).\n")
    print(f"Named states: {len(states)}, native table: {len(native)}, captured templates: {len(captured)}, "
          f"outside both tables: {len(states.keys() - routes.keys())}.")
    print("Static routes describe the stock program with activities enabled. "
          "A dispatcher or a captured record can still reach helpers in the VM. "
          "Check entry into actual bytecode in a game fixture.\n")

    def links(seeds):
        direct = sorted({name for n in seeds for name in profile(n)[0]})
        return ", ".join(f"`{name}`" for name in direct) or "—", len(closure(seeds) - set(seeds))

    print("| State | Phase route | Direct BL targets in stock bytecode | All other BL-reachable symbols |")
    print("|---|---|---|---:|")
    for name in sorted(states):
        phases = (name, f"{name}_LOOP", f"{name}_END")
        seeds = [by_name[phase] for phase in phases if phase in by_name]
        route = routes.get(name, "script to audit")
        direct, count = links(seeds)
        print(f"| `{name}` | {route} | {direct} | {count} |")

    callbacks, sites, matched = perception_sites(coded, symbols, ops)
    kinds = {kind for registrations in callbacks.values() for kind, _ in registrations}
    print(f"\nLiteral perception registrations: {matched}/{sites} sites, "
          f"{len(callbacks)} callback symbols across {len(kinds)} perception kinds.\n")
    print("| Callback | Perceptions | Registered by | Direct BL targets in stock bytecode | All other BL-reachable symbols |")
    print("|---|---|---|---|---:|")
    for name, registrations in sorted(callbacks.items()):
        kinds = ", ".join(f"`{kind}`" for kind in sorted({kind for kind, _ in registrations}))
        callers = ", ".join(f"`{caller}`" for caller in sorted({caller for _, caller in registrations}))
        direct, count = links([by_name[name]] if name in by_name else [])
        print(f"| `{name}` | {kinds} | {callers} | {direct} | {count} |")

    print("\n| Other script roots | Symbols | Other BL-reachable symbols |")
    print("|---|---:|---:|")
    npc_constructors = instances["C_NPC"]
    print(f"| NPC constructors | {len(npc_constructors)} | {len(closure(npc_constructors) - set(npc_constructors))} |")
    for title, prefixes in (
        ("Initial world state", ("STARTUP_", "INIT_", "INITPERCEPTIONS")),
        ("Conversation functions", ("DIA_", "PC_")),
        ("Item handlers by name", ("USE", "EQUIP_", "UNEQUIP_")),
        ("Spell functions by name", ("SPELL_",)),
    ):
        seeds = [n for n in functions if symbols[n]["name"].startswith(prefixes)]
        print(f"| {title} | {len(seeds)} | {len(closure(seeds) - set(seeds))} |")
    print("\nThese edges follow literal BL instructions. Engine callbacks, runtime registrations, "
          "symbol references from save files and bridge entry points require separate review.")

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
