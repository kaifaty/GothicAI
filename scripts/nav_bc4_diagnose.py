#!/usr/bin/env python3
"""Describe BC4's recorded validation outcomes and read-only v4/v5 train data.

The corpus phase streams the 700 eligible v5 recordings and must run in a gq
batch. Neither phase opens held-out NAVDEMO recordings or evaluates a policy.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import importlib.util
import json
from pathlib import Path

import numpy as np


MAX_OUTPUT_BYTES = 1_500_000


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def module(name):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(name + ".py"))
    loaded = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(loaded)
    return loaded


def write_json(path, value):
    data = (json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":")) + "\n").encode()
    if len(data) > MAX_OUTPUT_BYTES:
        raise ValueError(f"Diagnostic output exceeds {MAX_OUTPUT_BYTES} bytes: {len(data)}")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    print(json.dumps(dict(output=str(path), bytes=len(data))), flush=True)


def distribution(values):
    values = np.asarray([value for value in values if value is not None], dtype=np.float64)
    if not len(values):
        return dict(n=0)
    if not np.isfinite(values).all():
        raise ValueError("Nonfinite diagnostic value")
    quantiles = np.quantile(values, [0, .1, .25, .5, .75, .9, 1])
    return dict(n=len(values), mean=float(values.mean()),
                **dict(zip(("min", "p10", "p25", "median", "p75", "p90", "max"), map(float, quantiles))))


def outcome(episode):
    # A true success may retain a stale failure string. An empty failure string
    # on its own is never sufficient evidence for success.
    return "success" if episode["success"] is True else episode.get("failure") or "failure_unspecified"


def outcome_summary(episodes):
    decisions = sum(episode["decisions"] for episode in episodes)
    terminations = Counter(dict.fromkeys(("success", "path_timeout", "body_blocked", "actor_down"), 0))
    terminations.update(outcome(episode) for episode in episodes)
    counts = {key: sum(episode["action_counts"][key] for episode in episodes) for key in ("turn_only", "idle")}
    return dict(episodes=len(episodes), successes=terminations["success"], terminations=dict(terminations),
                decisions=decisions, action_counts=counts,
                decision_fractions={key: value / decisions if decisions else None for key, value in counts.items()},
                mean_episode_fractions={key: float(np.mean([episode["action_counts"][key] / episode["decisions"]
                                                           for episode in episodes])) if episodes else None
                                        for key in counts},
                elapsed_ms=sum(episode["elapsed_ms"] for episode in episodes),
                blocked_ms=sum(episode.get("blocked_ms", 0) for episode in episodes),
                success_with_nonempty_failure=sum(episode["success"] is True and bool(episode.get("failure"))
                                                  for episode in episodes))


def paired_outcomes(pairs):
    counts = Counter(dict.fromkeys(("both_success", "hd_only", "bc4_only", "neither_success"), 0))
    for bc4, hd in pairs:
        counts[("both_success" if hd["success"] is True else "bc4_only") if bc4["success"] is True
               else ("hd_only" if hd["success"] is True else "neither_success")] += 1
    return dict(episodes=len(pairs), net_bc4_minus_hd=counts["bc4_only"] - counts["hd_only"], **counts,
                bc4=outcome_summary([pair[0] for pair in pairs]), hd=outcome_summary([pair[1] for pair in pairs]))


def outcomes(task, archive):
    scenario_file = task / "input/detour-val.jsonl"
    scenarios = {value["id"]: value for value in
                 (json.loads(line) for line in scenario_file.read_text(encoding="utf-8").splitlines() if line.strip())}
    if len(scenarios) != 121:
        raise ValueError(f"Expected 121 frozen detour-val scenarios, found {len(scenarios)}")
    paths = dict(bc4=archive / "detour-validation/bc4/metrics.json",
                 hd=Path.home() / "gothic-ai/nav-dagger3/hd-detour/metrics.json")
    models = {}
    for name, path in paths.items():
        episodes = read_json(path)["episodes"]
        indexed = {(episode["id"], episode["action_seed"]): episode for episode in episodes}
        if len(indexed) != len(episodes) or len(episodes) != 363:
            raise ValueError(f"Expected 363 unique paired detour outcomes: {path}")
        for (identifier, _), episode in indexed.items():
            if identifier not in scenarios or episode["split"] != "validation":
                raise ValueError(f"Unexpected detour scenario: {identifier}")
            if (episode["start"], episode["goal"]) != (scenarios[identifier]["start"], scenarios[identifier]["goal"]):
                raise ValueError(f"Paired detour endpoints differ: {identifier}")
        models[name] = indexed
    if models["bc4"].keys() != models["hd"].keys():
        raise ValueError("BC4/HD detour id/seed populations differ")
    seeds = sorted({seed for _, seed in models["bc4"]})
    if len(seeds) != 3 or any((identifier, seed) not in models["bc4"] for identifier in scenarios for seed in seeds):
        raise ValueError("Detour validation does not contain all three paired seeds")
    pairs = [(models["bc4"][key], models["hd"][key]) for key in sorted(models["bc4"])]
    result = dict(scenarios=len(scenarios), action_seeds=seeds,
                  definitions=dict(success="native success == true; stale failure strings do not override success",
                      turn_only="nonzero turn and zero forward/strafe, decision weighted",
                      idle="zero forward/strafe/turn/jump/climb, decision weighted",
                      familiar_validation="original lean run did not record input-cell counters; exposure cannot be reconstructed"),
                  paired=paired_outcomes(pairs), by_paired_result={}, by_seed={}, by_obstacle_type={}, by_goal_kind={},
                  by_site_id={}, by_place_id={}, by_region={}, scenarios_detail=[])
    for bc4_success, hd_success, label in ((True, True, "both_success"), (False, True, "hd_only"),
                                          (True, False, "bc4_only"), (False, False, "neither_success")):
        result["by_paired_result"][label] = paired_outcomes(
            [pair for pair in pairs if pair[0]["success"] is bc4_success and pair[1]["success"] is hd_success])
    result["success_with_stale_failure"] = [dict(model=name, id=episode["id"], action_seed=episode["action_seed"],
                                                failure=episode["failure"])
        for name, indexed in models.items() for episode in indexed.values()
        if episode["success"] is True and episode.get("failure")]
    for seed in seeds:
        result["by_seed"][str(seed)] = paired_outcomes([pair for pair in pairs if pair[0]["action_seed"] == seed])
    for field in ("obstacle_type", "goal_kind", "site_id", "place_id", "region"):
        for value in sorted({str(scenario.get(field, "unknown")) for scenario in scenarios.values()}):
            selected = [pair for pair in pairs if str(scenarios[pair[0]["id"]].get(field, "unknown")) == value]
            result["by_" + field][value] = paired_outcomes(selected)
    for identifier, scenario in sorted(scenarios.items()):
        selected = [(models["bc4"][(identifier, seed)], models["hd"][(identifier, seed)]) for seed in seeds]
        row = dict(id=identifier, **{key: scenario.get(key) for key in
                   ("obstacle_type", "goal_kind", "site_id", "place_id", "region")},
                   paired=paired_outcomes(selected), by_seed={})
        for bc4, hd in selected:
            row["by_seed"][str(bc4["action_seed"])] = {
                name: dict(success=episode["success"], termination=outcome(episode), raw_failure=episode.get("failure", ""),
                           decisions=episode["decisions"], elapsed_ms=episode["elapsed_ms"],
                           blocked_ms=episode.get("blocked_ms", 0),
                           turn_only=episode["action_counts"]["turn_only"], idle=episode["action_counts"]["idle"])
                for name, episode in (("bc4", bc4), ("hd", hd))}
        result["scenarios_detail"].append(row)
    write_json(archive / "diagnostics/detour-outcomes.json", result)


def train_entries(manifest):
    indexed = {entry["id"]: entry for entry in manifest["episodes"]}
    if len(indexed) != len(manifest["episodes"]):
        raise ValueError("Duplicate corpus episode ID")
    selected, seen = [], set()
    for family, identifiers in manifest["splits"]["train"].items():
        for identifier in identifiers:
            entry = indexed[identifier]
            if identifier in seen or entry["family"] != family or entry["split"] != "train" or entry["training_eligible"] is not True:
                raise ValueError(f"Invalid exact training entry: {identifier}")
            seen.add(identifier)
            selected.append(entry)
    return selected


def episode_data(entry):
    directory = Path(entry["episode"])
    metadata = read_json(directory / "episode.json")
    if entry["split"] != "train" or metadata.get("split", "train") != "train":
        raise ValueError(f"Refusing a held-out native episode: {directory}")
    decisions = entry["decisions"]
    actions = np.loadtxt(directory / "actions.txt", ndmin=2)
    if actions.shape != (decisions + 1, 9) or int(actions[-1, 0]) != metadata["elapsed_ms"]:
        raise ValueError(f"Native action count/time differs: {directory}")
    keys = actions[:-1]
    travelled = entry.get("travelled_cm")
    if travelled is None:
        trajectory = np.loadtxt(directory / "trajectory.txt", ndmin=2)
        if trajectory.shape != (decisions + 1, 5):
            raise ValueError(f"Native trajectory count differs: {directory}")
        travelled = float(np.linalg.norm(np.diff(trajectory[:, [1, 3]], axis=0), axis=1).sum())
    detour = metadata.get("detour_length_cm")
    ratio = travelled / detour if metadata.get("goal_kind") not in ("follow", "approach") and detour and detour > 0 else None
    return dict(id=entry["id"], family=entry["family"], start=metadata["start"], goal=metadata["goal"],
                decisions=decisions, elapsed_ms=metadata["elapsed_ms"], success=metadata["success"] is True,
                termination=outcome(metadata), blocked_ms=metadata.get("blocked_ms", 0),
                max_blocked_ms=metadata.get("max_blocked_ms", 0), blocked_spans=metadata.get("blocked_spans", 0),
                back=int(np.count_nonzero(keys[:, 1] < 0)), strafe=int(np.count_nonzero(keys[:, 2])),
                travelled_xz_cm=travelled, length_over_detour=ratio,
                residency="local" if entry.get("local") is True else "stranger" if entry.get("local") is False else "unknown")


def corpus_summary(episodes):
    decisions = sum(episode["decisions"] for episode in episodes)
    elapsed = sum(episode["elapsed_ms"] for episode in episodes)
    blocked = sum(episode["blocked_ms"] for episode in episodes)
    return dict(episodes=len(episodes), decisions=decisions, successes=sum(episode["success"] for episode in episodes),
                terminations=dict(Counter(episode["termination"] for episode in episodes)),
                back_decisions=sum(episode["back"] for episode in episodes),
                strafe_decisions=sum(episode["strafe"] for episode in episodes),
                back_fraction=sum(episode["back"] for episode in episodes) / decisions if decisions else None,
                strafe_fraction=sum(episode["strafe"] for episode in episodes) / decisions if decisions else None,
                elapsed_ms=elapsed, blocked_ms=blocked, blocked_time_fraction=blocked / elapsed if elapsed else None,
                episodes_with_blocked_spans=sum(episode["blocked_spans"] > 0 for episode in episodes),
                distributions={key: distribution([episode[key] for episode in episodes]) for key in
                               ("decisions", "elapsed_ms", "travelled_xz_cm", "length_over_detour", "blocked_ms", "max_blocked_ms", "blocked_spans")})


def grouped_corpus(episodes):
    return dict(all=corpus_summary(episodes),
                by_family={family: corpus_summary([episode for episode in episodes if episode["family"] == family])
                           for family in sorted({episode["family"] for episode in episodes})})


def familiar_counts():
    return dict(episodes=0, decisions=0, any_positive_decisions=0, near_positive_decisions=0, far_positive_decisions=0,
                near_positive_cells=0, far_positive_cells=0)


def familiar_summary(counts):
    decisions = counts["decisions"]
    return dict(counts,
                any_positive_fraction=counts["any_positive_decisions"] / decisions if decisions else None,
                near_positive_fraction=counts["near_positive_decisions"] / decisions if decisions else None,
                far_positive_fraction=counts["far_positive_decisions"] / decisions if decisions else None,
                near_nonzero_cell_fraction=counts["near_positive_cells"] / (decisions * 64 * 64) if decisions else None,
                far_nonzero_cell_fraction=counts["far_positive_cells"] / (decisions * 50 * 100) if decisions else None,
                policy_visible_nonzero_cell_fraction=(counts["near_positive_cells"] + counts["far_positive_cells"])
                    / (decisions * (64 * 64 + 50 * 100)) if decisions else None)


def familiar_train(entries):
    demos = module("nav_demos")
    total = familiar_counts()
    groups = defaultdict(familiar_counts)
    for index, entry in enumerate(entries):
        counts = familiar_counts()
        counts["episodes"] = 1
        for records in demos.iter_chunks(Path(entry["episode"]), chunk_records=32):
            near = records["memory_near"]["channels"][..., 7] > 0
            far = records["memory_far"]["channels"][:, 50:100, :, 7] > 0
            near_any, far_any = near.any(axis=(1, 2)), far.any(axis=(1, 2))
            counts["decisions"] += len(records)
            counts["any_positive_decisions"] += int(np.count_nonzero(near_any | far_any))
            counts["near_positive_decisions"] += int(np.count_nonzero(near_any))
            counts["far_positive_decisions"] += int(np.count_nonzero(far_any))
            counts["near_positive_cells"] += int(np.count_nonzero(near))
            counts["far_positive_cells"] += int(np.count_nonzero(far))
        if counts["decisions"] != entry["decisions"]:
            raise ValueError(f"Familiar stream decision count differs: {entry['id']}")
        residency = "local" if entry["local"] else "stranger"
        for accumulator in (total, groups[("family", entry["family"])], groups[("residency", residency)],
                            groups[("family_residency", entry["family"] + "/" + residency)]):
            for key, value in counts.items():
                accumulator[key] += value
        if (index + 1) % 50 == 0:
            print(json.dumps(dict(event="familiar_progress", episodes=index + 1, decisions=total["decisions"])), flush=True)
    return dict(all=familiar_summary(total), **{
        "by_" + group: {name: familiar_summary(counts) for (kind, name), counts in groups.items() if kind == group}
        for group in ("family", "residency", "family_residency")})


def matched_corpora(v4, v5):
    def identity(episode):
        return episode["family"], episode["id"], episode["start"], episode["goal"]
    old = {identity(episode): episode for episode in v4}
    new = {identity(episode): episode for episode in v5}
    identities = sorted(old.keys() & new.keys())
    previous, current = [old[key] for key in identities], [new[key] for key in identities]
    deltas = {}
    for field in ("decisions", "elapsed_ms", "travelled_xz_cm", "length_over_detour", "blocked_ms", "max_blocked_ms", "blocked_spans"):
        deltas[field] = distribution([new[key][field] - old[key][field] for key in identities
                                      if old[key][field] is not None and new[key][field] is not None])
    for field in ("back", "strafe"):
        deltas[field + "_fraction"] = distribution([new[key][field] / new[key]["decisions"]
                                                    - old[key][field] / old[key]["decisions"] for key in identities])
    return dict(episodes=len(identities), v4_only=len(old.keys() - new.keys()), v5_only=len(new.keys() - old.keys()),
                both_success=sum(old[key]["success"] and new[key]["success"] for key in identities),
                v4_only_success=sum(old[key]["success"] and not new[key]["success"] for key in identities),
                v5_only_success=sum(not old[key]["success"] and new[key]["success"] for key in identities),
                neither_success=sum(not old[key]["success"] and not new[key]["success"] for key in identities),
                v4=corpus_summary(previous), v5=corpus_summary(current), delta_v5_minus_v4=deltas)


def weight_stats(path):
    policy = module("nav_bc_numpy").NumpyPolicy(path)
    result = dict(format_version=policy.format_version, encoders={})
    for name in ("near_encoder", "far_encoder"):
        weights = policy.weights[name + ".0.weight"].astype(np.float64)
        norms = np.linalg.norm(weights.transpose(1, 0, 2, 3).reshape(weights.shape[1], -1), axis=1)
        encoder = dict(channel_l2=norms.tolist(), original_10_channel_mean_l2=float(norms[:10].mean()),
                       original_10_channels_combined_l2=float(np.linalg.norm(norms[:10])))
        if len(norms) == 11:
            encoder.update(familiar_l2=float(norms[10]), familiar_l2_over_original_channel_mean=float(norms[10] / norms[:10].mean()),
                           familiar_squared_l2_fraction=float(norms[10] ** 2 / np.square(norms).sum()),
                           familiar_mean=float(weights[:, 10].mean()), familiar_max_abs=float(np.abs(weights[:, 10]).max()))
        result["encoders"][name] = encoder
    return result


def corpus(task, archive):
    manifests = dict(v4=read_json(Path.home() / "gothic-ai/nav-demos-v4/manifest.json"),
                     v5=read_json(Path("/mnt/shared/gothic-ai/nav-demos-v5/manifest.json")))
    entries = {name: train_entries(manifest) for name, manifest in manifests.items()}
    for name, expected in (("v4", (602, 405626)), ("v5", (700, 405599))):
        if (len(entries[name]), sum(entry["decisions"] for entry in entries[name])) != expected:
            raise ValueError(f"Exact {name} training population differs from {expected}")
    data, result = {}, dict(definitions=dict(
        train="only exact IDs from each manifest's splits.train",
        all_attempts="all recorded train attempts, including ineligible failures",
        matched="same family, id, native start and native goal; descriptive comparison, not a causal effect",
        travelled_xz_cm="sum physical XZ trajectory segments; excludes vertical distance",
        length_over_detour="physical XZ travel / initial Detour length for static goals only",
        blocked_time_fraction="native blocked_ms / elapsed_ms",
        familiar="raw memory channel 7 > 0, near 64x64 and policy-visible far rows 50:100 only",
        caveat="v4/v5 differ in memory, water padding, NPCs and eligibility; family mix and matched outcomes are reported separately"))
    for name, manifest in manifests.items():
        data[name] = [episode_data(entry) for entry in manifest["episodes"]]
        selected = {entry["id"] for entry in entries[name]}
        training = [episode for episode in data[name] if episode["id"] in selected]
        result[name] = dict(train=grouped_corpus(training), all_attempts=grouped_corpus(data[name]))
        print(json.dumps(dict(event="corpus_native_complete", corpus=name, episodes=len(data[name]))), flush=True)
    result["matched_all_attempts"] = matched_corpora(data["v4"], data["v5"])
    train_ids = {name: {entry["id"] for entry in values} for name, values in entries.items()}
    training = {name: [episode for episode in values if episode["id"] in train_ids[name]] for name, values in data.items()}
    result["matched_both_train"] = matched_corpora(training["v4"], training["v5"])
    result["matched_all_attempts_by_family"] = {
        family: matched_corpora([episode for episode in data["v4"] if episode["family"] == family],
                                [episode for episode in data["v5"] if episode["family"] == family])
        for family in sorted({episode["family"] for values in data.values() for episode in values})}
    result["matched_both_train_by_family"] = {
        family: matched_corpora([episode for episode in training["v4"] if episode["family"] == family],
                                [episode for episode in training["v5"] if episode["family"] == family])
        for family in sorted({episode["family"] for values in training.values() for episode in values})}
    result["familiar_train"] = familiar_train(entries["v5"])
    if result["familiar_train"]["all"]["decisions"] != 405599:
        raise ValueError("Familiar exposure denominator differs from 405599 training decisions")
    result["first_convolution"] = dict(bc4=weight_stats(archive / "bc4/policy.navbc"),
                                        hd=weight_stats(task / "input/hd/policy.navbc"))
    write_json(archive / "diagnostics/corpus.json", result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("outcomes", "corpus"))
    parser.add_argument("--task", type=Path, required=True, help="absolute task root (~/gothic-ai/nav-bc4)")
    parser.add_argument("--archive", type=Path, required=True, help="absolute output root (/mnt/shared/gothic-ai/nav-bc4)")
    args = parser.parse_args()
    if not args.task.is_absolute() or not args.archive.is_absolute():
        parser.error("--task and --archive must be absolute")
    (outcomes if args.stage == "outcomes" else corpus)(args.task, args.archive)


if __name__ == "__main__":
    main()
