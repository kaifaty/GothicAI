#!/usr/bin/env python3
"""Validate pilot captures, compose review boards, or measure full-frame detail.

``detail CAPTURES OUTPUT.csv VARIANT...`` writes one row per requested variant
and spot. Columns contain mean_y (linear Rec.709), log_spread (p90 - p10 of
ln(Y + .001)), detail_k9 (mean absolute residual from a reflected 9x9 box),
and their *_ratio values against CAPTURES/original at the same spot. A ratio
is empty when the original metric is zero. Inputs must be exactly 1280x720.
Run with: uv run --no-project --with numpy --with pillow --with scipy python
``check`` includes the window for pilot-3a/pilot-3b; pass ``--iteration-3``
to check the same eight targets in original/pilot-2 captures.
"""
import csv
import json
from pathlib import Path
import re
import sys

SPOTS = ("roof", "upper", "house", "interior", "facade", "quay", "stairs")
MATERIALS = (
    "NW_CITY_ROOF_01", "NW_CITY_PFLASTERSTEIN_01", "NW_CITY_BODENDIELEN_01",
    "NW_CITY_ROOF_INSIDE_01", "NW_CITY_WALL_03", "NW_CITY_HAFENKAI_BODEN_01",
    "NW_HARBOUR_STAIRS_01",
)
ITERATION3_MATERIALS = (*MATERIALS, "NW_CITY_WINDOW_03")


def texture_key(name):
    name = name.replace("\\", "/").rsplit("/", 1)[-1].upper()
    if name.endswith(".TGA"):
        return name[:-4]
    if name.endswith(".TEX"):
        return name[:-6] if name.endswith("-C.TEX") else name[:-4]
    return name


def check(directory, scenario, material_index, iteration3=False):
    def require(condition, reason):
        if not condition:
            raise SystemExit(f"pilot capture {directory.name}: {reason}")
    require((directory / "exit.txt").read_text().strip() == "0", "nonzero game exit")
    log = (directory / "game.log").read_text(errors="replace")
    require("[bench] done" in log, "scenario did not finish")
    require(not re.search(r"materials: (invalid|unsupported|unable)|\[bench\] (can not|unknown|marvin failed|goto failed)", log), "game or material error")
    commands = [line.split("#", 1)[0].strip() for line in scenario.read_text().splitlines()]
    shots = [command[5:] for command in commands if command.startswith("shot ")]
    require(tuple(shots) == SPOTS, "unexpected shot list")
    for command in commands:
        if command.startswith(("goto ", "shot ")):
            require(f"[bench] {command}\n" in log, f"command not reached: {command}")
    for spot in SPOTS:
        image = directory / (spot + ".png")
        require(image.is_file() and image.stat().st_size > 0, f"missing shot: {spot}")
    index = json.loads(material_index.read_text())
    require(bool(re.search(rf"materials: {len(index)} entries from ", log)), "wrong material count")
    census = json.loads((directory / "census.json").read_text())
    opened = {texture_key(row["name"]) for row in census["files"] if row["kind"] == "texture"}
    replaced = {texture_key(row["name"]) for row in census["replacements"] if row["kind"] == "texture"}
    targets = set(ITERATION3_MATERIALS if iteration3 or directory.name in ("pilot-3a", "pilot-3b") else MATERIALS)
    if index:
        require(targets <= replaced, "pilot textures not replaced: " + ", ".join(sorted(targets - replaced)))
        require(not (targets & opened), "pilot originals opened: " + ", ".join(sorted(targets & opened)))
    else:
        require(targets <= opened, "pilot originals not opened: " + ", ".join(sorted(targets - opened)))
        require(not (targets & replaced), "baseline replaces pilot materials")
    events = directory / "events.jsonl"
    for line in events.read_text().splitlines() if events.exists() else ():
        event = json.loads(line)
        kind = event.get("kind", "")
        require(not (kind == "error" or kind.endswith("_error") or event.get("pass") is False), f"event failure: {kind}")
    print(f"{directory.name}: shots={len(shots)} materials={len(index)} pilot_replaced={len(targets & replaced)} pilot_originals={len(targets & opened)}")


def compose(captures, output, iteration2=False, iteration3=False):
    from PIL import Image, ImageDraw, ImageFont
    output.mkdir(parents=True, exist_ok=True)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 22)
    except OSError:
        font = ImageFont.load_default(size=22)
    variants = ("original", "city-3", "pilot-b", "pilot-c")
    labels = ("original", "A: city-3", "B: new prompt + colour", "C: B + Marigold + colour")
    if iteration2:
        variants = ("original", "city-3", "pilot-b", "pilot-2")
        labels = ("original", "city-3", "iteration 1 B", "iteration 2")
    if iteration3:
        variants = ("original", "pilot-2", "pilot-3a", "pilot-3b")
        labels = ("original", "iteration 2", "iteration 3a", "iteration 3b")
    for spot in SPOTS:
        panels = []
        for variant in variants:
            with Image.open(captures / variant / (spot + ".png")) as source:
                panel = source.convert("RGB")
                panel.thumbnail((768, 432), Image.Resampling.LANCZOS)
                panels.append(panel)
        width = max(panel.width for panel in panels)
        height = max(panel.height for panel in panels)
        board = Image.new("RGB", (width * 4, height + 44), "#17191b")
        draw = ImageDraw.Draw(board)
        for position, (label, panel) in enumerate(zip(labels, panels)):
            board.paste(panel, (position * width, 44))
            draw.text((position * width + 12, 10), f"{spot} | {label}", font=font, fill="white")
        board.save(output / f"{spot}.jpg", quality=90, optimize=True)
    print(f"composed {len(SPOTS)} {'/'.join(variants)} boards: {output}")


def measure_frame(path):
    import numpy as np
    from PIL import Image
    from scipy.ndimage import uniform_filter

    with Image.open(path) as image:
        if image.size != (1280, 720):
            raise ValueError(f"{path}: expected 1280x720, got {image.width}x{image.height}")
        srgb = np.asarray(image.convert("RGB"), dtype=np.float64) / 255
    linear = np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)
    y = linear @ np.array([.2126, .7152, .0722])
    log_y = np.log(y + .001)
    low, high = np.percentile(log_y, [10, 90])
    return {
        "mean_y": float(y.mean()),
        "log_spread": float(high - low),
        "detail_k9": float(np.abs(log_y - uniform_filter(log_y, size=9, mode="reflect")).mean()),
    }


def detail(captures, output, variants):
    fields = ("mean_y", "log_spread", "detail_k9")
    rows = []
    for spot in SPOTS:
        original = measure_frame(captures / "original" / f"{spot}.png")
        for variant in variants:
            measured = original if variant == "original" else measure_frame(captures / variant / f"{spot}.png")
            rows.append({
                "spot": spot, "variant": variant, **measured,
                **{field + "_ratio": measured[field] / original[field] if original[field] else ""
                   for field in fields},
            })
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=("spot", "variant", *fields,
                                                   *(field + "_ratio" for field in fields)), lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    print(f"measured {len(rows)} frames: {output}")


if __name__ == "__main__":
    if len(sys.argv) in (5, 6) and sys.argv[1] == "check" and (len(sys.argv) == 5 or sys.argv[5] == "--iteration-3"):
        check(*map(Path, sys.argv[2:5]), iteration3=len(sys.argv) == 6)
    elif len(sys.argv) == 4 and sys.argv[1] in ("compose", "compose-2", "compose-3"):
        compose(*map(Path, sys.argv[2:]), iteration2=sys.argv[1] == "compose-2",
                iteration3=sys.argv[1] == "compose-3")
    elif len(sys.argv) >= 5 and sys.argv[1] == "detail":
        detail(Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4:])
    else:
        raise SystemExit("usage: capture-quality-pilot.py check DIRECTORY SCENARIO INDEX [--iteration-3] | compose|compose-2|compose-3 CAPTURES OUTPUT | detail CAPTURES OUTPUT.csv VARIANT...")
