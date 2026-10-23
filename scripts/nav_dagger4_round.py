#!/usr/bin/env python3
"""Run one bounded DAgger4 training/validation phase and queue its successor."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from types import SimpleNamespace


ARCHIVE = Path("/mnt/shared/gothic-ai/nav-dagger4")
V5 = Path("/mnt/shared/gothic-ai/nav-demos-v5")
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
    for model in ("bc3", "hd", "r1"):
        for family in (*FAMILIES, "safety", "detour"):
            for seeds in ((seed,) for seed in SEEDS) if family in ("object", "detour") else (SEEDS,):
                result.append(dict(kind="validate", model=model, family=family, seeds=seeds))
    result.append(dict(kind="compare"))
    return result


def resources(phase):
    if phase["kind"] == "validate":
        return ["--threads", "28", "--cpu-quota", "2800%", "--mem", "20G"]
    result = ["--threads", "6", "--cpu-quota", "600%", "--mem", "16G"]
    if phase["kind"] in ("train", "parity"):
        result += ["--gpu-mem", "4G"]
    return result


def read_json(path):
    return json.loads(path.read_text())


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
    source = Path.home() / "gothic-ai/nav-demos-v4" if v4 else V5 / "recordings"
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
    command = [python(task), "-B", "-I", scripts(task) / "nav_bc_train.py", "train",
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
    source = Path.home() / "gothic-ai/nav-demos-v4"
    _, entries = load_training_manifest(source / "manifest.json", source)
    entry = next(value for value in entries if value["decisions"] >= 1000)
    output = ARCHIVE / "round1/run/parity"
    run_command([python(task), "-B", "-I", scripts(task) / "check_nav_policy.py", "--weights", weights(task, "r1"),
        "--source", entry["directory"], "--output", output, "--steps", "1000", "--torch"])
    contract = task / "build/opengothic/gothic-navpolicy-contract"
    run_command([contract, weights(task, "r1"), output / "parity.bin.gz"])
    run_command([contract, "--memory", weights(task, "r1")])
    emit("dagger4_recorded_parity_complete", steps=1000, source=str(entry["directory"]), memory_version=4)


def validation_command(task, phase):
    family = phase["family"]
    output = ARCHIVE / "round1" / ("detour-validation" if family == "detour" else "validation") / phase["model"]
    command = [python(task), "-B", "-I", scripts(task) / "nav_rl.py", "drive"]
    if family == "detour":
        command += ["--detour-scenarios", task / "input/detour-val.jsonl"]
    else:
        command += ["--manifest", V5 / "manifest.json", "--lean", "--families", "point" if family == "safety" else family]
        if family == "safety":
            command += ["--indices", "0", "--safety-fixtures", task / "input/fixtures.json", "--fence-control"]
        if family == "jump":
            command += ["--jump-links", task / "input/jump-links.txt"]
    command += ["--runner", task / "build/opengothic/gothic-body-runner", "--assets", Path.home() / "gothic-ai/game",
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
    summary = [python(task), "-B", "-I", scripts(task) / "nav_rl.py", "summarize", "--source", output, "--output", output]
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
        normal = read_json(ARCHIVE / f"round1/validation/{model}/metrics.json")
        detour = read_json(ARCHIVE / f"round1/detour-validation/{model}/metrics.json")
        if len(normal["episodes"]) != 1491 or len(detour["episodes"]) != 363 or len(normal["setup_rejections"]) != 2:
            raise ValueError(f"Incomplete {model} validation")
        for name, value in (("validation", normal), ("detour", detour)):
            pairs = {(entry["id"], entry["action_seed"]) for entry in value["episodes"]}
            if len(pairs) != len(value["episodes"]) or name in identity and pairs != identity[name]:
                raise ValueError("Validation models do not share identical paired cohorts")
            identity[name] = pairs
            check_episodes(task, model, value["episodes"])
        results[model] = dict(point=normal["by_family"]["point"]["arrived"],
            object=normal["by_family"]["object"]["arrived"], detour=detour["all"]["arrived"],
            by_family=normal["by_family"])
    losses = {name: results["hd"][name] - results["r1"][name] for name in ("point", "detour")}
    emit("dagger4_round1_validation_complete", models=results, decline_vs_hd=losses,
         stop_before_round2=any(value > 5 for value in losses.values()), automatically_start_round2=False)


def enqueue(task, step, request, queue):
    identifier = None
    try:
        queue.disk_check()
        command = [str(queue.GQ), "submit", "--class", "batch", *resources(phases()[step]), "--max", "2h",
            "--name", f"nav dagger4 round1 step {step + 1}/{len(phases())} {phases()[step]['kind']}", "--",
            str(python(task)), "-B", "-I", str(Path(__file__).resolve()), "--task", str(task), "--step", str(step)]
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
    with log.open("a", buffering=1) as stream:
        os.dup2(stream.fileno(), 1)
        os.dup2(stream.fileno(), 2)
    queue.disk_check()
    emit("dagger4_round1_phase_started", step=step, **phase)
    if phase["kind"] == "prepare":
        prepare_part(task, phase)
    elif phase["kind"] == "assemble":
        assemble(task, queue)
    elif phase["kind"] == "train":
        train_epoch(task, phase)
    elif phase["kind"] == "parity":
        parity(task)
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
    os.environ.update(UV_CACHE_DIR=str(Path.home() / ".cache/uv"), TMPDIR=str(args.task / "tmp"),
        OMP_NUM_THREADS="1" if phases()[args.step]["kind"] == "validate" else "6",
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
