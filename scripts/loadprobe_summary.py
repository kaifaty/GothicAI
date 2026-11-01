#!/usr/bin/env python3
"""Summarize <OUT>/runs/*/game.log as the load probe's measurement CSV."""
import argparse
import csv
from pathlib import Path
import re
from statistics import median
import sys


HEADER = "run,seg,world,total_s,vm,data,chunks,pack,view,physics,cloth,vobs,movable,waynet_idx,dialogs_player,startup,store_old,restore,tex_n,tex_mb,tex_s,fp_end,fp_peak,gpu_end,npcs,items,d_data,d_chunks,d_pack,d_view,d_gpu_view,d_phys,d_vobs,d_gpu_vobs,d_worldend,d_startup,d_gpu_startup".split(",")
HEADER.extend(("warm_after_end_s", "fp_warm", "gpu_warm", "warm_5s_after_warm_s", "fp_warm_5s", "gpu_warm_5s"))
HEADER.extend(f"malloc_{metric}_{stage}" for stage in ("end", "warm", "warm_5s")
              for metric in ("in_use", "allocated"))
TEXTURE_FIELDS = ["tex_live_bytes", "tex_retired_bytes", "tex_peak_bytes", "tex_live_n", "tex_evicted", "tex_reloaded"]
TEXTURE_FIELDS.extend(("tex_full_bytes", "tex_tail_bytes", "tex_promoted", "tex_demoted",
                       "tex_promotion_queue", "tex_near_tail_n", "tex_near_tail_seconds", "tex_full_ring2_bytes"))
TEXTURE_FIELDS.extend(f"tex_source_{source}_{metric}" for source in ("base", "hd", "material", "other")
                      for metric in ("bytes", "full_bytes"))
TEXTURE_FIELDS.extend(f"tex_{category}_{metric}" for category in ("landscape", "static", "dynamic", "other")
                      for metric in ("bytes", "n"))
HEADER.extend(f"{field}_{stage}" for stage in ("end", "warm", "warm_5s") for field in TEXTURE_FIELDS)
VOB_STREAMING_FIELDS = ["vob_near", "vob_far", "vob_outside", "vob_created", "vob_released", "vob_draw_instances", "vob_pending",
                        "vob_publication_ms", "vob_create_ms", "vob_near_missing", "vob_near_missing_seconds", "vob_ready_ms"]
HEADER.extend(f"{field}_{stage}" for stage in ("end", "warm", "warm_5s") for field in VOB_STREAMING_FIELDS)
VOBS_HEADER = "run,seg,world,kind,name,count,ms,inclusive_ms,stage_ms,share_pct,unique,hits,misses,cold_miss_pct,repeat_n".split(",")


def fields(text):
    row = {}
    for key, value in re.findall(r"(\w+)=(\S+)", text):
        try:
            row[key] = float(value)
        except ValueError:
            row[key] = value
    return row


def parse(path):
    rows = []
    marks = {}
    pending_vobs = []
    with open(path, errors="replace") as source:
        for line in source:
            vobs = re.search(r"\[loadprobe-vobs\] (.*)", line)
            if vobs:
                pending_vobs.append(fields(vobs.group(1)))
                continue
            match = re.search(r"\[loadprobe(-(?:malloc|textures))?\] stage=(\S+) (.*)", line)
            if not match:
                continue
            row = {"stage": match.group(2)}
            row.update(fields(match.group(3)))
            key = (row["stage"], row["t_ms"])
            if match.group(1):
                if key in marks:
                    marks[key].update(row)
            else:
                if row["stage"] == "vobs":
                    row["vobs_breakdown"] = pending_vobs
                    pending_vobs = []
                elif row["stage"] in ("session_begin", "change_begin", "world_begin"):
                    pending_vobs = []
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
                values.extend((round(mark[key], 9) if key == "tex_near_tail_seconds" else int(mark[key]))
                              if key in mark else "" for key in TEXTURE_FIELDS)
            for stage in (end["stage"], "stream_warm", "stream_warm_5s"):
                marks = [row for row in segment if row["stage"] == stage]
                mark = marks[-1] if marks else {}
                values.extend((round(mark[key], 9) if key.endswith(("_ms", "_seconds")) else int(mark[key]))
                              if key in mark else "" for key in VOB_STREAMING_FIELDS)
            yield values


def vobs_rows(out):
    for run in sorted(Path(out).glob("**/runs/*/game.log")):
        for index, segment in enumerate(segments(parse(run))):
            world = next((row["world"] for row in segment if row["stage"] == "world_begin"), "?")
            for stage in segment:
                if stage["stage"] != "vobs":
                    continue
                for detail in stage.get("vobs_breakdown", []):
                    row = {key: detail[key] for key in VOBS_HEADER if key in detail}
                    row.update(run=run.parent.name, seg=index, world=world.replace(".ZEN", ""),
                               name=detail.get("name", "meshes"), stage_ms=stage["stage_ms"], repeat_n=1)
                    if "ms" in row:
                        row["share_pct"] = row["ms"] / row["stage_ms"] * 100 if row["stage_ms"] else 0
                    if row["kind"] == "meshes":
                        requests = row["hits"] + row["misses"]
                        row["cold_miss_pct"] = row["misses"] / requests * 100 if requests else 0
                    yield row


def median_vobs_rows(rows):
    groups = {}
    keys = ("run", "seg", "world", "kind", "name")
    for row in rows:
        groups.setdefault(tuple(row[key] for key in keys), []).append(row)
    for key, repeats in groups.items():
        result = dict(zip(keys, key))
        for field in VOBS_HEADER:
            values = [row[field] for row in repeats if field in row and field not in keys]
            if values:
                result[field] = median(values)
        result["repeat_n"] = len(repeats)
        yield result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("out", nargs="?", default=".")
    parser.add_argument("--vobs", action="store_true", help="emit the vobs type/substage CSV, including nested repeat directories")
    parser.add_argument("--median", action="store_true", help="with --vobs, aggregate repeats by run, segment, world and counter")
    args = parser.parse_args()
    if args.median and not args.vobs:
        parser.error("--median requires --vobs")
    if args.vobs:
        rows = vobs_rows(args.out)
        if args.median:
            rows = median_vobs_rows(rows)
        writer = csv.DictWriter(sys.stdout, fieldnames=VOBS_HEADER, lineterminator="\n")
        writer.writeheader()
        writer.writerows({key: round(value, 6) if isinstance(value, float) else value
                          for key, value in row.items()} for row in rows)
        return
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(HEADER)
    writer.writerows(summary_rows(args.out))


if __name__ == "__main__":
    main()
