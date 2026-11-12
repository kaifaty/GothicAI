"""Numbered frame sheets of records from Blender (scripts/content/anim/blender/gothic_rig.py): one PNG per view, every
frame with its number and time, the blade drawn. This is the sheet the user confirms a defect list on.

    uv run -q --with pillow python blender_sheet.py <base models> <out dir> "<label>" <record.MAN.json>...
        [--views side,front,q] [--cols 8] [--size 400] [--ortho 3.0] [--height 0.9]

Writes <out dir>/frames/<view>/f####.png, then <out dir>/<record stem>-<view>.png per record. The renders show the
body of Gothic: they are original content, keep them out of git (scratchpad and the NAS).
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ANIM = Path(__file__).resolve().parents[4] / "scripts" / "content" / "anim"  # no numpy here: the script runs under uv with pillow
BLENDER = "/Applications/Blender.app/Contents/MacOS/Blender"
ap = argparse.ArgumentParser()
ap.add_argument("base", type=Path)
ap.add_argument("out", type=Path)
ap.add_argument("label")
ap.add_argument("records", nargs="+", type=Path)
ap.add_argument("--views", default="side,front,q")
ap.add_argument("--cols", type=int, default=8)
ap.add_argument("--size", type=int, default=400)
ap.add_argument("--ortho", type=float, default=3.0)
ap.add_argument("--height", type=float, default=0.9, help="the frame's middle, m (1.35 for the arms and the head)")
args = ap.parse_args()
args.out.mkdir(parents=True, exist_ok=True)
rig = args.out / "rig.json"
rig.write_text(json.dumps({"models": str(args.base), "anims": [str(r.resolve()) for r in args.records],
                           "render": {"dir": str(args.out / "frames"), "views": args.views.split(","), "size": [args.size, args.size],
                                      "sword": True, "ortho": args.ortho, "height": args.height}}))
log = args.out / "blender.log"
with log.open("w") as fh:
    if subprocess.run([BLENDER, "--background", "--factory-startup", "--python", str(ANIM / "blender" / "gothic_rig.py"), "--", str(rig)],
                      stdout=fh, stderr=subprocess.STDOUT).returncode:
        sys.exit(f"Blender failed, see {log}")
tile = Path(__file__).with_name("tile.py")
for i, r in enumerate(args.records):
    stem = r.name.removesuffix(".MAN.json")
    subprocess.run([sys.executable, str(tile), str(args.out / "frames"), str(i), str(args.out / stem), str(args.cols), f"{args.label}, {stem}"],
                   check=True)
