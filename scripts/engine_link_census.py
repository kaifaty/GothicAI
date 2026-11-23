#!/usr/bin/env python3
"""Link census of the engine platform: which upper-layer symbols a platform-only program would miss.

Reads a CMake + Ninja build dir where `Gothic2Notr` is built. The object set is exactly what the
build system links into the target: its explicit and implicit `.o` and `.a` inputs, and the
objects of every archive built in the same tree (`ninja -t query`). Each object is mapped to its
source (`ninja -t compdb`); a source in OpenGothic/ outside lib/ and the build dir is our code and
gets its layer from `engine_layers.layer_of`, anything else is a third-party library.

A hole is an undefined symbol of a platform object that is defined only by our objects of
another layer: no platform object and no third-party object defines it. Weak definitions
(inline functions, template instances) count as definitions. Symbols defined nowhere in the
build are system ones; they are only counted.

    python3 scripts/engine_link_census.py OpenGothic/build             # markdown report
    python3 scripts/engine_link_census.py OpenGothic/build --json      # the same as JSON

Step 8.2 of the engine layers plan: reports/engine/2026-11-13-empty-island/README.md.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from engine_layers import LAYERS, layer_of

LINKABLE = (".o", ".obj", ".a", ".lib")
OBJECT = (".o", ".obj")

# GNU nm: undefined (U), weak undefined (w, v); global definitions; weak ones (W, V).
ELF_UNDEFINED = set("Uwv")
ELF_DEFINED = set("ABCDGRSTVWiu")
ELF_WEAK = set("VW")
ELF_LINE = re.compile(r"^(?:[0-9a-fA-F]+)?\s+(?P<type>\S)\s+(?P<name>\S.*)$")
# nm -m (Mach-O): "addr (__TEXT,__text) weak private external __Z3foov".
MACHO_LINE = re.compile(r"^(?:[0-9a-fA-F]+)?\s*\((?P<section>[^)]*)\)(?:\s+\(alignment [^)]*\))?\s+"
                        r"(?P<kind>(?:weak\s+)?(?:private\s+)?external|non-external)\s+"
                        r"(?:\(was a private external\)\s+)?(?:\[[a-z ]+\]\s+)*(?P<name>\S.*?)"
                        r"(?:\s+\((?:from|weak import)[^)]*\))?$")


@dataclass
class Symbols:
    undefined: set[str] = field(default_factory=set)
    defined: dict[str, bool] = field(default_factory=dict)  # name -> weak


def parse_nm(text: str, fmt: str) -> Symbols:
    """Global symbols of one `nm` (ELF) or `nm -m` (Mach-O) output; Mach-O loses its leading `_`."""
    out = Symbols()
    for line in text.splitlines():
        if not line.strip() or line.endswith(":"):
            continue
        if fmt == "macho":
            m = MACHO_LINE.match(line)
            if not m or m["kind"] == "non-external":
                continue
            name = m["name"].removeprefix("_")
            if m["section"] == "undefined":
                out.undefined.add(name)
            else:
                out.defined[name] = out.defined.get(name, True) and m["kind"].startswith("weak")
        else:
            m = ELF_LINE.match(line)
            if not m:
                continue
            kind, name = m["type"], m["name"]
            if kind in ELF_UNDEFINED:
                out.undefined.add(name)
            elif kind in ELF_DEFINED:
                out.defined[name] = out.defined.get(name, True) and kind in ELF_WEAK
    out.undefined -= out.defined.keys()
    return out


def ninja(build: Path, *args: str) -> str:
    return subprocess.run(["ninja", "-C", str(build), *args], capture_output=True, text=True, check=True).stdout


def query_inputs(text: str) -> tuple[str, list[str]]:
    """Rule and explicit plus implicit inputs of one `ninja -t query` block; order-only ones are dropped."""
    rule, inputs, section = "", [], None
    for line in text.splitlines():
        stripped = line.strip()
        if line.startswith("  ") and not line.startswith("    "):
            section = stripped.split(":", 1)[0]
            if section == "input":
                rule = stripped.split(":", 1)[1].strip()
            continue
        if section != "input" or not stripped or stripped.startswith("||"):
            continue
        inputs.append(stripped[1:].strip() if stripped.startswith("|") else stripped)
    return rule, inputs


def link_objects(build: Path, target: str) -> list[tuple[str, str]]:
    """(object, library) pairs linked into `target`; objects of the target itself have its library name."""
    seen: set[str] = set()
    result: list[tuple[str, str]] = []

    def walk(node: str, library: str) -> None:
        if node in seen:
            return
        seen.add(node)
        try:
            rule, inputs = query_inputs(ninja(build, "-t", "query", node))
        except subprocess.CalledProcessError:
            rule, inputs = "", []
        if rule == "phony":
            for i in inputs:
                walk(i, library)
            return
        for i in inputs:
            if i.endswith(OBJECT):
                if i not in seen:
                    seen.add(i)
                    result.append((i, library))
            elif i.endswith(LINKABLE):
                walk(i, archive_name(i))

    walk(target, target)
    return result


def archive_name(path: str) -> str:
    name = Path(path).name
    for suffix in (".a", ".lib"):
        name = name.removesuffix(suffix)
    return name.removeprefix("lib")


def source_map(build: Path) -> dict[str, str]:
    """Object path (relative to the build dir) -> absolute source path."""
    out = {}
    for entry in json.loads(ninja(build, "-t", "compdb")):
        directory = Path(entry["directory"])
        obj = Path(os.path.normpath(directory / entry["output"]))
        src = os.path.normpath(directory / entry["file"])
        try:
            out[obj.relative_to(build).as_posix()] = src
        except ValueError:
            out[obj.as_posix()] = src
    return out


def classify(source: str | None, engine: Path, build: Path) -> tuple[str, str | None]:
    """(path relative to OpenGothic/, layer) for our code; (source, None) for a third-party one."""
    if source is None:
        return "", None
    path = Path(source)
    for outside in (engine / "lib", build):
        if path.is_relative_to(outside):
            return source, None
    if not path.is_relative_to(engine):
        return source, None
    rel = path.relative_to(engine).as_posix()
    return rel, layer_of(rel)


def run_nm(nm: str, fmt: str, obj: Path) -> Symbols:
    args = [nm, "-m", str(obj)] if fmt == "macho" else [nm, str(obj)]
    out = subprocess.run(args, capture_output=True, text=True, check=True).stdout
    return parse_nm(out, fmt)


def demangle(names: list[str], cxxfilt: str) -> dict[str, str]:
    if not names:
        return {}
    try:
        out = subprocess.run([cxxfilt, "-n"], input="\n".join(names) + "\n", capture_output=True, text=True,
                             check=True).stdout.splitlines()
    except (OSError, subprocess.CalledProcessError):
        return {n: n for n in names}
    return dict(zip(names, out)) if len(out) == len(names) else {n: n for n in names}


@dataclass
class Unit:
    object: str
    library: str
    source: str        # path relative to OpenGothic/ for our code, absolute (or empty) otherwise
    layer: str | None  # None for third-party code
    symbols: Symbols


def analyse(units: list[Unit]) -> dict:
    """Holes, third-party needs and system symbols of the platform objects among `units`."""
    definers: dict[str, list[tuple[Unit, bool]]] = defaultdict(list)
    for u in units:
        for name, weak in u.symbols.defined.items():
            definers[name].append((u, weak))

    holes: dict[str, dict[str, list[dict]]] = defaultdict(dict)  # platform file -> symbol -> definers
    third: dict[str, dict[str, set[str]]] = defaultdict(lambda: defaultdict(set))  # library -> file -> symbols
    system: Counter = Counter()  # symbol -> references
    weak_only: dict[str, set[str]] = defaultdict(set)  # library -> symbols it holds only as weak copies
    internal = third_refs = 0
    platform = [u for u in units if u.layer == "platform"]
    for u in platform:
        for name in sorted(u.symbols.undefined):
            found = definers.get(name, [])
            if any(d.layer == "platform" for d, _ in found):
                internal += 1
            elif any(d.layer is None for d, _ in found):
                third_refs += 1
                # A strong definition is the library's own code; weak copies of header code (libc++'s
                # bad_function_call in Tempest objects on macOS) count only when no library has a strong one.
                outside = [(d, weak) for d, weak in found if d.layer is None]
                strong = [d for d, weak in outside if not weak]
                for d in strong or [d for d, _ in outside]:
                    third[d.library][u.source].add(name)
                    if not strong:
                        weak_only[d.library].add(name)
            elif found:
                holes[u.source][name] = sorted(
                    ({"file": d.source, "layer": d.layer, "weak": weak} for d, weak in found),
                    key=lambda x: (x["weak"], x["file"]))
            else:
                system[name] += 1
    return {"holes": holes, "third": third, "system": system, "internal": internal, "third_refs": third_refs,
            "weak_only": weak_only,
            "platform": platform}


def report(units: list[Unit], cxxfilt: str) -> dict:
    a = analyse(units)
    names = sorted({s for per in a["holes"].values() for s in per})
    readable = demangle(names, cxxfilt)
    holes = []
    for file in sorted(a["holes"]):
        for name in sorted(a["holes"][file], key=lambda n: readable[n]):
            holes.append({"file": file, "symbol": readable[name], "mangled": name, "defined_in": a["holes"][file][name]})
    by_file = Counter(h["file"] for h in holes)
    by_layer: Counter = Counter()
    pairs_by_layer: Counter = Counter()
    for layer in LAYERS:
        hit = [h for h in holes if any(d["layer"] == layer for d in h["defined_in"])]
        by_layer[layer] = len({h["mangled"] for h in hit})
        pairs_by_layer[layer] = len(hit)
    third = []
    for library in sorted(a["third"]):
        files = a["third"][library]
        third.append({"library": library, "symbols": len(set().union(*files.values())),
                      "weak_only_symbols": len(a["weak_only"].get(library, ())),
                      "files": [{"file": f, "symbols": len(s)} for f, s in sorted(files.items())]})
    objects: Counter = Counter(u.layer or "third-party" for u in units)
    return {
        "objects": dict(sorted(objects.items())),
        "platform_files": len(a["platform"]),
        "platform_undefined": sum(len(u.symbols.undefined) for u in a["platform"]),
        "resolved_in_platform": a["internal"],
        "third_party_references": a["third_refs"],
        "hole_references": len(holes),
        "hole_symbols": len(names),
        "hole_files": len(by_file),
        "system_symbols": len(a["system"]),
        "system_references": sum(a["system"].values()),
        "holes_by_file": dict(by_file.most_common()),
        "hole_symbols_by_layer": {l: n for l, n in by_layer.items() if n},
        "hole_references_by_layer": {l: n for l, n in pairs_by_layer.items() if n},
        "holes": holes,
        "third_party": third,
    }


def markdown(r: dict, target: str) -> str:
    out = [f"# Перепись компоновки платформы: `{target}`", "",
           "Создано `scripts/engine_link_census.py`; слои — по `LAYER_RULES` из `scripts/engine_layers.py`.", "",
           "| Объектов по слоям | |", "|---|---:|"]
    out += [f"| `{k}` | {v} |" for k, v in r["objects"].items()]
    out += ["", (f"Файлов платформы: **{r['platform_files']}**, неопределённых ссылок у них: **{r['platform_undefined']}**. "
            f"Из них в платформе: {r['resolved_in_platform']}, в сторонних библиотеках: {r['third_party_references']}, "
            f"дыр: **{r['hole_references']}** ({r['hole_symbols']} символов в {r['hole_files']} файлах), "
            f"системных: {r['system_references']} ({r['system_symbols']} символов)."), "",
            "## Дыры", "", "| Файл платформы | Символ | Где определён |", "|---|---|---|"]
    for h in r["holes"]:
        where = [f"`{d['file']}` ({d['layer']}{', слабое' if d['weak'] else ''})" for d in h["defined_in"]]
        if len(where) > 3:
            where = where[:3] + [f"и ещё {len(where) - 3}"]
        symbol = h["symbol"].replace("|", "\\|")
        out.append(f"| `{h['file']}` | `{symbol}` | {', '.join(where)} |")
    out += ["", "| Файл платформы | Дыр |", "|---|---:|"] + [f"| `{f}` | {n} |" for f, n in r["holes_by_file"].items()]
    out += ["", "| Слой, где определено | Символов | Ссылок |", "|---|---:|---:|"]
    out += [f"| `{l}` | {n} | {r['hole_references_by_layer'][l]} |" for l, n in r["hole_symbols_by_layer"].items()]
    out += ["", "## Сторонние библиотеки платформы", "", "| Библиотека | Символов | Из них только слабые копии | Файлы платформы (ссылок) |",
            "|---|---:|---:|---|"]
    for t in r["third_party"]:
        files = ", ".join(f"`{f['file']}` ({f['symbols']})" for f in t["files"])
        out.append(f"| `{t['library']}` | {t['symbols']} | {t['weak_only_symbols']} | {files} |")
    return "\n".join(out) + "\n"


def collect(build: Path, target: str, engine: Path, nm: str, fmt: str, jobs: int) -> list[Unit]:
    objects = link_objects(build, target)
    sources = source_map(build)
    with ThreadPoolExecutor(jobs) as pool:
        symbols = list(pool.map(lambda o: run_nm(nm, fmt, build / o[0]), objects))
    units = []
    for (obj, library), syms in zip(objects, symbols):
        source, layer = classify(sources.get(obj), engine, build)
        units.append(Unit(obj, library, source or obj, layer, syms))
    return units


def engine_dir(build: Path) -> Path:
    cache = build / "CMakeCache.txt"
    if cache.is_file():
        m = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache.read_text(errors="replace"), re.MULTILINE)
        if m:
            return Path(m.group(1)).resolve()
    raise SystemExit(f"{cache}: no CMAKE_HOME_DIRECTORY; pass --engine")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("build", type=Path, help="CMake + Ninja build dir with the target built")
    ap.add_argument("--target", default="Gothic2Notr")
    ap.add_argument("--engine", type=Path, help="OpenGothic source dir (default: from CMakeCache.txt)")
    ap.add_argument("--nm", default="nm")
    ap.add_argument("--cxxfilt", default="c++filt")
    ap.add_argument("--format", choices=("elf", "macho"), default="macho" if sys.platform == "darwin" else "elf")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    build = args.build.resolve()
    engine = args.engine.resolve() if args.engine else engine_dir(build)
    units = collect(build, args.target, engine, args.nm, args.format, args.jobs)
    if not units:
        raise SystemExit(f"{build}: no objects linked into {args.target}")
    r = report(units, args.cxxfilt)
    print(json.dumps(r, ensure_ascii=False, indent=1) if args.json else markdown(r, args.target), end="\n" if args.json else "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
