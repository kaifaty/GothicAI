#!/usr/bin/env python3
"""Prepare the city-4 comparison tour, check captures and write JPEG triples."""
import json
from pathlib import Path
import re
import subprocess
import sys

HERE = Path(__file__).resolve().parent
VARIANTS = ("original", "city-3", "city-4")


def commands(scenario):
    return [line.split("#", 1)[0].strip() for line in scenario.read_text().splitlines()
            if line.split("#", 1)[0].strip()]


def prepare(index, output, inspection):
    targets = json.loads(index.read_text())
    if len(targets) != 65:
        raise SystemExit("city-4 requires exactly 65 packed materials")
    blocks = {}
    current = None
    inspection_text = HERE.joinpath("city-intent.bench").read_text() + "\n" + inspection.read_text()
    for line in inspection_text.splitlines():
        match = re.match(r"^# ([A-Z0-9_]+\.TGA) \|", line)
        if match:
            current = match[1]
            blocks[current] = []
        if current is not None:
            blocks[current].append(line)
    misc = [name for name in targets if name.startswith("NW_MISC_")]
    if len(misc) != 7 or set(misc) - blocks.keys():
        raise SystemExit("city-4 requires all seven NW_MISC inspection cameras")
    if set(targets) - blocks.keys():
        raise SystemExit("missing city-4 census camera: " + ", ".join(sorted(set(targets) - blocks.keys())))
    text = HERE.joinpath("city-3.bench").read_text().rstrip() + "\n\n"
    pilot = HERE.joinpath("quality-pilot.bench").read_text()
    text += re.sub(r"^shot (\w+)$", r"shot pilot_\1", pilot, flags=re.MULTILINE).rstrip() + "\n\n"
    text += "# Market and upper-quarter gates: seven NW_MISC inspection cameras.\nset time 12 0\n"
    for name in misc:
        block = "\n".join(blocks[name])
        if block.count("\nshot ") != 1 or "\ncam hold\n" not in block:
            raise SystemExit("incomplete NW_MISC inspection camera: " + name)
        text += block.replace("\nwait 600\n", "\nwait 6000\n").rstrip() + "\n\n"
    shots = [line[5:] for line in text.splitlines() if line.startswith("shot ")]
    if len(shots) != 35 or len(set(shots)) != 35:
        raise SystemExit("city-4 requires 35 unique shots (33 comparison points plus FPS markers)")
    output.write_text(text)
    inspection = output.with_name("city-4-inspection.bench")
    inspection.write_text(inspection_text)
    subprocess.run(["bash", str(HERE / "capture-city.sh"), "--append-census", str(output),
                    str(inspection), str(index)], check=True)
    print("city-4 tour: 33 comparison points; census cameras=65")


def check(directory, scenario, index, variant):
    if variant not in VARIANTS:
        raise SystemExit("unknown city-4 capture variant")
    expected = 58 if variant == "city-3" else 65
    if len(json.loads(index.read_text())) != expected:
        raise SystemExit(f"{variant}: expected packed index with {expected} materials")
    if variant != "city-3":
        subprocess.run(["bash", str(HERE / "capture-city.sh"), "--check", str(directory),
                        str(scenario), str(index), "before" if variant == "original" else "after"], check=True)
        if variant == "original":
            census = json.loads((directory / "census.json").read_text())
            if census["replacements"]:
                raise SystemExit("original baseline must have zero replacements")
        return
    log = (directory / "game.log").read_text(errors="replace")
    if ((directory / "exit.txt").read_text().strip() != "0" or "[bench] done" not in log
            or not re.search(r"materials: 58 entries from ", log)
            or re.search(r"materials: (invalid|unsupported|unable)|\[bench\] (can not|unknown|marvin failed|goto failed)", log)):
        raise SystemExit("city-3: unsuccessful capture or wrong material count")
    for command in commands(scenario):
        if command.startswith(("goto ", "shot ")) and f"[bench] {command}\n" not in log:
            raise SystemExit("city-3: command not reached: " + command)
        if command.startswith("shot "):
            image = directory / (command[5:] + ".png")
            if not image.is_file() or not image.stat().st_size:
                raise SystemExit("city-3: missing shot: " + command[5:])
    events = directory / "events.jsonl"
    for line in events.read_text().splitlines() if events.exists() else ():
        event = json.loads(line)
        kind = event.get("kind", "")
        if kind == "error" or kind.endswith("_error") or event.get("pass") is False:
            raise SystemExit("city-3: benchmark error: " + kind)
    print("city-3: successful capture; materials=58")


def compose(captures, scenario, output):
    from PIL import Image, ImageDraw, ImageFont
    spots = [line[5:] for line in commands(scenario)
             if line.startswith("shot ") and line[5:] not in ("city_a", "city_night")]
    if len(spots) != 33 or len(set(spots)) != 33:
        raise SystemExit("city-4 requires 33 unique comparison points")
    output.mkdir(parents=True, exist_ok=True)
    try:
        font = ImageFont.truetype("/System/Library/Fonts/Supplemental/Arial.ttf", 22)
    except OSError:
        font = ImageFont.load_default(size=22)
    for spot in spots:
        panels = []
        for variant in VARIANTS:
            with Image.open(captures / variant / (spot + ".png")) as source:
                panel = source.convert("RGB")
                panel.thumbnail((768, 432), Image.Resampling.LANCZOS)
                panels.append(panel)
        width, height = max(p.width for p in panels), max(p.height for p in panels)
        board = Image.new("RGB", (width * 3, height + 44), "#17191b")
        draw = ImageDraw.Draw(board)
        for position, (variant, panel) in enumerate(zip(VARIANTS, panels)):
            board.paste(panel, (position * width, 44))
            draw.text((position * width + 12, 10), f"{spot} | {variant}", font=font, fill="white")
        board.save(output / (spot + ".jpg"), quality=90, optimize=True)
    print(f"city-4: {len(spots)} original / city-3 / city-4 JPEG triples: {output}")


if __name__ == "__main__":
    if len(sys.argv) == 5 and sys.argv[1] == "prepare":
        prepare(*map(Path, sys.argv[2:]))
    elif len(sys.argv) == 6 and sys.argv[1] == "check":
        check(*map(Path, sys.argv[2:5]), sys.argv[5])
    elif len(sys.argv) == 5 and sys.argv[1] == "compose":
        compose(*map(Path, sys.argv[2:]))
    else:
        raise SystemExit("usage: capture-city-4.py prepare INDEX OUTPUT INSPECTION | check CAPTURE SCENARIO INDEX VARIANT | compose CAPTURES SCENARIO OUTPUT")
