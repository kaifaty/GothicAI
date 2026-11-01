"""Check CSV measurements from a small session and native world transition log."""
import csv
import io
from pathlib import Path
import subprocess
import sys


SCRIPT = Path(__file__).parents[1] / "loadprobe_summary.py"


def test_session_and_world_change_csv(tmp_path):
    run = tmp_path / "runs" / "ryzen-work-NEWWORLD-chain"
    run.mkdir(parents=True)
    log = "unrelated game output\n"
    for stage, elapsed, duration, footprint, gpu, count, mb, texture_ms, extra in (
        ("session_begin", 100, -1, 100, 10, 2, 2, 100, ""),
        ("world_begin", 200, -1, 100, 10, 2, 2, 100, "world=NEWWORLD.ZEN"),
        ("world_data_read", 500, 100.6, 130, 10, 2, 2, 100, ""),
        ("vobs", 1000, 200, 170, 30, 5, 12, 500, "items=23"),
        ("scripts_startup", 4900, 321.6, 220, 50, 12, 22, 1100, "npcs=10"),
        ("session_end", 5100, -1, 210, 50, 12, 22, 1100, ""),
        ("stream_warm", 5500, -1, 235, 60, 12, 22, 1100, ""),
        ("stream_warm_5s", 10500, -1, 230, 58, 12, 22, 1100, ""),
        ("change_begin", 13000, -1, 210, 50, 12, 22, 1100, "to=OLDWORLD.ZEN"),
        ("world_begin", 13100, -1, 200, 50, 12, 22, 1100, "world=OLDWORLD.ZEN"),
        ("vobs", 13500, 90.4, 250, 70, 14, 24, 1400, "items=7"),
        ("scripts_startup", 14800, 42.3, 230, 75, 15, 25, 1600, "npcs=4"),
        ("change_end", 15000, -1, 225, 75, 15, 25, 1600, ""),
        ("world_begin", 16000, -1, 225, 75, 15, 25, 1600, "world=ADDONWORLD.ZEN"),
    ):
        log += (f"[loadprobe] stage={stage} stage_ms={duration} t_ms={elapsed} rss_mb={footprint} "
                f"footprint_mb={footprint} gpu_mb={gpu} tex_n={count} tex_mb={mb} tex_ms={texture_ms} {extra}\n")
        if stage in ("session_end", "stream_warm", "stream_warm_5s"):
            log += (f"[loadprobe-malloc] stage={stage} t_ms={elapsed} "
                    f"malloc_in_use_mb={footprint - 30.123} malloc_allocated_mb={footprint + 10.234}\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} tex_live_bytes={elapsed + 1000} "
                    f"tex_retired_bytes=32 tex_peak_bytes=32000 tex_live_n=4 tex_evicted=3 tex_reloaded=1\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} tex_full_bytes=1000 tex_tail_bytes={elapsed} "
                    "tex_promoted=5 tex_demoted=2 tex_promotion_queue=1 tex_near_tail_n=2 tex_near_tail_seconds=0.125 "
                    "tex_full_ring2_bytes=700 tex_source_hd_bytes=800 tex_source_hd_full_bytes=600\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} "
                    f"tex_landscape_bytes={elapsed + 400} tex_landscape_n=1 tex_static_bytes=300 tex_static_n=1\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} "
                    f"tex_dynamic_bytes=200 tex_dynamic_n=1 tex_other_bytes=100 tex_other_n=1\n")
    (run / "game.log").write_text(log)
    output = subprocess.check_output([sys.executable, str(SCRIPT), str(tmp_path)], text=True)
    reader = csv.DictReader(io.StringIO(output))
    rows = list(reader)
    assert len(reader.fieldnames) == 139
    assert len(rows) == 2
    assert [(row["seg"], row["world"], row["total_s"]) for row in rows] == [
        ("0", "NEWWORLD", "5.0"), ("1", "OLDWORLD", "2.0")]
    assert all(row["run"] == run.name for row in rows)
    assert [(row["tex_n"], row["tex_mb"], row["tex_s"], row["fp_end"], row["fp_peak"], row["gpu_end"])
            for row in rows] == [("12", "22", "1.1", "210", "220", "50"), ("3", "3", "0.5", "225", "250", "75")]
    assert (rows[0]["data"], rows[0]["vobs"], rows[0]["startup"], rows[0]["npcs"], rows[0]["items"]) == (
        "101", "200", "322", "10", "23")
    assert (rows[0]["d_data"], rows[0]["d_vobs"], rows[0]["d_gpu_vobs"], rows[0]["d_startup"]) == (
        "30", "40", "20", "50")
    assert (rows[1]["d_vobs"], rows[1]["d_startup"], rows[1]["d_gpu_startup"]) == ("50", "-20", "5")
    assert (rows[0]["warm_after_end_s"], rows[0]["fp_warm"], rows[0]["gpu_warm"],
            rows[0]["warm_5s_after_warm_s"], rows[0]["fp_warm_5s"], rows[0]["gpu_warm_5s"]) == (
        "0.4", "235", "60", "5.0", "230", "58")
    assert all(rows[1][key] == "" for key in (
        "warm_after_end_s", "fp_warm", "gpu_warm", "warm_5s_after_warm_s", "fp_warm_5s", "gpu_warm_5s"))
    malloc_fields = [f"malloc_{metric}_{stage}" for stage in ("end", "warm", "warm_5s")
                     for metric in ("in_use", "allocated")]
    assert [rows[0][key] for key in malloc_fields] == ["179.877", "220.234", "204.877", "245.234", "199.877", "240.234"]
    assert all(rows[1][key] == "" for key in malloc_fields)
    assert [rows[0][f"tex_live_bytes_{stage}"] for stage in ("end", "warm", "warm_5s")] == ["6100", "6500", "11500"]
    assert [rows[0][f"tex_{field}_warm_5s"] for field in (
        "full_bytes", "tail_bytes", "promoted", "demoted", "promotion_queue", "near_tail_n", "near_tail_seconds")] == [
        "1000", "10500", "5", "2", "1", "2", "0.125"]
    assert [rows[0][f"tex_{field}_warm_5s"] for field in (
        "full_ring2_bytes", "source_hd_bytes", "source_hd_full_bytes")] == ["700", "800", "600"]
    assert [rows[0][f"tex_{field}_warm_5s"] for field in (
        "retired_bytes", "peak_bytes", "live_n", "evicted", "reloaded", "landscape_bytes", "landscape_n",
        "static_bytes", "static_n", "dynamic_bytes", "dynamic_n", "other_bytes", "other_n")] == [
        "32", "32000", "4", "3", "1", "10900", "1", "300", "1", "200", "1", "100", "1"]
    texture_fields = [key for key in reader.fieldnames if key.startswith("tex_") and key.endswith(("_end", "_warm", "_warm_5s"))]
    assert all(rows[1][key] == "" for key in texture_fields)

    (run / "game.log").write_text(log.split("[loadprobe] stage=change_begin")[0])
    output = subprocess.check_output([sys.executable, str(SCRIPT), str(tmp_path)], text=True)
    assert list(csv.DictReader(io.StringIO(output))) == [rows[0]]

    (run / "game.log").write_text("\n".join(line for line in log.splitlines()
                                          if not line.startswith("[loadprobe-malloc]")))
    output = subprocess.check_output([sys.executable, str(SCRIPT), str(tmp_path)], text=True)
    old_rows = list(csv.DictReader(io.StringIO(output)))
    assert all(row[key] == "" for row in old_rows for key in malloc_fields)
    assert [{key: value for key, value in row.items() if key not in malloc_fields} for row in old_rows] == [
        {key: value for key, value in row.items() if key not in malloc_fields} for row in rows]


def test_vobs_breakdown_repeat_medians(tmp_path):
    for repeat, (duration, own_ms, inclusive_ms) in enumerate(((100, 30, 40), (300, 50, 100), (200, 70, 90))):
        run = tmp_path / f"repeat-{repeat}" / "runs" / "ryzen-work-NEWWORLD-chain"
        run.mkdir(parents=True)
        (run / "game.log").write_text(
            "[loadprobe] stage=session_begin t_ms=0 stage_ms=-1\n"
            "[loadprobe] stage=world_begin t_ms=1 stage_ms=-1 world=NEWWORLD.ZEN\n"
            f"[loadprobe-vobs] kind=type name=static_mesh count=10 ms={own_ms} inclusive_ms={inclusive_ms}\n"
            f"[loadprobe-vobs] kind=part name=mesh_gpu count=2 ms={own_ms / 2} inclusive_ms={inclusive_ms / 2}\n"
            f"[loadprobe-vobs] kind=miss_part name=mesh_gpu count=1 ms={own_ms / 4} inclusive_ms={inclusive_ms / 4}\n"
            f"[loadprobe-vobs] kind=meshes unique={repeat + 2} hits={repeat + 8} misses={repeat + 2}\n"
            f"[loadprobe] stage=vobs t_ms=400 stage_ms={duration}\n"
            "[loadprobe] stage=session_end t_ms=500 stage_ms=-1\n"
            "[loadprobe] stage=change_begin t_ms=600 stage_ms=-1\n"
            "[loadprobe] stage=world_begin t_ms=601 stage_ms=-1 world=OLDWORLD.ZEN\n"
            "[loadprobe] stage=vobs t_ms=800 stage_ms=100\n"
            "[loadprobe] stage=change_end t_ms=900 stage_ms=-1\n"
            "[loadprobe] stage=world_begin t_ms=1000 stage_ms=-1 world=ADDONWORLD.ZEN\n"
            "[loadprobe-vobs] kind=type name=static_mesh count=999 ms=999 inclusive_ms=999\n"
            "[loadprobe] stage=vobs t_ms=1100 stage_ms=999\n")
    output = subprocess.check_output([sys.executable, str(SCRIPT), str(tmp_path), "--vobs"], text=True)
    rows = list(csv.DictReader(io.StringIO(output)))
    assert len(rows) == 12
    assert all((row["world"], row["seg"], row["repeat_n"]) == ("NEWWORLD", "0", "1") for row in rows)
    types = [row for row in rows if row["kind"] == "type"]
    assert [float(row["ms"]) for row in types] == [30, 50, 70]
    assert [float(row["inclusive_ms"]) for row in types] == [40, 100, 90]
    output = subprocess.check_output([sys.executable, str(SCRIPT), "--vobs", "--median", str(tmp_path)], text=True)
    medians = {row["kind"]: row for row in csv.DictReader(io.StringIO(output))}
    assert len(medians) == 4
    assert all(row["repeat_n"] == "3" for row in medians.values())
    assert [float(medians["type"][field]) for field in ("count", "ms", "inclusive_ms", "stage_ms", "share_pct")] == [
        10, 50, 90, 200, 30]
    assert [float(medians["part"][field]) for field in ("ms", "inclusive_ms", "share_pct")] == [25, 45, 15]
    assert medians["part"]["name"] == medians["miss_part"]["name"] == "mesh_gpu"
    assert [float(medians["miss_part"][field]) for field in ("count", "ms", "inclusive_ms", "share_pct")] == [1, 12.5, 22.5, 7.5]
    assert [float(medians["meshes"][field]) for field in ("unique", "hits", "misses", "cold_miss_pct")] == [3, 9, 3, 25]
    assert medians["meshes"]["ms"] == ""
