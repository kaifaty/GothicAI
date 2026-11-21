"""Plan interleaved perf.sh runs and execute them inside one quiet queue job."""

import os
from pathlib import Path
import re
import shlex
import subprocess
import sys


def plan(lines, repeats):
    variants = []
    singles = []
    for line in lines:
        if not line.strip():
            continue
        name, separator, settings = line.rstrip("\n").partition("|")
        if not separator or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*(/round-[1-9][0-9]*)?", name):
            raise ValueError(f"expected name|PERF_ENV or name/round-N|PERF_ENV, got {line.strip()!r}")
        for assignment in shlex.split(settings):
            key, equals, _ = assignment.partition("=")
            if not equals or not key.isidentifier():
                raise ValueError(f"invalid PERF_ENV assignment for {name}: {assignment!r}")
        if "/" in name:
            singles.append((name, settings))
        else:
            variants.append((name, settings))

    runs = []
    for round_no in range(1, repeats + 1):
        order = variants if round_no % 2 else reversed(variants)
        runs.extend((f"{name}/round-{round_no}", settings) for name, settings in order)
    runs.extend(singles)
    if not runs:
        raise ValueError("empty variant list")
    if len({name for name, _ in runs}) != len(runs):
        raise ValueError("multiple runs would write to the same variant and round")
    return runs


def run(out, places):
    with (out / "variants.txt").open() as source:
        runs = plan(source, 1)
    perf = Path(__file__).with_name("perf.sh")
    result = 0
    for name, settings in runs:
        print(f"perf: {name}", flush=True)
        env = dict(os.environ, PERF_REPEAT="1", PERF_ENV=f"{os.environ.get('PERF_ENV', '')} {settings}")
        status = subprocess.run(["bash", perf, "__run", out / name, *places], env=env).returncode
        if status:
            print(f"perf: {name} failed ({status})", file=sys.stderr, flush=True)
            if status < 0:
                return 128 - status
            result = result or status
    return result


if __name__ == "__main__":
    try:
        if sys.argv[1] == "plan":
            for name, settings in plan(sys.stdin, int(sys.argv[2])):
                print(f"{name}|{settings}")
        elif sys.argv[1] == "run":
            sys.exit(run(Path(sys.argv[2]), sys.argv[3:]))
        else:
            sys.exit("usage: perf_variants.py plan REPEATS | run OUT PLACE...")
    except (OSError, ValueError) as error:
        sys.exit(f"perf: {error}")
