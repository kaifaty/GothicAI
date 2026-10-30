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
        ("change_begin", 8000, -1, 210, 50, 12, 22, 1100, "to=OLDWORLD.ZEN"),
        ("world_begin", 8100, -1, 200, 50, 12, 22, 1100, "world=OLDWORLD.ZEN"),
        ("vobs", 8500, 90.4, 250, 70, 14, 24, 1400, "items=7"),
        ("scripts_startup", 9800, 42.3, 230, 75, 15, 25, 1600, "npcs=4"),
        ("change_end", 10000, -1, 225, 75, 15, 25, 1600, ""),
        ("world_begin", 11000, -1, 225, 75, 15, 25, 1600, "world=ADDONWORLD.ZEN"),
    ):
        log += (f"[loadprobe] stage={stage} stage_ms={duration} t_ms={elapsed} rss_mb={footprint} "
                f"footprint_mb={footprint} gpu_mb={gpu} tex_n={count} tex_mb={mb} tex_ms={texture_ms} {extra}\n")
    (run / "game.log").write_text(log)
    output = subprocess.check_output([sys.executable, str(SCRIPT), str(tmp_path)], text=True)
    reader = csv.DictReader(io.StringIO(output))
    rows = list(reader)
    assert len(reader.fieldnames) == 37
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
