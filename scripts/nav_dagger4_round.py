#!/usr/bin/env python3
"""Run one bounded DAgger4 training/validation phase and queue its successor."""
from __future__ import annotations

import argparse
from collections import Counter
import faulthandler
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time
from types import SimpleNamespace


ARCHIVE = Path("/mnt/shared/gothic-ai/nav-dagger4")
V4 = Path("/mnt/shared/gothic-ai/archive/nav-demos-v4-20261029")
V5 = Path("/mnt/shared/gothic-ai/nav-demos-v5")
V5_RECORDINGS = Path("/mnt/shared/gothic-ai/nav-lean-data/corpus-v5")
BASELINE = Path("/mnt/shared/gothic-ai/archive/nav-dagger3-20261030")
SCREEN_HD = Path("/mnt/shared/gothic-ai/nav-eval-screen/runs/20261030T214924Z-e9cc38adb/metrics.json")
ROUND_BASE_FINGERPRINT = "ea8c24940fb0ddcaf787e842f96babea96a5d639086fda0b69ceb4f765f77220"
ROUND_VIDEOS_FINGERPRINT = "6ec7b09259522c9da886ba4885424f9fcd5b76a611bc5376e242f0a4a2a613eb"
REPORT = "2026-10-31-dagger4-r1"
SEEDS = (18102027, 18102028, 18102029)
FAMILIES = ("point", "object", "approach", "follow_below_gate", "jump")
COUNTS = dict(point=180, object=537, approach=303, follow_below_gate=306, jump=135, safety=30)


def phases():
    result = [dict(kind="prepare", corpus="v4", start=start, stop=min(start + 100, 602))
              for start in range(0, 602, 100)]
    result += [dict(kind="prepare", corpus="v5-dagger", start=start, stop=min(start + 100, 1300))
               for start in range(0, 1300, 100)]
    result.append(dict(kind="assemble"))
    result += [dict(kind="train", epoch=epoch) for epoch in range(1, 21)]
    result.append(dict(kind="parity"))
    result += [dict(kind="screen"), dict(kind="screen-gate")]
    for family in (*FAMILIES, "safety", "detour"):
        for seeds in ((seed,) for seed in SEEDS) if family in ("object", "detour") else (SEEDS,):
            result.append(dict(kind="validate", model="r1", family=family, seeds=seeds))
    result.append(dict(kind="compare"))
    return result


def resources(phase):
    if phase["kind"] in ("validate", "screen"):
        return ["--threads", "28", "--cpu-quota", "2800%", "--mem", "20G"]
    result = ["--threads", "6", "--cpu-quota", "600%", "--mem", "16G"]
    if phase["kind"] in ("train", "parity"):
        result += ["--gpu-mem", "4G"]
    return result


def read_json(path):
    return json.loads(path.read_text())


def round_environment(task):
    base = task / "input/base"
    if (not (base / "base.json").is_file()
            or read_json(base / "base.json").get("fingerprint") != ROUND_BASE_FINGERPRINT):
        raise ValueError(f"Round-one base pin is missing or has a different fingerprint: {base}")
    settings = Path.home() / "gothic-ai/game-settings"
    if not all((settings / "system" / name).is_file() for name in ("Gothic.ini", "SystemPack.ini")):
        raise ValueError(f"Prepared round-one game settings are missing: {settings}")
    return dict(GOTHIC_DIR=str(settings), OPENGOTHIC_BASE=str(base),
        OPENGOTHIC_BASE_FINGERPRINT=ROUND_BASE_FINGERPRINT,
        OPENGOTHIC_VIDEOS_FINGERPRINT=ROUND_VIDEOS_FINGERPRINT,
        OPENGOTHIC_BASE_CACHE=str(task / "base-cache"), OPENGOTHIC_BASE_STORE=str(task / "base-store"))


def emit(event, **values):
    print(json.dumps(dict(event=event, **values)), flush=True)


def python(task):
    return task / ".venv/bin/python"


def scripts(task):
    return task / "source/scripts"


def cache(task, corpus):
    return task / "hot-cache" / corpus


def part_cache(task, corpus, start):
    return cache(task, corpus + "-parts") / f"part-{start // 100:02d}"


def weights(task, model):
    return dict(bc3=ARCHIVE / "input/bc3.navbc", hd=ARCHIVE / "smoke/policy.navbc",
                r1=ARCHIVE / "round1/run/policy.navbc")[model]


def checked_weights(task, model):
    with weights(task, model).open("rb") as stream:
        header = json.loads(stream.readline())
    memory = 3 if model == "bc3" else 4
    if (header["version"] != (2 if model == "bc3" else 3)
            or header["preprocessing"].get("navigation_memory_version", 3) != memory
            or header["preprocessing"].get("input_variant", "compact") != ("compact" if model == "bc3" else "hd")):
        raise ValueError(f"Unexpected {model} policy format, input or memory")


def run_command(command):
    subprocess.run(list(map(str, command)), check=True)


def prepare_part(task, phase):
    import nav_bc_data as data
    v4 = phase["corpus"] == "v4"
    source = V4 if v4 else V5_RECORDINGS
    manifest = source / "manifest.json" if v4 else task / "input/v5-compact.json"
    dagger = [] if v4 else [ARCHIVE / f"round1/phase-{number}" for number in range(1, 7)]
    result = data.prepare_manifest(source, part_cache(task, phase["corpus"], phase["start"]), manifest,
        input_variant="hd", dagger_sources=dagger, maneuver_links=task / "input/accepted-links.txt",
        episode_range=(phase["start"], phase["stop"]))
    expected = dict(start=phase["start"], stop=phase["stop"], total=602 if v4 else 1300)
    if result.get("preparation_range") != expected or len(result["episodes"]) != phase["stop"] - phase["start"]:
        raise ValueError("Prepared cache range differs from the fixed training population")
    emit("dagger4_prepared_part", corpus=phase["corpus"], **expected,
         decisions=sum(entry["steps"] for entry in result["episodes"]))


def assemble(task, queue):
    from nav_bc_train import aggregate_training_index, read_index
    for corpus, total, decisions in (("v4", 602, 405626), ("v5-dagger", 1300, 974444)):
        parts = [part_cache(task, corpus, start) for start in range(0, total, 100)]
        entries, seen = [], set()
        for start, path in zip(range(0, total, 100), parts):
            index = read_index(path)
            if index.get("preparation_range") != dict(start=start, stop=min(start + 100, total), total=total):
                raise ValueError(f"Incomplete cache preparation range: {path}")
            for entry in index["episodes"]:
                if entry["id"] in seen:
                    raise ValueError(f"Duplicated episode across cache parts: {entry['id']}")
                seen.add(entry["id"])
                entries.append(entry)
        if len(entries) != total or sum(entry["steps"] for entry in entries) != decisions:
            raise ValueError(f"Frozen {corpus} training totals differ")
        dagger = [entry for entry in entries if entry["source_kind"] == "dagger"]
        if corpus == "v4" and dagger:
            raise ValueError("V4 balance reference must contain only teacher episodes")
        if corpus == "v5-dagger":
            teacher_steps = sum(entry["teacher_control_steps"] for entry in dagger)
            if (len(dagger) != 600 or sum(entry["steps"] for entry in dagger) != 568845
                    or teacher_steps != 188259
                    or sum(entry["effective_steps"] for entry in dagger) != 568845 - .5 * 188259):
                raise ValueError("DAgger cache must preserve 600 episodes and teacher-control weight 0.5")
        index = aggregate_training_index(parts[0], [str(path) for path in parts[1:]], None)
        index.pop("preparation_range", None)
        index.pop("storage", None)
        index["splits"] = dict(train=dict(episodes=total, steps=decisions,
            pairs=len({tuple(entry["pair"]) for entry in entries})))
        queue.atomic_json(cache(task, corpus) / "index.json", index)
        emit("dagger4_assembled_cache", corpus=corpus, episodes=total, decisions=decisions,
             dagger_episodes=len(dagger))
    index = aggregate_training_index(cache(task, "v4"), [str(cache(task, "v5-dagger"))], str(cache(task, "v4")))
    if len(index["episodes"]) != 1902 or sum(entry["steps"] for entry in index["episodes"]) != 1380070:
        raise ValueError("Aggregate must include v4 + v5 + round 1 exactly once")
    for family, minimum in index["family_balance"]["minimum_shares"].items():
        if index["family_balance"]["weighted_shares"][family] + 1e-8 < minimum:
            raise ValueError(f"Aggregate {family} share is below the v4 floor")
    emit("dagger4_aggregate_ready", episodes=1902, decisions=1380070, family_balance=index["family_balance"])


def train_epoch(task, phase):
    epoch = phase["epoch"]
    output = ARCHIVE / "round1/run"
    history = read_json(output / "history.json") if (output / "history.json").is_file() else []
    if len(history) != epoch - 1:
        raise ValueError(f"Epoch {epoch} requires exactly {epoch - 1} completed epochs")
    command = [python(task), "-X", "faulthandler", "-B", "-I", scripts(task) / "nav_bc_train.py", "train",
        "--cache", cache(task, "v4"), "--aggregate-cache", cache(task, "v5-dagger"),
        "--balance-v4-cache", cache(task, "v4"), "--output", output,
        "--epochs", str(epoch), "--batch", "8", "--segment", "64", "--lr", ".0003",
        "--seed", "17", "--device", "cuda", "--threads", "6"]
    command += ["--init", ARCHIVE / "input/hd/last.pt"] if epoch == 1 else ["--resume"]
    run_command(command)
    history = read_json(output / "history.json")
    config = read_json(output / "training_config.json")
    initialized = config["initialization"]
    if (len(history) != epoch or initialized["reset"] or initialized["expanded"]
            or config["max_dagger_fraction"] != .5 or config["dagger_teacher_control_weight"] != .5):
        raise ValueError("Training must retain all HD tensors and the fixed DAgger weights")
    record = history[-1]
    if record["source_decisions"]["dagger"] > record["source_decisions"]["teacher"]:
        raise ValueError("Actual epoch DAgger decision share exceeds 50%")
    for family, minimum in config["family_balance"]["minimum_shares"].items():
        if record["weighted_family_shares"].get(family, 0.) + 1e-6 < minimum:
            raise ValueError(f"Actual epoch {family} share is below the v4 floor")
    checked_weights(task, "r1")
    emit("dagger4_epoch_complete", **record)


def parity(task):
    from nav_bc_data import load_training_manifest
    source = V4
    _, entries = load_training_manifest(source / "manifest.json", source)
    entry = next(value for value in entries if value["decisions"] >= 1000)
    output = ARCHIVE / "round1/run/parity"
    run_command([python(task), "-X", "faulthandler", "-B", "-I", scripts(task) / "check_nav_policy.py", "--weights", weights(task, "r1"),
        "--source", entry["directory"], "--output", output, "--steps", "1000", "--torch"])
    contract = task / "build/opengothic/gothic-navpolicy-contract"
    run_command([contract, weights(task, "r1"), output / "parity.bin.gz"])
    run_command([contract, "--memory", weights(task, "r1")])
    emit("dagger4_recorded_parity_complete", steps=1000, source=str(entry["directory"]), memory_version=4)


def screen_command(task):
    output = ARCHIVE / "round1/screen"
    command = [python(task), "-X", "faulthandler", "-B", "-I", scripts(task) / "nav_rl.py", "drive",
        "--eval-screen", task / f"source/reports/navigation/2026-10-31-eval-screen/eval-screen.jsonl",
        "--runner", task / "build/opengothic/gothic-body-runner", "--assets", os.environ["GOTHIC_DIR"],
        "--save", task / "input/save.sav", "--cache", task / "validation-cache",
        "--links", task / "input/accepted-links.txt", "--jump-links", task / "input/jump-links.txt",
        "--roads", task / "input/places.json", "--weights", weights(task, "r1"), "--output", output,
        "--physics", "cv", "--parallel", "18", "--action-mode", "sample", "--limit-ms", "30000",
        "--watchdog-seconds", "900", "--resume"]
    return command, output


def screen(task):
    checked_weights(task, "r1")
    command, output = screen_command(task)
    started = time.monotonic()
    run_command(command)
    run_command([python(task), "-X", "faulthandler", "-B", "-I", scripts(task) / "nav_rl.py", "summarize",
                 "--source", output, "--output", output])
    result = read_json(output / "metrics.json")
    if len(result["episodes"]) != 168:
        raise ValueError("Round-one screen must finish all 168 frozen episodes")
    check_episodes(task, "r1", result["episodes"])
    emit("dagger4_screen_complete", episodes=168, arrived=sum(bool(entry["success"]) for entry in result["episodes"]),
         seconds=time.monotonic() - started)


def screen_comparison(task):
    import nav_rl as rl
    from nav_eval_screen import METRICS, paired_difference
    jobs = rl.eval_screen_jobs(task / "source/reports/navigation/2026-10-31-eval-screen/eval-screen.jsonl")
    expected = {(value["family"], value["id"], seed) for _, value, seed in jobs}
    candidate = read_json(ARCHIVE / "round1/screen/metrics.json")["episodes"]
    reference = read_json(SCREEN_HD)["episodes"]
    for name, episodes in (("r1", candidate), ("hd", reference)):
        if (len(episodes) != 168 or len(expected) != 168
                or {(entry["family"], entry["id"], entry["action_seed"]) for entry in episodes} != expected):
            raise ValueError(f"{name} screen does not match the exact frozen 168 episode cohort")
    comparisons = {metric: paired_difference(candidate, reference, metric) for metric in METRICS}
    return dict(screen_gate_result(comparisons), screen_successes=dict(
        r1=sum(bool(entry["success"]) for entry in candidate), hd=sum(bool(entry["success"]) for entry in reference)))


def screen_gate_result(comparisons):
    complete = all(value["delta"] is not None and value["se"] is not None for value in comparisons.values())
    inferior = [metric for metric, value in comparisons.items() if value["delta"] is not None
        and value["se"] is not None and (value["delta"] > 2 * value["se"]
        if metric in ("harmful_fall", "water", "swimming") else value["delta"] < -2 * value["se"])]
    return dict(complete=complete, passed=complete and not inferior, inferior_metrics=inferior, comparisons=comparisons)


def finish_report(task, gate):
    history = read_json(ARCHIVE / "round1/run/history.json")
    if len(history) != 20:
        raise ValueError("The round-one report requires all 20 completed training epochs")
    controls = [read_json(ARCHIVE / f"round1/phase-{number}/control-metrics.json") for number in range(1, 7)]
    final = history[-1]
    lines = ["# DAgger4 round 1 — 2026-10-31", "",
        f"Training: {len(history)} epochs; HD initialization; batch 8; segment 64; learning rate 0.0003; seed 17.",
        f"Training loss: {history[0]['loss']} → {final['loss']}; total epoch time: {sum(epoch['seconds'] for epoch in history)} s.",
        f"Final epoch source decisions: `{json.dumps(final['source_decisions'], sort_keys=True)}`.",
        f"Final epoch weighted family shares: `{json.dumps(final['weighted_family_shares'], sort_keys=True)}`.",
        "Aggregate: v4 602/405626 + v5 700/405599 + DAgger 600/568845 (episodes/decisions).",
        "DAgger teacher-controlled decisions: 188259; loss weight 0.5; sampled DAgger decisions capped at 50%.",
        "Student: HD input without familiar; journey memory 4; sampled execution. Temperature search: not run.", "",
        "Recorded parity: 1000 v4 train steps; Torch/NumPy/C++ checks passed.", "",
        f"Screen: complete={gate['complete']}, passed={gate['passed']}; 168 frozen episodes.",
        f"Screen successes: `{json.dumps(gate['screen_successes'], sort_keys=True)}` /168; totals do not determine the gate.",
        f"HD screen reference: `{SCREEN_HD}`.", "",
        "| Criterion | R1 | HD | R1 − HD | Paired SE | Routes |",
        "|---|---:|---:|---:|---:|---:|"]
    for metric, value in gate["comparisons"].items():
        lines.append(f"| {metric} | {value['left']} | {value['right']} | {value['delta']} | {value['se']} | {value['routes']} |")
    lines += ["", "Screen noise boundary: two paired route standard errors; three seeds grouped within each route.",
              "Lower is better for harmful_fall, water and swimming; higher is better for the other criteria.",
              f"Criteria beyond the adverse boundary: {', '.join(gate['inferior_metrics']) or 'none'}.", ""]
    lines += ["DAgger collection: phase 1 is retained unchanged in the aggregate.", "",
              "| Phase | Episodes | Decisions | Teacher decisions | Teacher / all decisions, % |",
              "|---|---:|---:|---:|---:|"]
    for number, control in enumerate(controls, start=1):
        counts = control["all"]
        lines.append(f"| {number} | {counts['episodes']} | {counts['decisions']} | {counts['teacher_control_decisions']} "
                     f"| {100 * counts['teacher_control_decisions'] / counts['decisions']:.2f} |")
    reasons = ("heading", "memory_danger", "stall", "beta")
    lines += ["", "Teacher-control reasons below are percentages of all decisions in the corresponding family and phase group.", "",
              "| Phases | Family | Episodes | Decisions | Teacher decisions | Teacher, % | Heading, % | Memory danger, % | Stall, % | Beta, % |",
              "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for group, values in (("1", controls[:1]), ("2–6", controls[1:]), ("1–6", controls)):
        families = {}
        for control in values:
            for family, counts in [("all", control["all"]), *control["by_family"].items()]:
                total = families.setdefault(family, Counter())
                for key in ("episodes", "decisions", "teacher_control_decisions"):
                    total[key] += counts[key]
                for reason in reasons:
                    total[reason] += counts["by_reason"][reason]["decisions"]
        for family, counts in families.items():
            shares = " | ".join(f"{100 * counts[reason] / counts['decisions']:.2f}" for reason in reasons)
            lines.append(f"| {group} | {family} | {counts['episodes']} | {counts['decisions']} | {counts['teacher_control_decisions']} "
                         f"| {100 * counts['teacher_control_decisions'] / counts['decisions']:.2f} | {shares} |")
    lines.append("")
    lines += ["Full validation: not run; screen gate did not pass.", "", "Nav-live: not run.",
              "Chain stopped at the screen gate; full validation and nav-live were not started.",
              "Test: not run. Round 2: not started.", ""]
    content = "\n".join(lines)
    for destination in (ARCHIVE / f"reports/{REPORT}/README.md", task / f"source/reports/navigation/{REPORT}/README.md"):
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(content)
    emit("dagger4_round1_report_written", report=str(ARCHIVE / f"reports/{REPORT}/README.md"))


def paired_report_difference(left, right, metric):
    """Compare identical episode cohorts, clustering their seeds by route."""
    import numpy as np
    if metric not in ("arrival", "elapsed_s", "idle", "follow", "falls", "episodes_with_falls",
                      "harmful_fall", "water", "swimming", "unattributed_damage"):
        raise ValueError(f"Unknown paired report metric: {metric}")
    indexed = [{(ep["family"], ep["id"], ep["action_seed"]): ep for ep in values}
               for values in (left, right)]
    if (not indexed[0] or indexed[0].keys() != indexed[1].keys()
            or len(indexed[0]) != len(left) or len(indexed[1]) != len(right)):
        raise ValueError(f"Paired report cohorts differ: {metric}")
    route_keys = {}
    for key in sorted(indexed[0]):
        route_keys.setdefault(key[:2], []).append(key)
    routes, episodes = len(route_keys), len(indexed[0])
    if metric == "elapsed_s":
        widths = {len(keys) for keys in route_keys.values()}
        if len(widths) != 1 or not widths.issubset({1, 3}):
            raise ValueError("Elapsed median comparison requires equal paired seed counts per route")
        a, b = [np.asarray([[float(values[key]["elapsed_s"]) for key in keys]
                            for keys in route_keys.values()]) for values in indexed]
        value_a, value_b = float(np.median(a)), float(np.median(b))
        se = None
        if routes > 1:
            rng = np.random.default_rng(18102027)
            bootstrap = np.empty(5000)
            # Bound temporary arrays while resampling each route's seeds together.
            for start in range(0, len(bootstrap), 500):
                selected = rng.integers(routes, size=(500, routes))
                bootstrap[start:start + 500] = (np.median(a[selected].reshape(500, -1), axis=1)
                                                - np.median(b[selected].reshape(500, -1), axis=1))
            se = float(bootstrap.std(ddof=1))
        return dict(left=value_a, right=value_b, delta=value_a - value_b, se=se,
                    routes=routes, episodes=episodes)
    clusters = []
    for values in indexed:
        totals = np.zeros((routes, 2))
        for number, keys in enumerate(route_keys.values()):
            for key in keys:
                ep = values[key]
                denominator = 1.
                if metric == "arrival":
                    numerator = float(ep["transition_success"] if ep["family"] == "jump" else ep["success"])
                elif metric == "idle":
                    numerator, denominator = float(ep["action_counts"]["idle"]), float(ep["decisions"])
                elif metric == "follow":
                    numerator, denominator = float(ep["follow_in_band_ms"]), float(ep["follow_observed_ms"])
                elif metric == "falls":
                    numerator = float(ep["falls"])
                elif metric == "episodes_with_falls":
                    numerator = float(ep["falls"] > 0)
                elif metric == "harmful_fall":
                    numerator = float(ep["safety"]["harmful_fall"])
                elif metric == "unattributed_damage":
                    numerator = float(ep["safety"]["unattributed_damage"])
                else:
                    numerator = float(ep["entered_deep_water" if metric == "water" else "swimming"])
                totals[number] += (numerator, denominator)
        clusters.append(totals)
    a, b = clusters
    den_a, den_b = float(a[:, 1].sum()), float(b[:, 1].sum())
    if not den_a or not den_b:
        return dict(left=None, right=None, delta=None, se=None, routes=routes, episodes=episodes)
    rate_a, rate_b = float(a[:, 0].sum() / den_a), float(b[:, 0].sum() / den_b)
    influence = ((a[:, 0] - rate_a * a[:, 1]) / (den_a / routes)
                 - (b[:, 0] - rate_b * b[:, 1]) / (den_b / routes))
    se = float(influence.std(ddof=1) / np.sqrt(routes)) if routes > 1 else None
    scale = episodes if metric in ("falls", "unattributed_damage") else 1
    return dict(left=rate_a * scale, right=rate_b * scale, delta=(rate_a - rate_b) * scale,
                se=se * scale if se is not None else None, routes=routes, episodes=episodes)


def cleanup_runtime(task):
    for name in ("hot-cache", "validation-cache", "base-cache", "base-store", "build"):
        directory = task / name
        if directory.exists():
            shutil.rmtree(directory)
    emit("dagger4_runtime_removed", task=str(task))


def screen_gate(task):
    gate = screen_comparison(task)
    emit("dagger4_screen_gate", **gate)
    if not gate["passed"]:
        finish_report(task, gate)
        cleanup_runtime(task)
        if not gate["complete"]:
            raise ValueError("Screen is incomplete: every criterion needs an observed denominator and paired SE")
        return False
    return True


def validation_command(task, phase):
    family = phase["family"]
    output = ARCHIVE / "round1" / ("detour-validation" if family == "detour" else "validation") / phase["model"]
    command = [python(task), "-X", "faulthandler", "-B", "-I", scripts(task) / "nav_rl.py", "drive"]
    if family == "detour":
        command += ["--detour-scenarios", task / "input/detour-val.jsonl"]
    else:
        command += ["--manifest", V5 / "manifest.json", "--lean", "--families", "point" if family == "safety" else family]
        if family == "safety":
            command += ["--indices", "0", "--safety-fixtures", task / "input/fixtures.json", "--fence-control"]
        if family == "jump":
            command += ["--jump-links", task / "input/jump-links.txt"]
    command += ["--runner", task / "build/opengothic/gothic-body-runner", "--assets", os.environ["GOTHIC_DIR"],
        "--save", task / "input/save.sav", "--cache", task / "validation-cache",
        "--links", task / "input/accepted-links.txt", "--roads", task / "input/places.json",
        "--weights", weights(task, phase["model"]), "--output", output, "--physics", "cv", "--parallel", "18",
        "--action-mode", "sample", "--action-seeds", ",".join(map(str, phase["seeds"])),
        "--limit-ms", "240000" if family == "detour" else "30000", "--watchdog-seconds", "900", "--resume"]
    return command, output


def expected_cohort(task, family, seeds):
    import nav_rl as rl
    if family == "detour":
        scenarios = rl.detour_scenarios(task / "input/detour-val.jsonl")
    elif family == "safety":
        scenarios = rl.safety_scenarios(SimpleNamespace(safety_fixtures=task / "input/fixtures.json", fence_control=True, limit_ms=30000))
    else:
        scenarios = rl.validation_scenarios(V5 / "manifest.json", family)
    rejections = ARCHIVE / "round1/validation/setup-rejections"
    rejected = {read_json(path)["id"] for path in rejections.glob("*.json")}
    return {(value["id"], seed) for value in scenarios if value["id"] not in rejected for seed in seeds}


def check_episodes(task, model, episodes):
    memory = 3 if model == "bc3" else 4
    for episode in episodes:
        directory = Path(episode["episode"])
        metadata = read_json(directory / "episode.json")
        if (episode["teacher_control_decisions"] != 0 or episode["recording"] != "text_only"
                or metadata["navigation_memory_version"] != memory or metadata["demo_version"] != (2 if model == "bc3" else 3)
                or metadata["observation_source"] != "learner_memory" or metadata["record"] is not False
                or metadata["teacher_beta"] != 0 or metadata["action_mode"] != "sample"
                or metadata["weights"] != str(weights(task, model)) or any(directory.glob("samples.bin*"))):
            raise ValueError(f"Lean {model} validation identity differs: {directory}")


def validate(task, phase):
    checked_weights(task, phase["model"])
    command, output = validation_command(task, phase)
    run_command(command)
    summary = [python(task), "-X", "faulthandler", "-B", "-I", scripts(task) / "nav_rl.py", "summarize", "--source", output, "--output", output]
    if phase["family"] != "detour":
        summary += ["--object-probes", task / "input/object-probes.jsonl"]
    run_command(summary)
    result = read_json(output / "metrics.json")
    episodes = [entry for entry in result["episodes"] if entry["family"] == phase["family"] and entry["action_seed"] in phase["seeds"]]
    expected = expected_cohort(task, phase["family"], phase["seeds"])
    if len(episodes) != len(expected) or {(entry["id"], entry["action_seed"]) for entry in episodes} != expected:
        raise ValueError("Validation cohort differs from its frozen scenarios and sampling seeds")
    target = (363 if phase["family"] == "detour" else COUNTS[phase["family"]]) * len(phase["seeds"]) // 3
    if len(episodes) != target or (phase["family"] != "detour" and len(result["setup_rejections"]) != 2):
        raise ValueError("Validation episode/setup rejection totals differ")
    check_episodes(task, phase["model"], episodes)
    emit("dagger4_validation_phase_complete", **phase, episodes=len(episodes), arrived=sum(bool(entry["success"]) for entry in episodes))


def compare(task):
    results, identity = {}, {}
    for model in ("bc3", "hd", "r1"):
        if model == "r1":
            normal = read_json(ARCHIVE / "round1/validation/r1/metrics.json")
            detour = read_json(ARCHIVE / "round1/detour-validation/r1/metrics.json")
        else:
            normal = read_json(BASELINE / f"hd-validation/{model}/metrics.json")
            detour = read_json(BASELINE / ("hd-detour/metrics.json" if model == "hd" else "detour-baseline/bc3/metrics.json"))
        if len(normal["episodes"]) != 1491 or len(detour["episodes"]) != 363 or len(normal["setup_rejections"]) != 2:
            raise ValueError(f"Incomplete {model} validation")
        for name, value in (("validation", normal), ("detour", detour)):
            pairs = {(entry["family"], entry["id"], entry["action_seed"]) for entry in value["episodes"]}
            if len(pairs) != len(value["episodes"]) or name in identity and pairs != identity[name]:
                raise ValueError("Validation models do not share identical paired cohorts")
            identity[name] = pairs
            if any(entry["teacher_control_decisions"] != 0 or entry["recording"] != "text_only" for entry in value["episodes"]):
                raise ValueError("Paired validation requires lean episodes without teacher control")
            if model == "r1":
                check_episodes(task, model, value["episodes"])
        fingerprints = {normal.get("base_fingerprint"), detour.get("base_fingerprint")}
        results[model] = dict(base_fingerprint=fingerprints.pop() if len(fingerprints) == 1 else None,
            point=normal["by_family"]["point"]["arrived"],
            object=normal["by_family"]["object"]["arrived"], detour=detour["all"]["arrived"],
            by_family=normal["by_family"])
    losses = {name: results["hd"][name] - results["r1"][name] for name in ("point", "detour")}
    gate = screen_comparison(task)
    if not gate["passed"]:
        raise ValueError("Completed full validation must retain its passed screen gate")
    result = dict(base_fingerprint=results["r1"]["base_fingerprint"], models=results, decline_vs_hd=losses,
        automatically_start_round2=False, report_pending_live=True, next_action="nav-live.sh --round dagger4-r1")
    destination = ARCHIVE / "round1/compare.json"
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(dict(event="dagger4_round1_validation_complete", **result), indent=2) + "\n")
    emit("dagger4_round1_validation_complete", **result)
    cleanup_runtime(task)


def final_report(task, live_output=None):
    import numpy as np
    import nav_live

    destination = ARCHIVE / f"reports/{REPORT}/README.md"
    previous = destination.read_text()
    prefix = re.split(r"^## (?:Завершённые части полного validation|Полный validation)$",
                      previous, maxsplit=1, flags=re.M)[0]
    collection = "## Сбор r1 и управление учителем" + previous.split("## Сбор r1 и управление учителем", 1)[1]
    prefix = prefix.replace(
        "Исторические HD screen/validation/detour не сохраняют fingerprint базы в доступных результатах; их runtime-кэши отсутствуют. Тождество их базы базе r1 напрямую не подтверждено. Готовые результаты HD читаются, повторного запуска HD не было.",
        "База прошлых прогонов HD screen/validation/detour не записана. По решению пользователя HD не повторяется; используются готовые результаты.")
    normal, detour = {}, {}
    for model in ("r1", "hd", "bc3"):
        normal[model] = read_json(ARCHIVE / "round1/validation/r1/metrics.json" if model == "r1"
                                  else BASELINE / f"hd-validation/{model}/metrics.json")
        detour[model] = read_json(ARCHIVE / "round1/detour-validation/r1/metrics.json" if model == "r1"
                                  else BASELINE / ("hd-detour/metrics.json" if model == "hd" else "detour-baseline/bc3/metrics.json"))
        if len(normal[model]["episodes"]) != 1491 or len(detour[model]["episodes"]) != 363:
            raise ValueError(f"Final report requires complete {model} validation")
    comparison = read_json(ARCHIVE / "round1/compare.json")
    if any(comparison["models"][model]["by_family"] != normal[model]["by_family"] for model in normal):
        raise ValueError("Final report differs from the completed compare artifact")
    lines = ["## Полный validation", "",
        "Каждая модель: 1491/1491 эпизод; три action seed 18102027/18102028/18102029. HD/BC3 переиспользованы. "
        "Медиана времени включает все эпизоды, в том числе завершившиеся по лимиту. Idle — доля решений без движения, поворота, jump/climb.", "",
        "| Семейство | Модель | n | arrived | Доля, % | Медиана, с | Idle, % |",
        "|---|---|---:|---:|---:|---:|---:|"]
    families = (*FAMILIES, "safety")
    for family in families:
        for model in normal:
            value = normal[model]["by_family"][family]
            lines.append(f"| {family} | {model} | {value['n']} | {value['arrived']} | "
                         f"{100 * value['arrival_fraction']:.6f} | {value['median_elapsed_s']:.6f} | {100 * value['idle_fraction']:.6f} |")
    lines += ["", "### Detour-val", "", "| Модель | n | arrived | Доля, % | Медиана, с | Idle, % |",
              "|---|---:|---:|---:|---:|---:|"]
    for model, result in detour.items():
        value = result["all"]
        lines.append(f"| {model} | {value['n']} | {value['arrived']} | {100 * value['arrival_fraction']:.6f} | "
                     f"{value['median_elapsed_s']:.6f} | {100 * value['idle_fraction']:.6f} |")
    lines += ["", "### Follow и safety", "",
        "Follow: суммарное время в полосе 2–5 м / суммарное наблюдаемое время после первого сближения.", "",
        "| Модель | В полосе, мс | Наблюдаемое время, мс | Доля, % |",
        "|---|---:|---:|---:|"]
    for model, result in normal.items():
        value = result["by_family"]["follow_below_gate"]
        lines.append(f"| {model} | {value['follow_in_band_ms']:.0f} | {value['follow_observed_ms']:.0f} | {100 * value['follow_fraction']:.6f} |")
    lines += ["", "| Модель | Падений | Эпизодов с падением /30 | Вредных падений /30 | Глубокая вода /9 | Плавание /9 | unattributed_damage, HP |",
              "|---|---:|---:|---:|---:|---:|---:|"]
    for model, result in normal.items():
        value = result["by_family"]["safety"]
        lines.append(f"| {model} | {value['falls']} | {value['episodes_with_falls']} | {value['harmful_fall_episodes']} | "
                     f"{value['deep_water_episodes']} | {value['swimming_episodes']} | {value['unattributed_damage']} |")

    def number(value):
        return "—" if value is None else f"{value:.6f}"

    def difference(label, left, right, metric, scale=1):
        value = paired_report_difference(left, right, metric)
        delta, se = value["delta"], value["se"]
        z = ("—" if se is None or delta is None else f"{delta / se:.3f}" if se else
             "0" if not delta else "+∞" if delta > 0 else "−∞")
        return f"| {label} | {number(None if delta is None else scale * delta)} | {number(None if se is None else scale * se)} | {z} | {value['routes']} |"

    lines += ["", "### Парные различия r1 − HD", "",
        "SE долей, сумм и отношений: delta method по парным маршрутам, три seed сгруппированы внутри маршрута. "
        "SE разности медиан: 5000 парных bootstrap-выборок маршрутов, seed 18102027. "
        "Число SE — знаковая разность / SE; при нулевых разности и SE указано 0. п.п. — процентные пункты.", "",
        "| Метрика | r1 − HD | Парный SE | Число SE | Маршрутов |", "|---|---:|---:|---:|---:|"]
    for family in (*families, "detour"):
        source = detour if family == "detour" else normal
        left, right = ([row for row in source[model]["episodes"] if row["family"] == family] for model in ("r1", "hd"))
        for metric, label, scale in (("arrival", "arrived, п.п.", 100), ("elapsed_s", "медиана времени, с", 1), ("idle", "idle, п.п.", 100)):
            lines.append(difference(f"{family}: {label}", left, right, metric, scale))
    following = [[row for row in normal[model]["episodes"] if row["family"] == "follow_below_gate"] for model in ("r1", "hd")]
    lines.append(difference("follow: время в полосе, п.п.", *following, "follow", 100))
    safety = [[row for row in normal[model]["episodes"] if row["family"] == "safety"] for model in ("r1", "hd")]
    for metric, label, scale in (("falls", "safety: падений", 1), ("episodes_with_falls", "safety: эпизоды с падением, п.п.", 100),
                                 ("harmful_fall", "safety: вредные падения, п.п.", 100), ("unattributed_damage", "safety: unattributed_damage, HP", 1)):
        lines.append(difference(label, *safety, metric, scale))
    water = [[row for row in values if row["region"] == "water"] for values in safety]
    for metric in ("water", "swimming"):
        lines.append(difference(f"safety: {metric}, п.п.", *water, metric, 100))

    baseline_live = Path("/mnt/shared/gothic-ai/nav-live/baseline")
    baseline_marker = read_json(baseline_live / "complete.json")
    live_output = Path(live_output) if live_output is not None else Path("/mnt/shared/gothic-ai/nav-live/dagger4-r1")
    suite = read_json(live_output / "qualified-suite.json" if (live_output / "qualified-suite.json").is_file()
                      else Path(baseline_marker["suite_path"]))
    cases = nav_live.eligible_cases(suite, nav_live.suite_cases(suite))
    expected = sum(len(case["seeds"]) for case in cases)
    if len(cases) != 11 or expected != 27:
        raise ValueError("The final live report requires the frozen eleven places /27 runs")
    lines += ["", "## Живой набор: r1 против HD", "",
        "Заморожены 11 мест; план — 27 записей r1. Три позиции routine-return имеют по одному разному seed; остальные восемь мест — по три. "
        "Поэтому замороженный план содержит 27, а не 33 поездки. №4 и лестница №9 остаются исключёнными; follow сохраняет пометку «эталон не квалифицирован».", "",
        "В ячейках: приезды/n; медиана времени; медиана пути / пути учителя; сумма контактов; сумма падений. "
        "Для follow дополнительно время в полосе / весь горизонт, включая исходный разрыв и паузы цели. "
        "Видео: медианный и худший seed по прежнему порядку baseline (приезд/время; для follow — полоса/дистанция/стояние).", "",
        "| Место | r1 | HD |", "|---|---|---|"]
    completed = 0
    live_differences = []

    def live_rows(case, model):
        directory = live_output / model / case["id"]
        if model == "HD" and not directory.exists():
            directory = baseline_live / model / case["id"]
        return [(seed, directory / f"seed-{seed}", read_json(directory / f"seed-{seed}/summary.json"))
                for seed in case["seeds"] if (directory / f"seed-{seed}/summary.json").is_file()]

    def live_cell(rows, following):
        if not rows:
            return "не записано"
        values = [value for _, _, value in rows]
        times = [value["elapsed_seconds"] for value in values]
        ratios = [value["path_to_teacher_ratio"] for value in values]
        contacts = sum(value["obstacle_contacts"] for value in values)
        falls = sum(value["falls"] for value in values) if all(value["falls"] is not None for value in values) else None
        cell = (f"{sum(value['success'] for value in values)}/{len(rows)}; {np.median(times):.3f} с; "
                f"путь ×{np.median(ratios):.6f}; контактов {contacts}; падений {falls if falls is not None else '—'}")
        if following:
            fraction = sum(value["follow_band_fraction"] * value["elapsed_seconds"] for value in values) / sum(times)
            cell += f"; полоса {100 * fraction:.6f}%"
        ranked = sorted(rows, key=(lambda item: (-item[2]["follow_band_fraction"], item[2]["follow_mean_distance_m"], item[2]["standing_seconds"], item[0]))
                        if following else lambda item: (not item[2]["success"], item[2]["elapsed_seconds"], item[0]))
        median, worst = ranked[len(ranked) // 2], ranked[-1]
        cell += f"; [медианный seed {median[0]}]({median[1]}/source.mp4)"
        if worst[0] != median[0]:
            cell += f"; [худший {worst[0]}]({worst[1]}/source.mp4)"
        return cell

    for case in cases:
        left, right = live_rows(case, "dagger4-r1"), live_rows(case, "HD")
        completed += len(left)
        following = case["goal_kind"] == "follow"
        lines.append(f"| {case['id']} | {live_cell(left, following)} | {live_cell(right, following)} |")
        if len(left) != len(case["seeds"]) or len(right) != len(left):
            continue
        if any(value.get("base_fingerprint") != ROUND_BASE_FINGERPRINT for _, _, value in left):
            raise ValueError("Fresh live R1 summaries must carry the pinned base fingerprint")
        live_metrics = [("arrival", "приезд, п.п.", 100), ("elapsed_s", "медиана времени, с", 1),
                        ("path", "медиана пути ×", 1), ("contacts", "контакты, сумма", 1), ("falls", "падения, сумма", 1)]
        if following:
            live_metrics.append(("follow", "полоса, п.п.", 100))
        for metric, label, scale in live_metrics:
            converted = []
            for rows in (left, right):
                values = []
                for seed, _, value in rows:
                    values.append(dict(family="live", id=str(seed), action_seed=seed, success=value["success"],
                        elapsed_s=value["path_to_teacher_ratio"] if metric == "path" else value["elapsed_seconds"],
                        falls=value["obstacle_contacts"] if metric == "contacts" else value["falls"],
                        follow_observed_ms=value["elapsed_seconds"] * 1000,
                        follow_in_band_ms=(value["follow_band_fraction"] or 0) * value["elapsed_seconds"] * 1000))
                converted.append(values)
            measure = "elapsed_s" if metric == "path" else "falls" if metric == "contacts" else metric
            live_differences.append(difference(f"{case['id']}: {label}", *converted, measure, scale))
    lines += ["", f"R1: записано {completed}/{expected} замороженных поездок (целевое 33; шесть дополнительных seed не входят в заморозку). "
        "Исторические HD/it35/BC4 и учитель переиспользуются; их база не записана.", ""]
    smoke_runs = len(list((live_output / "smoke").rglob("episode.json")))
    lines += [f"Отдельные проверки r1 smoke: {smoke_runs}/3 прогона; план — две записи и один контроль без записи. "
              "В число 27 поездок набора они не входят.", ""]
    if live_differences:
        lines += ["### Парные различия живого набора r1 − HD", "",
            "SE условны на фиксированное место: парные action seed, а не независимые места. Для трёх routine-return с одним seed SE не определён. "
            "Медианы: 5000 парных bootstrap-выборок seed; остальные метрики — парная SE среднего/отношения, для сумм масштабированная на n.", "",
            "| Метрика | r1 − HD | Парный SE | Число SE | Пар seed |", "|---|---:|---:|---:|---:|", *live_differences, ""]
    prefix = re.sub(r"^Состояние:.*$", f"Состояние: 20 эпох и parity завершены; screen 168/168, гейт пройден. "
                    f"Полный validation 1491/1491 и detour-val 363/363 завершены. Живой r1: {completed}/{expected}. Test и раунд 2 не запускались.",
                    prefix, count=1, flags=re.M)
    content = prefix + "\n".join(lines) + "\n" + collection
    if len(content.encode()) > 1_500_000:
        raise ValueError("Round-one report exceeds 1.5 MB")
    for path in (destination, task / f"source/reports/navigation/{REPORT}/README.md"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
    comparison["report_pending_live"] = completed != expected
    if completed == expected:
        comparison["next_action"] = ""
    (ARCHIVE / "round1/compare.json").write_text(json.dumps(comparison, ensure_ascii=False, indent=2) + "\n")
    emit("dagger4_final_report_written", live_completed=completed, live_expected=expected, report=str(destination))


def enqueue(task, step, request, queue):
    identifier = None
    try:
        queue.disk_check()
        command = [str(queue.GQ), "submit", "--class", "batch", *resources(phases()[step]), "--max", "2h",
            "--name", f"nav dagger4 round1 step {step + 1}/{len(phases())} {phases()[step]['kind']}", "--",
            str(python(task)), "-X", "faulthandler", "-I", str(scripts(task) / "nav_job.py"), "--max-seconds", "7200", "--",
            str(python(task)), "-X", "faulthandler", "-B", "-I", str(Path(__file__).resolve()), "--task", str(task), "--step", str(step)]
        result = subprocess.run(command, check=True, capture_output=True, text=True,
                                env={**os.environ, "GQ_OWNER": "nav-dagger4"})
        identifier = result.stdout.strip()
        if not re.fullmatch(r"[0-9a-f]{12}", identifier):
            raise ValueError(f"Invalid continuation ID: {identifier!r}")
        queue.atomic_json(request, dict(status="submitted", job=identifier))
    except Exception as error:
        if identifier and re.fullmatch(r"[0-9a-f]{12}", identifier):
            subprocess.run([str(queue.GQ), "cancel", identifier], check=False, env={**os.environ, "GQ_OWNER": "nav-dagger4"})
        queue.atomic_json(request, dict(status="failed", detail=str(error)))
        raise
    result = subprocess.run([str(queue.GQ), "wait", identifier], check=False,
        stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    queue.atomic_json(request, dict(status="finished", job=identifier, returncode=result.returncode))
    return result.returncode


def run(task, step, queue):
    plan = phases()
    phase = plan[step]
    marker = ARCHIVE / f"round1/phases/{step:02d}.passed"
    if marker.exists() or step and not (ARCHIVE / f"round1/phases/{step - 1:02d}.passed").is_file():
        raise ValueError("Round phases must run once, in order")
    log = ARCHIVE / f"logs/round1-{step:02d}-{phase['kind']}.log"
    log.parent.mkdir(parents=True, exist_ok=True)
    emit("dagger4_round1_phase_log", step=step, stdout=str(log), stderr="gq")
    with log.open("a", buffering=1) as stream:
        os.dup2(stream.fileno(), 1)
    # Keep child diagnostics and Python tracebacks on the queue's stderr.
    queue.disk_check()
    if shutil.disk_usage(task).free < 20 * 1024**3:
        raise ValueError("Round-one phase requires at least 20 GiB free")
    emit("dagger4_round1_phase_started", step=step, **phase)
    if phase["kind"] == "prepare":
        prepare_part(task, phase)
    elif phase["kind"] == "assemble":
        assemble(task, queue)
    elif phase["kind"] == "train":
        train_epoch(task, phase)
    elif phase["kind"] == "parity":
        parity(task)
    elif phase["kind"] == "screen":
        screen(task)
    elif phase["kind"] == "screen-gate":
        if not screen_gate(task):
            emit("dagger4_round1_stopped", step=step, reason="screen_gate")
            return
    elif phase["kind"] == "validate":
        validate(task, phase)
    else:
        compare(task)
    marker.parent.mkdir(parents=True, exist_ok=True)
    marker.write_text("Completed fixed round-one phase\n")
    if step + 1 < len(plan):
        identifier = queue.submit_next(task, script=__file__, owner="nav-dagger4", extra=("--step", str(step + 1)))
        emit("dagger4_round1_continued", step=step, next_step=step + 1, next_job=identifier)
    else:
        emit("dagger4_round1_complete", phases=len(plan))


def main():
    faulthandler.enable()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--step", type=int, choices=range(len(phases())), required=True)
    parser.add_argument("--enqueue", type=Path)
    args = parser.parse_args()
    if sys.platform != "linux" or not args.task.is_absolute() or not args.task.is_dir():
        parser.error("Run on Ryzen/Linux with an existing absolute task directory")
    sys.path.insert(0, str(scripts(args.task)))
    import nav_demos_v4_chain as queue
    if args.enqueue is not None:
        return enqueue(args.task, args.step, args.enqueue, queue)
    os.environ.update(round_environment(args.task))
    emit("dagger4_round1_base", step=args.step, fingerprint=os.environ["OPENGOTHIC_BASE_FINGERPRINT"],
         path=os.environ["OPENGOTHIC_BASE"], assets=os.environ["GOTHIC_DIR"])
    os.environ.update(UV_CACHE_DIR=str(Path.home() / ".cache/uv"), TMPDIR=str(args.task / "tmp"),
        OMP_NUM_THREADS="1" if phases()[args.step]["kind"] in ("validate", "screen") else "6",
        OPENBLAS_NUM_THREADS="1", MKL_NUM_THREADS="1", OPENGOTHIC_BODY_TRACE="0",
        PYTHONUNBUFFERED="1", PYTHONDONTWRITEBYTECODE="1")
    os.environ.pop("OPENGOTHIC_NAV_JOURNEY_MEMORY", None)
    os.environ.pop("OPENGOTHIC_NAV_FAMILIAR_MAP", None)
    os.chdir(args.task / "source")
    with queue.own_task(args.task):
        run(args.task, args.step, queue)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
