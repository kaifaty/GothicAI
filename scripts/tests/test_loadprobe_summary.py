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
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} "
                    f"tex_landscape_bytes={elapsed + 400} tex_landscape_n=1 tex_static_bytes=300 tex_static_n=1\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} "
                    f"tex_dynamic_bytes=200 tex_dynamic_n=1 tex_other_bytes=100 tex_other_n=1\n")
    (run / "game.log").write_text(log)
    output = subprocess.check_output([sys.executable, str(SCRIPT), str(tmp_path)], text=True)
    reader = csv.DictReader(io.StringIO(output))
    rows = list(reader)
    assert len(reader.fieldnames) == 91
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
