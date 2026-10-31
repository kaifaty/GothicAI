#!/usr/bin/env python3
"""Verify the archived demos-v5 corpus and publish its final HD training manifest.

No output is written until every recording, checksum, quota and held-out boundary
has been checked. This module does not launch games, evaluate hold-outs or train.
"""
from __future__ import annotations

import argparse
from collections import Counter
from copy import deepcopy
import hashlib
import importlib.util
import json
import math
from pathlib import Path

import numpy as np


_SPEC = importlib.util.spec_from_file_location("nav_demos", Path(__file__).with_name("nav_demos.py"))
demos = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(demos)
_RL_SPEC = importlib.util.spec_from_file_location("nav_rl", Path(__file__).with_name("nav_rl.py"))
rl = importlib.util.module_from_spec(_RL_SPEC)
_RL_SPEC.loader.exec_module(rl)

REPORT = Path("reports/navigation/2026-10-22-demos-v5")
QUOTAS = {"point": 400, "object": 100, "approach": 60, "follow_below_gate": 60,
          "jump": 168, "water": 40}
ATTEMPT_QUOTAS = {"point", "jump"}
MAX_BYTES = 30_000_000_000
MAX_REPORT_BYTES = 1_500_000
WATER_DEPTH_EPSILON_CM = 1


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def rows(path):
    with Path(path).open(encoding="utf-8") as stream:
        return [json.loads(line) for line in stream if line.strip()]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def tree_bytes(root):
    return sum(path.stat().st_size for path in Path(root).rglob("*") if path.is_file())


def physical_water(directory, elapsed_ms):
    """Count wet/deep entry transitions from the native passive physical trace."""
    source = Path(directory) / "perception-safety-frames.jsonl"
    require(source.is_file(), f"Missing physical water evidence: {directory}")
    frames = rows(source)
    require(len(frames) >= 2 and frames[0].get("elapsed_ms") == 0
            and frames[-1].get("elapsed_ms") == elapsed_ms,
            f"Incomplete physical water evidence: {directory}")
    previous_ms = -1
    was_wet = was_deep = False
    counts = Counter()
    maximum = 0.
    for frame in frames:
        elapsed = frame.get("elapsed_ms")
        depth = frame.get("water_depth_cm")
        threshold = frame.get("deep_water_threshold_cm")
        require(type(elapsed) is int and previous_ms < elapsed <= elapsed_ms,
                f"Invalid physical frame time: {directory}")
        require(type(depth) in (int, float) and math.isfinite(depth) and depth >= 0
                and type(threshold) in (int, float) and math.isfinite(threshold) and threshold > 0
                and type(frame.get("swimming")) is bool,
                f"Missing or invalid native water fields: {directory}")
        wet = depth > 10
        deep = depth + WATER_DEPTH_EPSILON_CM >= threshold
        counts.update(physical_frames=1, entered_water_frames=int(wet), deep_water_frames=int(deep),
                      swimming_frames=int(frame["swimming"]), water_entries=int(wet and not was_wet),
                      deep_water_entries=int(deep and not was_deep))
        previous_ms, was_wet, was_deep = elapsed, wet, deep
        maximum = max(maximum, depth)
    return dict(counts, max_water_depth_cm=maximum)


def path_metrics(directory, metadata):
    """Use physical XZ travel and the native initial Detour length."""
    trajectory = np.loadtxt(Path(directory) / "trajectory.txt", ndmin=2)
    require(trajectory.shape == (metadata["decisions"] + 1, 5)
            and np.isfinite(trajectory).all(), f"Invalid physical trajectory: {directory}")
    times = trajectory[:, 0]
    require(times[0] == 0 and times[-1] == metadata["elapsed_ms"]
            and (times == np.floor(times)).all() and (np.diff(times[:-1]) == 100).all()
            and 0 < times[-1] - times[-2] <= 100, f"Invalid trajectory times: {directory}")
    length = float(np.linalg.norm(np.diff(trajectory[:, [1, 3]], axis=0), axis=1).sum())
    detour = metadata.get("detour_length_cm")
    moving = metadata.get("goal_kind") in ("follow", "approach")
    defined = not moving and type(detour) in (float, int) and math.isfinite(detour) and detour > 0
    return dict(travelled_cm=length, detour_length_cm=detour,
                length_over_detour=length / detour if defined else None)


def heldout_boundaries(v4, detours):
    """Keep original references; these scenarios are never opened as episodes."""
    heldout = {split: deepcopy(v4["splits"][split]) for split in ("validation", "test")}
    forbidden_ids, forbidden_pairs = set(), set()
    forbidden_groups = {("object", "place_id"): set(), ("moving", "region"): set(),
                        ("jump", "site_id"): set(), ("jump", "transition_id"): set()}
    for split, families in heldout.items():
        for family, values in families.items():
            for value in values:
                require(value.get("episode") is None and value.get("recorded") is False,
                        f"v4 {split} contains a recording reference")
                scenario = value.get("scenario", value)
                forbidden_ids.add(scenario.get("id", value.get("id")))
                # v4 splits are family-specific: moving test endpoints can
                # also belong to the frozen point training selection.
                if family == "point" and scenario.get("start") and scenario.get("goal"):
                    forbidden_pairs.add((scenario["start"], scenario["goal"]))
                group = "moving" if family in ("follow", "approach", "follow_below_gate") else family
                for (group_family, key), forbidden in forbidden_groups.items():
                    if group == group_family and scenario.get(key) is not None:
                        forbidden.add(scenario[key])
        for (family, key), forbidden in forbidden_groups.items():
            policy = v4.get("split_policy", {}).get(family, {})
            field = "site_id" if family == "jump" else policy.get("group")
            if field == key:
                forbidden.update(policy.get("groups", {}).get(split, []))
        heldout[split]["detour"] = []
        for scenario in detours[split]:
            require(scenario.get("split", split) in (split, "val" if split == "validation" else split),
                    f"Detour {split} source contains another split")
            forbidden_ids.add(scenario.get("id"))
            if scenario.get("start") and scenario.get("goal"):
                forbidden_pairs.add((scenario["start"], scenario["goal"]))
            heldout[split]["detour"].append(dict(id=scenario["id"], family="detour", split=split,
                scenario=deepcopy(scenario), episode=None, recorded=False, decisions=0))
    forbidden_ids.discard(None)
    return heldout, forbidden_ids, forbidden_pairs, forbidden_groups


def verified_episodes(task, archive_root, summary, forbidden_ids, forbidden_pairs, forbidden_groups):
    root = archive_root / "recordings"
    require(summary.get("complete") is True and summary.get("navigation_memory_version") == 4
            and summary.get("demo_version") == 3 and summary.get("student_training_run") is False,
            "Collection is incomplete or uses the wrong format, memory or training state")
    inventory = {path.parent.resolve() for path in root.rglob("samples.bin.gz")}
    metadata_inventory = {path.parent.resolve() for path in root.rglob("episode.json")}
    require(inventory == metadata_inventory, "Archived sample/metadata inventory differs")
    require(not any(path.is_symlink() for path in root.rglob("*")), "Archive contains a symbolic link")
    entries, visited, identities = [], set(), set()
    for saved in summary["episodes"]:
        directory = Path(saved["episode"])
        # Local summary paths are accepted only as an interrupted control-file
        # copy; actual samples must always be read from shared storage.
        if directory.is_relative_to(task / "recordings"):
            directory = root / directory.relative_to(task / "recordings")
        directory = directory.resolve()
        require(directory.is_relative_to(root.resolve()) and directory in inventory and directory not in visited,
                f"Missing, duplicate or nonarchive episode: {directory}")
        visited.add(directory)
        metadata = read_json(directory / "episode.json")
        family, identifier = saved["family"], saved["id"]
        require(family in QUOTAS and (family, identifier) not in identities, f"Invalid episode identity: {identifier}")
        identities.add((family, identifier))
        scenario = saved["scenario"]
        require(saved.get("split") == metadata.get("split") == scenario.get("split") == "train"
                and saved["id"] == metadata.get("id") == scenario.get("id")
                and directory == (root / family / "train" / identifier).resolve(),
                f"Episode scenario, split or directory differs: {directory}")
        require(identifier not in forbidden_ids
                and (scenario.get("start"), scenario.get("goal")) not in forbidden_pairs
                and (scenario.get("goal"), scenario.get("start")) not in forbidden_pairs,
                f"Held-out scenario was recorded: {identifier}")
        group = "moving" if family in ("follow_below_gate", "approach") else family
        require(all(scenario.get(key) not in forbidden for (group_family, key), forbidden in forbidden_groups.items()
                    if group == group_family), f"Held-out group was recorded: {identifier}")
        require(all(scenario.get(key) == metadata.get(key) for key in
                    ("start", "goal", "goal_kind", "place_id", "region", "site_id", "transition_id")),
                f"Episode endpoints differ from the collection summary: {identifier}")
        require(metadata.get("physics_mode") == "cv" and metadata.get("perception_mode") == "gaze"
                and metadata.get("teacher_version") == (2 if family == "jump" else 3)
                and metadata.get("teacher_variant") == ("v2" if family == "jump" else "v3b")
                and metadata.get("teacher_familiar_hint") is False,
                f"Incorrect physics, perception or teacher: {identifier}")
        require(type(metadata.get("local")) is bool
                and metadata.get("residency") == ("local" if metadata["local"] else "stranger")
                and isinstance(metadata.get("knownroads"), dict)
                and isinstance(metadata.get("knownroads_selection"), dict),
                f"Missing knownroads residency evidence: {identifier}")
        require(saved.get("local") == metadata["local"] and saved.get("residency") == metadata["residency"],
                f"Residency differs from the collection summary: {identifier}")
        checksum = digest(directory / "samples.bin.gz")
        require(saved.get("samples_sha256") == metadata.get("samples_sha256") == checksum,
                f"Sample checksum differs from final summary or metadata: {identifier}")
        if "collection_metrics" in metadata:
            require(metadata["collection_metrics"].get("samples_sha256") == checksum,
                    f"Sample checksum differs from metadata metrics: {identifier}")
        checked = demos.summarize(directory, validate=True)
        require(checked["format_versions"] == [3] and checked["navigation_memory_versions"] == [4],
                f"Expected NAVDEMO v3 and memory 4: {identifier}")
        require(saved.get("validated") is True and saved["decisions"] == checked["decisions"]
                and saved.get("key_counts") == checked["key_counts"],
                f"Sample decisions or actions differ from final summary: {identifier}")
        require(type(saved.get("training_eligible")) is bool
                and saved["training_eligible"] == metadata.get("training_eligible"),
                f"Training eligibility differs from metadata: {identifier}")
        water = physical_water(directory, metadata["elapsed_ms"])
        require(water["physical_frames"] == metadata.get("frames", -2) + 1,
                f"Missing native physical frames: {identifier}")
        require(all(saved.get(key) == value for key, value in water.items()),
                f"Native water evidence differs from collection summary: {identifier}")
        metrics = path_metrics(directory, metadata)
        entries.append(dict(id=identifier, family=family, split="train", episode=str(directory),
            decisions=checked["decisions"], training_eligible=saved["training_eligible"],
            teacher_goal_success=saved.get("teacher_goal_success", metadata.get("success")),
            samples_sha256=checksum, scenario=deepcopy(scenario), key_counts=checked["key_counts"],
            teacher_variant=metadata["teacher_variant"], teacher_familiar_hint=False,
            local=metadata["local"], residency=metadata["residency"], npc=metadata["npc"],
            knownroads={key: value for key, value in metadata["knownroads"].items()
                        if key not in ("known_node_ids", "known_edge_ids")},
            knownroads_selection=metadata["knownroads_selection"],
            **water, **metrics))
    require(visited == inventory, "Archived episode inventory differs from final collection summary")
    return sorted(entries, key=lambda value: (value["family"], value["id"]))


def family_totals(episodes):
    groups = {}
    for family, quota in QUOTAS.items():
        selected = [entry for entry in episodes if entry["family"] == family]
        eligible = sum(entry["training_eligible"] for entry in selected)
        actual = len(selected) if family in ATTEMPT_QUOTAS else eligible
        require(actual == quota if family in ATTEMPT_QUOTAS else actual >= quota,
                f"Unmet {family} quota: {actual}, expected {quota}")
        counts = sum((Counter(entry["key_counts"]) for entry in selected), Counter())
        decisions = sum(entry["decisions"] for entry in selected)
        groups[family] = dict(episodes=len(selected), training_eligible=eligible, decisions=decisions,
            backward=counts["back"], strafe=counts["strafe"], jump=counts["jump"], climb=counts["climb"],
            backward_fraction=counts["back"] / decisions, strafe_fraction=counts["strafe"] / decisions)
    return groups


def compare_v4(episodes, v4):
    """Read only successful shared train recordings with identical endpoints."""
    previous = {(entry["family"], entry["id"]): entry for entry in v4["episodes"]
                if entry.get("split") == "train"}
    comparisons = []
    for entry in episodes:
        old = previous.get((entry["family"], entry["id"]))
        if old is None or entry["scenario"].get("goal_kind") in ("follow", "approach"):
            continue
        if not entry["teacher_goal_success"] or not old.get("teacher_goal_success"):
            continue
        if any(old["scenario"].get(key) != entry["scenario"].get(key) for key in ("start", "goal")):
            continue
        old_directory = Path(old["episode"])
        old_metadata = read_json(old_directory / "episode.json")
        old_metrics = path_metrics(old_directory, old_metadata)
        if entry["length_over_detour"] is None or old_metrics["length_over_detour"] is None:
            continue
        comparisons.append(dict(id=entry["id"], family=entry["family"],
            v4_travelled_cm=old_metrics["travelled_cm"], v5_travelled_cm=entry["travelled_cm"],
            v4_length_over_detour=old_metrics["length_over_detour"],
            v5_length_over_detour=entry["length_over_detour"]))
    return comparisons


def compact_manifest(manifest, full_path, full_sha256):
    root = Path(manifest["root"])
    fields = ("id", "family", "split", "training_eligible", "decisions", "samples_sha256")
    return dict(schema_version=2, dataset=manifest["dataset"], consumer=manifest["consumer"],
        input_variant=manifest["input_variant"], complete=manifest["complete"], root=str(root),
        format=deepcopy(manifest["format"]), totals=deepcopy(manifest["totals"]),
        episodes=[dict({key: entry[key] for key in fields},
                       episode=str(Path(entry["episode"]).relative_to(root))) for entry in manifest["episodes"]],
        full_manifest=dict(path=str(full_path), sha256=full_sha256),
        splits={split: dict(manifest="full_manifest", pointer=f"/splits/{split}")
                for split in ("validation", "test")}, student_training_run=False)


def report_text(manifest, groups, comparisons, full_manifest):
    episodes = manifest["episodes"]
    local = sum(entry["local"] for entry in episodes)
    lines = ["# Демонстрации навигации v5", "",
        f"Записано {len(episodes)} train-эпизодов; годных {manifest['totals']['training_episodes']}; "
        f"решений {manifest['totals']['decisions']}. NAVDEMO v3, память 4 (поход 300 с / 300 м, familiar), gaze, CV. "
        "Учитель v3b; переходы размечены учителем v2. Знакомая подсказка учителю выключена. "
        "Ученик не обучался.", "",
        "| Семейство | Эпизоды | Годные | Решения | Назад | Вбок | jump | climb |",
        "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for family, group in groups.items():
        lines.append(f"| {family} | {group['episodes']} | {group['training_eligible']} | {group['decisions']} | "
            f"{group['backward_fraction']:.2%} | {group['strafe_fraction']:.2%} | {group['jump']} | {group['climb']} |")
    water = [entry for entry in episodes if entry["family"] == "water"]
    safety = manifest["safety"]
    lines.extend(["", f"Местные: {local}/{len(episodes)} ({local / len(episodes):.2%}); "
        f"чужаки: {len(episodes) - local}. Признак получен из knownroads и записан в каждый эпизод. "
        f"Объём корпуса: {manifest['totals']['dataset_bytes']} байт "
        f"({manifest['totals']['dataset_bytes'] / 1e9:.3f} ГБ), бюджет 30 ГБ.", "",
        "## Водные сценарии", "",
        f"Train-сценариев {len(water)}, годных {sum(entry['training_eligible'] for entry in water)}. "
        f"Входов в воду {safety['water']['water_entries']}, глубоких входов {safety['water']['deep_water_entries']}; "
        "Вход — начало непрерывного интервала, включая мокрый первый кадр: вода глубже 10 см; глубокая вода — порог груди соответствующей гильдии "
        "с допуском 1 см. Счётчики рассчитаны по нативным физическим кадрам, включая начало и конец эпизода.", "",
        "| Сценарий | Начало → цель | Годен | Входы в воду | Глубокие входы | Кадры глубокой воды | Плавание, кадры |",
        "|---|---|---:|---:|---:|---:|---:|"])
    for entry in water:
        scenario = entry["scenario"]
        lines.append(f"| {entry['id']} | {scenario['start']} → {scenario['goal']} | {int(entry['training_eligible'])} | "
            f"{entry['water_entries']} | {entry['deep_water_entries']} | {entry['deep_water_frames']} | {entry['swimming_frames']} |")
    lines.extend(["", f"Все попытки: входов в воду {safety['all']['water_entries']}, "
        f"глубоких входов {safety['all']['deep_water_entries']}, "
        f"эпизодов с глубокой водой или плаванием {safety['all']['unsafe_episodes']}; "
        f"цель 0 глубоких {'выполнена' if safety['all']['target_met'] else 'не выполнена'}. "
        f"В годном train: глубоких входов {safety['train']['deep_water_entries']}, "
        f"кадров глубокой воды {safety['train']['deep_water_frames']}, плавания {safety['train']['swimming_frames']}. "
        "Негодные попытки сохранены для диагностики и не входят в train.", "",
        "## Сравнение с v4", "",
        "Парные общие train-сценарии совпадают по семейству, id, началу и цели и успешны у обоих учителей. "
        "Длина — сумма отрезков физической траектории в XZ; Detour — нативная длина исходного пути. "
        "Для движущихся целей отношение не определено; они исключены.", "",
        "| Семейство | Общие успешные | Средняя длина v4, м | Средняя длина v5, м | Длина / Detour v4 | Длина / Detour v5 |",
        "|---|---:|---:|---:|---:|---:|"])
    for family in QUOTAS:
        selected = [entry for entry in comparisons if entry["family"] == family]
        if not selected:
            continue
        mean = lambda key: sum(entry[key] for entry in selected) / len(selected)
        lines.append(f"| {family} | {len(selected)} | {mean('v4_travelled_cm') / 100:.3f} | "
            f"{mean('v5_travelled_cm') / 100:.3f} | {mean('v4_length_over_detour'):.4f} | {mean('v5_length_over_detour'):.4f} |")
    if not comparisons:
        lines.append("| Общих успешных с определённым Detour нет | 0 | — | — | — | — |")
    lines.extend(["", "## Проверка и манифест", "",
        f"Все {len(episodes)} эпизодов прошли `nav_demos.py check` (`summarize(validate=True)`). "
        "SHA-256 каждого samples.bin.gz совпал с итоговой сводкой и метаданными; набор файлов на хранилище "
        "точно совпал со сводкой. Годный train не содержит глубокой воды или плавания.", "",
        "[manifest.json](manifest.json) содержит компактные записи с путями относительно корня корпуса, "
        "контрольными суммами и входом ученика `hd`. Загрузчик `nav_bc_data.load_training_manifest` "
        "принимает этот файл и явный корень корпуса, читает метаданные только годного train. "
        "Validation/test сохранены ссылками на сценарии v4 и detour-наборы в полном манифесте: "
        "они не записывались, не проверялись игрой и не включены в train. "
        "Подготовка признаков NAVDEMO v3 и обучение в эту задачу не входят.", "",
        f"Полный манифест: `{full_manifest['path']}`. SHA-256: `{full_manifest['sha256']}`.", ""])
    return "\n".join(lines)


def finalize(task, archive_root):
    task, archive_root = Path(task), Path(archive_root)
    require(task.is_absolute() and archive_root.is_absolute(), "Absolute task and archive paths are required")
    task, archive_root = task.resolve(), archive_root.resolve()
    source = task / "input/v4-manifest.json"
    v4 = read_json(source)
    require(v4.get("dataset") == "nav-demos-v4", "Expected the frozen v4 manifest")
    detours = {split: rows(task / "input" / name) for split, name in
               (("validation", "detour-val.jsonl"), ("test", "detour-test.jsonl"))}
    for name in ("v4-manifest.json", "detour-val.jsonl", "detour-test.jsonl", "holdout-cv.jsonl", "ppo-validation.jsonl"):
        require(digest(task / "input" / name) == digest(archive_root / "input" / name),
                f"Archived frozen input differs: {name}")
    heldout, forbidden_ids, forbidden_pairs, forbidden_groups = heldout_boundaries(v4, detours)
    forbidden_pairs.update(rl.historical_pairs([task / "input/holdout-cv.jsonl", task / "input/ppo-validation.jsonl"]))
    summary_path = archive_root / "recordings/collection-metrics.json"
    if not summary_path.is_file():
        summary_path = task / "recordings/collection-metrics.json"
    summary = read_json(summary_path)
    episodes = verified_episodes(task, archive_root, summary, forbidden_ids, forbidden_pairs, forbidden_groups)
    groups = family_totals(episodes)
    safety = {}
    for name, selected in (("all", episodes), ("train", [entry for entry in episodes if entry["training_eligible"]]),
                           ("water", [entry for entry in episodes if entry["family"] == "water"])):
        unsafe = sum(entry["deep_water_frames"] > 0 or entry["swimming_frames"] > 0 for entry in selected)
        safety[name] = dict(episodes=len(selected), unsafe_episodes=unsafe, target_met=unsafe == 0,
            **{key: sum(entry[key] for entry in selected) for key in
               ("water_entries", "deep_water_entries", "deep_water_frames", "swimming_frames")})
    require(safety["train"]["target_met"], "Teacher entered deep water or swam in eligible training data")
    require(any(entry["local"] for entry in episodes) and any(not entry["local"] for entry in episodes),
            "Corpus must contain both local NPCs and strangers")
    dataset_bytes = tree_bytes(archive_root / "recordings")
    require(tree_bytes(archive_root) <= MAX_BYTES, "Corpus exceeds the 30 GB storage budget")
    comparisons = compare_v4(episodes, v4)
    train = {family: [entry["id"] for entry in episodes
                     if entry["family"] == family and entry["training_eligible"]] for family in QUOTAS}
    manifest = dict(schema_version=1, dataset="nav-demos-v5", consumer="nav-bc", input_variant="hd",
        complete=True, root=str(archive_root / "recordings"),
        format=dict(name="NAVDEMO", version=3, navigation_memory_version=4, perception_mode="gaze", physics="cv"),
        episodes=episodes, splits=dict(train=train, **heldout),
        totals=dict(episodes=len(episodes), decisions=sum(entry["decisions"] for entry in episodes),
                    training_episodes=sum(entry["training_eligible"] for entry in episodes),
                    training_decisions=sum(entry["decisions"] for entry in episodes if entry["training_eligible"]),
                    local_episodes=sum(entry["local"] for entry in episodes), dataset_bytes=dataset_bytes),
        sources={"v4_manifest": dict(path=str(archive_root / "input/v4-manifest.json"), sha256=digest(source),
                    original="reports/navigation/2026-10-21-demos-v4/manifest.json", root=v4["root"]),
                 "detour_validation": dict(path=str(archive_root / "input/detour-val.jsonl"),
                    sha256=digest(task / "input/detour-val.jsonl")),
                 "detour_test": dict(path=str(archive_root / "input/detour-test.jsonl"),
                    sha256=digest(task / "input/detour-test.jsonl")),
                 "holdout_cv": dict(path=str(archive_root / "input/holdout-cv.jsonl"),
                    sha256=digest(task / "input/holdout-cv.jsonl")),
                 "ppo_validation": dict(path=str(archive_root / "input/ppo-validation.jsonl"),
                    sha256=digest(task / "input/ppo-validation.jsonl")),
                 "recordings/collection-metrics.json": dict(path=str(summary_path), sha256=digest(summary_path))},
        by_family=groups, paired_v4=comparisons, safety=safety, student_training_run=False)
    if "split_policy" in v4:
        manifest["split_policy"] = deepcopy(v4["split_policy"])
    full_path = archive_root / "manifest.json"
    encoded = json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    compact = compact_manifest(manifest, full_path, hashlib.sha256(encoded.encode("utf-8")).hexdigest())
    readme = report_text(manifest, groups, comparisons, compact["full_manifest"])
    artifacts = {"manifest.json": json.dumps(compact, ensure_ascii=False, separators=(",", ":")) + "\n",
                 "README.md": readme}
    require(sum(len(content.encode("utf-8")) for content in artifacts.values()) <= MAX_REPORT_BYTES,
            "Final report and compact manifest exceed 1.5 MB")
    added_bytes = sum(len(content.encode("utf-8")) -
                      ((archive_root / "report" / name).stat().st_size
                       if (archive_root / "report" / name).is_file() else 0)
                      for name, content in artifacts.items())
    added_bytes += len(encoded.encode("utf-8")) - (full_path.stat().st_size if full_path.is_file() else 0)
    require(tree_bytes(archive_root) + added_bytes <= MAX_BYTES,
            "Final artifacts exceed the 30 GB storage budget")
    full_path.write_text(encoded, encoding="utf-8")
    for destination in (archive_root / "report", task / "source" / REPORT):
        destination.mkdir(parents=True, exist_ok=True)
        for name, content in artifacts.items():
            (destination / name).write_text(content, encoding="utf-8")
    return dict(complete=True, episodes=len(episodes), training_episodes=manifest["totals"]["training_episodes"],
                dataset_bytes=dataset_bytes, report=str(archive_root / "report/README.md"),
                manifest=str(archive_root / "report/manifest.json"), full_manifest=str(full_path))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--archive-root", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(finalize(args.task, args.archive_root), ensure_ascii=False))


if __name__ == "__main__":
    main()
