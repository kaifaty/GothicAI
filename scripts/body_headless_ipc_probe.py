#!/usr/bin/env python3
"""Bounded JSONL IPC probe against the real Linux headless runner.

Run on dev-ubuntu only. This probe checks input scheduling; ordered combat
parity remains a separate required gate against the recorded rendered trace.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from typing import Any

import body_headless_suite as suite


def check(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def next_command(runner: suite.Runner, command: dict[str, Any]) -> dict[str, Any]:
    runner.sequence += 1
    return {"id": runner.sequence, **command}


def write_fragment(runner: suite.Runner, data: bytes, label: str) -> None:
    runner.protocol.write(json.dumps({"direction": "request_fragment", "label": label,
                                     "wall_monotonic": time.monotonic(),
                                     "bytes": data.decode("utf-8")})+"\n")
    runner.protocol.flush()
    runner.process.stdin.write(data)
    runner.process.stdin.flush()


def response(runner: suite.Runner, request: dict[str, Any]) -> dict[str, Any]:
    value = runner.receive()
    check(value.get("id") == request["id"] and value.get("schema_version") == 1,
          f"response ID/schema mismatch: {value}")
    check(value.get("ok") is True, f"command failed: {value}")
    runner.sample()
    return value


def assert_headless(value: dict[str, Any]) -> None:
    for field in ("graphics_device_created", "window_created", "audio_device_created"):
        check(value.get(field) is False, f"runner lacks no-device attestation: {field}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("runner", "assets", "save", "scenario", "reference", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--engine-library", type=Path,
                        help="record reused engine library hash; entry-point fix must not rebuild it")
    parser.add_argument("--expected-library-sha256",
                        help="reject if the reused engine library changed since full mechanics evidence")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("native simulations run only on Linux dev-ubuntu, never the Mac")
    for name in ("runner", "assets", "save", "scenario", "reference", "output", "engine_library"):
        value = getattr(args, name)
        if value is not None:
            setattr(args, name, value.resolve())
    args.output.mkdir(parents=True, exist_ok=True)
    result: dict[str, Any] = {"schema_version": 1, "kind": "body_headless_ipc_probe", "status": "failed",
                              "scope": "input scheduling and command lifecycle; not full mechanics parity",
                              "checks": {}, "partial_line_wait_ms": 250,
                              "response_wall_timeout_s": 5}
    runner = None
    try:
        settings = args.reference / "run/Gothic.ini"
        result["provenance"] = {"runner_sha256": suite.sha256(args.runner),
                                "save_sha256": suite.sha256(args.save),
                                "scenario_sha256": suite.sha256(args.scenario),
                                "settings_sha256": suite.sha256(settings),
                                "probe_sha256": suite.sha256(Path(__file__).resolve())}
        if args.expected_library_sha256 and args.engine_library is None:
            raise RuntimeError("--expected-library-sha256 requires --engine-library")
        if args.engine_library is not None:
            digest = suite.sha256(args.engine_library)
            result["provenance"]["engine_library_sha256"] = digest
            if args.expected_library_sha256:
                check(digest == args.expected_library_sha256,
                      "engine library differs from the recorded full mechanics build")
        workdir = args.output / "run"
        workdir.mkdir()
        (workdir / "Gothic.ini").write_bytes(settings.read_bytes())
        lockstep = args.output / "lockstep"
        realtime = args.output / "realtime"
        lockstep.mkdir()
        realtime.mkdir()
        environment = suite.rng_environment("isolated")
        environment.update({"DISPLAY": "", "WAYLAND_DISPLAY": "", "GOTHIC_AI": "0",
                            "VK_DRIVER_FILES": "/nonexistent/body-headless-no-vulkan.json",
                            "VK_ICD_FILENAMES": "/nonexistent/body-headless-no-vulkan.json",
                            "OPENGOTHIC_TEXTURES": "", "OPENGOTHIC_BODY_TRACE": "1"})
        result["environment"] = {name: environment[name] for name in
                                 ("DISPLAY", "WAYLAND_DISPLAY", "GOTHIC_AI", "VK_DRIVER_FILES",
                                  "VK_ICD_FILENAMES", "OPENGOTHIC_BODY_ANIMATION_RNG")}
        runner = suite.Runner([str(args.runner), "-g", str(args.assets), "-devmode",
                              "--save", str(args.save), "--scenario", str(args.scenario),
                              "--output", str(lockstep), "--mode", "lockstep", "--stdio"],
                             workdir, args.output, timeout_s=5, environment=environment)
        # Reloads may exceed the IPC response budget; scheduling assertions after
        # reset use the explicit five-second watchdog.
        runner.timeout_s = 180
        initial = runner.request({"cmd": "reset", "seed": 17,
                                  "output_dir": str(lockstep), "mode": "lockstep"})
        runner.timeout_s = 5
        assert_headless(initial)
        before = runner.request({"cmd": "status"})
        time.sleep(.25)
        after = runner.request({"cmd": "status"})
        check((before["game_ms"], before["sim_step"]) == (after["game_ms"], after["sim_step"]),
              "lockstep advanced while waiting for input")
        result["checks"]["lockstep_freeze"] = {"status": "passed", "before": before, "after": after}
        advanced = runner.request({"cmd": "advance", "substeps": 600, "until_decision": True})
        check(advanced.get("stopped_on") == "native_decision" and
              advanced.get("native_choices", 0) > before["native_choices"] and
              0 < advanced.get("steps_advanced", 0) <= 600 and advanced.get("done") is False,
              f"advance did not stop at a real native decision: {advanced}")
        result["checks"]["advance_until_decision"] = {"status": "passed", "response": advanced}

        runner.timeout_s = 180
        reset = runner.request({"cmd": "reset", "seed": 17,
                                "output_dir": str(realtime), "mode": "realtime"})
        runner.timeout_s = 5
        assert_headless(reset)
        check(reset["generation"] > initial["generation"], "reset reused the previous generation")
        before = runner.request({"cmd": "status"})
        partial = next_command(runner, {"cmd": "status"})
        encoded = (json.dumps(partial)+"\n").encode()
        split = len(encoded)//2
        write_fragment(runner, encoded[:split], "partial JSONL frame without newline")
        started = time.monotonic()
        time.sleep(.25)
        runner.sample()
        write_fragment(runner, encoded[split:], "finish partial JSONL frame")
        after = response(runner, partial)
        delta_ms = after["game_ms"]-before["game_ms"]
        delta_steps = after["sim_step"]-before["sim_step"]
        # Three fixed steps distinguish continuing simulation from the one due
        # tick before the old blocking getline entered its partial-line wait.
        check(delta_ms >= 100 and delta_steps >= 3,
              f"partial input stalled realtime clock: {delta_ms} ms / {delta_steps} steps")
        result["checks"]["realtime_partial_line"] = {"status": "passed", "before": before, "after": after,
                                                       "game_ms_advanced": delta_ms,
                                                       "substeps_advanced": delta_steps,
                                                       "observed_wall_ms": (time.monotonic()-started)*1000}

        first = next_command(runner, {"cmd": "status"})
        second = next_command(runner, {"cmd": "status"})
        started = time.monotonic()
        write_fragment(runner, (json.dumps(first)+"\n"+json.dumps(second)+"\n").encode(),
                       "two complete pipelined status frames in one write")
        replies = [response(runner, first), response(runner, second)]
        wall_ms = (time.monotonic()-started)*1000
        check(wall_ms < 5000, "pipelined replies exceeded the bounded response deadline")
        check(replies[1]["sim_step"] >= replies[0]["sim_step"], "pipelined status clock regressed")
        for value in replies:
            assert_headless(value)
        result["checks"]["pipelined_status"] = {"status": "passed", "responses": replies,
                                                 "observed_wall_ms": wall_ms}
        closing = runner.request({"cmd": "close"})
        runner.process.wait(timeout=15)
        check(runner.process.returncode == 0, f"runner failed clean close: {runner.process.returncode}")
        result["checks"]["clean_close"] = {"status": "passed", "response": closing, "exit_code": 0}
        result["status"] = "passed"
        return 0
    except (OSError, ValueError, RuntimeError, KeyError, TypeError, subprocess.SubprocessError) as error:
        result["error"] = str(error)
        print(f"body headless IPC probe failed: {error}", file=sys.stderr)
        return 2
    finally:
        if runner is not None:
            result["resources"] = runner.samples
            runner.terminate()
        suite.write_json(args.output / "ipc-probe.json", result)
        print(f"body headless IPC probe {result['status']}: {args.output / 'ipc-probe.json'}")


if __name__ == "__main__":
    raise SystemExit(main())
