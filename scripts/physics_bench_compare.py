#!/usr/bin/env python3
"""Summarize numeric stage 1 bench changes and physics frame timings."""

import argparse
import csv
import json
import math
import hashlib
import re
from pathlib import Path
from collections import Counter

from physics_suite import SCENARIOS
import bench_report

MISSING_HEIGHT_FLAGS = {leg + suffix: leg + flag for leg in ("l", "r", "fl", "fr")
                       for suffix, flag in (("_gy", "_has"), ("_toey", "_toehas"))}
TEXT_COLUMNS = {"seg", "an_bone", "an_layers"}


def metric_schema():
    # Current TestBench::writeHeader schema. A jointly truncated header cannot
    # establish exact agreement of the missing scalar or pose columns.
    fields = "seg,frame,t,x,y,z,rot,speed,bs,walk,occl,ik_valid,ik_on,ik_hardoff,ik_w,nlegs,bad,pelvis,chest,pelvis_target,pelvis_limit,floor".split(",")
    for leg in ("l", "r", "fl", "fr"):
        fields.extend(leg + "_" + suffix for suffix in "plant,has,gy,nx,ny,nz,toehas,toey,smooth,err,knee,tilt,roll,yaw,out,slack".split(","))
        fields.extend(leg + "_" + joint + "_" + axis for joint in ("hip", "kneea", "anka", "toea", "kneef", "ankf", "toef") for axis in ("x", "y", "z"))
    fields.extend((
        "g_on,g_src,g_w,g_err,g_yaw,g_pitch,g_yawgoal,g_pitchgoal,g_head,g_chest,g_waist,g_animyaw,g_animpitch,g_hipsyaw,g_fyaw,g_fpitch,pg_on,pg_src,pg_w,pg_err,pg_yaw,"
        "ln_on,ln_roll,ln_pitch,ln_speed,ln_turn,ln_slope,an_pop,an_bone,an_speed,an_layers,bl_on,bl_active,bl_started,bl_angle,bl_time,"
        "a_on,a_w,a_yaw,a_pitch,a_animyaw,a_animpitch,a_turn,a_bend,a_err,a_steady,a_lean,a_side,ik_crouch,m_crouch,m_peek,m_cant,m_walk,aw_w,aw_speed,aw_twist,aw_chest,aw_drop,"
        "m_stage,m_draw,m_hold,m_fatigue,m_breath,m_holding,m_short,m_disturb,m_amp,m_sway_yaw,m_sway_pitch,m_off,m_spread,m_cross,"
        "bd_w,bd_draw,bd_span,bd_travel,bd_miss,bd_follow,bd_fit,bd_offset,bd_angle,hd_hand,hd_fore,hd_arrow,bd_hold,bd_nock,rh_x,rh_y,rh_z,bd_held,bd_overgrip,bd_atmiddle,bd_nockgap,bd_square,bd_cant,bd_bowmove,bd_bowmiss,bd_back,bd_kick,bd_turn,"
        "lip_valid,lip_on,lip_t,lip_level,lip_mouth,lip_open,lip_f0,lip_f1,lip_alpha,lip_int,lip_picks,"
        "ge_on,ge_talk,ge_id,ge_mood,ge_starts,ge_cuts,ge_stops,ge_nods,ge_pitch,ge_yaw,ge_roll,ge_listen,ge_think,ge_lnods,ge_aside,"
        "bk_on,bk_w,bk_n,bk_pause,bk_shift,bk_double,face_neutral,face_friendly,face_angry,face_hostile,face_afraid,face_pain,"
        "f_on,f_bend,f_yaw,f_twist,f_head,f_hits,f_str,f_ht,hp_player,hp_subject,ai_on,ai_want,ai_sigma,ai_errx,ai_erry,ai_lead,ai_pv,ai_ps"
        ).split(","))
    return tuple(fields)


METRIC_SCHEMA = metric_schema()


def strict_json(raw, path):
    def unique(pairs):
        value = {}
        for key, item in pairs:
            if key in value:
                raise ValueError("duplicate JSON key: " + key)
            value[key] = item
        return value
    def finite(value):
        raise ValueError("nonfinite JSON constant: " + value)
    try:
        return json.loads(raw, object_pairs_hook=unique, parse_constant=finite)
    except ValueError as error:
        raise SystemExit(str(path) + ": " + str(error)) from error


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require_hash(value, label):
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{64}", value) is None:
        raise SystemExit("invalid SHA-256 provenance: " + label)


def scenario_coverage(path):
    foreach, records = [], Counter()
    indices = [None]
    for line in path.read_text().splitlines():
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        if words[0] == "foreach":
            try:
                count = int(words[{"slope": 4, "stairs": 2, "wp": 3}[words[1]]])
            except (KeyError, IndexError, ValueError) as error:
                raise SystemExit("invalid foreach coverage: " + str(path)) from error
            if count <= 0:
                raise SystemExit("empty foreach coverage: " + str(path))
            foreach.append((words[1], count))
            indices = list(range(count))
        elif words[0] == "end":
            indices = [None]
        elif words[0] == "rec":
            if len(words) < 2:
                raise SystemExit("missing record label: " + str(path))
            for index in indices:
                records[words[1].replace("$I", str(index)) if index is not None else words[1]] += 1
    return foreach, records


def validate_evidence(path, case, rows, row_count):
    """A successful manifest must be supported by its actual local artifacts."""
    if type(case.get("metric_rows")) is not int or case["metric_rows"] < 0:
        raise SystemExit("invalid declared metric row count: " + str(path))
    if case["metric_rows"] != row_count:
        raise SystemExit("CSV row count differs from manifest: " + str(path))
    if case.get("pass") is not True:
        return
    if type(case.get("returncode")) is not int or case["returncode"] != 0 or case.get("done") is not True or case.get("errors") != []:
        raise SystemExit("successful scenario has incomplete execution metadata: " + str(path))
    source = path / "scenario.bench"
    if not source.is_file() or sha(source) != case["scenario_sha256"]:
        raise SystemExit("actual scenario hash differs or source is missing: " + str(path))
    log = path / "bench.log"
    if not log.is_file() or "done" not in log.read_text().splitlines():
        raise SystemExit("successful scenario lacks actual completion log: " + str(path))
    text = log.read_text()
    expected_foreach, expected_records = scenario_coverage(source)
    actual_foreach = [(kind, int(count)) for kind, count in re.findall(r"^foreach (\w+): (\d+) spots$", text, re.MULTILINE)]
    actual_records = Counter(label for label in re.findall(r"^rec (\S+)", text, re.MULTILINE) if label != "stop")
    stops = [(label, int(count)) for label, count in re.findall(r"^rec stop (\S+) frames=(\d+)$", text, re.MULTILINE)]
    if actual_foreach != expected_foreach or actual_records != expected_records or Counter(label for label, _ in stops) != expected_records:
        raise SystemExit("actual log record/foreach coverage differs: " + str(path))
    expected_rows = Counter()
    for label, count in stops:
        if count <= 0:
            raise SystemExit("record has no numeric frames: " + str(path) + " " + label)
        expected_rows[label] += count
    if not rows or Counter(row["seg"] for row in rows.values()) != expected_rows:
        raise SystemExit("CSV segment coverage differs from completed records: " + str(path))
    for label, count in expected_rows.items():
        frames = {int(float(row["frame"])) for row in rows.values() if row["seg"] == label}
        if frames != set(range(count)):
            raise SystemExit("CSV frame coverage is incomplete: " + str(path) + " " + label)
    events = path / "events.jsonl"
    if not events.is_file():
        raise SystemExit("successful scenario lacks event evidence: " + str(path))
    event_records, event_stops, notes = Counter(), [], Counter()
    previous_time = -1
    for line in events.read_text().splitlines():
        event = strict_json(line, events)
        if not isinstance(event, dict) or not isinstance(event.get("kind"), str) or not event["kind"]:
            raise SystemExit("invalid event object/kind: " + str(events))
        if type(event.get("t")) is not int or event["t"] < previous_time or not isinstance(event.get("seg"), str) or type(event.get("frame")) is not int or event["frame"] < -1:
            raise SystemExit("missing or nonmonotonic event identity/time: " + str(events))
        previous_time = event["t"]
        if event["kind"] == "rec":
            event_records[event.get("seg")] += 1
        elif event["kind"] == "rec_stop":
            if type(event.get("frames")) is not int:
                raise SystemExit("invalid event frame count: " + str(events))
            event_stops.append((event.get("seg"), event["frames"]))
        elif event["kind"] == "note":
            if not isinstance(event.get("text"), str):
                raise SystemExit("missing note text: " + str(events))
            notes[event["text"]] += 1
    if event_records != expected_records or Counter(event_stops) != Counter(stops):
        raise SystemExit("event record coverage differs from completed log: " + str(path))
    combat_prefixes = ("arrow shot", "dice shot", "arrow dice", "damage ", "fight over", "arrow hit", "arrow missed")
    required_notes = Counter(line for line in text.splitlines() if line.startswith(combat_prefixes))
    if required_notes - notes:
        raise SystemExit("combat log lacks timestamped note evidence: " + str(path))
    combat = bench_report.combat(str(path))
    if any(type(event.get("t")) is not int for kind in ("shots", "rolls", "damage", "fights") for event in combat[kind]):
        raise SystemExit("parsed combat event lacks its timestamp: " + str(path))
    timing = case.get("physics_frame_timing")
    if not isinstance(timing, dict) or type(timing.get("frames")) is not int or timing["frames"] <= 0 or timing.get("warmup_frames") != 32:
        raise SystemExit("missing or invalid physics timing metadata: " + str(path))
    if not all(type(timing.get(q + "_ns")) in (int, float) and math.isfinite(timing[q + "_ns"]) and timing[q + "_ns"] > 0 for q in ("p50", "p95")) or timing["p95_ns"] < timing["p50_ns"]:
        raise SystemExit("invalid physics timing quantiles: " + str(path))
    trace = path / "physics-time.csv"
    if not trace.is_file():
        raise SystemExit("successful scenario lacks physics timing trace: " + str(path))
    try:
        samples = [list(map(int, line.split(","))) for line in trace.read_text().splitlines()]
    except ValueError as error:
        raise SystemExit("invalid physics timing sample: " + str(trace)) from error
    # The first field is dt, not a monotonic tick ID. Every native tick writes
    # four fields; at minimum the trace must cover all recorded metric frames.
    if len(samples) < row_count or any(len(sample) != 4 or any(value < 0 for value in sample) for sample in samples):
        raise SystemExit("invalid physics timing sample: " + str(trace))
    values = sorted(sample[1] for sample in samples[32:])
    if len(values) != timing["frames"] or not values or any(values[int(p * (len(values) - 1))] != timing[q + "_ns"] for q, p in (("p50", .5), ("p95", .95))):
        raise SystemExit("physics timing trace differs from manifest: " + str(path))


def percentile(values, p):
    values.sort()
    return values[int(p * (len(values) - 1))] if values else None


def semantic_differences(a, b, path=""):
    if isinstance(a, dict) and isinstance(b, dict):
        if a.keys() != b.keys():
            return [{"path": path, "reason": "keys differ"}]
        return [difference for key in a for difference in semantic_differences(a[key], b[key], path + "/" + str(key))]
    if isinstance(a, (list, tuple)) and isinstance(b, (list, tuple)):
        if len(a) != len(b):
            return [{"path": path, "reason": "length differs", "bullet": len(a), "jolt": len(b)}]
        return [difference for i, (x, y) in enumerate(zip(a, b)) for difference in semantic_differences(x, y, path + "/" + str(i))]
    if type(a) in (int, float) and type(b) in (int, float):
        if not math.isfinite(a) or not math.isfinite(b):
            same = (math.isnan(a) and math.isnan(b)) or a == b
        else:
            tolerance = 0 if type(a) is int and type(b) is int else 0.001 if any(key in path for key in ("draw", "sigma", "chance")) else 0.1
            delta = abs(a - b)
            same = delta <= tolerance or tolerance > 0 and math.isclose(delta, tolerance, rel_tol=1e-9, abs_tol=1e-9)
    else:
        same = a == b
    return [] if same else [{"path": path, "bullet": a, "jolt": b}]


def combat_semantics(path, combat):
    """Keep game outcomes; remove log positions and circular parser associations."""
    def normalize(value):
        if isinstance(value, dict):
            return {key: normalize(item) for key, item in value.items()
                    if key not in ("run", "line", "hit_line") and not (key == "roll" and isinstance(item, dict))}
        if isinstance(value, (list, tuple)):
            return [normalize(item) for item in value]
        return value
    result = {key: normalize(combat[key]) for key in ("shots", "rolls", "damage", "fights")}
    result["arrow_impacts"] = []
    result["damage_hp"] = []
    for line in (path / "bench.log").read_text().splitlines():
        impact = bench_report.RE_HIT.match(line)
        if impact:
            g = impact.groups()
            result["arrow_impacts"].append({"victim": g[0], "result": g[1], "hp_before": int(g[2]), "hp_after": int(g[3]), "draw": float(g[4]), "flight_cm": float(g[5]), "point": [float(v) for v in g[6:9]] if g[6] is not None else None, "by": g[9] or bench_report.HERO})
        damage = bench_report.RE_DAMAGE.match(line)
        if damage:
            result["damage_hp"].append(int(damage.group(8)) if damage.group(8) is not None else None)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bullet", type=Path)
    parser.add_argument("jolt", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--diagnostic-allow-incomplete", action="store_true", help="report failed/truncated scenarios as diagnostic evidence; never a document pass")
    args = parser.parse_args()
    manifests = [strict_json((path / "manifest.json").read_text(), path / "manifest.json") for path in (args.bullet, args.jolt)]
    if any(not isinstance(m, dict) or type(m.get("schema_version")) is not int or m["schema_version"] != 1 or not isinstance(m.get("scenarios"), dict) for m in manifests):
        raise SystemExit("invalid suite manifest schema")
    if [m.get("backend") for m in manifests] != ["bullet", "jolt"]:
        raise SystemExit("parent manifests must identify bullet then jolt")
    for field in ("save_sha256", "assets", "mode"):
        if not isinstance(manifests[0].get(field), str) or not manifests[0][field].strip() or manifests[0][field] != manifests[1].get(field):
            raise SystemExit("provenance differs or is missing: " + field)
    for m in manifests:
        for field in ("runner_sha256", "save_sha256"):
            require_hash(m.get(field), field)
        for file_field, hash_field in (("runner", "runner_sha256"), ("save", "save_sha256")):
            if isinstance(m.get(file_field), str) and Path(m[file_field]).is_file() and sha(Path(m[file_field])) != m[hash_field]:
                raise SystemExit("actual provenance file hash differs: " + file_field)
    for name in SCENARIOS:
        cases = [m.get("scenarios", {}).get(name, {}) for m in manifests]
        if any(not isinstance(case, dict) or type(case.get("pass")) is not bool for case in cases):
            raise SystemExit("invalid scenario execution status: " + name)
        if any(not case.get("pass") for case in cases) and not args.diagnostic_allow_incomplete:
            raise SystemExit("scenario missing or unsuccessful: " + name)
        if any(not case.get("scenario_sha256") for case in cases):
            raise SystemExit("scenario input provenance missing: " + name)
        for case in cases:
            require_hash(case["scenario_sha256"], name + "/scenario_sha256")
        if cases[0].get("scenario_sha256") != cases[1].get("scenario_sha256"):
            raise SystemExit("scenario inputs differ: " + name)
    all_execution = all(m.get("scenarios", {}).get(name, {}).get("pass") for m in manifests for name in SCENARIOS)
    result = {"schema_version": 1, "comparator_sha256": sha(Path(__file__).resolve()), "scope": "full_numeric" if all_execution else "diagnostic_incomplete", "all_execution_pass": all_execution, "provenance": [{key: m[key] for key in ("backend", "runner_sha256", "save_sha256", "assets", "mode")} for m in manifests], "scenarios": {}}
    title = "Numeric physics regression" if all_execution else "Numeric physics regression (incomplete diagnostic)"
    lines = ["# " + title, "", "Execution and structural/outcome agreement are separate from exact numerical agreement. Failed or truncated scenarios cannot pass the document gate. Physics timing includes public query, movement and transform work between ticks plus the tick itself; it excludes rendering and total frame cost. The document timing gate requires both Jolt quantiles to be no higher than Bullet.", "", "| Scenario | Execution B/J | Bullet/Jolt rows | Matched keys | Position p95/max cm | Structural/outcomes | All numeric exact | Bullet physics p50/p95 μs | Jolt physics p50/p95 μs | Timing gate |", "|---|---|---:|---:|---:|---|---|---:|---:|---|"]
    for name in SCENARIOS:
        files = [path / name / "metrics.csv" for path in (args.bullet, args.jolt)]
        if not all(path.exists() for path in files) and not args.diagnostic_allow_incomplete:
            raise SystemExit("missing metrics for " + name)
        rows = []
        headers = []
        csv_issues = []
        for backend_index, path in enumerate(files):
            issues = []
            csv_issues.append(issues)
            if not path.exists():
                if manifests[backend_index]["scenarios"][name].get("pass"):
                    raise SystemExit("successful scenario lacks metrics: " + str(path))
                headers.append(None)
                rows.append({})
                continue
            with path.open() as file:
                data = {}
                row_count = 0
                reader = csv.DictReader(file)
                headers.append(reader.fieldnames)
                required = {"seg", "frame", "t", "x", "y", "z", "bs", "walk", "ik_valid", "ik_on", "ik_hardoff", "nlegs", "bad", "hp_player", "hp_subject", "f_hits"}
                required.update(leg + suffix for leg in ("l", "r", "fl", "fr") for suffix in ("_has", "_toehas"))
                if not required.issubset(reader.fieldnames or []):
                    raise SystemExit("required CSV columns missing: " + str(path))
                if len(set(reader.fieldnames)) != len(reader.fieldnames):
                    raise SystemExit("duplicate CSV column: " + str(path))
                if tuple(reader.fieldnames) != METRIC_SCHEMA:
                    raise SystemExit("CSV metric schema is incomplete or differs: " + str(path))
                for row_number, row in enumerate(reader, 2):
                    row_count += 1
                    if None in row or any(value is None for value in row.values()):
                        if args.diagnostic_allow_incomplete and not manifests[backend_index]["scenarios"][name].get("pass"):
                            issues.append({"row": row_number, "reason": "invalid CSV row width; excluded from partial diagnostic"})
                            continue
                        raise SystemExit("invalid CSV row width: " + str(path))
                    if not all(math.isfinite(float(row[c])) for c in required - {"seg"}):
                        raise SystemExit("nonfinite required physical/discrete metric: " + str(path))
                    if not float(row["frame"]).is_integer() or float(row["frame"]) < 0 or not row["seg"]:
                        raise SystemExit("invalid segment/frame identity: " + str(path))
                    for column, value in row.items():
                        if column in TEXT_COLUMNS:
                            continue
                        try:
                            number = float(value)
                        except ValueError as error:
                            raise SystemExit("invalid numeric metric: " + str(path) + " " + column) from error
                        if math.isfinite(number):
                            continue
                        # A missing foot/toe ray has the explicit -inf height marker.
                        # Other nonfinite metrics cannot establish exact agreement.
                        if column not in MISSING_HEIGHT_FLAGS or number != -math.inf or float(row[MISSING_HEIGHT_FLAGS[column]]) != 0:
                            raise SystemExit("nonfinite numeric metric: " + str(path) + " " + column)
                    key = row["seg"], row["frame"]
                    if key in data:
                        raise SystemExit("duplicate segment/frame key: " + str(path) + " " + str(key))
                    data[key] = row
                validate_evidence(path.parent, manifests[backend_index]["scenarios"][name], data, row_count)
                rows.append(data)
        if headers[0] != headers[1] and all(header is not None for header in headers):
            raise SystemExit("CSV headers differ for " + name)
        keys = rows[0].keys() & rows[1].keys()
        deltas = {}
        position = []
        nonfinite_mismatches = Counter()
        text_mismatches = Counter()
        for key in keys:
            a, b = rows[0][key], rows[1][key]
            for column in a.keys() & b.keys():
                try:
                    x, y = float(a[column]), float(b[column])
                except ValueError:
                    if a[column] != b[column]:
                        text_mismatches[column] += 1
                    continue
                if math.isfinite(x) and math.isfinite(y):
                    deltas.setdefault(column, []).append(abs(x - y))
                elif not ((math.isnan(x) and math.isnan(y)) or x == y):
                    nonfinite_mismatches[column] += 1
            position.append(math.dist([float(a[c]) for c in ("x", "y", "z")], [float(b[c]) for c in ("x", "y", "z")]))
        columns = {name: {"p50_delta": percentile(values, 0.5), "p95_delta": percentile(values, 0.95), "max_delta": max(values), "different_rows": sum(value != 0 for value in values)} for name, values in deltas.items()}
        timings = [m["scenarios"].get(name, {}).get("physics_frame_timing") for m in manifests]
        item = {"bullet_rows": len(rows[0]), "jolt_rows": len(rows[1]), "matched_row_keys": len(keys), "position_delta_cm": {"p50": percentile(position, 0.5), "p95": percentile(position, 0.95), "max": max(position) if position else None}, "columns": columns, "physics_frame_timing": {"bullet": timings[0], "jolt": timings[1]}}
        item["execution"] = {backend: m["scenarios"].get(name, {}) for backend, m in zip(("bullet", "jolt"), manifests)}
        item["csv_issues"] = dict(zip(("bullet", "jolt"), csv_issues))
        timing_ratios = {quantile: timings[1][quantile + "_ns"] / timings[0][quantile + "_ns"] for quantile in ("p50", "p95")} if all(timings) else None
        item["physics_frame_timing"]["jolt_to_bullet_ratio"] = timing_ratios
        item["physics_frame_timing"]["document_gate_pass"] = all(case.get("pass") is True for case in item["execution"].values()) and bool(timing_ratios) and all(ratio <= 1 for ratio in timing_ratios.values())
        outcomes = []
        ordered = []
        for path in (args.bullet / name, args.jolt / name):
            arrow_outcomes = Counter()
            for segment, arrows in bench_report.arrows(str(path)).items():
                for arrow in arrows:
                    arrow_outcomes[(segment, str(arrow[0]))] += 1
            combat = bench_report.combat(str(path))
            ordered.append(combat_semantics(path, combat))
            damage = Counter()
            for hit in combat["damage"]:
                damage[(str(hit.get("victim", "")), str(hit.get("zone", "")))] += 1
            events = Counter()
            event_file = path / "events.jsonl"
            if event_file.exists():
                for line in event_file.read_text().splitlines():
                    event = strict_json(line, event_file)
                    events[event.get("kind", "unknown")] += 1
            outcomes.append({"arrows": {"/".join(k): v for k, v in sorted(arrow_outcomes.items())}, "damage": {"/".join(k): v for k, v in sorted(damage.items())}, "events": dict(sorted(events.items()))})
        item["outcomes"] = {"bullet": outcomes[0], "jolt": outcomes[1], "same": outcomes[0] == outcomes[1]}
        combat_differences = semantic_differences(ordered[0], ordered[1])
        item["ordered_combat"] = {"same": not combat_differences, "difference_count": len(combat_differences), "examples": combat_differences[:200], "float_tolerances": "0.1; draw/sigma/chance0.001; integer HP/damage/event time exact"}
        item["row_coverage_equal"] = rows[0].keys() == rows[1].keys()
        item["position_threshold_diagnostics"] = {
            "scope": "reported tolerances; does not waive discrete, IK, combat or exact-value differences",
            "within_0_1cm_rows": sum(value <= 0.1 for value in position),
            "within_1cm_rows": sum(value <= 1 for value in position),
            "all_within_0_1cm": bool(position) and item["row_coverage_equal"] and max(position) <= 0.1,
            "all_within_1cm": bool(position) and item["row_coverage_equal"] and max(position) <= 1,
        }
        item["nonfinite_mismatches"] = dict(nonfinite_mismatches)
        item["text_mismatches"] = dict(text_mismatches)
        item["all_numeric_columns_exact"] = bool(keys) and item["row_coverage_equal"] and not nonfinite_mismatches and all(value["different_rows"] == 0 for value in columns.values())
        discrete = ("bs", "walk", "ik_valid", "ik_on", "ik_hardoff", "nlegs", "bad", "hp_player", "hp_subject", "f_hits") + tuple(leg + suffix for leg in ("l", "r", "fl", "fr") for suffix in ("_has", "_toehas"))
        item["discrete_metrics_same"] = all(columns.get(column, {}).get("max_delta", 0) == 0 for column in discrete)
        item["structural_and_outcome_parity"] = all(case.get("pass") for case in item["execution"].values()) and bool(keys) and item["row_coverage_equal"] and not nonfinite_mismatches and not text_mismatches and item["discrete_metrics_same"] and item["outcomes"]["same"] and item["ordered_combat"]["same"]
        result["scenarios"][name] = item
        def timing(value):
            return f"{value['p50_ns'] / 1000:.1f}/{value['p95_ns'] / 1000:.1f}" if value else "unmeasured"
        p = item["position_delta_cm"]
        p95 = f"{p['p95']:.3f}" if p["p95"] is not None else "—"
        maximum = f"{p['max']:.3f}" if p["max"] is not None else "—"
        execution = "/".join(str(case.get("pass", False)) for case in item["execution"].values())
        lines.append(f"| {name} | {execution} | {len(rows[0])}/{len(rows[1])} | {len(keys)} | {p95}/{maximum} | {item['structural_and_outcome_parity']} | {item['all_numeric_columns_exact']} | {timing(timings[0])} | {timing(timings[1])} | {item['physics_frame_timing']['document_gate_pass']} |")
    result["all_structural_and_outcome_parity"] = all(case["structural_and_outcome_parity"] for case in result["scenarios"].values())
    result["all_numeric_columns_exact"] = all(case["all_numeric_columns_exact"] for case in result["scenarios"].values())
    result["all_physics_timing_gates_pass"] = all(case["physics_frame_timing"]["document_gate_pass"] for case in result["scenarios"].values())
    result["document_bench_gate_pass"] = all_execution and result["all_structural_and_outcome_parity"] and result["all_numeric_columns_exact"]
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "bench-comparison.json").write_text(json.dumps(result, indent=2) + "\n")
    (args.output / "bench-comparison.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if result["document_bench_gate_pass"] and result["all_physics_timing_gates_pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
