#!/usr/bin/env python3
"""Tables of the content stress measurement from a run folder.

    summarise.py <run folder> [set...]

The run folder holds, per set, <set>-load-cold, <set>-load-warm (scripts/loadprobe.sh),
<set>-fly (scripts/bench.sh streaming-flythrough) and <set>-perf.stdout with the path of a
scripts/perf.sh run. Missing parts print as a dash.
"""
import csv
import io
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3] if (Path(__file__).resolve().parents[3] / "scripts/loadprobe_summary.py").exists() else Path.cwd()
MIB = 1 << 20


def load(run: Path, name: str) -> dict | None:
    folder = run / name
    if not folder.is_dir():
        return None
    out = subprocess.run([sys.executable, str(ROOT / "scripts/loadprobe_summary.py"), str(folder.resolve())],
                         capture_output=True, text=True)
    rows = list(csv.DictReader(io.StringIO(out.stdout)))
    return rows[0] if rows else None


TRUNCATED = {}


def fly(run: Path, name: str) -> dict | None:
    """The flythrough report; a recording that overflowed the frame profiler (20 000 attempts) is
    reported up to the overflow from a copy with the overflow column cleared."""
    folder = run / name
    report = folder / "report.json"
    if not (folder / "frameprofile.csv").is_file():
        return None
    data = json.loads(report.read_text()) if report.is_file() else {}
    if "places" not in data:
        copy = run / f"{name}-truncated"
        if not (copy / "report.json").is_file():
            copy.mkdir(exist_ok=True)
            for item in folder.iterdir():
                if item.is_file() and item.name not in ("report.json", "report.md"):
                    shutil.copy2(item, copy / item.name)
            with (folder / "frameprofile.csv").open(newline="") as source, (copy / "frameprofile.csv").open("w", newline="") as target:
                reader = csv.DictReader(source)
                writer = csv.DictWriter(target, fieldnames=reader.fieldnames)
                writer.writeheader()
                for row in reader:
                    TRUNCATED[name] = max(TRUNCATED.get(name, 0), int(row["overflow"]))
                    row["overflow"] = "0"
                    writer.writerow(row)
            subprocess.run([sys.executable, str(ROOT / "scripts/perf_report.py"), "run", str(copy.resolve())],
                           capture_output=True, text=True, check=True)
        else:
            with (folder / "frameprofile.csv").open(newline="") as source:
                TRUNCATED[name] = int(next(csv.DictReader(source))["overflow"])
        data = json.loads((copy / "report.json").read_text())
    return data["places"]["streaming-flythrough"]["views"]


def perf(run: Path, name: str) -> dict | None:
    stdout = run / f"{name}-perf.stdout"
    if not stdout.is_file():
        return None
    found = re.findall(r"(/\S*\.bench/perf-[0-9TZ]+-\d+)", stdout.read_text())
    for folder in reversed(found):
        report = Path(folder) / "report.json"
        if report.is_file():
            return json.loads(report.read_text())["places"]
    return None


def cell(value, digits=1, scale=1.0):
    if value in (None, "", "?"):
        return "—"
    return f"{float(value) * scale:.{digits}f}".replace(".", ",")


def table(title, header, rows):
    print(f"\n### {title}\n")
    print("| " + " | ".join(header) + " |")
    print("|" + "|".join("---" if i == 0 else "---:" for i in range(len(header))) + "|")
    for row in rows:
        print("| " + " | ".join(row) + " |")


def main():
    run = Path(sys.argv[1])
    sets = sys.argv[2:] or ["base", "t2", "t4", "w4", "w16", "g4", "g16", "g4t2"]

    stages = [("total_s", "итого, с", 2), ("chunks", "чтение кусков", 0), ("pack", "упаковка Near", 0),
              ("view", "вид ландшафта", 0), ("physics", "статичная физика", 0), ("vobs", "vob'ы", 0),
              ("startup", "STARTUP", 0), ("tex_mb", "текстуры при загрузке, МиБ", 0)]
    for kind, label in (("warm", "тёплые производные кэши"), ("cold", "холодные производные кэши")):
        data = {s: load(run, f"{s}-load-{kind}") for s in sets}
        table(f"Загрузка NEWWORLD, {label} (мс, кроме итога)", ["Стадия"] + sets,
              [[name] + [cell(data[s] and data[s].get(key), digits) for s in sets] for key, name, digits in stages])
    data = {s: load(run, f"{s}-load-warm") for s in sets}
    memory = [("fp_warm", "RSS в тёплой точке, МБ", 0, 1.0), ("gpu_warm", "память GPU в тёплой точке, МБ", 0, 1.0),
              ("malloc_in_use_warm", "живой malloc, МиБ", 0, 1.0),
              ("tex_live_bytes_end", "текстуры живые, МиБ", 0, 1 / MIB)]
    table("Память после загрузки (стартовая точка, тёплые кэши)", ["Метрика"] + sets,
          [[name] + [cell(data[s] and data[s].get(key), digits, scale) for s in sets] for key, name, digits, scale in memory])

    flights = {s: fly(run, f"{s}-fly") for s in sets}
    rows = []
    for view, label in (("run", "бег 5 м/с"), ("fly", "полёт 20 м/с")):
        def get(s, *path):
            value = flights[s] and flights[s].get(view)
            for key in path:
                value = value.get(key) if isinstance(value, dict) else None
            return value
        rows += [
            [f"{label}: кадр p50, мс"] + [cell(get(s, "frame_ms", "p50"), 2) for s in sets],
            [f"{label}: кадр p99, мс"] + [cell(get(s, "frame_ms", "p99"), 2) for s in sets],
            [f"{label}: кадр max, мс"] + [cell(get(s, "frame_ms", "max"), 1) for s in sets],
            [f"{label}: GPU mean, мс"] + [cell(get(s, "gpu", "mean_ms"), 2) for s in sets],
            [f"{label}: память GPU в конце, МБ"] + [cell(get(s, "streaming", "gpu_end_mb"), 0) for s in sets],
            [f"{label}: страницы ландшафта, МиБ"] + [cell(get(s, "streaming", "page_capacity_peak_bytes"), 0, 1 / MIB) for s in sets],
            [f"{label}: текстуры живые, МиБ"] + [cell(get(s, "streaming", "textures", "live_end_bytes"), 0, 1 / MIB) for s in sets],
            [f"{label}: текстуры пик, МиБ"] + [cell(get(s, "streaming", "textures", "peak_bytes"), 0, 1 / MIB) for s in sets],
        ]
    def warm(s, *path):
        value = flights[s] and flights[s].get("run", {}).get("streaming")
        for key in path:
            value = value.get(key) if isinstance(value, dict) else None
        return value
    rows += [
        ["прогрев: публикация Far p99, мс"] + [cell(warm(s, "warmup_far_publication_ms", "p99"), 2) for s in sets],
        ["прогрев: публикация Far max, мс"] + [cell(warm(s, "warmup_far_publication_ms", "max"), 2) for s in sets],
        ["прогрев: очередь max, мс"] + [cell(warm(s, "warmup_queue_max_ms"), 1) for s in sets],
    ]
    table("Облёт стриминга (рынок → гавань → лес), окно 800×600", ["Метрика"] + sets, rows)

    zones = ["tick", "world_logic", "physics", "parallel_animation", "prepare", "encode", "encode_main", "encode_shadows",
             "storage_commit", "submit", "present"]
    rows = []
    for zone in zones:
        row = [zone]
        for s in sets:
            value = None
            for item in (flights[s] or {}).get("run", {}).get("zones", []):
                if item["name"] == zone:
                    value = item["mean_ms"]
            row.append(cell(value, 2))
        rows.append(row)
    table("Зоны CPU на проходе «бег», среднее на кадр, мс", ["Зона"] + sets, rows)

    places = {s: perf(run, s) for s in sets}
    names = sorted({p for value in places.values() if value for p in value})
    rows = []
    for place in names:
        def summary(s, *path):
            value = places[s] and places[s].get(place, {}).get("summary")
            for key in path:
                value = value.get(key) if isinstance(value, dict) else None
            return value
        rows += [
            [f"{place}: кадр p50, мс"] + [cell(summary(s, "frame_ms", "p50"), 2) for s in sets],
            [f"{place}: кадр p95, мс"] + [cell(summary(s, "frame_ms", "p95"), 2) for s in sets],
            [f"{place}: GPU mean, мс"] + [cell(summary(s, "gpu", "mean_ms"), 2) for s in sets],
            [f"{place}: треугольники, млн"] + [cell(summary(s, "amounts", "triangles", "per_frame"), 2, 1e-6) for s in sets],
            [f"{place}: вызовы отрисовки"] + [cell(summary(s, "amounts", "draw_calls", "per_frame"), 0) for s in sets],
        ]
    if rows:
        table("Кадр по местам (perf.sh), окно 1728×1117", ["Метрика"] + sets, rows)
    if TRUNCATED:
        print("\nОблёт с переполнением профилировщика (отчёт до переполнения, потеряно попыток кадра в конце прохода «полёт»): "
              + ", ".join(f"{name.removesuffix('-fly')} — {lost}" for name, lost in sorted(TRUNCATED.items())) + ".")


if __name__ == "__main__":
    main()
