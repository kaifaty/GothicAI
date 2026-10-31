"""Pilot of feat icon styles: every style x every feat through headless `codex exec` image_gen.

Usage: python3 run.py PILOT_DIR [JOBS]. Results land in PILOT_DIR/out/<style>-<feat>.png, the exact prompt
next to each, a log in PILOT_DIR/run.log. Already generated pairs are skipped, so a rerun only fills gaps.
"""

import json
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

root = Path(sys.argv[1])
jobs = int(sys.argv[2]) if len(sys.argv) > 2 else 3
spec = json.loads((root / "pilot.json").read_text())
out = root / "out"
out.mkdir(exist_ok=True)
work = root / "codex-dir"  # an empty trusted git repo: codex refuses an untrusted directory
if not (work / ".git").exists():
    work.mkdir(exist_ok=True)
    subprocess.run(["git", "init", "-q"], cwd=work, check=True)
    subprocess.run(["git", "commit", "-q", "--allow-empty", "-m", "init"], cwd=work, check=True)

WRAP = (
    "Use your image generation tool exactly once with the prompt below. Do not view, judge or edit the "
    "result. Reply with only the absolute path of the generated file.\n\nPrompt:\n"
)


def log(line: str) -> None:
    with open(root / "run.log", "a") as f:
        f.write(time.strftime("%H:%M:%S ") + line + "\n")


def one(style: str, feat: str) -> None:
    target = out / f"{style}-{feat}.png"
    if target.exists():
        return
    prompt = (
        f"{spec['styles'][style]['text']}\n\nSubject: {spec['feats'][feat]['emblem']}\n\n{spec['common']}"
    )
    (out / f"{style}-{feat}.txt").write_text(prompt)
    for attempt in (1, 2):
        start = time.time()
        r = subprocess.run(
            ["codex", "exec", "--skip-git-repo-check", "-s", "read-only"],
            input=WRAP + prompt,
            cwd=work,
            capture_output=True,
            text=True,
            timeout=300,
        )
        paths = re.findall(r"(/\S+\.png)", r.stdout)
        if r.returncode == 0 and paths and Path(paths[-1]).exists():
            shutil.copy(paths[-1], target)
            log(f"{style}-{feat}: ok in {time.time() - start:.0f} s ({paths[-1]})")
            return
        log(f"{style}-{feat}: attempt {attempt} failed, exit {r.returncode}: {r.stderr[-300:]!r}")


pairs = [(s, f) for f in spec["feats"] for s in spec["styles"]]
with ThreadPoolExecutor(jobs) as pool:
    list(pool.map(lambda p: one(*p), pairs))
log(f"done: {len(list(out.glob('*.png')))}/{len(pairs)}")
