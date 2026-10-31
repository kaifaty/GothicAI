#!/usr/bin/env python3
"""Render the accepted CV links and small, recorded teacher route examples.

Requires matplotlib (for example: uv run --with matplotlib scripts/nav_jump_figures.py).
Only compact JSON/text evidence is read; samples.bin and memory tensors are not used.
The map backdrop consists of named place centres, not terrain or a navigation mesh.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
from matplotlib.patches import Circle
import numpy as np


TYPES = {1: "jump", 2: "climb", 3: "drop"}
COLORS = {"jump": "#c26419", "climb": "#087f78", "drop": "#7956a1"}
SPLITS = {"train": "#66717d", "validation": "#3f7b9c", "test": "#a44755"}
SITE_STYLES = {"train": "solid", "validation": "dashed", "test": "dotted"}
EXAMPLES = (0, 182, 213, 210)
ANCHORS = {
    "XARDAS_TOWER_IN1": ("Xardas's tower", (-78, -27)),
    "CITY_MERCHANT_PATCH": ("Khorinis", (-47, -27)),
    "CRYPT_IN": ("Crypt", (20, -12)),
    "TROLLAREA_PORTAL": ("Ancient portal", (20, -12)),
    "SHIPWRECK": ("Shipwreck", (-74, 15)),
    "FARM1_OUT": ("Lobart's farm", (-72, 11)),
    "MAGECAVE": ("Mage cave", (-75, -16)),
    "PASS_ORKS": ("Pass", (18, 8)),
}


def json_rows(path: Path) -> list[dict]:
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def read_links(path: Path) -> list[dict]:
    links = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        fields = line.split()
        if len(fields) != 8:
            raise ValueError(f"Expected eight link fields at {path}:{number}")
        coords = np.asarray(fields[:6], dtype=float).reshape(2, 3)
        if not np.isfinite(coords).all():
            raise ValueError(f"Non-finite link at {path}:{number}")
        links.append({"id": int(fields[7]), "type": TYPES[int(fields[6])], "xyz": coords})
    if len({link["id"] for link in links}) != len(links):
        raise ValueError("Accepted links contain duplicate identifiers")
    return links


def point(value: str) -> np.ndarray:
    if not value.startswith("cm:"):
        raise ValueError(f"Expected a centimetre coordinate: {value}")
    return np.asarray(value[3:].split(","), dtype=float) / 100


def axes_style(ax: plt.Axes) -> None:
    ax.set_facecolor("#fbfcfd")
    ax.grid(color="#dbe1e7", linewidth=.65, alpha=.8)
    ax.set_axisbelow(True)
    ax.tick_params(labelsize=9, colors="#465160")
    for spine in ax.spines.values():
        spine.set_color("#cbd2da")


def export(fig: plt.Figure, output: Path, preview: Path | None, name: str) -> None:
    svg = output / f"{name}.svg"
    fig.savefig(svg, metadata={"Date": None}, facecolor="white")
    svg.write_text("\n".join(line.rstrip(" \t") for line in svg.read_text().splitlines()) + "\n")
    if preview:
        fig.savefig(preview / f"{name}.png", dpi=140, facecolor="white")
    plt.close(fig)


def site_numbers(sites: dict[str, list[dict]], links: list[dict]) -> list[dict]:
    lookup = {link["id"]: link for link in links}
    result = []
    for number, (_, routes) in enumerate(sites.items(), 1):
        identifiers = sorted({route["transition_id"] for route in routes})
        splits = {route["split"] for route in routes}
        if len(splits) != 1:
            raise ValueError("A route site spans dataset splits")
        centres = np.asarray([lookup[i]["xyz"].mean(axis=0) for i in identifiers])
        result.append({"number": number, "centre": centres.mean(axis=0),
                       "route": routes[0], "links": len(identifiers), "routes": len(routes)})
    return result


def separated_labels(ax: plt.Axes, sites: list[dict]) -> None:
    """Move numeric labels only; every leader ends at the measured site centre."""
    placed: list[np.ndarray] = []
    for site in sites:
        pos = site["centre"][[0, 2]]
        screen = ax.transData.transform(pos)
        candidates = [(13, 11), (13, -14), (-19, 11), (-19, -14),
                      (20, 26), (20, -28), (-30, 26), (-30, -28), (0, 40)]
        offset = max(candidates, key=lambda d: min(
            (float(np.linalg.norm(screen + d - other)) for other in placed), default=1000))
        placed.append(screen + offset)
        split = site["route"]["split"]
        ax.annotate(f"{site['number']:02d}", pos, xytext=offset, textcoords="offset points",
                    fontsize=8, fontweight="bold", color=SPLITS[split], zorder=7,
                    ha="center", va="center", bbox={"facecolor": "white", "edgecolor": "none", "pad": 1},
                    arrowprops={"arrowstyle": "-", "color": SPLITS[split], "linewidth": .6})


def render_map(links: list[dict], routes: list[dict], places: list[dict],
               output: Path, preview: Path | None) -> None:
    groups: dict[str, list[dict]] = defaultdict(list)
    for route in routes:
        groups[route["site_id"]].append(route)
    sites = site_numbers(groups, links)
    represented = {route["transition_id"] for route in routes}
    counts = Counter(link["type"] for link in links)
    fig = plt.figure(figsize=(15, 10.5))
    grid = fig.add_gridspec(1, 2, width_ratios=(1.8, 1), left=.065, right=.98,
                           bottom=.11, top=.87, wspace=.14)
    ax, table = fig.add_subplot(grid[0]), fig.add_subplot(grid[1])
    axes_style(ax)
    cloud = np.asarray([p["center"] for p in places], dtype=float) / 100
    ax.scatter(cloud[:, 0], cloud[:, 2], s=18, color="#b7c1cb", alpha=.5,
               edgecolors="none", zorder=1)
    for kind in COLORS:
        for link in (link for link in links if link["type"] == kind):
            xz = link["xyz"][:, [0, 2]]
            ax.annotate("", xz[1], xytext=xz[0], zorder=4,
                        arrowprops={"arrowstyle": "-|>", "color": COLORS[kind],
                                    "linewidth": 1.6, "mutation_scale": 6,
                                    "shrinkA": 0, "shrinkB": 0})
            middle = xz.mean(axis=0)
            ax.scatter(*middle, marker="o" if link["id"] <= 99 else "^", s=19,
                       facecolor="white" if link["id"] <= 99 else COLORS[kind],
                       edgecolor=COLORS[kind], linewidth=.8, zorder=4)
    for site in sites:
        split = site["route"]["split"]
        ax.add_patch(Circle(site["centre"][[0, 2]], 14, fill=False, edgecolor=SPLITS[split],
                            linestyle=SITE_STYLES[split], linewidth=1.1, zorder=3))
    lookup = {p["id"]: p for p in places}
    for identifier, (name, offset) in ANCHORS.items():
        if identifier not in lookup:
            continue
        xyz = np.asarray(lookup[identifier]["center"]) / 100
        ax.annotate(name, xyz[[0, 2]], xytext=offset, textcoords="offset points",
                    fontsize=9, color="#495563", zorder=2,
                    arrowprops={"arrowstyle": "-", "color": "#a6b0bc", "linewidth": .6},
                    bbox={"facecolor": "white", "edgecolor": "none", "pad": 1, "alpha": .85})
    all_xz = np.concatenate([cloud[:, [0, 2]], *(link["xyz"][:, [0, 2]] for link in links)])
    lo, hi = all_xz.min(axis=0) - 55, all_xz.max(axis=0) + 55
    ax.set_xlim(lo[0], hi[0])
    ax.set_ylim(lo[1], hi[1])
    ax.set_aspect("equal", adjustable="box")
    ax.set_xlabel("World X (m)")
    ax.set_ylabel("World Z (m)")
    fig.canvas.draw()
    separated_labels(ax, sites)
    table.axis("off")
    handles = [Line2D([], [], color=COLORS[k], linewidth=2.5,
                      label=f"{k.capitalize()}: {counts[k]}") for k in COLORS]
    handles.append(Line2D([], [], marker="o", linestyle="none", color="#b7c1cb",
                          label=f"Named place centres: {len(places)}", markersize=5))
    legacy_count = sum(link["id"] <= 99 for link in links)
    handles.extend([
        Line2D([], [], marker="o", linestyle="none", markerfacecolor="white", color="#66717d",
               label=f"Rechecked legacy: {legacy_count}", markersize=5),
        Line2D([], [], marker="^", linestyle="none", color="#66717d",
               label=f"Newly discovered: {len(links) - legacy_count}", markersize=5),
    ])
    table.legend(handles=handles, loc="upper left", bbox_to_anchor=(0, 1.01),
                 frameon=False, fontsize=8.5, borderaxespad=0, labelspacing=.8, ncol=2,
                 columnspacing=1, handlelength=1.7)
    table.text(0, .83, f"{len(sites)} route sites / {len(represented)} accepted links", fontsize=11,
               fontweight="bold", transform=table.transAxes)
    table.text(0, .80, "Rings: train solid / validation dashed / test dotted", fontsize=8.4,
               color="#495563", transform=table.transAxes)
    table.text(0, .767, "Site    split    region                  links / routes", fontsize=9,
               fontfamily="DejaVu Sans Mono", transform=table.transAxes, color="#495563")
    for row, site in enumerate(sites):
        route = site["route"]
        split = {"train": "train", "validation": "valid", "test": "test"}[route["split"]]
        line = (f"{site['number']:02d}      {split:<5}    {route['region']:<19}"
                f"{site['links']:>3} / {site['routes']:<3}")
        table.text(0, .732 - row * .026, line, fontsize=8.7,
                   fontfamily="DejaVu Sans Mono", color=SPLITS[route["split"]], transform=table.transAxes)
    table.text(0, .035, f"{len(links) - len(represented)} accepted links have no qualified route.\n"
               "They are plotted, without an inferred site or split.", fontsize=9,
               color="#495563", transform=table.transAxes, linespacing=1.5)
    fig.suptitle(f"Schematic XZ map: {len(links)} CV-accepted directed links", x=.065, ha="left",
                 fontsize=22, fontweight="bold", y=.965)
    fig.text(.065, .91, "NEWWORLD.ZEN  |  top-down X/Z coordinates  |  arrowheads indicate link direction",
             fontsize=11, color="#495563")
    fig.text(.065, .045, "Grey cloud = measured named-place centres only; no terrain, walls or ground surface are inferred.\n"
             "Rings locate route-backed site centroids (display radius 14 m); labels are offset for readability.",
             fontsize=10, color="#495563", linespacing=1.5)
    export(fig, output, preview, "map")


def render_example(route: dict, places: dict[str, dict], root: Path,
                   output: Path, preview: Path | None) -> dict:
    index = route["index"]
    directory = root / "episodes" / f"{index:04d}"
    episode = json.loads((directory / "episode.json").read_text())
    trajectory = np.loadtxt(directory / "trajectory.txt", ndmin=2)
    if not episode.get("transition_success"):
        raise ValueError(f"Example {index} does not have a successful transition")
    time = trajectory[:, 0] / 1000
    xyz = trajectory[:, 1:4] / 100
    # Include the final frame recorded in episode.json, which can fall between 100 ms samples.
    final_time = episode["elapsed_ms"] / 1000
    final_xyz = np.asarray(episode["final_position"]) / 100
    if time[-1] < final_time:
        time = np.append(time, final_time)
        xyz = np.vstack([xyz, final_xyz])
    plan = np.asarray(route["teacher_plan"]["corners_cm"], dtype=float) / 100
    start, goal = point(route["start"]), point(route["goal"])
    link = np.asarray([route["transition_start_m"], route["transition_end_m"]])
    kind, color = route["type"], COLORS[route["type"]]
    evidence = episode["transition_evidence"]
    landing = episode["landing_metrics"].get("first_completed_landing")
    event_s = evidence["event_ms"] / 1000
    # takeoff_position_cm can refer to an earlier supported pose, especially for drops.
    # The event marker follows its timestamp on the recorded trajectory instead.
    event = np.asarray([np.interp(event_s, time, xyz[:, axis]) for axis in range(3)])
    fig = plt.figure(figsize=(11, 8.5))
    grid = fig.add_gridspec(2, 2, width_ratios=(1.15, 1), height_ratios=(1.2, 1),
                           left=.075, right=.97, top=.82, bottom=.18, wspace=.27, hspace=.3)
    ax, info, height = fig.add_subplot(grid[:, 0]), fig.add_subplot(grid[0, 1]), fig.add_subplot(grid[1, 1])
    axes_style(ax)
    ax.plot(plan[:, 0], plan[:, 2], color="#515c6b", linestyle=(0, (5, 3)),
            linewidth=2.2, marker=".", markersize=6, label="Initial qualifying teacher plan", zorder=2)
    ax.plot(xyz[:, 0], xyz[:, 2], color="#2462a8", linewidth=2.8,
            label="Recorded teacher trajectory", zorder=3)
    ax.annotate("", link[1, [0, 2]], xytext=link[0, [0, 2]], zorder=4,
                arrowprops={"arrowstyle": "-|>", "color": color, "linewidth": 4,
                            "mutation_scale": 18, "shrinkA": 0, "shrinkB": 0, "alpha": .8})
    ax.scatter(*link[0, [0, 2]], s=70, facecolor="white", edgecolor=color, linewidth=2, zorder=5)
    ax.scatter(*link[1, [0, 2]], s=75, facecolor=color, edgecolor="white", linewidth=1, zorder=5)
    for p, marker, c, size, label in [
        (start, "o", "#24814f", 70, "Route start"),
        (goal, "*", "#be4351", 180, "Route goal"),
        (final_xyz, "s", "#2462a8", 65, "Teacher final pose"),
    ]:
        ax.scatter(*p[[0, 2]], marker=marker, color=c, s=size,
                   edgecolor="white", linewidth=1, zorder=7, label=label)
    ax.scatter(*event[[0, 2]], marker="D", color="#d6981c", s=38,
               edgecolor="white", linewidth=.7, zorder=6, label="Native event / first landing support")
    if landing:
        landed_xyz = np.asarray(landing["position_cm"]) / 100
        ax.scatter(*landed_xyz[[0, 2]], marker="^", color="#d6981c", s=65,
                   edgecolor="white", linewidth=.7, zorder=6)
    extent = np.vstack([xyz[:, [0, 2]], plan[:, [0, 2]], goal[[0, 2]], start[[0, 2]], link[:, [0, 2]]])
    centre = (extent.max(axis=0) + extent.min(axis=0)) / 2
    span = max(float(np.ptp(extent, axis=0).max()) * 1.4, 3.2)
    ax.set_xlim(centre[0] - span / 2, centre[0] + span / 2)
    ax.set_ylim(centre[1] - span / 2, centre[1] + span / 2)
    ax.set_aspect("equal", adjustable="box")
    ax.ticklabel_format(axis="both", useOffset=False)
    ax.set_xlabel("World X (m)")
    ax.set_ylabel("World Z (m)")
    ax.set_title("Top-down route", fontsize=11, loc="left", pad=10)
    info.axis("off")
    legend_handles, legend_labels = ax.get_legend_handles_labels()
    legend_handles.insert(2, Line2D([], [], color=color, linewidth=4))
    legend_labels.insert(2, f"Target {kind} link (directed)")
    info.legend(legend_handles, legend_labels, loc="upper left", fontsize=9,
                frameon=False, borderaxespad=0, labelspacing=.7, handlelength=2.6)
    horizontal = float(np.linalg.norm(link[1, [0, 2]] - link[0, [0, 2]]))
    delta_y = float(link[1, 1] - link[0, 1])
    distance = float(np.linalg.norm(np.diff(xyz[:, [0, 2]], axis=0), axis=1).sum())
    actions = episode["action_decisions"]
    stats = (f"Link XZ length {horizontal:.2f} m  |  endpoint delta Y {delta_y:+.2f} m\n"
             f"Recorded XZ travel {distance:.2f} m  |  duration {final_time:.2f} s\n"
             f"Native decisions: jump {actions['jump']}, climb {actions['climb']}\n"
             f"Teacher replans {episode['replans']}  |  final goal gap {episode['distance_cm'] / 100:.2f} m\n"
             f"Route success: {str(bool(episode['success'])).lower()}  |  transition success: true")
    info.text(0, -.04, stats, fontsize=9, color="#465160", linespacing=1.7, transform=info.transAxes)
    axes_style(height)
    height.plot(time, xyz[:, 1] - start[1], color="#2462a8", linewidth=2)
    height.axvline(event_s, color="#d6981c", linestyle=(0, (3, 3)), linewidth=1.2)
    height.scatter(event_s, np.interp(event_s, time, xyz[:, 1]) - start[1], color="#d6981c", marker="D", s=35, zorder=4)
    if landing:
        height.scatter(landing["elapsed_ms"] / 1000, landing["position_cm"][1] / 100 - start[1],
                       marker="^", color="#d6981c", s=60, zorder=4)
    height.axhline(link[1, 1] - start[1], color=color, linewidth=1, linestyle=":")
    height.set_xlabel("Elapsed time (s)", fontsize=9)
    height.set_ylabel("Y relative to route start (m)", fontsize=9)
    height.set_title("Recorded height (100 ms samples + final frame)", loc="left", fontsize=10)
    place = places.get(route["place_id"], {}).get("en", route["place_id"])
    fig.suptitle(f"{kind.capitalize()} / {place}", x=.075, ha="left", y=.96,
                 fontsize=17, fontweight="bold", wrap=True)
    origin = "legacy" if route["transition_id"] <= 99 else "newly discovered"
    fig.text(.075, .878, f"Episode {index:04d}  |  link {route['transition_id']} ({origin})  |  "
             f"{route['region']} / {route['split']}", fontsize=11, color="#465160")
    fig.text(.075, .025, "Dashed path = initial qualifying plan; later replans are not overlaid. Gold diamond = native event (interpolated).\n"
             "Gold triangle = first completed landing support; the dotted height line marks the target link's end Y.\n"
             "No ground geometry is inferred. Goal tolerance can permit success before the final pose reaches the goal marker.",
             fontsize=8.5, color="#465160", linespacing=1.5, va="bottom")
    name = f"episode-{index:04d}-{kind}"
    export(fig, output, preview, name)
    return {"index": index, "file": str(output / f"{name}.svg"), "link_id": route["transition_id"],
            "type": kind, "region": route["region"], "place": place, "split": route["split"],
            "link_xz_m": horizontal, "link_delta_y_m": delta_y, "recorded_xz_m": distance,
            "duration_s": final_time, "native_event_s": event_s,
            "first_completed_landing_s": landing["elapsed_ms"] / 1000 if landing else None,
            "action_decisions": actions, "replans": episode["replans"],
            "final_goal_gap_m": episode["distance_cm"] / 100}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Compact evidence directory")
    parser.add_argument("--output", type=Path, required=True, help="Absolute output directory for SVGs")
    parser.add_argument("--preview-dir", type=Path, help="Optional absolute directory for PNG previews")
    parser.add_argument("--examples", type=int, nargs="+", default=EXAMPLES)
    args = parser.parse_args()
    if not args.output.is_absolute() or (args.preview_dir and not args.preview_dir.is_absolute()):
        parser.error("Output and preview paths must be absolute")
    args.output.mkdir(parents=True, exist_ok=True)
    if args.preview_dir:
        args.preview_dir.mkdir(parents=True, exist_ok=True)
    plt.rcParams.update({"font.family": "DejaVu Sans", "svg.fonttype": "none", "svg.hashsalt": "nav-jump-figures",
                         "font.size": 10, "axes.labelcolor": "#465160"})
    links = read_links(args.input / "accepted-links-cv.txt")
    routes = json_rows(args.input / "routes" / "routes.jsonl")
    places = json.loads((args.input / "places.json").read_text())["places"]
    render_map(links, routes, places, args.output, args.preview_dir)
    selected = {route["index"]: route for route in routes}
    stats = [render_example(selected[index], {place["id"]: place for place in places},
                            args.input, args.output, args.preview_dir) for index in args.examples]
    print(json.dumps({"accepted_links": len(links), "examples": stats}, indent=2))


if __name__ == "__main__":
    main()
