#!/usr/bin/env python3
"""Run one declared sleeping-corpse full save/load through two fresh rendered game processes."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

from ragdoll_session_compare import read_json, snapshot, complete_process, check_archive


SAVE_SCENARIO = """seed 73013
cheat god
set time 12 0
ragdoll on
goto NW_XARDAS_TOWER_VALLEY_091
clear 2500
face -106
wait 500
insert BDT_1013_BANDIT_L
freeze BDT_1013_BANDIT_L
put BDT_1013_BANDIT_L 300 0
watch BDT_1013_BANDIT_L
npcequip watched ItMi_Gold 7
wait 600
down watched dead
waitragdoll watched sleep 30000
ragdoll session-save watched sleeping
"""


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def launch(binary, assets, save, output, scenario, timeout):
    output.mkdir()
    run = output / "run"
    run.mkdir()
    shutil.copy2(save, run / "save_slot_1.sav")
    # Must enable before GameSession construction, including the second process.
    (run / "Gothic.ini").write_text("[GAME]\nragdoll=1\n[INTERNAL]\nvidResIndex=2\n[SOUND]\nsoundVolume=0\nmusicVolume=0\n")
    source = output / "scenario.bench"
    source.write_text(scenario)
    argv = [str(binary), "-g", str(assets), "-window", "-devmode", "-save", "1"]
    declared = {"TEMPEST_BACKGROUND": "1", "GOTHIC_AI": "0", "OPENGOTHIC_BENCH": str(source),
                "OPENGOTHIC_BENCH_OUT": str(output), "OPENGOTHIC_TEXTURES": "",
                "OPENGOTHIC_BODY_ANIMATION_RNG": "isolated", "OPENGOTHIC_RAGDOLL_TRACE": "0",
                "OPENGOTHIC_RAGDOLL_PERF": "0"}
    env = dict(os.environ)
    for name in ("OPENGOTHIC_BENCH_KEEP", "OPENGOTHIC_BENCH_REALTIME", "OPENGOTHIC_RAGDOLL_CREATION_DUMP",
                 "OPENGOTHIC_PHYSICS_TRACE", "OPENGOTHIC_BODY_TRACE"):
        env.pop(name, None)
    env.update(declared)
    subprocess.run(["bash", str(Path(__file__).with_name("base-ensure.sh")), str(assets)],
                   env=dict(env, GAME_BIN=str(binary)), check=True)
    start = time.monotonic()
    before_sha = sha(binary)
    with (output / "game.log").open("w") as log:
        try:
            result = subprocess.run(argv, cwd=run, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
            rc = result.returncode
        except subprocess.TimeoutExpired:
            rc = 124
    info = {"argv": argv, "env": declared, "returncode": rc, "wall_seconds": time.monotonic()-start,
            "binary_sha256_before": before_sha, "binary_sha256_after": sha(binary),
            "scenario_sha256": sha(source), "ini_sha256": sha(run / "Gothic.ini"),
            "loaded_save_sha256": sha(run / "save_slot_1.sav")}
    (output / "launcher.json").write_text(json.dumps(info, indent=2) + "\n")
    print(output.name + ": rc=" + str(rc), flush=True)
    if rc != 0:
        raise ValueError("actual game process failed: " + output.name + ", rc=" + str(rc))
    if info["binary_sha256_before"] != info["binary_sha256_after"]:
        raise ValueError("game executable changed while the full-session fixture was running")
    return info


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("binary", "assets", "save", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    args = parser.parse_args()
    args.binary, args.assets, args.save, args.output = (value.resolve() for value in
                                                     (args.binary, args.assets, args.save, args.output))
    args.output.mkdir(exist_ok=False)
    candidate = args.output / "Gothic2Notr"
    shutil.copy2(args.binary, candidate)
    candidate.chmod(0o555)
    manifest = {"schema_version": 1, "pass": False, "scope": "two fresh rendered processes, full GameSession archive",
                "binary_source": str(args.binary), "binary_sha256": sha(candidate),
                "assets": str(args.assets), "source_save": str(args.save), "source_save_sha256": sha(args.save),
                "fixture": "Human Bandit dead on fixed flat spot; wait for real sleep, save; fresh load ragdoll=1 before construction; dt=0 initial snapshot and 1 second normal continuation",
                "processes": {}}
    destination = args.output / "manifest.json"
    destination.write_text(json.dumps(manifest, indent=2) + "\n")
    try:
        first = args.output / "save-process"
        # The preserved v3 run from a copied Mac executable timed out at startup.
        # Execute the normal frozen path; retain the byte copy and verify it
        # before/after both processes instead of changing the runtime location.
        manifest["processes"]["save"] = launch(args.binary, args.assets, args.save, first, SAVE_SCENARIO, args.timeout)
        # Refuse to start the second process if Human creation/sleep did not qualify.
        complete_process(first)
        saved = snapshot(first / "session-sleeping.json", "save")
        archive = first / "session-sleeping.sav"
        manifest["full_save_sha256"] = check_archive(archive, saved)
        destination.write_text(json.dumps(manifest, indent=2) + "\n")
        second = args.output / "load-process"
        selector = "id:" + str(saved["actor_id"])
        scenario = ("ragdoll session-initial " + selector + " initial\nwatch " + selector +
                    "\nwait 1000\nragdoll session-snapshot " + selector + " settled\n")
        if sha(args.binary) != manifest["binary_sha256"]:
            raise ValueError("candidate executable differs before the second process")
        manifest["processes"]["load"] = launch(args.binary, args.assets, archive, second, scenario, args.timeout)
        if manifest["processes"]["load"]["loaded_save_sha256"] != manifest["full_save_sha256"]:
            raise ValueError("second process did not load the exact first process full save")
        compare = Path(__file__).with_name("ragdoll_session_compare.py")
        command = [sys.executable, str(compare), "--saved", str(first / "session-sleeping.json"),
                   "--initial", str(second / "session-initial.json"), "--settled", str(second / "session-settled.json"),
                   "--save-file", str(archive), "--process-one", str(first), "--process-two", str(second),
                   "--output", str(args.output / "comparison.json")]
        result = subprocess.run(command, check=False)
        manifest["comparison_argv"] = command
        manifest["pass"] = result.returncode == 0 and read_json(args.output / "comparison.json")["pass"] is True
    except (ValueError, KeyError, TypeError, OSError) as error:
        manifest["error"] = str(error)
    destination.write_text(json.dumps(manifest, indent=2) + "\n")
    print("PASS" if manifest["pass"] else "FAIL", flush=True)
    return 0 if manifest["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
