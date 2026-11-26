"""Single capture game adapted from tree-nanite-step3/stand.py, arm 01-A."""
import csv
from hashlib import file_digest
import json
from math import dist
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

SETTINGS = {
    "GOTHIC_AI": "0",
    "TEMPEST_BACKGROUND": "0",
    "TEMPEST_WINDOW_SIZE": "2560x1440",
    "OPENGOTHIC_BENCH_VISIBLE": "1",
    "OPENGOTHIC_UPSCALE": "taau",
    "OPENGOTHIC_RENDER_SCALE": "0.5",
    "OPENGOTHIC_MS": "1",
    "OPENGOTHIC_VSM": "1",
    "OPENGOTHIC_VSM_STATS": "0",
    "OPENGOTHIC_RT": "1",
    "OPENGOTHIC_GI": "1",
    "OPENGOTHIC_TREE_BOUGH_LOD": "1",
    "OPENGOTHIC_HIZ_HISTORY": "1",
    "OPENGOTHIC_TREE_WIND_FADE": "1",
    "OPENGOTHIC_TREE_WIND_CACHE": "0",
    "OPENGOTHIC_SHADOW_LEVELS": "1",
}
REQUIRED_LOG = (
    "[bench] done", "vsm = 1", "gi = 1", "tree HiZ history = 1", "tree bough LOD = 1",
    "tree shadow levels = 1", "tree wind fade = 1", "7175 source cards",
    "(taau) 1280x720 -> 2560x1440", "world check passed: NEWWORLD.ZEN",
    "applied=IMMEDIATE extent=2560x1440 result=0", "NVIDIA GeForce RTX 3080",
)
EXPECTED = {
    "content/meshes/meshes.json": "56a9984155d87251d03e285b1ed21fe300007980743b307bd67c9a89b91a4b42",
    "base/base.json": "529a6fe0d76d901849f665b3e9aafdb8e14ea30be68d1dec34650dc029d9580d",
    "content/worlds/NEWWORLD/vobs.json": "51f756d2a7ab2bbeb030764210f90eaecbc25e434af281a87647572708e5a172",
}


def digest(path):
    with path.open("rb") as f:
        return file_digest(f, "sha256").hexdigest()


def run(task, inputs):
    if not os.environ.get("GQ_HOME") or not task.is_absolute() or not inputs.is_absolute():
        raise ValueError("Run inside gq with absolute task and input paths")
    output = task / "result"
    output.mkdir()
    hashes = {name: digest(inputs / name) for name in EXPECTED}
    if hashes != EXPECTED:
        raise ValueError(f"Input SHA-256 mismatch: {hashes}")
    trees = [r for r in json.loads((inputs / "content/worlds/NEWWORLD/vobs.json").read_bytes())["vobs"]
             if r.get("visual_name") == "OWN_TREE_FIR_A_4.3DS"]
    if len(trees) != 1024 or len({r["vob_name"] for r in trees}) != 1024:
        raise ValueError("Expected 1,024 distinct fir placements")
    with (output / "placements.csv").open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(("key", "vob_id", "x", "y", "z"))
        writer.writerows((r["vob_name"], r["id"], *r["position"]) for r in trees)
    work = task / "game"
    work.mkdir()
    (work / "Gothic.ini").write_text("[ENGINE]\nzMaxFps=0\n[INTERNAL]\nvidResIndex=0\n")
    settings_dir = work / "settings"
    shutil.copytree(inputs / "game-settings", settings_dir)
    cache = work / "cache"
    pipeline = cache / "derived/pipelines/vulkan-000010de00002206.bin"
    pipeline.parent.mkdir(parents=True)
    shutil.copyfile(inputs / "vulkan-pipeline.bin", pipeline)
    route = output / "capture.bench"
    for name in ("stand.py", "capture.bench", "analyze.py"):
        shutil.copyfile(Path(__file__).with_name(name), output / name)
    env = {k:v for k,v in os.environ.items() if not k.startswith(("OPENGOTHIC_", "TEMPEST_"))}
    env.update(SETTINGS)
    env.update(
        LD_PRELOAD=str(inputs / "present.so"),
        OPENGOTHIC_BASE=str(inputs / "base"), OPENGOTHIC_CONTENT=str(inputs / "content"),
        OPENGOTHIC_BASE_CACHE=str(cache), OPENGOTHIC_BASE_STORE=str(task / "base-store"),
        OPENGOTHIC_BENCH=str(route), OPENGOTHIC_BENCH_OUT=str(output),
        OPENGOTHIC_FRAME_PROFILE=str(output / "frameprofile.csv"), OPENGOTHIC_TREE_NANITE="0",
        FIR_PIXEL_OUT=str(output / "capture"),
    )
    binary = task / "bin/Gothic2Notr"
    (output / "metadata.json").write_text(json.dumps(dict(input_sha256=hashes, binary_sha256=digest(binary),
        settings={k:env[k] for k in env if k.startswith(("OPENGOTHIC_", "TEMPEST_", "FIR_")) or k == "GOTHIC_AI"}),
        indent=2) + "\n")
    command = [str(binary), "-g", str(settings_dir), "-window", "-devmode", "-nomenu", "-nointro",
               "-w", "NEWWORLD.ZEN", "-ms"]
    started = time.monotonic()
    rc = None
    try:
        with (output / "game.log").open("w") as log:
            rc = subprocess.run(command, cwd=work, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=79.5).returncode
    finally:
        (output / "launch.json").write_text(json.dumps(dict(returncode=rc, seconds=time.monotonic()-started)) + "\n")
    log = (output / "game.log").read_text(errors="replace")
    missing = [mark for mark in REQUIRED_LOG if mark not in log]
    if rc or missing or "[bench] error" in log or "Tree Nanite Vulkan path active" in log:
        raise ValueError(f"Game exit {rc}, missing {missing}; see game.log")
    events = [json.loads(line) for line in (output / "events.jsonl").read_text().splitlines()]
    windows = [r for r in events if r["kind"] == "perf_segment"]
    if ([r["segment"] for r in windows] != ["perf-coverage/inside", "perf-coverage/walk"]
            or windows[0]["camera_cm"] != [54140, 1760, -10780]
            or any(r["actors"] or not r["residency_manager"] for r in windows)):
        raise ValueError(f"Unexpected capture route: {windows}")
    positions = [r["position_cm"] for r in events if r["kind"] == "position"]
    if len(positions) != 3 or dist(positions[1], positions[2]) == 0:
        raise ValueError(f"No measured hero movement: {positions}")


if __name__ == "__main__":
    run(*map(Path, sys.argv[1:]))
