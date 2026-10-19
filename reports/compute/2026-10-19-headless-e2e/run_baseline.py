#!/usr/bin/env python3
"""Run under gq quiet: run_baseline.py BINARY GAME_DIR SAVE BENCH OUTPUT."""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

binary, game, save, bench, output = map(lambda x: str(Path(x).resolve()), sys.argv[1:])
out = Path(output)
out.mkdir(parents=True, exist_ok=True)
env = dict(os.environ, OPENGOTHIC_BODY_TRACE="0", GOTHIC_AI="0")
with (out / "runner.log").open("w", buffering=1) as log:
    started = time.monotonic()
    proc = subprocess.Popen([
        binary, "-g", game, "--save", save, "--scenario", bench,
        "--output", output, "--mode", "realtime", "--stdio",
    ], cwd=output, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=log, text=True, bufsize=1)

    def rpc(command):
        proc.stdin.write(json.dumps({"cmd": command}) + "\n")
        proc.stdin.flush()
        line = proc.stdout.readline()
        if not line:
            raise RuntimeError(f"runner exited before {command}: {proc.poll()}")
        response = json.loads(line)
        if not response.get("ok"):
            raise RuntimeError(response)
        return response

    try:
        first = rpc("reset")
        loaded = time.monotonic()
        log.write('headless_measure_loaded\n')
        while True:
            time.sleep(1)
            last = rpc("status")
            if last["done"]:
                break
            if time.monotonic() - loaded > 90:
                raise RuntimeError("60 second idle bench did not finish within 90s")
        result = {key: last[key] for key in (
            "ok", "loaded", "done", "simulated_ms", "reset_wall_ms",
            "elapsed_wall_ms", "max_realtime_lag_ms", "graphics_device_created",
            "window_created", "audio_device_created", "worker_execution",
        )}
        result.update(pid=proc.pid, started_mono_s=started, loaded_mono_s=loaded,
                      process_to_loaded_s=loaded-started, first_reset_wall_ms=first["reset_wall_ms"])
        rpc("close")
        result["exit_code"] = proc.wait(timeout=10)
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        print(json.dumps(result), flush=True)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
