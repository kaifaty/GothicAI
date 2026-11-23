#!/usr/bin/env python3
"""Read-only native tick/CPU evidence in one bounded Linux arena; no model.

The sampler observes the native process and this probe's cgroup. Bracketing
counter intervals can include adjacent ticks and collector work; they are not
exact per-tick CPU measurements and do not establish a cause by themselves.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import threading
import time

from body_arena_env import ArenaRunner
from body_headless_suite import rng_environment, sha256, write_json
from body_observe_suite import provenance

PROFILE = {"mode": "realtime", "task": "native_duel", "seed": 223, "side": 1,
           "distance_cm": 400, "timeout_ms": 10000, "poll_ms": 20,
           "sampler_ms": 20, "thread_sampler_ms": 50, "max_samples": 2048,
           "max_threads": 64, "ipc_capacity_bytes": 32*1024*1024,
           "sampler_capacity_bytes": 16*1024*1024, "wall_timeout_s": 45,
           "visibility_watch": True, "model": None, "quality_credit": False}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def parse_stat(value):
    """Linux /proc stat has a parenthesized comm which may contain spaces/')."""
    tail = value[value.rfind(")")+2:].split()
    require(len(tail) >= 22, "short /proc stat row")
    return {"state": tail[0], "minflt": int(tail[7]), "majflt": int(tail[9]),
            "utime_ticks": int(tail[11]), "stime_ticks": int(tail[12]),
            "num_threads": int(tail[17]), "starttime_ticks": int(tail[19]),
            "rss_pages": int(tail[21])}


def key_values(value):
    result = {}
    for line in value.splitlines():
        key, number = line.replace(":", "", 1).split()
        result[key] = int(number)
    return result


def pressure(value):
    result = {}
    for line in value.splitlines():
        parts = line.split()
        result[parts[0]] = {key: (int(number) if key == "total" else float(number))
                            for key, number in (part.split("=") for part in parts[1:])}
    return result


def schedstat(value):
    fields = value.split()
    require(len(fields) == 3, "invalid schedstat")
    return dict(zip(("runtime_ns", "runqueue_ns", "timeslices"), map(int, fields)))


def cgroup_path(pid):
    rows = Path(f"/proc/{pid}/cgroup").read_text().splitlines()
    paths = [line.split(":", 2)[2] for line in rows if line.startswith("0::")]
    require(len(paths) == 1, "a unique cgroup v2 path is required")
    root = Path("/sys/fs/cgroup").resolve()
    path = (root/paths[0].lstrip("/")).resolve()
    require(path.is_relative_to(root) and (path/"cpu.stat").is_file(), "unreadable cgroup v2 CPU counters")
    return path


def hierarchy_limits(path):
    result = []
    while path.is_relative_to(Path("/sys/fs/cgroup")):
        row = {"path": str(path)}
        for name in ("cpu.max", "memory.max", "cpuset.cpus.effective"):
            file = path/name
            if file.exists():
                row[name] = file.read_text().strip()
        result.append(row)
        if path == Path("/sys/fs/cgroup"):
            break
        path = path.parent
    return result


class Sampler:
    def __init__(self, pid):
        self.pid = pid
        self.group = cgroup_path(pid)
        self.metadata = {"pid": pid, "clock_domain": "linux-clock-monotonic",
                         "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"),
                         "page_size_bytes": os.sysconf("SC_PAGE_SIZE"),
                         "cgroup": str(self.group), "hierarchy_limits": hierarchy_limits(self.group),
                         "cgroup_scope": "native runner plus collector/sampler and any children",
                         "process_scope": "native runner including its worker threads"}
        self.rows = []
        self.encoded = []
        self.bytes = 0
        self.error = None
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self.loop, name="native-causal-sampler", daemon=True)

    def sample(self, threads=False):
        begin = time.monotonic_ns()
        proc = Path(f"/proc/{self.pid}")
        row = {"kind": "external_native_sample", "begin_wall_monotonic_ns": begin,
               "pid": self.pid, "process": parse_stat((proc/"stat").read_text()),
               "main_schedstat": schedstat((proc/"schedstat").read_text()),
               "io": key_values((proc/"io").read_text()),
               "cgroup_cpu_stat": key_values((self.group/"cpu.stat").read_text()),
               "sampler_thread_cpu_ns": time.thread_time_ns(), "pressure": {}}
        for domain in ("cpu", "io", "memory"):
            file = self.group/(domain+".pressure")
            if file.exists():
                row["pressure"][domain] = pressure(file.read_text())
        if threads:
            tasks = sorted((proc/"task").iterdir(), key=lambda path: int(path.name))
            require(len(tasks) <= PROFILE["max_threads"], "native thread sampler capacity exceeded")
            row["threads"] = {}
            for task in tasks:
                try:
                    row["threads"][task.name] = {"comm": (task/"comm").read_text().strip(),
                        "schedstat": schedstat((task/"schedstat").read_text()),
                        "stat": parse_stat((task/"stat").read_text())}
                except FileNotFoundError:
                    # A native loading thread may end while /proc is sampled.
                    row["threads"][task.name] = {"ended_during_sample": True}
        row["end_wall_monotonic_ns"] = time.monotonic_ns()
        encoded = json.dumps(row, separators=(",", ":"))+"\n"
        require(len(self.rows) < PROFILE["max_samples"] and
                self.bytes+len(encoded.encode()) <= PROFILE["sampler_capacity_bytes"],
                "external sampler bounded evidence capacity exceeded")
        self.rows.append(row)
        self.encoded.append(encoded)
        self.bytes += len(encoded.encode())

    def loop(self):
        next_threads = 0
        try:
            while not self.stop_event.is_set():
                now = time.monotonic_ns()
                threads = now >= next_threads
                self.sample(threads)
                if threads:
                    next_threads = now+PROFILE["thread_sampler_ms"]*1_000_000
                self.stop_event.wait(PROFILE["sampler_ms"]/1000)
        except Exception as error:
            if not self.stop_event.is_set():
                self.error = str(error)

    def start(self):
        self.thread.start()

    def stop(self, output):
        self.stop_event.set()
        self.thread.join(timeout=2)
        require(not self.thread.is_alive(), "external sampler did not stop")
        (output/"external-samples.jsonl").write_text("".join(self.encoded))
        write_json(output/"external-metadata.json", {**self.metadata, "samples": len(self.rows),
                                                   "bytes": self.bytes, "error": self.error})


class CausalRunner(ArenaRunner):
    def __init__(self, args):
        self.args, self.buffer, self.sequence, self.samples = args, b"", 0, []
        self.records, self.record_bytes = [], 0
        self.stderr = (args.output/"runner-stderr.log").open("wb")
        self.ipc = (args.output/"ipc.jsonl").open("x")
        workdir = args.output/"run"
        workdir.mkdir()
        (workdir/"Gothic.ini").write_bytes((args.reference/"run/Gothic.ini").read_bytes())
        self.argv = [str(args.runner), "-g", str(args.assets), "-devmode", "--save", str(args.save),
                     "--scenario", str(args.scenario), "--output", str(args.output/"initial"),
                     "--mode", "realtime", "--stdio", "--scheduling-diagnostics"]
        if args.compact_replies:
            self.argv.append("--compact-replies")
        if args.serial_workers:
            self.argv.append("--serial-workers")
        self.process = subprocess.Popen(self.argv, cwd=workdir, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=self.stderr, env=rng_environment("isolated"))

    def record(self, direction, value):
        row = {"direction": direction, "wall_monotonic_ns": time.monotonic_ns(), "value": value}
        encoded = json.dumps(row, separators=(",", ":"))+"\n"
        require(self.record_bytes+len(encoded.encode()) <= PROFILE["ipc_capacity_bytes"],
                "native IPC bounded evidence capacity exceeded")
        self.records.append(encoded)
        self.record_bytes += len(encoded.encode())

    def persist(self):
        (self.args.output/"ipc.jsonl").write_text("".join(self.records))


def delta(before, after):
    result = {}
    for key in set(before)&set(after):
        if type(before[key]) is int and type(after[key]) is int:
            require(after[key] >= before[key], "sampled native counter moved backwards: "+key)
            result[key] = after[key]-before[key]
    return result


def worker_profile(response, serial):
    """Bind an actual worker mode; old binaries cannot attest serial execution."""
    require(type(serial) is bool, "serial worker profile must be an explicit boolean")
    if "worker_execution" not in response:
        require(not serial, "serial worker response lacks actual worker execution evidence")
        return {"attested": False, "legacy_diagnostic_only": True}
    value = response["worker_execution"]
    require(isinstance(value, dict) and value.get("serial") is serial and
            value.get("mode") == ("caller_serial" if serial else "default_parallel") and
            type(value.get("worker_pool_created")) is bool, "native worker execution profile differs")
    cap = value.get("native_task_parallelism_cap")
    if serial:
        require(value["worker_pool_created"] is False and type(cap) is int and cap == 1,
                "serial worker profile created a pool or changed its task cap")
    else:
        require("native_task_parallelism_cap" in value and cap is None,
                "default worker profile reports a serial task cap")
    return {"attested": True, "legacy_diagnostic_only": False}


def join_tick(tick, samples, clock_ticks):
    begin, end = tick
    before = [row for row in samples if row["end_wall_monotonic_ns"] <= begin["wall_monotonic_ns"]]
    after = [row for row in samples if row["begin_wall_monotonic_ns"] >= end["wall_monotonic_ns"]]
    result = {"tick_id": begin["tick_id"], "sim_step_before": begin["sim_step"],
              "sim_step_after": end["sim_step"], "game_ms_before": begin["game_ms"],
              "game_ms_after": end["game_ms"], "begin_wall_monotonic_ns": begin["wall_monotonic_ns"],
              "end_wall_monotonic_ns": end["wall_monotonic_ns"],
              "wall_duration_ms": (end["wall_monotonic_ns"]-begin["wall_monotonic_ns"])/1e6,
              "start_lag_ms": max(0, begin["wall_monotonic_ns"]-begin["tick_due_wall_monotonic_ns"])/1e6,
              "counter_scope": "bracketing interval; may include adjacent ticks", "bracketed": bool(before and after)}
    if before and after:
        left, right = before[-1], after[0]
        result.update(sample_begin_ns=left["begin_wall_monotonic_ns"], sample_end_ns=right["end_wall_monotonic_ns"],
                      native_process_delta=delta({key: left["process"][key] for key in
                          ("utime_ticks", "stime_ticks", "minflt", "majflt")},
                          {key: right["process"][key] for key in ("utime_ticks", "stime_ticks", "minflt", "majflt")}),
                      main_schedstat_delta=delta(left["main_schedstat"], right["main_schedstat"]),
                      native_io_delta=delta(left["io"], right["io"]),
                      whole_probe_cgroup_cpu_delta=delta(left["cgroup_cpu_stat"], right["cgroup_cpu_stat"]))
        result["native_cpu_ms"] = 1000*sum(result["native_process_delta"].get(key, 0)
                                          for key in ("utime_ticks", "stime_ticks"))/clock_ticks
        result["pressure_total_delta_us"] = {domain: {kind: values["total"]-left["pressure"][domain][kind]["total"]
            for kind, values in right["pressure"][domain].items() if kind in left["pressure"][domain]}
            for domain in set(left["pressure"])&set(right["pressure"])}
    return result


def audit_saved_output(output):
    output = Path(output)
    manifest = json.loads((output/"manifest.json").read_text())
    probe = json.loads((output/"probe.json").read_text())
    metadata = json.loads((output/"external-metadata.json").read_text())
    serial = manifest.get("serial_workers")
    require(type(serial) is bool and probe["status"] == "collected" and
            probe["profile"] == {**PROFILE, "serial_workers": serial} and
            manifest["driver_sha256"] == sha256(Path(__file__)), "probe/helper/profile binding differs")
    require(metadata["error"] is None, "external sampler failed: "+str(metadata["error"]))
    def rows(name):
        return [json.loads(line) for line in (output/name).read_text().splitlines() if line]
    samples, ipc = rows("external-samples.jsonl"), rows("ipc.jsonl")
    require(0 < len(samples) <= PROFILE["max_samples"] and metadata["samples"] == len(samples), "sampler count differs")
    for index, row in enumerate(samples):
        require(row["begin_wall_monotonic_ns"] <= row["end_wall_monotonic_ns"] and
                (not index or samples[index-1]["end_wall_monotonic_ns"] <= row["begin_wall_monotonic_ns"]),
                "external sampler clocks overlap or move backwards")
        require("usage_usec" in row["cgroup_cpu_stat"] and "nr_throttled" in row["cgroup_cpu_stat"], "missing actual cgroup CPU evidence")
    cpu = [float(row["cpu.max"].split()[0])/int(row["cpu.max"].split()[1])
           for row in metadata["hierarchy_limits"] if row.get("cpu.max", "max").split()[0] != "max"]
    memory = [int(row["memory.max"]) for row in metadata["hierarchy_limits"] if row.get("memory.max", "max") != "max"]
    require(cpu and min(cpu) <= 2 and memory and min(memory) <= 6*1024**3, "actual cgroup CPU200%/Memory6GiB budget missing")
    requests, responses = {}, {}
    for row in ipc:
        table = requests if row["direction"] == "request" else responses
        identifier = row["value"]["id"]
        require(identifier not in table, "duplicate native IPC ID")
        table[identifier] = row
    require(set(requests) == set(responses), "native IPC request/response chain incomplete")
    allowed = {"reset", "body_visibility_watch", "arena_start", "body_events", "body_observations", "status", "close"}
    worker_attestations = []
    for identifier, request in requests.items():
        response = responses[identifier]
        value = response["value"]
        require(request["value"]["cmd"] in allowed and value.get("ok") is not False and
                value.get("clock_domain") == "linux-clock-monotonic" and
                request["wall_monotonic_ns"] <= value["wall_monotonic_ns"] <= response["wall_monotonic_ns"],
                "probe altered physics/ownership or IPC clock bounds")
        require(all(value.get(key) is False for key in
                    ("graphics_device_created", "window_created", "audio_device_created")), "native device evidence missing")
        worker_attestations.append(worker_profile(value, serial))
    scheduling = [row for row in rows("episode/observer-timing.jsonl") if row.get("kind") == "runner_scheduling"]
    require([row["seq"] for row in scheduling] == list(range(1, len(scheduling)+1)), "native scheduling rows dropped")
    closing = probe["close_response"]["scheduling_diagnostics"]
    require(closing["rows_emitted"] == len(scheduling) and closing["dropped_total"] == 0, "native scheduling capture differs")
    ticks, active = [], None
    for row in scheduling:
        if row["phase"] == "tick_begin":
            require(active is None, "overlapping native ticks")
            active = row
        elif row["phase"] == "tick_end":
            require(active is not None and row["tick_id"] == active["tick_id"] and
                    row["sim_step"] == active["sim_step"]+1 and row["game_ms"] > active["game_ms"] and
                    row["wall_monotonic_ns"] >= active["wall_monotonic_ns"], "native BEGIN/END differs")
            ticks.append((active, row)); active = None
        else:
            require(row["phase"] == "input_read", "unknown native scheduling phase")
    require(active is None and ticks, "native tick capture incomplete")
    final = probe["final"]["arena"]
    require(final["started"] and (final["terminated"] or final["truncated"]) and not final["infrastructure"] and
            all(actor["owner"] == "native" for actor in final["actors"]), "native duel did not end naturally")
    joins = [join_tick(tick, samples, metadata["clock_ticks_per_second"]) for tick in ticks
             if tick[0]["game_ms"] >= final["start_game_ms"]]
    names = ("manifest.json", "probe.json", "external-metadata.json", "external-samples.jsonl", "ipc.jsonl",
             "episode/observer-timing.jsonl", "episode/observations.jsonl", "episode/events.jsonl")
    return {"schema_version": 1, "kind": "body_native_causal_saved_audit", "passed": True,
            "model": None, "quality_credit": False, "causal_conclusion": "observational counters only",
            "worker_profile": {"serial_workers": serial,
                "attested_responses": sum(row["attested"] for row in worker_attestations),
                "legacy_diagnostic_only_responses": sum(row["legacy_diagnostic_only"] for row in worker_attestations),
                "actual_worker_mode_attested": all(row["attested"] for row in worker_attestations)},
            "ticks": len(joins), "slow_ticks_above_33ms": sum(row["wall_duration_ms"] > 33 for row in joins),
            "maximum_tick_wall_ms": max(row["wall_duration_ms"] for row in joins),
            "tick_intervals": joins, "raw_sha256": {name: sha256(output/name) for name in names}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "assets", "save", "reference", "source", "engine-library", "config", "scenario",
                 "melee-scenario", "native-reference", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--expected-binary-sha256")
    parser.add_argument("--serial-workers", action="store_true", help="opt-in startup caller execution; default keeps the original worker profile")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("native causal probe runs only on dev-ubuntu")
    for name, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, name, value.resolve())
    args.output.mkdir(exist_ok=False)
    result = {"schema_version": 1, "kind": "body_native_causal_probe", "status": "failed",
              "profile": {**PROFILE, "serial_workers": args.serial_workers},
              "model": None, "quality_credit": False, "quality_gate_evaluated": False,
              "imported_model_modules": []}
    runner = sampler = None
    try:
        if args.expected_binary_sha256:
            require(sha256(args.runner) == args.expected_binary_sha256, "frozen diagnostic binary SHA changed")
        args.native_serial_workers = args.serial_workers
        args.compact_replies = json.loads(args.config.read_text()).get("runtime", {}).get("compact_replies", False)
        manifest = {**provenance(args), "kind": result["kind"], "driver_sha256": sha256(Path(__file__)),
                    "profile": result["profile"], "serial_workers": args.serial_workers,
                    "compact_replies": args.compact_replies, "quality_credit": False,
                    "runtime_limits": {"cpu_quota_percent": 200, "memory_max_gib": 6},
                    "helper_sha256": {name: {"path": str(Path(sys.modules[name].__file__).resolve()),
                                             "sha256": sha256(Path(sys.modules[name].__file__))}
                                      for name in ("body_arena_env", "body_observe_suite", "body_headless_suite")}}
        write_json(args.output/"manifest.json", manifest)
        runner = CausalRunner(args)
        result.update(runner_pid=runner.process.pid, runner_argv=runner.argv)
        sampler = Sampler(runner.process.pid); sampler.start()
        episode = args.output/"episode"; episode.mkdir()
        result["initial"] = runner.call({"cmd": "reset", "scenario": str(args.scenario), "output_dir": str(episode),
            "mode": "realtime", "arena": {"scenario": PROFILE["task"], "seed": PROFILE["seed"], "side": PROFILE["side"],
                                           "distance_cm": PROFILE["distance_cm"], "timeout_ms": PROFILE["timeout_ms"]}})
        runner.sample()
        runner.call({"cmd": "body_visibility_watch", "enabled": True})
        runner.call({"cmd": "body_observations", "max": 256})
        result["start"] = response = runner.call({"cmd": "arena_start", "provider": "native"})
        started = time.monotonic()
        while not (response["arena"]["terminated"] or response["arena"]["truncated"] or response["arena"]["infrastructure"]):
            require(time.monotonic()-started < PROFILE["wall_timeout_s"], "native diagnostic wall budget exhausted")
            require(sampler.error is None, "external sampler failed: "+str(sampler.error))
            runner.call({"cmd": "body_events"})
            response = runner.call({"cmd": "body_observations", "max": 256})
            if not (response["arena"]["terminated"] or response["arena"]["truncated"]):
                time.sleep(PROFILE["poll_ms"]/1000)
        result["final"] = runner.call({"cmd": "status"})
        runner.sample()
        # Get a post-terminal external bracket while the unchanged native process
        # is idle; this does not pause, advance or alter the physical episode.
        time.sleep(PROFILE["sampler_ms"]/1000*2)
        sampler.stop(args.output); sampler = None
        result["close_response"] = runner.call({"cmd": "close"})
        require(runner.process.wait(timeout=15) == 0, "native probe did not close cleanly")
        runner.persist()
        result["status"] = "collected"
        write_json(args.output/"probe.json", result)
        audit = audit_saved_output(args.output)
        write_json(args.output/"audit.json", audit)
    except Exception as error:
        result["error"] = str(error)
    finally:
        if sampler is not None:
            sampler.stop(args.output)
        if runner is not None:
            runner.terminate()
            runner.persist()
        result["imported_model_modules"] = [name for name in sys.modules if name.split(".")[0] in
                                            ("torch", "tensorflow", "jax", "onnxruntime")]
        if result["imported_model_modules"]:
            result.update(status="failed", error="model library unexpectedly imported")
        if result.get("error"):
            result["status"] = "failed"
        write_json(args.output/"probe.json", result)
    print(json.dumps({"status": result["status"], "error": result.get("error"), "model": None}))
    return 0 if result["status"] == "collected" else 1


if __name__ == "__main__":
    raise SystemExit(main())
