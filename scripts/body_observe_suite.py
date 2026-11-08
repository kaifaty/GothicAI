#!/usr/bin/env python3
"""Exercise passive numeric observations in the real CPU runner on dev-ubuntu.

All phases reuse one engine process, with sequential fresh-world resets. The
suite never substitutes a Python simulation for native combat or physics.
Raw observation/timing journals are emitted by the engine, independently of
this driver's bounded IPC queue polling.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import select
import subprocess
import sys
import time
from typing import Any

from body_headless_suite import (process_sample, rng_environment,
                                 service_snapshot, sha256, write_json)


def native_worker_arguments(args):
    value=getattr(args,"native_serial_workers",False)
    if type(value) is not bool:
        raise ValueError("native_serial_workers must be an explicit boolean")
    return ["--serial-workers"] if value else []


class ObserverRunner:
    """Bounded JSONL protocol reader; large rows live in the engine journal."""
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.stderr = (args.output / "runner-stderr.log").open("wb")
        self.ipc = (args.output / "ipc.jsonl").open("w", encoding="utf-8")
        self.buffer = b""
        self.sequence = 0
        self.samples: list[dict[str, Any]] = []
        workdir = args.output / "run"
        workdir.mkdir(exist_ok=True)
        (workdir / "Gothic.ini").write_bytes((args.reference / "run/Gothic.ini").read_bytes())
        self.process = subprocess.Popen(
            [str(args.runner), "-g", str(args.assets), "-devmode", "--save", str(args.save),
             "--scenario", str(args.scenario), "--output", str(args.output / "initial"),
             "--mode", "lockstep", "--stdio"] +
            (["--compact-replies"] if getattr(args, "compact_replies", False) else [])+native_worker_arguments(args)+
            list(getattr(args, "runner_extra_args", [])), cwd=workdir,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.stderr,
            env=rng_environment("isolated"))

    def record(self, direction: str, value: dict[str, Any]) -> None:
        compact = dict(value)
        # Preserve the complete numeric rows in the engine journal. Protocol
        # evidence retains a digest/count instead of duplicating 48k samples.
        for key in ("observations", "rows", "samples"):
            if isinstance(compact.get(key), list) and key != "samples":
                rows = compact.pop(key)
                compact[key + "_count"] = len(rows)
                compact[key + "_sha256"] = hashlib.sha256(
                    json.dumps(rows, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
        self.ipc.write(json.dumps({"direction": direction, "wall_monotonic": time.monotonic(),
                                   "value": compact}, ensure_ascii=False)+"\n")
        self.ipc.flush()

    def request(self, command: dict[str, Any], timeout_s: float = 180,
                *, allow_error: bool = False) -> dict[str, Any]:
        self.sequence += 1
        request = {"id": self.sequence, **command}
        self.record("request", request)
        self.process.stdin.write((json.dumps(request)+"\n").encode())
        self.process.stdin.flush()
        deadline = time.monotonic()+timeout_s
        while b"\n" not in self.buffer:
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise RuntimeError("runner wall watchdog expired; infrastructure failure")
            ready, _, _ = select.select([self.process.stdout], [], [], min(5, remaining))
            if not ready:
                if self.process.poll() is not None:
                    raise RuntimeError(f"runner exited before response: {self.process.returncode}")
                continue
            chunk = os.read(self.process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError(f"runner closed JSONL output: {self.process.poll()}")
            self.buffer += chunk
            if len(self.buffer) > 16*1024*1024:
                raise RuntimeError("runner JSONL response exceeded bounded 16 MiB reader")
        line, self.buffer = self.buffer.split(b"\n", 1)
        response = json.loads(line)
        if not isinstance(response, dict) or response.get("id") != request["id"] or response.get("schema_version") != 1:
            raise RuntimeError("runner response schema/ID mismatch")
        self.record("response", response)
        if response.get("ok") is False and not allow_error:
            raise RuntimeError(f"runner rejected command: {response.get('error')}")
        return response

    def sample(self) -> dict[str, Any]:
        sample = process_sample(self.process.pid)
        self.samples.append(sample)
        if sample["forbidden_device_descriptors"] or sample["display_socket_descriptors"]:
            raise RuntimeError("runner opened graphics/audio device or display socket")
        return sample

    def terminate(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        self.stderr.close()
        self.ipc.close()


def provenance(args: argparse.Namespace) -> dict[str, Any]:
    baseline = json.loads((args.reference / "manifest.json").read_text())
    assets = baseline["assets"]
    for entry in assets:
        path = args.assets / entry["path"]
        if not path.is_file() or path.stat().st_size != entry["size"] or sha256(path) != entry["sha256"]:
            raise RuntimeError(f"archived asset bytes changed: {entry['path']}")
    actual = {str(path.relative_to(args.assets)) for path in args.assets.rglob("*") if path.is_file()}
    if actual != {entry["path"] for entry in assets}:
        raise RuntimeError("asset inventory differs from archived reference")
    if sha256(args.save) != baseline["save_sha256"]:
        raise RuntimeError("reset save changed from archived reference")
    if sha256(args.reference / "run/Gothic.ini") != baseline["settings_sha256"]:
        raise RuntimeError("archived settings changed")
    digest, files = hashlib.sha256(), 0
    for path in sorted(args.source.rglob("*")):
        if path.is_file():
            digest.update(str(Path("OpenGothic") / path.relative_to(args.source)).encode())
            digest.update(bytes.fromhex(sha256(path)))
            files += 1
    return {"schema_version": 1, "kind": "body_observe_stage3",
            "recorded_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "host": platform.node(), "platform": platform.platform(), "python": platform.python_version(),
            "binary_sha256": sha256(args.runner), "engine_library_sha256": sha256(args.engine_library),
            "source_sha256": digest.hexdigest(), "source_files_hashed": files,
            "save_sha256": sha256(args.save), "settings_sha256": baseline["settings_sha256"],
            "game_version": baseline["game_version"],
            "config_sha256": sha256(args.config), "scenario_sha256": sha256(args.scenario),
            "melee_scenario_sha256": sha256(args.melee_scenario), "assets": assets,
            "asset_hash_coverage": "all regular files in assets",
            "reference_manifest_sha256": sha256(args.reference / "manifest.json"),
            "headless": True, "renderer": "none", "display": None, "audio_playback": False,
            "graphics_device_created": False, "window_created": False, "audio_device_created": False,
            "model": None, "sim_fps": 30, "time_multiplier": 1,
            "native_serial_workers":bool(native_worker_arguments(args)),
            "animation_rng_mode": "isolated", "observer_ownership": "passive",
            "environment": {key: os.environ.get(key) for key in
                            ("DISPLAY", "WAYLAND_DISPLAY", "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "GOTHIC_AI")},
            "runtime_limits": {"cpu_quota_percent": 200, "memory_max_gib": 12,
                               "tasks_max": 128, "wall_timeout_sec": 2400},
            "services_before": service_snapshot()}


def run_phase(runner: ObserverRunner, args: argparse.Namespace, manifest: dict[str, Any],
              name: str, role: str, scenario: Path, *, enabled: bool = True,
              mode: str = "lockstep", pair_id: int | None = None,
              withheld_queue: bool = False) -> dict[str, Any]:
    output = args.output / name
    output.mkdir()
    initial_samples = len(runner.samples)
    initial = runner.request({"cmd": "reset", "scenario": str(scenario),
                              "output_dir": str(output), "mode": mode})
    watch = runner.request({"cmd": "body_watch", "enabled": enabled, "rate_hz": 20,
                            "max_actors": 4, "queue_capacity": 16 if withheld_queue else 256,
                            "ray_range_cm": 300})
    runner.sample()
    started, processing_s, requests, drained, steps = time.monotonic(), 0.0, 0, 0, 0
    batches_file = (output / "observation-batches.jsonl").open("w", encoding="utf-8")
    queue_probe_before: dict[str, Any] | None = None
    queue_probe_after: dict[str, Any] | None = None
    last_minute = -1
    while True:
        before = time.monotonic()
        advanced = runner.request({"cmd": "advance", "substeps": 60})
        if mode == "lockstep":
            processing_s += time.monotonic()-before
        requests += 1
        steps += advanced.get("steps_advanced", 0)
        if not withheld_queue:
            pull_started = time.monotonic()
            packet = runner.request({"cmd": "body_observations", "max": 256})
            if mode == "lockstep":
                processing_s += time.monotonic()-pull_started
            rows = packet.get("observations", packet.get("rows", []))
            if not isinstance(rows, list):
                raise RuntimeError("body_observations response has no numeric row array")
            batches_file.write(json.dumps(packet, ensure_ascii=False, separators=(",", ":"))+"\n")
            batches_file.flush()
            drained += len(rows)
        elif requests == 2:
            queue_probe_before = runner.request({"cmd": "status"})
            queue_probe_after = runner.request({"cmd": "body_observations", "max": 256})
            batches_file.write(json.dumps(queue_probe_after, ensure_ascii=False, separators=(",", ":"))+"\n")
            batches_file.flush()
        runner.sample()
        minute = advanced.get("simulated_ms", 0)//60000
        if minute != last_minute:
            print(f"body observe [{name}] {advanced.get('simulated_ms', 0)} game ms, {advanced.get('sim_step', 0)} substeps", flush=True)
            last_minute = minute
        if advanced.get("done"):
            break
        if requests > 1000:
            raise RuntimeError("phase exceeded bounded advance command budget")
    final = runner.request({"cmd": "status"})
    final_packet = runner.request({"cmd": "body_observations", "max": 256})
    batches_file.write(json.dumps(final_packet, ensure_ascii=False, separators=(",", ":"))+"\n")
    batches_file.close()
    # Disable should revoke sampling only; native combat and body ownership stay
    # untouched. A fresh reset reconstructs all exposed observation identities.
    disabled = runner.request({"cmd": "body_watch", "enabled": False})
    for key in ("graphics_device_created", "window_created", "audio_device_created"):
        if final.get(key) is not False:
            raise RuntimeError(f"runner did not attest device absence: {key}")
    result = {"name": name, "role": role, "status": "completed", "pair_id": pair_id,
              "mode": mode, "watch_enabled": enabled, "initial_status": initial,
              "watch_status": watch, "final": final, "disable_status": disabled,
              "simulated_ms": final["simulated_ms"], "sim_step": final["sim_step"],
              "processing_wall_s": processing_s if mode == "lockstep" else None,
              "substeps_per_processing_s": steps/processing_s if processing_s and mode == "lockstep" else None,
              "wall_s": time.monotonic()-started, "advance_requests": requests,
              "advance_request_substeps": steps, "drained_observations": drained,
              "final_packet": {key: value for key, value in final_packet.items() if key not in ("observations", "rows")},
              "observations_file": f"{name}/observation-batches.jsonl",
              "journal_file": f"{name}/observations.jsonl",
              "timings_file": f"{name}/observer-timing.jsonl", "events_file": f"{name}/events.jsonl",
              "scenario_file": str(scenario), "scenario_sha256": sha256(scenario),
              "resources": runner.samples[initial_samples:]}
    if withheld_queue:
        result["queue_probe"] = {"before_drain": queue_probe_before,
                                  "after_drain": {key: value for key, value in (queue_probe_after or {}).items()
                                                  if key not in ("observations", "rows")},
                                  "drained_rows": len((queue_probe_after or {}).get("observations", []))}
    candidate = {**manifest, "kind": "body_observe_phase", "mode": mode, "realtime": mode == "realtime",
                 "watch_enabled": enabled, "scenario_sha256": result["scenario_sha256"], "run": result}
    write_json(output / "manifest.json", candidate)
    write_json(output / "phase.json", result)
    return result


def shorter_scenario(args: argparse.Namespace, duration_ms: int, name: str) -> Path:
    output = args.output / "scenarios"
    output.mkdir(exist_ok=True)
    source = args.scenario.read_text()
    if source.count("bodyobserve 17 600000") != 1 or source.count("wait 600100") != 1:
        raise RuntimeError("stage 3 scenario does not have one declared duration pair")
    source = source.replace("bodyobserve 17 600000", f"bodyobserve 17 {duration_ms}")
    source = source.replace("wait 600100", f"wait {duration_ms+100}")
    path = output / f"{name}.bench"
    path.write_text(source)
    return path


def privacy_scenario(args: argparse.Namespace) -> Path:
    output = args.output / "scenarios"
    output.mkdir(exist_ok=True)
    setup, separator, _ = args.scenario.read_text().partition("bodyobserve 17 600000")
    if not separator:
        raise RuntimeError("stage 3 scenario setup marker is missing")
    path = output / "privacy-edges.bench"
    path.write_text(setup + "bodyduel 17 1000\n" +
                    "bodyobserveprobe visible\nwait 800\n" +
                    "bodyobserveprobe occluded\nwait 800\n" +
                    "bodyobserveprobe hidden_move\nwait 800\n" +
                    "bodyobserveprobe reappear\nwait 800\n")
    return path


def native_parity(args: argparse.Namespace, off: str, on: str) -> dict[str, Any]:
    output = args.output / "native-observer-parity"
    completed = subprocess.run([sys.executable, str(Path(__file__).with_name("body_headless_parity.py")),
                                str(args.output / off), str(args.output / on),
                                "--profile", "full", "--output-dir", str(output)],
                               check=False, timeout=180)
    report = json.loads((output / "headless-parity.json").read_text())
    if completed.returncode != 0 or report.get("status") != "matched":
        raise RuntimeError(f"native observer on/off mechanics diverged: {report.get('status')}")
    return {"exit_code": completed.returncode, "report": report,
            "report_path": "native-observer-parity/headless-parity.json",
            "reference_run": off, "candidate_run": on}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "assets", "save", "reference", "source", "engine-library",
                 "config", "scenario", "melee-scenario", "output"):
        parser.add_argument("--"+name, type=Path, required=True)
    parser.add_argument("--smoke", action="store_true", help="short infrastructure probe; cannot claim stage 3 readiness")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("native simulation runs only on dev-ubuntu (Linux), never the Mac")
    for key, value in vars(args).items():
        if isinstance(value, Path):
            setattr(args, key, value.resolve())
    args.output.mkdir(exist_ok=True)
    result: dict[str, Any] = {"schema_version": 1, "kind": "body_observe_stage3_suite",
                              "status": "infrastructure_failure", "full_stage3_evidence": False,
                              "required_gate_failures": [], "runs": [], "comparisons": {}}
    runner: ObserverRunner | None = None
    manifest: dict[str, Any] = {}
    try:
        manifest = provenance(args)
        write_json(args.output / "manifest.json", manifest)
        runner = ObserverRunner(args)
        result["runner_pid"] = runner.process.pid
        result["single_engine_process"] = True
        perf = shorter_scenario(args, 3000 if args.smoke else 90000, "performance")
        queue = shorter_scenario(args, 6000, "bounded-queue")
        # Pair order alternates to reduce a monotonic thermal/cache bias. Every
        # phase fresh-loads the same save and executes the same scenario bytes.
        for pair in range(1 if args.smoke else 3):
            order = (False, True) if pair % 2 == 0 else (True, False)
            for enabled in order:
                name = f"performance-{'on' if enabled else 'off'}-{pair}"
                result["runs"].append(run_phase(runner, args, manifest, name,
                                                "paired_on" if enabled else "paired_off", perf,
                                                enabled=enabled, pair_id=pair))
                write_json(args.output / "suite.json", result)
        result["runs"].append(run_phase(runner, args, manifest, "bounded-queue", "bounded_queue", queue,
                                        withheld_queue=True))
        result["runs"].append(run_phase(runner, args, manifest, "privacy-edges", "privacy_edges",
                                        privacy_scenario(args)))
        if not args.smoke:
            for enabled in (False, True):
                name = "native-on" if enabled else "native-off"
                result["runs"].append(run_phase(runner, args, manifest, name,
                                                "native_on" if enabled else "native_off", args.melee_scenario,
                                                enabled=enabled))
            result["comparisons"]["native_observer_on_off"] = native_parity(args, "native-off", "native-on")
        realtime_scenario = shorter_scenario(args, 6000, "realtime-smoke") if args.smoke else args.scenario
        result["runs"].append(run_phase(runner, args, manifest, "passive-realtime", "passive_realtime",
                                        realtime_scenario, mode="realtime"))
        closed = runner.request({"cmd": "close"})
        runner.process.wait(timeout=15)
        if runner.process.returncode != 0:
            raise RuntimeError(f"runner did not close cleanly: {runner.process.returncode}")
        result.update({"status": "passed", "full_stage3_evidence": not args.smoke,
                       "services_after": service_snapshot(), "close_status": closed,
                       "limitations": ["ASR/TTS latency not measured; service state and shared GPU only",
                                       "passive subscription; masks do not imply a stage 4 action executor",
                                       "stage 3 readiness requires independent raw evidence validation"]})
        manifest.update({"suite_status": result["status"], "suite_scope": "smoke" if args.smoke else "full",
                         "completed_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                         "services_after": result["services_after"]})
        write_json(args.output / "manifest.json", manifest)
        write_json(args.output / "suite.json", result)
        if not args.smoke:
            validation_dir = args.output / "validated"
            completed = subprocess.run([sys.executable, str(Path(__file__).with_name("body_observe_report.py")),
                                        str(args.output), "--config", str(args.config),
                                        "--output-dir", str(validation_dir)], check=False, timeout=180)
            validation_file = validation_dir / "body-observe.json"
            parsed = json.loads(validation_file.read_text()) if validation_file.is_file() else {}
            result["validation"] = {"exit_code": completed.returncode, "status": parsed.get("status"),
                                     "report_path": "validated/body-observe.json", "issues": parsed.get("issues", [])}
            if completed.returncode != 0 or parsed.get("status") != "complete":
                result["full_stage3_evidence"] = False
                manifest["suite_status"] = "failed"
                write_json(args.output / "manifest.json", manifest)
                raise RuntimeError(f"independent stage 3 validation failed: {parsed.get('issues', ['missing report'])}")
        print(f"body observe suite {result['status']}; scope {'smoke' if args.smoke else 'full'}", flush=True)
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        result["status"] = "failed" if result.get("validation") else "infrastructure_failure"
        result["full_stage3_evidence"] = False
        result["error"] = str(error)
        result["required_gate_failures"].append(str(error))
        print(f"body observe suite failed: {error}", file=sys.stderr, flush=True)
        return 2
    finally:
        if runner is not None:
            runner.terminate()
        write_json(args.output / "suite.json", result)


if __name__ == "__main__":
    raise SystemExit(main())
