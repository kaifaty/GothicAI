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
    log = "unrelated game output\n[loadprobe-host] load_1=1.25 load_5=2.5 load_15=3.75\n"
    for stage, elapsed, duration, footprint, gpu, count, mb, texture_ms, extra in (
        ("session_begin", 100, -1, 100, 10, 2, 2, 100, ""),
        ("world_begin", 200, -1, 100, 10, 2, 2, 100, "world=NEWWORLD.ZEN"),
        ("world_data_read", 500, 100.6, 130, 10, 2, 2, 100, ""),
        ("vobs", 1000, 200, 170, 30, 5, 12, 500, "items=23"),
        ("scripts_startup", 4900, 321.6, 220, 50, 12, 22, 1100, "npcs=10"),
        ("session_end", 5100, -1, 210, 50, 12, 22, 1100, ""),
        ("first_frame", 5300, 200, 220, 55, 12, 22, 1100, ""),
        ("stream_warm", 5500, -1, 235, 60, 12, 22, 1100, ""),
        ("stream_warm_5s", 10500, -1, 230, 58, 12, 22, 1100, ""),
        ("pipelines_10s", 15100, 10000, 230, 58, 12, 22, 1100, ""),
        ("change_begin", 16000, -1, 210, 50, 12, 22, 1100, "to=OLDWORLD.ZEN"),
        ("world_begin", 16100, -1, 200, 50, 12, 22, 1100, "world=OLDWORLD.ZEN"),
        ("vobs", 16500, 90.4, 250, 70, 14, 24, 1400, "items=7"),
        ("scripts_startup", 17800, 42.3, 230, 75, 15, 25, 1600, "npcs=4"),
        ("change_end", 18000, -1, 225, 75, 15, 25, 1600, ""),
        ("world_begin", 19000, -1, 225, 75, 15, 25, 1600, "world=ADDONWORLD.ZEN"),
    ):
        if stage == "scripts_startup" and elapsed == 4900:
            log += "[loadprobe-placed-items] name=placement_queries count=1 ms=800.25 inclusive_ms=800.25 bytes=0 malloc_bytes=0 footprint_bytes=0 memory_sampled=0\n"
            log += "[loadprobe] stage=placed_items stage_ms=846 t_ms=4800 rss_mb=170 footprint_mb=170 gpu_mb=30 tex_n=12 tex_mb=22 tex_ms=1100\n"
            log += "[loadprobe-overhead] startup_memory_ns=1200 startup_memory_samples=6\n"
            log += "[loadprobe-startup] name=model count=3 ms=12.25 inclusive_ms=20.5 bytes=4096 malloc_bytes=1024 footprint_bytes=-2048\n"
        if stage == "scripts_startup" and elapsed == 17800:
            log += "[loadprobe-startup] name=script count=1 ms=42.3 inclusive_ms=42.3 bytes=0 malloc_bytes=0 footprint_bytes=0 memory_sampled=0\n"
        if stage == "session_end":
            log += "[loadprobe-requests] name=model count=10 unique=3 repeated=7\n"
            log += "[loadprobe-owner] stage=session_end name=model_cpu bytes=65536\n"
        log += (f"[loadprobe] stage={stage} stage_ms={duration} t_ms={elapsed} rss_mb={footprint} "
                f"footprint_mb={footprint} gpu_mb={gpu} tex_n={count} tex_mb={mb} tex_ms={texture_ms} {extra}\n")
        if stage in ("session_end", "first_frame", "pipelines_10s"):
            pipeline_stage = "load_end" if stage == "session_end" else stage
            log += (f"[loadprobe-pipelines] stage={pipeline_stage} name=graphics count=12 ms=30.5 "
                    "post_load_count=2 post_load_ms=4.125\n")
        if stage in ("session_end", "stream_warm", "stream_warm_5s"):
            for values in (
                "vob_near=120 vob_far=800 vob_outside=900",
                "vob_created=950 vob_released=30 vob_draw_instances=1840",
                "vob_pending=2 vob_publication_ms=0.125 vob_create_ms=12.5",
                "vob_near_missing=0 vob_near_missing_seconds=0 vob_ready_ms=420.123456789",
            ):
                log += f"[loadprobe-vob-streaming] stage={stage} t_ms={elapsed} {values}\n"
            log += (f"[loadprobe-npc-tier] stage={stage} t_ms={elapsed} npc_full=37 npc_simplified=1016 "
                    "npc_to_full=4 npc_to_simplified=3 npc_full_tick_ms=0.250000 npc_simplified_tick_ms=2.125000\n")
            log += (f"[loadprobe-npc-tier] stage={stage} t_ms={elapsed} npc_full_tick_ms_total=12.500 "
                    "npc_simplified_tick_ms_total=106.250 npc_tier_ticks=50 npc_tier_mode=auto\n")
            log += (f"[loadprobe-npc-logic] stage={stage} t_ms={elapsed} npc_logic_samples=52650 "
                    "npc_logic_body_mismatch=0 npc_logic_weapon_mismatch=0 npc_logic_early=2 npc_logic_late=1\n")
            log += (f"[loadprobe-npc-logic] stage={stage} t_ms={elapsed} npc_logic_events=45 "
                    "npc_logic_event_mismatch=3 npc_logic_wall_ms=0.125\n")
            log += (f"[loadprobe-npc-logic] stage={stage} t_ms={elapsed} npc_logic_wait_early=4 "
                    "npc_logic_wait_late=3 npc_logic_reanchored=10 "
                    "npc_logic_body_samples=52640 npc_logic_weapon_samples=52500\n")
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
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} tex_level0_bytes=900 tex_level1_bytes=50 "
                    "tex_level2_bytes=40 tex_level3_bytes=0 tex_level4_bytes=0 tex_level5_bytes=10\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} tex_shortfall_n=3 "
                    "tex_shortfall_seconds=0.25 tex_loads=2\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} tex_jumps=1 tex_jumps_open=0 "
                    "tex_jump_sync_max_ms=12.5 tex_jump_zero_max_ms=40.25\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} tex_device_budget_bytes=9000 "
                    "tex_device_usage_bytes=900 tex_device_shared=0 tex_device_measured=1 tex_target_bytes=2250 "
                    "tex_target_source=1 tex_budget_bytes=2250\n")
            log += (f"[loadprobe-textures] stage={stage} t_ms={elapsed} tex_stage=2 tex_stage_deepest=3 "
                    "tex_stage_bytes0=3000 tex_stage_bytes=2000 tex_stage0_seconds=4.5 tex_stage1_seconds=0 "
                    "tex_stage2_seconds=1.5 tex_stage3_seconds=0 tex_stage4_seconds=0 tex_stage5_seconds=0\n")
    (run / "game.log").write_text(log)
    output = subprocess.check_output([sys.executable, str(SCRIPT), str(tmp_path)], text=True)
    reader = csv.DictReader(io.StringIO(output))
    rows = list(reader)
    assert len(reader.fieldnames) == 630
    assert len(rows) == 2
    assert rows[0]["load_1"] == "1.25"
    assert rows[0]["placed_items_placement_queries_ms"] == "800.25"
    assert rows[0]["placed_items_placement_queries_malloc_bytes"] == ""
    assert rows[1]["placed_items_placement_queries_ms"] == ""
    assert rows[0]["startup_memory_ns"] == "1200.0"
    assert rows[1]["startup_script_memory_sampled"] == "0.0"
    assert rows[1]["startup_script_malloc_bytes"] == rows[1]["startup_script_footprint_bytes"] == ""
    assert rows[1]["load_1"] == ""
    assert rows[0]["startup_model_ms"] == "12.25"
    assert rows[0]["startup_model_footprint_bytes"] == "-2048.0"
    assert rows[0]["requests_model_unique"] == "3.0"
    assert rows[0]["owner_model_cpu_end_bytes"] == "65536.0"
    assert rows[0]["first_frame_ms"] == "200.0"
    assert rows[0]["pipeline_pipelines_10s_graphics_post_load_ms"] == "4.125"
    assert rows[1]["startup_model_ms"] == ""
    assert rows[1]["first_frame_ms"] == ""
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
        "level0_bytes", "level1_bytes", "level2_bytes", "level5_bytes", "shortfall_n", "shortfall_seconds", "loads")] == [
        "900", "50", "40", "10", "3", "0.25", "2"]
    assert [rows[0][f"tex_{field}_warm_5s"] for field in (
        "jumps", "jumps_open", "jump_sync_max_ms", "jump_zero_max_ms")] == ["1", "0", "12.5", "40.25"]
    assert [rows[0][f"tex_{field}_warm_5s"] for field in (
        "device_budget_bytes", "target_bytes", "target_source", "stage", "stage_deepest", "stage_bytes0",
        "stage_bytes", "stage2_seconds")] == ["9000", "2250", "1", "2", "3", "3000", "2000", "1.5"]
    assert [rows[0][f"tex_{field}_warm_5s"] for field in (
        "retired_bytes", "peak_bytes", "live_n", "evicted", "reloaded", "landscape_bytes", "landscape_n",
        "static_bytes", "static_n", "dynamic_bytes", "dynamic_n", "other_bytes", "other_n")] == [
        "32", "32000", "4", "3", "1", "10900", "1", "300", "1", "200", "1", "100", "1"]
    texture_fields = [key for key in reader.fieldnames if key.startswith("tex_") and key.endswith(("_end", "_warm", "_warm_5s"))]
    assert all(rows[1][key] == "" for key in texture_fields)
    assert [rows[0][f"vob_{field}_warm"] for field in (
        "near", "far", "outside", "created", "released", "draw_instances", "pending", "publication_ms", "create_ms",
        "near_missing", "near_missing_seconds", "ready_ms")] == [
        "120", "800", "900", "950", "30", "1840", "2", "0.125", "12.5", "0", "0.0", "420.123456789"]
    assert all(rows[1][key] == "" for key in reader.fieldnames if key.startswith("vob_"))
    assert [rows[0][f"npc_{field}_warm_5s"] for field in (
        "full", "simplified", "to_full", "to_simplified", "full_tick_ms", "simplified_tick_ms",
        "full_tick_ms_total", "simplified_tick_ms_total", "tier_ticks")] == [
        "37", "1016", "4", "3", "0.25", "2.125", "12.5", "106.25", "50"]
    assert all(rows[1][key] == "" for key in reader.fieldnames if key.startswith("npc_") and key != "npcs")
    assert [rows[0][f"npc_logic_{field}_warm_5s"] for field in (
        "samples", "body_mismatch", "weapon_mismatch", "early", "late", "events", "event_mismatch", "wall_ms",
        "wait_early", "wait_late", "reanchored", "body_samples", "weapon_samples")] == [
        "52650", "0", "0", "2", "1", "45", "3", "0.125", "4", "3", "10", "52640", "52500"]

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
