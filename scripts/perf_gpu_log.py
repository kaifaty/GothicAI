#!/usr/bin/env python3
"""GPU clock journal of the perf stand: `perf_gpu_log.py OUT.csv` samples nvidia-smi once a second.

Each row carries time.monotonic_ns(), the CLOCK_MONOTONIC of the frame profile's begin_ns
(steady_clock in OpenGothic/common/frameprofile.cpp), so rows line up with frames.
SIGTERM stops nvidia-smi and exits 0; nvidia-smi ending on its own passes on its exit code.
See docs/performance.md.
"""

import csv
import signal
import subprocess
import sys
import time


QUERY = ("temperature.gpu,clocks.sm,clocks.mem,power.draw,pstate,fan.speed,utilization.gpu,"
         "clocks_throttle_reasons.active")
COLUMNS = ("mono_ns", "temp_c", "sm_mhz", "mem_mhz", "power_w", "pstate", "fan_pct", "util_pct", "reasons")
NUMERIC = ("temp_c", "sm_mhz", "mem_mhz", "power_w", "fan_pct", "util_pct")
# Warm-up stops once the card has been loaded this long and its temperature holds still.
WARM_MIN_S = 90
WARM_WINDOW_S = 60
WARM_SPREAD_C = 1.0


def number(text):
    """A float, or None for [N/A], [Not Supported] and other non-numbers."""
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    return value if value == value and abs(value) != float("inf") else None


def read_log(path):
    """Journal rows in order: mono_ns as int, the numeric columns as float or None."""
    rows = []
    try:
        with open(path, newline="") as stream:
            for raw in csv.DictReader(stream):
                try:
                    stamp = int(raw.get("mono_ns") or "")
                except ValueError:
                    continue
                row = {"mono_ns": stamp, "pstate": raw.get("pstate"), "reasons": raw.get("reasons")}
                row.update((name, number(raw.get(name))) for name in NUMERIC)
                rows.append(row)
    except FileNotFoundError:
        pass
    return rows


def warm_reason(rows, started_ns, now_ns, limit_s):
    """'stable' when the card is warm, 'max' at the warm-up ceiling, else None.

    Warm: at least WARM_MIN_S since the first warm-up launch and, over the journal's last
    WARM_WINDOW_S, max - min temperature within WARM_SPREAD_C.
    """
    elapsed = (now_ns - started_ns) / 1e9
    if elapsed >= WARM_MIN_S:
        window = [row for row in rows if now_ns - WARM_WINDOW_S * 1e9 <= row["mono_ns"] <= now_ns
                  and row["temp_c"] is not None]
        # The window must really span the minute, not a few samples after a gap.
        if (len(window) >= 2 and (window[-1]["mono_ns"] - window[0]["mono_ns"]) / 1e9 >= WARM_WINDOW_S - 2
                and max(row["temp_c"] for row in window) - min(row["temp_c"] for row in window) <= WARM_SPREAD_C):
            return "stable"
    return "max" if elapsed >= limit_s else None


class Stop(Exception):
    pass


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) != 1:
        print("usage: perf_gpu_log.py OUT.csv", file=sys.stderr)
        return 2

    def stop(*_):
        raise Stop

    signal.signal(signal.SIGTERM, stop)
    process = None
    code = 0
    try:
        with open(argv[0], "w", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(COLUMNS)
            stream.flush()
            process = subprocess.Popen(["nvidia-smi", f"--query-gpu={QUERY}", "--format=csv,noheader,nounits",
                                        "-lms", "1000"], stdout=subprocess.PIPE, text=True)
            for line in process.stdout:
                stamp = time.monotonic_ns()
                fields = [field.strip() for field in line.split(",")]
                if len(fields) != len(COLUMNS) - 1:
                    continue
                writer.writerow([stamp, *fields])
                stream.flush()
            code = process.wait()
    except Stop:
        pass
    finally:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    return code


if __name__ == "__main__":
    sys.exit(main())
