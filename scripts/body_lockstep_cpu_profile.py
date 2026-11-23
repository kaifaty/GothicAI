#!/usr/bin/env python3
"""CPU ms per simulated tick of lockstep collect runs, by function.

Run the collector (body_rl_collect.py) with --runner pointing at a wrapper that preloads
body-runner-cpu-sample.c (see its header) and with one CPUSAMPLE_OUT directory per runner
build. With --fork-reset each episode is a fork, sampled from its own .bin file; the template
(the lowest pid of a directory) only loads the world and is reported apart. Ticks come from the
collector's episodes.jsonl. Self time comes from the interrupted pc, so flat (CPUSAMPLE_FLAT=1)
samples are enough for it; unwound samples (a runner without AVX Jolt) also give inclusive time:
each function counts once per sample in which it is on the stack.

  body_lockstep_cpu_profile.py --profile old=SAMPLES:COLLECT_OUT --profile new=SAMPLES:COLLECT_OUT
"""

import argparse
import bisect
import collections
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from body_runner_step_profile import Symbolizer, read_maps, read_samples, short  # noqa: E402


def load(samples_dir, collect_out, sym):
    files = []
    for binfile in glob.glob(os.path.join(samples_dir, "*.bin")):
        maps_file = binfile[:-4] + ".maps"
        if os.path.exists(maps_file) and os.path.getsize(binfile) > 0:
            files.append((int(binfile.rsplit(".", 2)[-2]), binfile, maps_file))
    files.sort()
    # A template loads the world (seconds of CPU) and then only forks; an episode fork lives for
    # one duel. Tell them apart by CPU spent and lifetime.
    pcs = {"template": collections.Counter(), "episode": collections.Counter()}
    stacks = {"template": collections.Counter(), "episode": collections.Counter()}
    for pid, binfile, maps_file in files:
        rows, base = read_maps(maps_file)
        starts = [r[0] for r in rows]
        recs = read_samples(binfile)
        kind = "template" if len(recs) > 1500 and recs and recs[-1][0]-recs[0][0] > 3e9 else "episode"
        def locate(pc):
            i = bisect.bisect_right(starts, pc) - 1
            if i < 0 or pc >= rows[i][1]:
                return ("?", 0)
            return (rows[i][2], pc - base.get(rows[i][2], rows[i][0]))

        for _, frames in recs:
            pc = frames[2] if len(frames) > 2 else frames[-1]
            pcs[kind][locate(pc)] += 1
            # Return addresses point after their call: look up the call itself.
            stacks[kind][(locate(pc),)+tuple(locate(ret-1) for ret in frames[3:])] += 1
    by_binary = collections.defaultdict(set)
    for counter in pcs.values():
        for (path, vaddr) in counter:
            if path != "?":
                by_binary[path].add(vaddr)
    for counter in stacks.values():
        for stack in counter:
            for (path, vaddr) in stack:
                if path != "?":
                    by_binary[path].add(vaddr)
    for path, vaddrs in by_binary.items():
        sym.resolve(path, vaddrs)
    out = {}
    for kind, counter in pcs.items():
        funcs, libs = collections.Counter(), collections.Counter()
        for (path, vaddr), n in counter.items():
            name = "?" if path == "?" else short(sym.chain(path, vaddr)[-1])
            funcs[name] += n
            libs[os.path.basename(path)] += n
        inclusive = collections.Counter()
        for stack, n in stacks[kind].items():
            names = set()
            for path, vaddr in stack:
                if path != "?":
                    names.update(short(name) for name in sym.chain(path, vaddr))
            for name in names:
                inclusive[name] += n
        out[kind] = (funcs, libs, sum(counter.values()), inclusive)
    episodes = [json.loads(line) for p in glob.glob(os.path.join(collect_out, "worker-*/episodes.jsonl"))
                for line in open(p)]
    return out, sum(e["ticks"] for e in episodes), len(episodes)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--profile", action="append", required=True, help="name=SAMPLES_DIR:COLLECT_OUT")
    parser.add_argument("--period-us", type=int, default=1000)
    parser.add_argument("--top", type=int, default=50)
    args = parser.parse_args(argv)
    sym = Symbolizer()
    names, data = [], {}
    for spec in args.profile:
        name, rest = spec.split("=", 1)
        samples, collect = rest.split(":", 1)
        names.append(name)
        data[name] = load(samples, collect, sym)
    ms = args.period_us / 1000
    print("| | " + " | ".join(names) + " |")
    print("|---|" + "---:|" * len(names))
    print("| episodes | " + " | ".join(str(data[n][2]) for n in names) + " |")
    print("| ticks | " + " | ".join(str(data[n][1]) for n in names) + " |")
    print("| **episode CPU, ms/tick** | " + " | ".join(f"**{data[n][0]['episode'][2]*ms/data[n][1]:.3f}**" for n in names) + " |")
    print("| template CPU, s | " + " | ".join(f"{data[n][0]['template'][2]*ms/1000:.1f}" for n in names) + " |")
    print()
    print("Episode CPU by library, ms/tick")
    libs = collections.Counter()
    for n in names:
        libs.update(data[n][0]["episode"][1])
    for lib, _ in libs.most_common(12):
        print(f"  {lib:40s} " + " ".join(f"{data[n][0]['episode'][1][lib]*ms/data[n][1]:8.3f}" for n in names))
    print()
    for title, column in (("self", 0), ("inclusive, unwound samples only", 3)):
        print(f"Episode CPU by function ({title}), ms/tick")
        funcs = collections.Counter()
        for n in names:
            for f, c in data[n][0]["episode"][column].items():
                funcs[f] = max(funcs[f], c / data[n][1])
        for f, _ in funcs.most_common(args.top):
            print(f"  {f[:90]:90s} " + " ".join(f"{data[n][0]['episode'][column][f]*ms/data[n][1]:8.3f}" for n in names))
        print()


if __name__ == "__main__":
    main()
