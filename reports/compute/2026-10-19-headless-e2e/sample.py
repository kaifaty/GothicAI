#!/usr/bin/env python3
"""Read-only, 1 Hz observer of this task's processes. Stop by creating OUT/STOP.

Run on Ryzen: python3 sample.py STATE OUT [EXTRA_STATE ...]. Logs are polled at 20 Hz only to
timestamp existing markers; process/thread CPU uses /proc counters, never ps %CPU.
GPU pmon and query-gpu run continuously at 1 Hz. No game/queue settings change.
"""
import csv
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import threading
import time

state, out = map(lambda s: Path(s).resolve(), sys.argv[1:3])
states = [state] + [Path(s).resolve() for s in sys.argv[3:]]
out.mkdir(parents=True, exist_ok=True)
hz = os.sysconf("SC_CLK_TCK")
page = os.sysconf("SC_PAGE_SIZE")
gpu = {}
pmon = {}
children = []


def gpu_reader():
    proc = subprocess.Popen([
        "stdbuf", "-oL", "nvidia-smi", "--query-gpu=utilization.gpu,memory.used",
        "--format=csv,noheader,nounits", "--loop=1",
    ], stdout=subprocess.PIPE, text=True)
    children.append(proc)
    for line in proc.stdout:
        try:
            util, mem = map(float, line.strip().split(","))
            gpu.update(t=time.monotonic(), util=util, mem=mem)
        except ValueError:
            pass


def pmon_reader():
    proc = subprocess.Popen(["stdbuf", "-oL", "nvidia-smi", "pmon", "-s", "um", "-d", "1"],
                            stdout=subprocess.PIPE, text=True)
    children.append(proc)
    for line in proc.stdout:
        cols = line.split()
        if len(cols) < 12 or line.startswith("#"):
            continue
        try:
            pid = int(cols[1])
            number = lambda i: float(cols[i]) if cols[i] != "-" else None
            pmon[pid] = (time.monotonic(), number(3), number(9))
        except ValueError:
            pass


def stat(path):
    text = path.read_text()
    fields = text[text.rfind(")") + 2:].split()
    return ((int(fields[11]) + int(fields[12])) / hz,
            int(fields[19]) / hz, int(fields[21]) * page)


for target in (gpu_reader, pmon_reader):
    threading.Thread(target=target, daemon=True).start()

files = [open(out / name, "a", buffering=1) for name in
         ("process.csv", "threads.csv", "markers.jsonl")]
samples, threads = [csv.writer(f, lineterminator="\n") for f in files[:2]]
if files[0].tell() == 0:
    samples.writerow(["run", "role", "pid", "mono_s", "age_s", "cpu_s", "interval_s",
                      "cores", "rss_mib", "gpu_board_pct", "gpu_board_mib",
                      "gpu_process_sm_pct", "gpu_process_mib", "gpu_age_s", "pmon_age_s"])
if files[1].tell() == 0:
    threads.writerow(["run", "role", "pid", "tid", "comm", "mono_s", "cpu_s",
                      "interval_s", "delta_cpu_s"])
prior = {}
thread_prior = {}
log_offsets = {p: p.stat().st_size for owner in states
               for pattern in ("runs/*/game.log", "runs/*/server.log", "baseline-*/runner.log")
               for p in owner.glob(pattern)}
next_sample = 0
known_logs = set()
marker = re.compile(r"\[ai\] (bridge started|connected to|session )|"
                    r"\[app-cadence\]|game connected from|scenario .*: .*failed|"
                    r"headless_measure_|\"loaded\"|\"reset_wall_ms\"")
try:
    while not (out / "STOP").exists():
        now = time.monotonic()
        if now >= next_sample:
            next_sample = now + 1
            uptime = float(Path("/proc/uptime").read_text().split()[0])
            # Discovery is scoped by exact cwd or this source tree, never by pkill/name alone.
            for proc in Path("/proc").iterdir():
                if not proc.name.isdigit():
                    continue
                try:
                    cwd = Path(os.readlink(proc / "cwd"))
                    cmd = (proc / "cmdline").read_bytes().split(b"\0")
                    exe = Path(os.readlink(proc / "exe")).name
                    if exe == "Gothic2Notr" and any(cwd.parent == owner / "runs" for owner in states):
                        role, run = "game", cwd.name
                    elif exe == "gothic-body-runner" and state in cwd.parents:
                        role, run = "runner", cwd.name
                    elif exe.startswith("python") and any(cwd == owner / "source/server" for owner in states) and any(
                            x.endswith(b"/bin/gothic-ai") for x in cmd):
                        env = dict(x.split(b"=", 1) for x in
                                   (proc / "environ").read_bytes().split(b"\0") if b"=" in x)
                        role, run = "server", Path(os.fsdecode(env[b"LOG_DIR"])).name
                    else:
                        continue
                    pid = int(proc.name)
                    cpu, start, rss = stat(proc / "stat")
                    last = prior.get((pid, start))
                    dt = now - last[0] if last else None
                    cores = (cpu - last[1]) / dt if last else None
                    prior[pid, start] = (now, cpu)
                    pm = pmon.get(pid)
                    samples.writerow([run, role, pid, round(now, 6), round(uptime-start, 3),
                                      cpu, round(dt, 6) if dt else "",
                                      round(cores, 6) if cores is not None else "",
                                      round(rss/2**20, 3), gpu.get("util", ""), gpu.get("mem", ""),
                                      pm[1] if pm else "", pm[2] if pm else "",
                                      round(now-gpu["t"], 3) if gpu else "",
                                      round(now-pm[0], 3) if pm else ""])
                    for task in (proc / "task").iterdir():
                        try:
                            tcpu, tstart, _ = stat(task / "stat")
                            key = (pid, int(task.name), tstart)
                            previous = thread_prior.get(key)
                            thread_prior[key] = (now, tcpu)
                            # Preserve initial counters and every nonzero delta, omit idle rows.
                            delta = tcpu-previous[1] if previous else tcpu
                            if delta:
                                threads.writerow([run, role, pid, task.name,
                                                  (task / "comm").read_text().strip(),
                                                  round(now, 6), tcpu,
                                                  round(now-previous[0], 6) if previous else "",
                                                  round(delta, 6)])
                        except (OSError, ValueError):
                            pass
                except (OSError, ValueError, KeyError):
                    pass
        for owner in states:
            known_logs.update((owner / "runs").glob("*/game.log"))
            known_logs.update((owner / "runs").glob("*/server.log"))
            known_logs.update(owner.glob("baseline-*/runner.log"))
        for log in known_logs:
            try:
                with log.open(errors="replace") as stream:
                    stream.seek(log_offsets.get(log, 0))
                    for line in stream:
                        if marker.search(line):
                            # Branch IDs/paths are unnecessary in the public evidence.
                            line = re.sub(r"\b[0-9a-f]{32}\b", "BRANCH", line.strip())
                            line = re.sub(r"session \S+", "session BRANCH", line)
                            line = re.sub(r"ws://\S+", "ws://loopback", line)
                            files[2].write(json.dumps(dict(run=log.parent.name, log=log.name,
                                                         mono_s=time.monotonic(), line=line)) + "\n")
                    log_offsets[log] = stream.tell()
            except OSError:
                pass
        time.sleep(.05)
finally:
    for proc in children:
        proc.terminate()
    for stream in files:
        stream.close()
