#!/usr/bin/env python3
"""Summarize <OUT>/runs/*/game.log as the load probe's measurement CSV."""
import csv
from pathlib import Path
import re
import sys


HEADER = "run,seg,world,total_s,vm,data,chunks,pack,view,physics,cloth,vobs,movable,waynet_idx,dialogs_player,startup,store_old,restore,tex_n,tex_mb,tex_s,fp_end,fp_peak,gpu_end,npcs,items,d_data,d_chunks,d_pack,d_view,d_gpu_view,d_phys,d_vobs,d_gpu_vobs,d_worldend,d_startup,d_gpu_startup".split(",")
HEADER.extend(("warm_after_end_s", "fp_warm", "gpu_warm", "warm_5s_after_warm_s", "fp_warm_5s", "gpu_warm_5s"))
HEADER.extend(f"malloc_{metric}_{stage}" for stage in ("end", "warm", "warm_5s")
              for metric in ("in_use", "allocated"))
TEXTURE_FIELDS = ["tex_live_bytes", "tex_retired_bytes", "tex_peak_bytes", "tex_live_n", "tex_evicted", "tex_reloaded"]
TEXTURE_FIELDS.extend(f"tex_{category}_{metric}" for category in ("landscape", "static", "dynamic", "other")
                      for metric in ("bytes", "n"))
HEADER.extend(f"{field}_{stage}" for stage in ("end", "warm", "warm_5s") for field in TEXTURE_FIELDS)


def parse(path):
    rows = []
    marks = {}
    with open(path, errors="replace") as source:
        for line in source:
            match = re.search(r"\[loadprobe(-(?:malloc|textures))?\] stage=(\S+) (.*)", line)
            if not match:
                continue
            row = {"stage": match.group(2)}
            for key, value in re.findall(r"(\w+)=(\S+)", match.group(3)):
                try:
                    row[key] = float(value)
                except ValueError:
                    row[key] = value
            key = (row["stage"], row["t_ms"])
            if match.group(1):
                if key in marks:
                    marks[key].update(row)
            else:
                rows.append(row)
                marks[key] = row
    return rows


def segments(rows):
    current = []
    complete = False
    for row in rows:
        if complete and row["stage"] not in ("stream_warm", "stream_warm_5s"):
            yield current
            current = []
            complete = False
        current.append(row)
        if row["stage"] in ("session_end", "change_end"):
            complete = True
    if complete:
        yield current


def get(segment, stage, key="stage_ms"):
    values = [row[key] for row in segment if row["stage"] == stage]
    return values[-1] if values else 0


def delta(segment, stage, key):
    for index, row in enumerate(segment):
        if row["stage"] == stage and index > 0:
            return row[key] - segment[index - 1][key]
    return 0


def summary_rows(out):
    for run in sorted(Path(out).glob("runs/*/game.log")):
        for index, segment in enumerate(segments(parse(run))):
            worlds = [row["world"] for row in segment if row["stage"] == "world_begin"]
            end = next(row for row in reversed(segment) if row["stage"] in ("session_end", "change_end"))
            begin = segment[0]
            loaded = segment[:segment.index(end) + 1]
            values = [run.parent.name, index, (worlds[0] if worlds else "?").replace(".ZEN", ""),
                      round((end["t_ms"] - begin["t_ms"]) / 1000, 2)]
            for stage in ("scripts_vm", "world_data_read", "mesh_chunks_read", "landscape_pack", "landscape_view",
                          "physics_static", "cloth_surfaces", "vobs", "movable_vobs", "waynet_index"):
                values.append(round(get(segment, stage)))
            values.extend((round(get(segment, "dialogs") + get(segment, "player_postinit")),
                           round(get(segment, "scripts_startup")), round(get(segment, "change_store_old")),
                           round(get(segment, "change_restore_state"))))
            values.extend((int(end["tex_n"] - (begin["tex_n"] if index else 0)),
                           round(end["tex_mb"] - (begin["tex_mb"] if index else 0)),
                           round((end["tex_ms"] - (begin["tex_ms"] if index else 0)) / 1000, 2),
                           round(end["footprint_mb"]), round(max(row["footprint_mb"] for row in loaded)),
                           round(end["gpu_mb"]), int(get(segment, "scripts_startup", "npcs")),
                           int(get(segment, "vobs", "items"))))
            values.extend(round(delta(segment, stage, key)) for stage, key in (
                ("world_data_read", "footprint_mb"), ("mesh_chunks_read", "footprint_mb"),
                ("landscape_pack", "footprint_mb"), ("landscape_view", "footprint_mb"), ("landscape_view", "gpu_mb"),
                ("physics_static", "footprint_mb"), ("vobs", "footprint_mb"), ("vobs", "gpu_mb"),
                ("world_end", "footprint_mb"), ("scripts_startup", "footprint_mb"), ("scripts_startup", "gpu_mb")))
            previous = end
            for stage in ("stream_warm", "stream_warm_5s"):
                marks = [row for row in segment if row["stage"] == stage]
                if not marks:
                    values.extend(("", "", ""))
                    previous = None
                    continue
                mark = marks[-1]
                values.extend((round((mark["t_ms"] - previous["t_ms"]) / 1000, 2) if previous else "",
                               round(mark["footprint_mb"]), round(mark["gpu_mb"])))
                previous = mark
            for stage in (end["stage"], "stream_warm", "stream_warm_5s"):
                marks = [row for row in segment if row["stage"] == stage]
                mark = marks[-1] if marks else {}
                values.extend(round(mark[key], 3) if key in mark else ""
                              for key in ("malloc_in_use_mb", "malloc_allocated_mb"))
            for stage in (end["stage"], "stream_warm", "stream_warm_5s"):
                marks = [row for row in segment if row["stage"] == stage]
                mark = marks[-1] if marks else {}
                values.extend(int(mark[key]) if key in mark else "" for key in TEXTURE_FIELDS)
            yield values


def main():
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(HEADER)
    writer.writerows(summary_rows(sys.argv[1] if len(sys.argv) > 1 else "."))


if __name__ == "__main__":
    main()
