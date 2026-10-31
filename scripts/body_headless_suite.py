#!/usr/bin/env python3
"""Stage 2 local IPC/resource/provenance suite. Run only on dev-ubuntu.

Uses the real native engine runner; this file does not simulate game mechanics.
No third-party packages, model calls, display server, or GPU backend are required.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import select
import signal
import statistics
import subprocess
import sys
import time
from typing import Any


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def command_output(argv: list[str]) -> str:
    return subprocess.run(argv, capture_output=True, text=True, timeout=15).stdout.strip()


def service_snapshot() -> dict[str, Any]:
    return {
        "gpu": command_output(["nvidia-smi", "--query-gpu=memory.used,memory.free,utilization.gpu", "--format=csv,noheader"]),
        "services": {name: command_output(["systemctl", "--user", "show", name,
                                          "--property=LoadState,ActiveState,SubState,MainPID,MemoryCurrent,CPUUsageNSec"])
                     for name in ("gothic-voxtral", "gothic-voxcpm")},
        "service_latency_measurement": None,
    }


def distribution(values: list[float]) -> dict[str, Any]:
    if not values:
        return {"n": 0, "min": None, "median": None, "p95": None, "max": None}
    ordered = sorted(values)
    return {"n": len(values), "min": ordered[0], "median": statistics.median(values),
            "p95": ordered[min(len(ordered)-1, int((len(ordered)-1)*.95+.5))], "max": ordered[-1]}


def process_sample(pid: int) -> dict[str, Any]:
    """Linux /proc evidence, including device and X/Wayland socket descriptors."""
    root = Path(f"/proc/{pid}")
    status = {}
    for line in (root / "status").read_text().splitlines():
        key, _, value = line.partition(":")
        if key in ("VmRSS", "VmHWM", "VmSize", "Threads"):
            status[key] = int(value.split()[0])
    # comm can contain spaces; everything after the final ')' begins with state.
    fields = (root / "stat").read_text().rsplit(")", 1)[1].split()
    clock_ticks = os.sysconf("SC_CLK_TCK")
    descriptors = {}
    for path in (root / "fd").iterdir():
        try:
            descriptors[path.name] = os.readlink(path)
        except FileNotFoundError:
            pass  # a descriptor can close while /proc is sampled
    unix_paths = {}
    for line in Path("/proc/net/unix").read_text().splitlines()[1:]:
        columns = line.split()
        if len(columns) >= 8:
            unix_paths[columns[6]] = columns[7]
    sockets = [unix_paths.get(value[8:-1], "") for value in descriptors.values()
               if value.startswith("socket:[")]
    forbidden_devices = [value for value in descriptors.values()
                         if value.startswith(("/dev/nvidia", "/dev/dri", "/dev/snd"))]
    display_sockets = [value for value in sockets
                       if "/.X11-unix/" in value or "wayland-" in value]
    return {"t_wall_monotonic": time.monotonic(), "pid": pid,
            "rss_kib": status.get("VmRSS"), "peak_rss_kib": status.get("VmHWM"),
            "virtual_kib": status.get("VmSize"), "threads": status.get("Threads"),
            "file_descriptors": len(descriptors),
            "cpu_user_s": int(fields[11])/clock_ticks,
            "cpu_system_s": int(fields[12])/clock_ticks,
            "forbidden_device_descriptors": forbidden_devices,
            "display_socket_descriptors": display_sockets}


def provenance(args: argparse.Namespace) -> dict[str, Any]:
    reference_manifest = json.loads((args.reference / "manifest.json").read_text())
    expected = reference_manifest["assets"]
    observed = []
    for entry in expected:
        asset = args.assets / entry["path"]
        if not asset.is_file():
            raise RuntimeError(f"reference asset missing: {entry['path']}")
        digest = sha256(asset)
        if asset.stat().st_size != entry["size"] or digest != entry["sha256"]:
            raise RuntimeError(f"reference asset changed: {entry['path']}")
        observed.append({"path": entry["path"], "size": entry["size"], "sha256": digest})
    actual_paths = {str(path.relative_to(args.assets)) for path in args.assets.rglob("*") if path.is_file()}
    if actual_paths != {entry["path"] for entry in expected}:
        raise RuntimeError("assets directory differs from archived baseline manifest")
    if sha256(args.save) != reference_manifest["save_sha256"]:
        raise RuntimeError("reset save differs from archived baseline")
    if sha256(args.config) != reference_manifest["config_sha256"]:
        raise RuntimeError("scenario config differs from archived baseline")
    source = hashlib.sha256()
    source_files = 0
    for path in sorted(args.source.rglob("*")):
        if path.is_file():
            source.update(str(Path("OpenGothic") / path.relative_to(args.source)).encode())
            source.update(bytes.fromhex(sha256(path)))
            source_files += 1
    return {"schema_version": 1, "kind": "body_headless_stage2",
            "recorded_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "host": platform.node(), "platform": platform.platform(),
            "python": platform.python_version(), "headless": True,
            "binary_sha256": sha256(args.runner), "source_sha256": source.hexdigest(),
            "source_files_hashed": source_files,
            "save_sha256": sha256(args.save), "config_sha256": sha256(args.config),
            "assets": observed, "asset_hash_coverage": "all regular files in assets",
            "reference_manifest_sha256": sha256(args.reference / "manifest.json"),
            "reference_trace_sha256": sha256(args.reference / "events.jsonl"),
            "reference_binary_sha256": sha256(args.reference / "reference/Gothic2Notr"),
            "reference": str(args.reference), "sim_fps": 30, "model": None,
            "renderer": "none", "display": None, "audio_playback": False,
            "graphics_device_created": False, "window_created": False,
            "audio_device_created": False,
            "scenario_sha256": reference_manifest["scenario_sha256"],
            "settings_sha256": reference_manifest["settings_sha256"],
            "game_version": reference_manifest["game_version"],
            "environment": {key: os.environ.get(key) for key in
                            ("DISPLAY", "WAYLAND_DISPLAY", "VK_DRIVER_FILES", "VK_ICD_FILENAMES", "GOTHIC_AI")},
            "runtime_limits": {"cpu_quota_percent": 200, "memory_max_gib": 12,
                               "tasks_max": 128, "wall_timeout_sec": 1800},
            "services_before": service_snapshot()}


class Runner:
    """One process, one JSON command pending, wall timeouts distinct from game time."""
    def __init__(self, argv: list[str], workdir: Path, output: Path, timeout_s: float = 180,
                 environment: dict[str, str] | None = None):
        self.log = (output / "runner-stderr.log").open("wb")
        self.protocol = (output / "ipc.jsonl").open("w", encoding="utf-8")
        self.timeout_s = timeout_s
        self.samples: list[dict[str, Any]] = []
        self.sequence = 0
        self.stdout_buffer = b""
        self.process = subprocess.Popen(argv, cwd=workdir, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=self.log, env=environment)
        self.reader = self.process.stdout
        self.sample()

    def sample(self) -> dict[str, Any]:
        sample = process_sample(self.process.pid)
        self.samples.append(sample)
        if sample["forbidden_device_descriptors"] or sample["display_socket_descriptors"]:
            raise RuntimeError(f"runner created a forbidden graphics/audio descriptor: {sample}")
        return sample

    def receive(self) -> dict[str, Any]:
        deadline = time.monotonic()+self.timeout_s
        while b"\n" not in self.stdout_buffer:
            remaining = deadline-time.monotonic()
            if remaining <= 0:
                raise RuntimeError("runner wall watchdog expired; infrastructure failure")
            ready, _, _ = select.select([self.reader], [], [], min(remaining, 5))
            if not ready:
                if self.process.poll() is not None:
                    raise RuntimeError(f"runner exited before response: {self.process.returncode}")
                continue
            data = os.read(self.reader.fileno(), 65536)
            if not data:
                raise RuntimeError(f"runner closed protocol output: {self.process.poll()}")
            self.stdout_buffer += data
        line, self.stdout_buffer = self.stdout_buffer.split(b"\n", 1)
        value = json.loads(line)
        if not isinstance(value, dict):
            raise RuntimeError(f"runner response is not a JSON object: {value!r}")
        self.protocol.write(json.dumps({"direction": "response", "wall_monotonic": time.monotonic(),
                                       "value": value}, ensure_ascii=False)+"\n")
        self.protocol.flush()
        if value.get("ok") is False:
            raise RuntimeError(f"runner command failed: {value}")
        return value

    def request(self, command: dict[str, Any], *, delay_s: float = 0) -> dict[str, Any]:
        if delay_s:
            time.sleep(delay_s)
        self.sequence += 1
        request = {"id": self.sequence, **command}
        self.protocol.write(json.dumps({"direction": "request", "wall_monotonic": time.monotonic(),
                                       "value": request}, ensure_ascii=False)+"\n")
        self.protocol.flush()
        self.process.stdin.write((json.dumps(request)+"\n").encode())
        self.process.stdin.flush()
        response = self.receive()
        if response.get("id") != request["id"] or response.get("schema_version") != 1:
            raise RuntimeError(f"runner response ID/schema does not match request: {response}")
        if command.get("cmd", command.get("command")) != "close":
            self.sample()
        return response

    def terminate(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=10)
        self.log.close()
        self.protocol.close()


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2)+"\n")


def read_jsonl_strict(path: Path) -> list[dict[str, Any]]:
    def unique(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f"duplicate JSON key {key}")
            result[key] = value
        return result
    def finite(value: str) -> None:
        raise ValueError(f"non-finite JSON number {value}")
    rows = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip():
            continue
        try:
            value = json.loads(line, object_pairs_hook=unique, parse_constant=finite)
        except ValueError as error:
            raise ValueError(f"{path.name}:{number}: {error}") from error
        if not isinstance(value, dict):
            raise ValueError(f"{path.name}:{number}: JSON object required")
        rows.append(value)
    return rows


def block_animation(row: dict[str, Any]) -> tuple[str, int] | None:
    if (row.get("phase") != "native_attempt" or row.get("accepted") is not True or
        row.get("pose_changed") is not True):
        return None
    # The primitive may be stale on draw/idle/hit-reaction hooks. An explicit
    # requested animation takes precedence; primitive 9 is legacy fallback only.
    if "requested_anim" in row:
        if row["requested_anim"] != 35:
            return None
    elif row.get("primitive") != 9:
        return None
    layers = row.get("layers_after")
    if not isinstance(layers, list) or any(not isinstance(layer, dict) for layer in layers):
        raise RuntimeError(f"invalid layers_after for accepted block at seq {row.get('seq')}")
    matches = []
    for layer in layers:
        animation = layer.get("name", "")
        if not isinstance(animation, str):
            raise RuntimeError(f"invalid animation name for accepted block at seq {row.get('seq')}")
        match = re.fullmatch(r"T_(?:1H|2H)PARADE_0(?:_A([23]))?", animation)
        if match:
            matches.append((animation, {None: 0, "2": 1, "3": 2}[match.group(1)]))
    if len(matches) > 1:
        raise RuntimeError(f"ambiguous accepted block animation at seq {row.get('seq')}")
    if not matches and row.get("requested_anim") == 35:
        raise RuntimeError(f"accepted pose-changing block has no supported parade layer at seq {row.get('seq')}")
    return matches[0] if matches else None


def animation_fixture(trace: Path) -> dict[str, Any]:
    """Extract recorded random animation branches, with no HP/action/outcome fields."""
    rows = read_jsonl_strict(trace)
    entries, active, seen, seeds, last_seq = [], None, set(), [], -1
    for row in rows:
        if row.get("kind") != "body_combat":
            continue
        sequence = row.get("seq")
        if not isinstance(sequence, int) or isinstance(sequence, bool) or sequence <= last_seq:
            raise RuntimeError("archived fixture input has duplicate/regressed sequence")
        last_seq = sequence
        if row.get("phase") == "episode_start":
            if active is not None:
                raise RuntimeError("archived fixture input starts episode before terminal")
            seed = row.get("seed")
            if (not isinstance(seed, int) or isinstance(seed, bool) or not 0 <= seed <= 0xffffffff or
                not isinstance(row.get("t_game"), int) or isinstance(row["t_game"], bool) or row["t_game"] < 0 or
                any(not isinstance(row.get(key), int) or isinstance(row[key], bool) or row[key] <= 0
                    for key in ("actor_id", "target_id")) or row["actor_id"] == row["target_id"]):
                raise RuntimeError("archived fixture input has invalid episode seed/time/actor IDs")
            if seed in seeds:
                raise RuntimeError("archived fixture input repeats episode seed")
            seeds.append(seed)
            active = {"seed": seed, "start": row["t_game"],
                      "roles": {row["actor_id"]: "actor", row["target_id"]: "target"}}
        elif row.get("phase") == "episode_end":
            if active is None:
                raise RuntimeError("archived fixture input has terminal without start")
            active = None
        else:
            branch = block_animation(row)
            if branch is None:
                continue
            if active is None or row["actor_id"] not in active["roles"]:
                raise RuntimeError("archived accepted block has no valid episode role")
            entry = {"seed": active["seed"], "role": active["roles"][row["actor_id"]],
                     "relative_time_ms": row["t_game"]-active["start"], "animation": branch[0],
                     "variant": branch[1], "source_seq": sequence}
            if entry["relative_time_ms"] < 0:
                raise RuntimeError("archived accepted block precedes its episode start")
            key = (entry["seed"], entry["role"], entry["relative_time_ms"])
            if key in seen:
                raise RuntimeError(f"archived fixture has duplicate branch opportunity: {key}")
            seen.add(key)
            entries.append(entry)
    if active is not None or not entries:
        raise RuntimeError("archived fixture input is incomplete or has no accepted random block branches")
    return {"schema_version": 1, "kind": "body_animation_branch_fixture",
            "source_events_sha256": sha256(trace), "entries": entries}


def rng_environment(mode: str, fixture: Path | None = None) -> dict[str, str]:
    environment = dict(os.environ)
    environment["OPENGOTHIC_BODY_ANIMATION_RNG"] = mode
    if fixture is None:
        environment.pop("OPENGOTHIC_BODY_ANIMATION_FIXTURE", None)
    else:
        environment["OPENGOTHIC_BODY_ANIMATION_FIXTURE"] = str(fixture)
    return environment


def animation_control_evidence(trace: Path, mode: str,
                               fixture: dict[str, Any] | None = None) -> dict[str, Any]:
    rows = read_jsonl_strict(trace)
    errors = [row for row in rows if row.get("kind") in ("body_animation_control_error", "body_duel_error")]
    if errors:
        raise RuntimeError(f"animation control emitted error: {errors[0]}")
    summaries = [row for row in rows if row.get("kind") == "body_animation_control_summary"]
    if not summaries or any(row.get("mode") != mode or row.get("status") != "complete" or row.get("errors") not in (0, [])
                            for row in summaries):
        raise RuntimeError(f"missing or incomplete animation control summaries for {mode}")
    accepts = [row for row in rows if row.get("kind") == "body_combat" and row.get("phase") == "animation_branch_accept"]
    draws = [row for row in rows if row.get("kind") == "body_combat" and row.get("phase") == "animation_branch_draw"]
    native = animation_fixture(trace)["entries"]
    if len(accepts) != len(native):
        raise RuntimeError("animation accept events do not match accepted native block animation count")
    native_by_key = {(row["seed"], row["role"], row["relative_time_ms"]): row for row in native}
    seen_native, seen_sources = set(), set()
    expected_by_source = {row["source_seq"]: row for row in fixture["entries"]} if fixture else {}
    for accepted in accepts:
        key = (accepted.get("seed"), accepted.get("role"), accepted.get("relative_time_ms"))
        native_branch = native_by_key.get(key)
        if key in seen_native or native_branch is None:
            raise RuntimeError("duplicate/unexpected accepted eligible block animation")
        seen_native.add(key)
        if (accepted.get("mode") != mode or accepted.get("animation_name") != native_branch["animation"] or
            accepted.get("selected_variant") != native_branch["variant"]):
            raise RuntimeError("animation control accept does not describe actual native pose")
        if not any((row.get("seed"), row.get("role"), row.get("relative_time_ms")) == key and
                   row.get("mode") == mode and row.get("selected_variant") == accepted["selected_variant"]
                   for row in draws):
            raise RuntimeError("accepted block has no matching controlled random branch draw")
        if fixture:
            source = accepted.get("source_seq")
            expected = expected_by_source.get(source)
            if expected is None or source in seen_sources:
                raise RuntimeError("replay accepted block consumed missing/duplicate fixture entry")
            seen_sources.add(source)
            if (expected["seed"] != key[0] or expected["role"] != key[1] or
                expected["relative_time_ms"] != key[2] or
                expected["animation"] != native_branch["animation"] or expected["variant"] != native_branch["variant"]):
                raise RuntimeError("replay accepted animation does not match its recorded fixture branch")
            if not any(row.get("fixture_matched") is True and row.get("source_seq") == source and
                       (row.get("seed"), row.get("role"), row.get("relative_time_ms")) == key for row in draws):
                raise RuntimeError("replay accept lacks matching fixture draw evidence")
    if fixture:
        if seen_sources != expected_by_source.keys():
            raise RuntimeError("replay did not consume every recorded fixture entry exactly once")
        expected_counts = {seed: sum(row["seed"] == seed for row in fixture["entries"])
                           for seed in {row["seed"] for row in fixture["entries"]}}
        for seed, expected in expected_counts.items():
            found = [row for row in summaries if row.get("seed") == seed]
            if len(found) != 1 or found[0].get("expected_entries") != expected or found[0].get("consumed_entries") != expected or found[0].get("remaining_entries") != 0:
                raise RuntimeError(f"replay episode summary does not attest complete consumption for seed {seed}")
        totals = [row for row in rows if row.get("kind") == "body_animation_control_totals"]
        if (len(totals) != 1 or totals[0].get("mode") != mode or totals[0].get("status") != "complete" or
            totals[0].get("expected_entries") != len(expected_by_source) or
            totals[0].get("consumed_entries") != len(expected_by_source) or totals[0].get("remaining_entries") != 0 or
            totals[0].get("errors") not in (0, [])):
            raise RuntimeError("missing/incomplete global replay consumption attestation")
    return {"status": "passed", "mode": mode, "accepted_blocks": len(native),
            "branch_draws": len(draws), "summaries": summaries,
            "fixture_entries": len(expected_by_source), "consumed_source_sequences": sorted(seen_sources),
            "controlled_scope": "only random parade animation branch; native choices, movement, HP and outcomes execute normally"}


def trace_initial_state(path: Path) -> list[dict[str, Any]]:
    """First setup + first actor snapshots, with lifetime/time counters normalized.

    They remain proof of the exposed trace state only; this is not a deep memory
    equality claim about unexposed future controller/lease state.
    """
    states: dict[tuple[str, str], dict[str, Any]] = {}
    for line in path.read_text().splitlines():
        row = json.loads(line)
        if row.get("kind") != "body_combat" or row.get("phase") not in ("actor_setup", "phase_snapshot"):
            continue
        key = row["actor"], row["phase"]
        if key in states:
            continue
        states[key] = {name: value for name, value in row.items()
                       if name not in ("seq", "t_game", "step", "actor_id", "target_id", "seg", "choice_id")}
    return [states[key] for key in sorted(states)]


def mode_series(args: argparse.Namespace, manifest: dict[str, Any], mode: str,
                name: str, delay_s: float = 0, scenario: Path | None = None,
                animation_rng: str = "isolated", fixture: Path | None = None) -> dict[str, Any]:
    scenario = scenario or args.scenario
    output = args.output / name
    output.mkdir()
    workdir = output / "run"
    workdir.mkdir()
    # Same settings bytes as reference; no renderer/audio path consumes them.
    (workdir / "Gothic.ini").write_bytes((args.reference / "run/Gothic.ini").read_bytes())
    runner = Runner([str(args.runner), "-g", str(args.assets), "-g2", "-devmode", "--save", str(args.save),
                     "--scenario", str(scenario), "--output", str(output),
                     "--mode", mode, "--stdio"], workdir, output,
                    environment=rng_environment(animation_rng, fixture))
    start = time.monotonic()
    requests, steps, step_wall_s = 0, 0, 0.0
    try:
        reset = runner.request({"cmd": "reset", "scenario": str(scenario),
                                "output_dir": str(output), "mode": mode})
        if not reset.get("ok") or reset.get("done"):
            raise RuntimeError(f"reset did not create an active scenario: {reset}")
        before = runner.request({"cmd": "status"})
        time.sleep(.2)
        after = runner.request({"cmd": "status"})
        if mode == "lockstep" and (before["game_ms"], before["sim_step"]) != (after["game_ms"], after["sim_step"]):
            raise RuntimeError("lockstep game time advanced while waiting for a command")
        while True:
            step_started = time.monotonic()
            response = runner.request({"cmd": "advance", "substeps": 60}, delay_s=delay_s)
            step_wall_s += time.monotonic()-step_started-delay_s
            requests += 1
            steps += response.get("steps_advanced", 0)
            if response.get("done"):
                break
            if requests > 1000:
                raise RuntimeError("scenario exhausted bounded command budget; infrastructure failure")
        final = runner.request({"cmd": "status"})
        runner.request({"cmd": "close"})
        runner.process.wait(timeout=15)
        if runner.process.returncode:
            raise RuntimeError(f"runner failed on close: {runner.process.returncode}")
        result = {"mode": mode, "delay_per_advance_s": delay_s, "status": "completed",
                  "reset": reset, "final": final, "advance_requests": requests,
                  "substeps": final["sim_step"], "advance_request_substeps": steps,
                  "wall_s": time.monotonic()-start,
                  "advance_processing_wall_s": step_wall_s,
                  "substeps_per_processing_s": steps/step_wall_s if step_wall_s and mode == "lockstep" else None,
                  "resources": runner.samples}
        engine_summary = output / "runner-summary.json"
        if engine_summary.is_file():
            result["engine_summary"] = json.loads(engine_summary.read_text())
        candidate_manifest = dict(manifest)
        candidate_manifest.update({"mode": mode, "realtime": mode == "realtime", "run": result,
                                   "scenario_sha256": sha256(scenario), "animation_rng_mode": animation_rng,
                                   "animation_branch_fixture": {"path": str(fixture), "sha256": sha256(fixture)} if fixture else None,
                                   "comparison_scope": "controlled archived animation branch replay" if fixture else "isolated per-actor animation RNG mechanics"})
        # Flags are evidence from the runner summary plus /proc: missing engine
        # attestation is a failed gate, never silently supplied by the driver.
        attestation = result.get("engine_summary", {})
        for key in ("graphics_device_created", "window_created", "audio_device_created"):
            if attestation.get(key) is not False:
                raise RuntimeError(f"runner summary lacks no-device attestation: {key}")
            candidate_manifest[key] = attestation[key]
        write_json(output / "manifest.json", candidate_manifest)
        return result
    finally:
        runner.terminate()


def descendant_processes(pid: int) -> set[int]:
    """A rendered probe may have time/xvfb-run wrappers; inspect only its tree."""
    result, pending = {pid}, [pid]
    while pending:
        current = pending.pop()
        try:
            children = Path(f"/proc/{current}/task/{current}/children").read_text().split()
        except FileNotFoundError:
            continue
        for child in map(int, children):
            if child not in result:
                result.add(child)
                pending.append(child)
    return result


def rendered_series(args: argparse.Namespace, manifest: dict[str, Any]) -> dict[str, Any]:
    """Actual same-source rendered run, sequential and pinned to CPU lavapipe.

    This is additional diagnostic evidence. The stage 1 archived trace is never
    rewritten or substituted by this probe.
    """
    output = args.output / "rendered"
    output.mkdir()
    workdir = output / "run"
    workdir.mkdir()
    (workdir / "Gothic.ini").write_bytes((args.reference / "run/Gothic.ini").read_bytes())
    (workdir / "save_slot_1.sav").write_bytes(args.save.read_bytes())
    environment = rng_environment("isolated")
    environment.pop("DISPLAY", None)
    environment.pop("WAYLAND_DISPLAY", None)
    environment.update({"VK_DRIVER_FILES": "/usr/share/vulkan/icd.d/lvp_icd.json",
                        "VK_ICD_FILENAMES": "/usr/share/vulkan/icd.d/lvp_icd.json",
                        "LP_NUM_THREADS": "2", "ALSOFT_DRIVERS": "null", "GOTHIC_AI": "0",
                        "TEMPEST_BACKGROUND": "1", "TEMPEST_WINDOW_SIZE": "640x480",
                        "OPENGOTHIC_TEXTURES": "", "OPENGOTHIC_BODY_TRACE": "1",
                        "OPENGOTHIC_BENCH_REALTIME": "1",
                        "OPENGOTHIC_BENCH": str(args.scenario), "OPENGOTHIC_BENCH_OUT": str(output)})
    argv = ["/usr/bin/time", "-v", "-o", str(output / "resources.txt"),
            "/usr/bin/xvfb-run", "-a", "-s", "-screen 0 640x480x24",
            str(args.rendered), "-g", str(args.assets), "-g2", "-window", "-devmode", "-save", "1"]
    subprocess.run(["bash", str(Path(__file__).with_name("base-ensure.sh")), str(args.assets)],
                   env=dict(environment, GAME_BIN=str(args.rendered)), check=True)
    started = time.monotonic()
    samples = []
    with (output / "launcher.log").open("wb") as log:
        process = subprocess.Popen(argv, cwd=workdir, env=environment,
                                    stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            while process.poll() is None:
                if time.monotonic()-started > 900:
                    raise RuntimeError("rendered probe wall watchdog expired; infrastructure failure")
                text = (output / "launcher.log").read_text(errors="replace")
                bench_log = output / "bench.log"
                bench_text = bench_log.read_text(errors="replace") if bench_log.is_file() else ""
                for error in ("loading error:", "unable to load landscape mesh", "---crashlog(",
                              "Fatal glibc", "bodyduel: setup_failed", "bodyprobe: failed"):
                    if error in text or error in bench_text:
                        raise RuntimeError(f"rendered probe failed: {error}")
                for child in descendant_processes(process.pid):
                    try:
                        cmdline = Path(f"/proc/{child}/cmdline").read_bytes().split(b"\0")
                        if cmdline and Path(os.fsdecode(cmdline[0])).name == "Gothic2Notr":
                            sample = process_sample(child)
                            samples.append(sample)
                            if sample["forbidden_device_descriptors"]:
                                raise RuntimeError("CPU rendered probe opened GPU/sound hardware device")
                    except FileNotFoundError:
                        pass
                time.sleep(1)
            if process.returncode:
                raise RuntimeError(f"rendered probe exited {process.returncode}")
        finally:
            if process.poll() is None:
                os.killpg(process.pid, signal.SIGTERM)
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=10)
    bench_text = (output / "bench.log").read_text()
    if "done" not in bench_text.splitlines():
        raise RuntimeError("rendered probe lacks completed bench evidence")
    game_log = workdir / "log.txt"
    if game_log.is_file():
        (output / "game.log").write_bytes(game_log.read_bytes())
    combined = (output / "launcher.log").read_text(errors="replace")
    if game_log.is_file():
        combined += game_log.read_text(errors="replace")
    if "GPU = llvmpipe" not in combined:
        raise RuntimeError("rendered probe did not attest llvmpipe CPU renderer")
    if not samples:
        raise RuntimeError("rendered probe process was not observed for resource/device verification")
    result = {"status": "completed", "wall_s": time.monotonic()-started, "argv": argv,
              "resources": samples, "renderer_gpu_vram_mib": 0,
              "resource_report": (output / "resources.txt").read_text(),
              "renderer_evidence": "GPU = llvmpipe in actual engine log, CPU-only ICD, no hardware descriptors",
              "audio_driver": "OpenAL null"}
    candidate_manifest = dict(manifest)
    candidate_manifest.update({"kind": "body_stage2_same_source_rendered_probe", "headless": False,
                               "binary_sha256": sha256(args.rendered), "renderer": "lavapipe",
                               "display": "Xvfb", "graphics_device_created": True,
                               "window_created": True, "audio_device_created": True,
                               "audio_playback": False, "realtime": True, "run": result,
                               "animation_rng_mode": "isolated", "animation_branch_fixture": None,
                               "comparison_scope": "same-source rendered/headless with isolated per-actor animation RNG",
                               "environment": {key: environment.get(key) for key in
                                               ("VK_DRIVER_FILES", "VK_ICD_FILENAMES", "LP_NUM_THREADS",
                                                "ALSOFT_DRIVERS", "GOTHIC_AI")}})
    write_json(output / "manifest.json", candidate_manifest)
    return result


def senses_evidence(path: Path) -> dict[str, Any]:
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    errors = [row for row in rows if row.get("kind") in ("body_probe_error", "body_duel_error")]
    if errors:
        raise RuntimeError(f"senses probe emitted infrastructure error: {errors[0]}")
    result = {}
    for case in ("visible", "occluded"):
        events = {phase: [row for row in rows if row.get("kind") == "body_combat" and
                         row.get("phase") == phase and row.get("case") == case]
                  for phase in ("senses_vision", "senses_sound_start", "senses_sound_end")}
        if any(len(found) != 1 for found in events.values()):
            raise RuntimeError(f"senses probe lacks exactly one complete event trio for {case}")
        vision, start, end = (events[phase][0] for phase in events)
        visible = case == "visible"
        if (vision.get("can_see_npc_free_los") is not visible or
            vision.get("center_in_range") is not True or vision.get("head_in_range") is not True or
            vision.get("center_ray_hit") is not (not visible) or vision.get("head_ray_hit") is not (not visible)):
            raise RuntimeError(f"senses probe vision/range/ray invariant failed: {case}")
        if visible and vision.get("process_policy") != "normal":
            raise RuntimeError("senses probe native actor was not AiNormal")
        if not visible and any(not .01 < vision[field] < .99 for field in ("center_hit_fraction", "head_hit_fraction")):
            raise RuntimeError("occlusion was not a genuine world ray intersection")
        duration = start.get("duration_ms", 0)
        if (not 0 < duration <= 10000 or start.get("finished") is not False or
            start.get("source_visible_before") is not False or start.get("source_visible") is not visible or
            start.get("source_ray_hit") is not (not visible) or start.get("clock") != "simulation" or
            end.get("duration_ms") != duration or end.get("elapsed_ms", 0) < duration or
            end.get("finished") is not True or end.get("source_visible_after_cleanup") is not False):
            raise RuntimeError(f"senses probe sound duration/lifecycle invariant failed: {case}")
        result[case] = {"vision": vision, "sound_start": start, "sound_end": end}
    return {"status": "passed", "cases": result,
            "scope": "native sight/range/occlusion and 3D sound lifetime/source perceptions using CPU world geometry"}


def native_perception_evidence(path: Path) -> dict[str, Any]:
    actors, periodic, callbacks = set(), {}, 0
    for row in read_jsonl_strict(path):
        if row.get("kind") != "body_combat":
            continue
        if row.get("phase") == "actor_setup":
            actors.add(row["actor_id"])
        if row.get("phase") != "native_perception" or row.get("actor_id") not in actors:
            continue
        if row.get("process_policy") != 1 or row.get("gate") == "policy_disabled":
            raise RuntimeError("arena participant lost native AiNormal perception processing")
        if row.get("perception_kind") == "periodic" and row.get("gate") == "normal":
            actor = row["actor_id"]
            periodic[actor] = periodic.get(actor, 0)+1
        if row.get("native_callback_dispatched") is True:
            callbacks += 1
    if not actors or actors != periodic.keys() or not callbacks:
        raise RuntimeError("missing native periodic perceptions or actual callback dispatch evidence")
    return {"status": "passed", "periodic_by_actor": periodic, "native_callback_dispatches": callbacks}


def reset_series(args: argparse.Namespace) -> dict[str, Any]:
    output = args.output / "resets"
    output.mkdir()
    workdir = output / "run"
    workdir.mkdir()
    (workdir / "Gothic.ini").write_bytes((args.reference / "run/Gothic.ini").read_bytes())
    runner = Runner([str(args.runner), "-g", str(args.assets), "-g2", "-devmode", "--save", str(args.save),
                     "--scenario", str(args.scenario), "--output", str(output / "initial"),
                     "--mode", "lockstep", "--stdio"], workdir, output,
                    environment=rng_environment("isolated"))
    resets, latencies, reference_state, generations = [], [], None, set()
    try:
        for index in range(100):
            episode = output / f"episode-{index:03d}"
            episode.mkdir()
            started = time.monotonic()
            response = runner.request({"cmd": "reset", "scenario": str(args.scenario),
                                       "seed": 17, "output_dir": str(episode), "mode": "lockstep"})
            latencies.append((time.monotonic()-started)*1000)
            generation = response["generation"]
            if generation in generations:
                raise RuntimeError("reset reused control generation")
            generations.add(generation)
            # Setup commands include the same 500ms wait as immutable reference.
            advanced = runner.request({"cmd": "advance", "substeps": 30})
            if advanced.get("done"):
                raise RuntimeError("short reset episode completed unexpectedly")
            state = trace_initial_state(episode / "events.jsonl")
            if not state or not any(row["phase"] == "actor_setup" for row in state):
                raise RuntimeError("reset did not recreate combat actors")
            if reference_state is None:
                reference_state = state
            if state != reference_state:
                raise RuntimeError(f"reset {index} changed initial exposed actor/animation state")
            sample = runner.samples[-1]
            resets.append({"index": index, "generation": generation,
                           "game_ms": advanced["game_ms"], "sim_step": advanced["sim_step"],
                           "state_sha256": hashlib.sha256(json.dumps(state, sort_keys=True).encode()).hexdigest(),
                           "resources": sample})
        runner.request({"cmd": "close"})
        runner.process.wait(timeout=15)
        if runner.process.returncode:
            raise RuntimeError(f"reset runner failed on close: {runner.process.returncode}")
        # First ten reloads are the explicit warm-up interval. Thresholds are
        # conservative stage 2 operational gates, not fitted to observed values.
        warmed = [row["resources"] for row in resets[10:]]
        initial_rss = statistics.median(row["rss_kib"] for row in warmed[:10])
        final_rss = statistics.median(row["rss_kib"] for row in warmed[-10:])
        rss_limit = max(64*1024, initial_rss*.05)
        fd_span = max(row["file_descriptors"] for row in warmed)-min(row["file_descriptors"] for row in warmed)
        threads_span = max(row["threads"] for row in warmed)-min(row["threads"] for row in warmed)
        issues = []
        if final_rss-initial_rss > rss_limit:
            issues.append("warmed RSS growth exceeds max(64MiB,5% initial RSS)")
        if fd_span > 4:
            issues.append("warmed file descriptor span exceeds 4")
        if threads_span > 2:
            issues.append("warmed thread count span exceeds 2")
        return {"status": "passed" if not issues else "failed", "count": len(resets),
                "issues": issues, "latency_ms": distribution(latencies),
                "warmed_initial_rss_kib": initial_rss, "warmed_final_rss_kib": final_rss,
                "rss_growth_limit_kib": rss_limit, "file_descriptor_span": fd_span,
                "threads_span": threads_span, "initial_state": reference_state,
                "episodes": resets}
    finally:
        runner.terminate()


def run_report(argv: list[str], report_path: Path) -> dict[str, Any]:
    completed = subprocess.run(argv, check=False, timeout=180)
    report = json.loads(report_path.read_text()) if report_path.is_file() else None
    return {"exit_code": completed.returncode, "report": report,
            "report_path": str(report_path), "argv": argv}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--rendered", type=Path, required=True)
    parser.add_argument("--assets", type=Path, required=True)
    parser.add_argument("--save", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--engine-library", type=Path)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--scenario", type=Path)
    parser.add_argument("--senses-scenario", type=Path)
    parser.add_argument("--replay-only", action="store_true",
                        help="bounded archival replay smoke only; never claims full stage 2 readiness")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("game simulations run only on dev-ubuntu (Linux), never the Mac")
    if args.scenario is None:
        args.scenario = args.source.parent / "scripts/bench/body-melee.bench"
    if args.senses_scenario is None:
        args.senses_scenario = args.source.parent / "scripts/bench/body-headless-senses.bench"
    for key in ("runner", "rendered", "assets", "save", "reference", "source", "config", "scenario", "senses_scenario", "output"):
        setattr(args, key, getattr(args, key).resolve())
    args.output.mkdir(exist_ok=True)
    scripts = Path(__file__).resolve().parent
    result: dict[str, Any] = {"schema_version": 1, "status": "infrastructure_failure",
                              "required_gate_failures": [], "comparisons": {},
                              "historical_diagnostics": {}}
    failures: list[str] = result["required_gate_failures"]
    def capture(label: str, operation: Any, *, required: bool = True) -> Any:
        try:
            value = operation()
            result[label] = value
            return value
        except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
            result[label] = {"status": "infrastructure_failure", "error": str(error)}
            if required:
                failures.append(f"{label}: {error}")
            print(f"body headless suite evidence failed [{label}]: {error}", file=sys.stderr)
            return None
    def report(label: str, argv: list[str], path: Path, *, diagnostic: bool = False,
               full_rendered: bool = False) -> dict[str, Any] | None:
        evidence = capture(label, lambda: run_report(argv, path))
        if evidence is None:
            return None
        result["historical_diagnostics" if diagnostic else "comparisons"][label] = evidence
        parsed = evidence["report"]
        expected_status = "complete" if Path(argv[1]).name == "body_baseline_report.py" else "matched"
        valid = evidence["exit_code"] == 0 and parsed is not None and parsed.get("status") == expected_status
        if diagnostic:
            # Ordinary archival drift remains visible, never silently ignored.
            # Only complete comparator matched/diverged outputs are diagnostics;
            # absent/incomplete evidence is still a failed required gate.
            valid = evidence["exit_code"] in (0, 2) and parsed is not None and parsed.get("status") in ("matched", "diverged")
        elif full_rendered:
            valid = valid and parsed.get("comparison_kind") == "rendered_vs_headless" and parsed.get("full_stage2_evidence") is True
        if not valid:
            failures.append(f"{label}: report gate failed (exit {evidence['exit_code']}, status {parsed.get('status') if parsed else 'missing'})")
        return evidence
    try:
        manifest = provenance(args)
        if sha256(args.scenario) != manifest["scenario_sha256"]:
            raise RuntimeError("scenario bytes differ from archived baseline")
        if sha256(args.reference / "run/Gothic.ini") != manifest["settings_sha256"]:
            raise RuntimeError("archived reference settings changed")
        fixture = animation_fixture(args.reference / "events.jsonl")
        fixture_path = args.output / "archived-animation-branches.json"
        write_json(fixture_path, fixture)
        manifest.update({"animation_rng_mode": "isolated", "historical_reference_mode": "legacy shared std::rand",
                         "animation_branch_fixture": {"path": str(fixture_path), "sha256": sha256(fixture_path),
                                                      "source_events_sha256": fixture["source_events_sha256"],
                                                      "entries": len(fixture["entries"])},
                         "comparison_policy": {"isolated_rendered_vs_headless": "required full parity",
                                               "ordinary_historical_vs_isolated": "required diagnostic retained, drift allowed and reported",
                                               "historical_vs_branch_replay": "required archived_reference parity plus exact once fixture consumption"}})
        write_json(args.output / "manifest.json", manifest)
        if args.replay_only:
            capture("archived_replay", lambda: mode_series(args, manifest, "lockstep", "archived-replay",
                                                            animation_rng="replay", fixture=fixture_path))
            replay = args.output / "archived-replay"
            report("replay_completeness", [sys.executable, str(scripts / "body_baseline_report.py"), str(replay),
                                           "--manifest", str(replay / "manifest.json"), "--config", str(args.config),
                                           "--output-dir", str(replay / "baseline-report")], replay / "baseline-report/body-baseline.json")
            parity_output = replay / "historical-replay-parity"
            report("historical_vs_branch_replay", [sys.executable, str(scripts / "body_headless_parity.py"), str(args.reference),
                                                   str(replay), "--profile", "archived_reference", "--output-dir", str(parity_output)],
                   parity_output / "headless-parity.json")
            control = capture("replay_animation_control", lambda: animation_control_evidence(replay / "events.jsonl", "replay", fixture))
            if control:
                write_json(args.output / "replay-evidence.json", control)
            result.update({"kind": "archived_animation_replay_smoke", "status": "failed" if failures else "passed",
                           "full_stage2_evidence": False, "services_after": service_snapshot()})
            manifest.update({"suite_status": result["status"], "suite_scope": "archived replay smoke only",
                             "completed_at_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
                             "services_after": result["services_after"]})
            write_json(args.output / "manifest.json", manifest)
            print(f"body archived replay smoke {result['status']}")
            return 2 if failures else 0
        capture("rendered", lambda: rendered_series(args, manifest))
        capture("lockstep", lambda: mode_series(args, manifest, "lockstep", "lockstep"))
        capture("lockstep_delayed", lambda: mode_series(args, manifest, "lockstep", "lockstep-delayed", delay_s=.2))
        capture("realtime", lambda: mode_series(args, manifest, "realtime", "realtime"))
        capture("archived_replay", lambda: mode_series(args, manifest, "lockstep", "archived-replay",
                                                        animation_rng="replay", fixture=fixture_path))
        for name in ("rendered", "lockstep", "lockstep-delayed", "realtime"):
            candidate = args.output / name
            report(f"{name}_completeness", [sys.executable, str(scripts / "body_baseline_report.py"), str(candidate),
                                           "--manifest", str(candidate / "manifest.json"),
                                           "--config", str(args.config), "--output-dir", str(candidate / "baseline-report")],
                   candidate / "baseline-report/body-baseline.json")
            capture(f"{name}_animation_control", lambda candidate=candidate: animation_control_evidence(candidate / "events.jsonl", "isolated"))
            capture(f"{name}_native_perceptions", lambda candidate=candidate: native_perception_evidence(candidate / "events.jsonl"))
        # Full graphics-removal evidence is an actual rendered/headless pair
        # using the same compiled engine sources. Invariance alone cannot pass.
        for name in ("lockstep", "lockstep-delayed", "realtime"):
            candidate = args.output / name
            full_output = candidate / "rendered-vs-headless"
            report(f"rendered_vs_{name}", [sys.executable, str(scripts / "body_headless_parity.py"), str(args.output / "rendered"),
                                          str(candidate), "--profile", "full", "--output-dir", str(full_output)],
                   full_output / "headless-parity.json", full_rendered=True)
            historical = candidate / "historical-diagnostic"
            report(f"historical_vs_isolated_{name}", [sys.executable, str(scripts / "body_headless_parity.py"), str(args.reference),
                                                       str(candidate), "--profile", "archived_reference", "--output-dir", str(historical)],
                   historical / "headless-parity.json", diagnostic=True)
        for name in ("lockstep-delayed", "realtime"):
            parity_output = args.output / name / "lockstep-parity"
            report(f"lockstep_vs_{name}", [sys.executable, str(scripts / "body_headless_parity.py"), str(args.output / "lockstep"),
                                           str(args.output / name), "--profile", "full", "--output-dir", str(parity_output)],
                   parity_output / "headless-parity.json")
        replay = args.output / "archived-replay"
        report("replay_completeness", [sys.executable, str(scripts / "body_baseline_report.py"), str(replay),
                                       "--manifest", str(replay / "manifest.json"), "--config", str(args.config),
                                       "--output-dir", str(replay / "baseline-report")], replay / "baseline-report/body-baseline.json")
        replay_parity = replay / "historical-replay-parity"
        report("historical_vs_branch_replay", [sys.executable, str(scripts / "body_headless_parity.py"), str(args.reference),
                                               str(replay), "--profile", "archived_reference", "--output-dir", str(replay_parity)],
               replay_parity / "headless-parity.json")
        replay_control = capture("replay_animation_control", lambda: animation_control_evidence(replay / "events.jsonl", "replay", fixture))
        if replay_control:
            write_json(args.output / "replay-evidence.json", replay_control)
        capture("senses_run", lambda: mode_series(args, manifest, "lockstep", "senses", scenario=args.senses_scenario))
        senses = capture("senses", lambda: senses_evidence(args.output / "senses/events.jsonl"))
        if senses:
            write_json(args.output / "senses-evidence.json", senses)
        resets = capture("resets", lambda: reset_series(args))
        if resets:
            write_json(args.output / "resets.json", resets)
            if resets["issues"]:
                failures.append(f"reset stability gate failed: {resets['issues']}")
        def ipc_probe() -> dict[str, Any]:
            output = args.output / "ipc-probe"
            argv = [sys.executable, str(scripts / "body_headless_ipc_probe.py"),
                    "--runner", str(args.runner), "--assets", str(args.assets), "--save", str(args.save),
                    "--scenario", str(args.scenario), "--reference", str(args.reference), "--output", str(output)]
            if args.engine_library:
                argv += ["--engine-library", str(args.engine_library),
                         "--expected-library-sha256", sha256(args.engine_library)]
            completed = subprocess.run(argv, check=False, timeout=180)
            evidence = json.loads((output / "ipc-probe.json").read_text())
            if completed.returncode or evidence.get("status") != "passed":
                raise RuntimeError(f"IPC probe failed: {evidence.get('error', completed.returncode)}")
            return evidence
        capture("ipc", ipc_probe)
        result.update({"status": "failed" if failures else "passed", "full_stage2_evidence": not failures,
                       "services_after": service_snapshot(),
                       "limitations": ["ASR/TTS latency was not measured; service state and shared GPU usage only",
                                       "native decisions only: external body actions belong to stage 4",
                                       "reset equality covers exposed trace state, not future hidden neural state",
                                       "historical ordinary RNG drift is diagnostic; separate controlled animation branch replay verifies archival mechanics"]})
        manifest["completed_at_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        manifest["services_after"] = result["services_after"]
        manifest["suite_status"] = result["status"]
        write_json(args.output / "manifest.json", manifest)
        print(f"body headless suite {result['status']}; required gate failures: {len(failures)}")
        return 2 if failures else 0
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(f"body headless suite failed: {error}", file=sys.stderr)
        return 2
    finally:
        write_json(args.output / "suite.json", result)


if __name__ == "__main__":
    raise SystemExit(main())
