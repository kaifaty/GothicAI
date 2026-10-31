#!/usr/bin/env python3
"""Render five top views from saved NAVDEMO2 and saved truth-corridor probes.

Extract once on the recording host (numpy only):
  python -I scripts/nav_teacher_v3_report_figures.py --task /absolute/task \
    --bundle /absolute/selected-evidence.npz --extract-only
Then render with numpy and matplotlib, without a game or any new evaluator run:
  python -I scripts/nav_teacher_v3_report_figures.py \
    --bundle /absolute/selected-evidence.npz --output /absolute/report/figures

The bundle is temporary evidence outside git. Truth is drawn ONLY on the saved
5–15m corridor cells; all other colored cells are memory, never inferred truth.
"""
from __future__ import annotations

import argparse
import importlib.util
import json
import math
from pathlib import Path

import numpy as np


SELECTION = (
    ("false-drop", "object-15cce3111679129f", 133),
    ("false-wall", "point-0156131ae7538da6", 52),
    ("false-ledge", "object-39a83107a39adc9e", 877),
    ("follow", "follow-e6ef2cb27a9149ba", 490),
    ("successful-detour", "point-534e7c999f7ea3c6", 112),
)


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).resolve().parent / f"{name}.py")
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def rows(path):
    with Path(path).open(encoding="utf-8") as stream:
        for line in stream:
            if line.strip():
                yield json.loads(line)


def picked(path, predicate):
    for value in rows(path):
        if predicate(value):
            return value
    raise ValueError(f"Required saved evidence is missing: {path}")


def stats(values):
    values = np.asarray(values, dtype=float)
    return dict(count=len(values), min=float(values.min()) if len(values) else None,
                median=float(np.median(values)) if len(values) else None, max=float(values.max()) if len(values) else None)


def false_hazards(record, truth):
    memory_ground, true_ground, height_delta = [], [], []
    channels = dict(wall=0, ledge=0, drop=0, water=0, movable=0)
    false_cells = []
    dangerous = 0
    passable = 0
    for cell in truth["native_cells"]:
        memory = record[f"memory_{cell['layer']}"][cell["row"], cell["column"]]
        hazard = bool(np.any(memory["channels"][2:] > 0))
        dangerous += int(hazard)
        passable += int(cell["truth_passable"])
        if not hazard or not cell["truth_passable"]:
            continue
        false_cells.append(dict(layer=cell["layer"], row=cell["row"], column=cell["column"],
                                channels=memory["channels"].tolist(), memory_height_valid=bool(memory["height_valid"]),
                                memory_ground_cm=float(memory["ground_height_cm"]),
                                truth_ground_cm=cell["ground_height_cm"],
                                local_center_cm=[(cell["column"] + .5 - (32 if cell["layer"] == "near" else 50)) * (25 if cell["layer"] == "near" else 200),
                                                 (cell["row"] + .5 - (32 if cell["layer"] == "near" else 50)) * (25 if cell["layer"] == "near" else 200)]))
        for channel, name in enumerate(channels, 2):
            channels[name] += int(memory["channels"][channel] > 0)
        if memory["height_valid"]:
            memory_ground.append(float(memory["ground_height_cm"]) - float(record["position_cm"][1]))
        if cell["height_valid"]:
            true_ground.append(float(cell["ground_height_cm"]) - float(record["position_cm"][1]))
        if memory["height_valid"] and cell["height_valid"]:
            height_delta.append(float(memory["ground_height_cm"]) - float(cell["ground_height_cm"]))
    return dict(corridor_cells=len(truth["native_cells"]), truth_passable_cells=passable,
                memory_danger_cells=dangerous, false_danger_cells=len(false_cells),
                false_channel_counts=channels, memory_ground_minus_feet_cm=stats(memory_ground),
                truth_ground_minus_feet_cm=stats(true_ground), memory_minus_truth_ground_cm=stats(height_delta),
                false_cells=false_cells)


def extract(args):
    demos = module("nav_demos")
    root = args.task / "oracle-experiment" / "coverage" / "cases"
    arrays, cases = {}, []
    for ordinal, (tag, identifier, index) in enumerate(SELECTION):
        saved = root / identifier
        summary = json.loads((saved / "summary.json").read_text(encoding="utf-8"))
        source = Path(summary["source"])
        query = picked(saved / "queries.jsonl", lambda value: value["index"] == index)
        truth = picked(saved / "truth.jsonl", lambda value: value["label"] == query["label"])
        if not truth["corridor_available"]:
            raise ValueError(f"Selected saved corridor is unavailable: {identifier}#{index}")
        record = None
        offset = 0
        for chunk in demos.iter_chunks(source, chunk_records=16):
            if offset <= index < offset + len(chunk):
                record = chunk[index - offset].copy()
                break
            offset += len(chunk)
        if record is None:
            raise ValueError(f"Selected original NAVDEMO2 record is missing: {identifier}#{index}")
        if np.linalg.norm(record["position_cm"] - query["position_cm"]) > 1:
            raise ValueError(f"Selected original NAVDEMO2 pose mismatch: {identifier}#{index}")
        plan = next(value for ordinal_row, value in enumerate(rows(source / "teacher-paths.jsonl")) if ordinal_row == index)
        if plan["elapsed_ms"] != query["elapsed_ms"]:
            raise ValueError(f"Selected saved plan timestamp mismatch: {identifier}#{index}")
        metadata = json.loads((source / "episode.json").read_text(encoding="utf-8"))
        reference = next(rows(saved / "truth.jsonl"))
        case = dict(tag=tag, id=identifier, index=index, source=str(source), query=query, truth=truth,
                    reference=reference, plan=plan, goal_kind=summary["goal_kind"], split=summary["split"],
                    outcome=summary["outcome"], success=summary["success"], decisions=summary["decisions"],
                    elapsed_ms=metadata["elapsed_ms"], eye_layout=metadata["eye_layout"],
                    follow_fraction=summary.get("follow_fraction"), first_contact_ms=summary.get("first_contact_ms"),
                    evidence=false_hazards(record, truth))
        arrays[f"record_{ordinal}"] = np.array([record], dtype=demos.RECORD_V2_DTYPE)
        arrays[f"trajectory_{ordinal}"] = np.loadtxt(source / "trajectory.txt", ndmin=2)
        if summary["goal_kind"] == "follow":
            arrays[f"target_{ordinal}"] = np.loadtxt(source / "target-trajectory.txt", ndmin=2)
        cases.append(case)
        print(json.dumps(dict(event="figure_evidence", id=identifier, index=index,
                              false_danger=case["evidence"]["false_danger_cells"],
                              channels=case["evidence"]["false_channel_counts"])), flush=True)
    args.bundle.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.bundle, metadata=np.array(json.dumps(cases)), **arrays)
    light = [{key: case[key] for key in ("tag", "id", "index", "source", "query", "evidence")} for case in cases]
    args.bundle.with_suffix(".json").write_text(json.dumps(light, indent=2) + "\n", encoding="utf-8")


def local_to_world(points, position, yaw):
    points = np.asarray(points, dtype=float).reshape(-1, 2)
    angle = math.radians(yaw)
    right, forward = points[:, 0], points[:, 1]
    return np.column_stack((-right * math.sin(angle) + forward * math.cos(angle),
                            right * math.cos(angle) + forward * math.sin(angle))) + np.asarray(position)[[0, 2]]


def world_to_local(points, position, yaw):
    delta = np.asarray(points, dtype=float).reshape(-1, 2) - np.asarray(position)[[0, 2]]
    angle = math.radians(yaw)
    return np.column_stack((-delta[:, 0] * math.sin(angle) + delta[:, 1] * math.cos(angle),
                            delta[:, 0] * math.cos(angle) + delta[:, 1] * math.sin(angle)))


def draw_memory(ax, record, case, layer, patches, collections, colors):
    size, step, extent = (64, 25, 8) if layer == "near" else (100, 200, 100)
    memory = record[f"memory_{layer}"]["channels"]
    danger = np.any(memory[:, :, 2:] > 0, axis=2)
    free = (memory[:, :, 0] > 0) & (memory[:, :, 1] > 0) & ~danger
    category = np.zeros((size, size), dtype=np.uint8)
    category[free] = 1
    category[danger] = 2
    ax.imshow(category, origin="lower", extent=(-extent, extent, -extent, extent), interpolation="nearest",
              cmap=colors.ListedColormap(["#ededed", "#d5eadb", "#efc5be"]), vmin=0, vmax=2, rasterized=True)
    all_free, false, blocked = [], [], []
    for cell in case["truth"]["native_cells"]:
        if cell["layer"] != layer:
            continue
        x, y = (cell["column"] - size / 2) * step / 100, (cell["row"] - size / 2) * step / 100
        rectangle = patches.Rectangle((x, y), step / 100, step / 100)
        if not cell["truth_passable"]:
            blocked.append(rectangle)
        elif danger[cell["row"], cell["column"]]:
            false.append(rectangle)
        else:
            all_free.append(rectangle)
    for rectangles, color, width in ((all_free, "#16834b", 1), (false, "#b0009e", 1.5), (blocked, "#333333", 1)):
        ax.add_collection(collections.PatchCollection(rectangles, facecolor="none", edgecolor=color, linewidth=width))
    for value, style, color in ((case["truth"], "--", "#222222"),):
        points = np.asarray(value["corners_cm"], dtype=float)[:, [0, 2]]
        path = world_to_local(points, record["position_cm"], float(record["body_yaw_deg"])) / 100
        ax.plot(path[:, 0], path[:, 1], style, color=color, linewidth=1.6)
    plan = np.asarray(case["plan"]["local_plan_cm"], dtype=float).reshape(-1, 2) / 100
    if len(plan):
        ax.plot(plan[:, 0], plan[:, 1], color="#7951ab", linewidth=1.7)
    ax.plot(0, 0, "o", color="#1563b2", markersize=5)
    ax.annotate("", xy=(0, 1.2), xytext=(0, 0), arrowprops=dict(arrowstyle="->", color="#1563b2", lw=1.4))
    goal = world_to_local(np.asarray(case["query"]["goal_cm"])[None, [0, 2]], record["position_cm"], float(record["body_yaw_deg"]))[0] / 100
    ax.plot(goal[0], goal[1], "*", color="#d79700", markersize=11, markeredgecolor="#8e6500", markeredgewidth=.5)
    limit = 8 if layer == "near" else 18
    ax.set(xlim=(-limit, limit), ylim=(-limit, limit), aspect="equal", xlabel="right from body, m", ylabel="forward from body, m")
    ax.set_title(f"{layer.capitalize()} memory · {step / 100:g} m/cell" + (" (zoom)" if layer == "far" else ""), fontsize=10)
    ax.grid(alpha=.13)


def draw_world(ax, record, case, trajectory, target):
    origin = trajectory[0, [1, 3]]
    route = (trajectory[:, [1, 3]] - origin) / 100
    ax.plot(route[:, 0], route[:, 1], color="#1563b2", linewidth=1.3, label="saved v3 path")
    reference = (np.asarray(case["reference"]["corners_cm"])[:, [0, 2]] - origin) / 100
    ax.plot(reference[:, 0], reference[:, 1], "--", color="#222222", linewidth=1.4, label="Detour at first saved probe")
    current = (np.asarray(case["truth"]["corners_cm"])[:, [0, 2]] - origin) / 100
    ax.plot(current[:, 0], current[:, 1], "--", color="#d16c00", linewidth=1.2, label="Detour at selected frame")
    plan = local_to_world(case["plan"]["local_plan_cm"], record["position_cm"], float(record["body_yaw_deg"]))
    if len(plan):
        plan = (plan - origin) / 100
        ax.plot(plan[:, 0], plan[:, 1], color="#7951ab", linewidth=1.7, label="selected memory plan")
    if target is not None:
        moving = (target[:, [1, 3]] - origin) / 100
        ax.plot(moving[:, 0], moving[:, 1], color="#b47d00", linewidth=1.1, alpha=.8, label="saved target path")
    body = (np.asarray(record["position_cm"])[[0, 2]] - origin) / 100
    goal = (np.asarray(case["query"]["goal_cm"])[[0, 2]] - origin) / 100
    ax.plot(body[0], body[1], "o", color="#1563b2", markersize=6)
    ax.plot(goal[0], goal[1], "*", color="#d79700", markeredgecolor="#8e6500", markeredgewidth=.5, markersize=12, label="goal at selected frame")
    ax.plot(0, 0, "s", color="#1563b2", markersize=3)
    ax.set(aspect="equal", xlabel="world X from start, m", ylabel="world Z from start, m")
    ax.set_title("World top view · complete saved paths", fontsize=10)
    ax.grid(alpha=.18)


def render(args):
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib import pyplot as plt, patches, collections, colors, lines
    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 8, "svg.fonttype": "none"})
    args.output.mkdir(parents=True, exist_ok=True)
    archive = np.load(args.bundle, allow_pickle=False)
    cases = json.loads(str(archive["metadata"]))
    for ordinal, case in enumerate(cases):
        record = archive[f"record_{ordinal}"][0]
        trajectory = archive[f"trajectory_{ordinal}"]
        target = archive[f"target_{ordinal}"] if f"target_{ordinal}" in archive else None
        columns = 4 if target is not None else 3
        figure, axes = plt.subplots(1, columns, figsize=(16.8 if columns == 4 else 13.3, 4.7), gridspec_kw=dict(width_ratios=[1, 1, 1.2] + ([1.1] if columns == 4 else [])))
        for axis, layer in zip(axes[:2], ("near", "far")):
            draw_memory(axis, record, case, layer, patches, collections, colors)
        draw_world(axes[2], record, case, trajectory, target)
        evidence = case["evidence"]
        if case["tag"].startswith("false-"):
            title = f"Memory / saved truth-corridor disagreement: {case['tag'].removeprefix('false-')}"
        elif case["tag"] == "follow":
            title = f"Following a moving target · {case['follow_fraction']:.1%} time in 2–5m (required 80%)"
        else:
            direct = np.linalg.norm(np.asarray(case["reference"]["goal_cm"]) - case["reference"]["start_cm"])
            ratio = case["reference"]["length_cm"] / direct
            title = f"Successful point detour · Detour/direct {ratio:.2f}× · v3 reached in {case['elapsed_ms'] / 1000:.1f}s"
        figure.suptitle(title + f"\n{case['id']} · decision {case['index']} · {case['query']['elapsed_ms'] / 1000:.1f}s", fontsize=11)
        if target is not None:
            axis = axes[3]
            axis.axhspan(2, 5, color="#d5eadb", label="required 2–5m")
            axis.plot(target[:, 0] / 1000, target[:, 4] / 100, color="#1563b2", linewidth=.8)
            axis.axvline(case["query"]["elapsed_ms"] / 1000, color="#7951ab", linestyle="--", linewidth=1)
            if case["first_contact_ms"] is not None:
                axis.axvline(case["first_contact_ms"] / 1000, color="#d16c00", linewidth=1, label="first ≤5m contact")
            axis.set(xlabel="saved game time, s", ylabel="distance to moving target, m", ylim=(0, max(10, float(np.quantile(target[:, 4] / 100, .99)) * 1.1)))
            axis.set_title("Target distance · full episode", fontsize=10)
            axis.grid(alpha=.18)
            axis.legend(fontsize=7)
        legend = [patches.Patch(facecolor="#ededed", edgecolor="none", label="memory unknown"),
                  patches.Patch(facecolor="#d5eadb", edgecolor="none", label="memory free"),
                  patches.Patch(facecolor="#efc5be", edgecolor="none", label="memory danger"),
                  patches.Patch(facecolor="none", edgecolor="#16834b", label="truth-free corridor cell"),
                  patches.Patch(facecolor="none", edgecolor="#b0009e", linewidth=1.5, label="memory danger / truth free"),
                  patches.Patch(facecolor="none", edgecolor="#333333", label="truth-raster blocked corridor cell"),
                  lines.Line2D([], [], color="#7951ab", label="memory plan"),
                  lines.Line2D([], [], color="#1563b2", label="complete saved v3 path"),
                  lines.Line2D([], [], color="#222222", linestyle="--", label="Detour at first saved probe"),
                  lines.Line2D([], [], color="#d16c00", linestyle="--", label="Detour at selected frame")]
        if target is not None:
            legend.append(lines.Line2D([], [], color="#b47d00", label="complete saved target path"))
        figure.legend(handles=legend, loc="lower center", bbox_to_anchor=(.5, .10), ncol=4, fontsize=6.8, frameon=False)
        delta = evidence["memory_minus_truth_ground_cm"]
        ground = evidence["truth_ground_minus_feet_cm"]
        numbers = f"Selected corridor: {evidence['false_danger_cells']}/{evidence['truth_passable_cells']} truth-free cells marked dangerous; {evidence['corridor_cells']} corridor cells total. "
        if delta["count"]:
            numbers += f"Known memory–truth ground Δ median {delta['median']:+.0f}cm (n={delta['count']}); true ground−feet median {ground['median']:+.0f}cm."
        if not case["plan"]["local_plan_cm"]:
            numbers += " Selected memory plan is empty."
        figure.text(.5, .065, numbers, ha="center", fontsize=7)
        figure.text(.5, .027, "Truth is shown ONLY on saved native cells touched by the 5–15m Detour corridor (radius 15cm). Background outside those cells is memory.", ha="center", fontsize=7)
        figure.subplots_adjust(left=.055, right=.985, top=.76, bottom=.29, wspace=.38)
        target_file = args.output / f"{case['tag']}.svg"
        figure.savefig(target_file, format="svg", metadata={"Date": None})
        target_file.write_text("\n".join(line.rstrip() for line in target_file.read_text(encoding="utf-8").splitlines()) + "\n",
                               encoding="utf-8")
        if args.preview_output is not None:
            args.preview_output.mkdir(parents=True, exist_ok=True)
            figure.savefig(args.preview_output / f"{case['tag']}.png", format="png", dpi=140)
        plt.close(figure)
        print(json.dumps(dict(event="figure", path=str(target_file), bytes=target_file.stat().st_size)), flush=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path)
    parser.add_argument("--bundle", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--preview-output", type=Path, help="optional PNG inspection copies outside git")
    parser.add_argument("--extract-only", action="store_true")
    args = parser.parse_args(argv)
    if args.extract_only:
        if args.task is None or not args.task.is_absolute() or not args.bundle.is_absolute():
            parser.error("--extract-only requires absolute --task and --bundle")
        extract(args)
    else:
        if args.output is None:
            parser.error("rendering requires --output")
        render(args)


if __name__ == "__main__":
    main()
