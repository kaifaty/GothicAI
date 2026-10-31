"""Asset-free finalization checks using tiny genuine NAVDEMO v3 recordings."""
from copy import deepcopy
import gzip
import importlib.util
import json
from pathlib import Path
import shutil

import numpy as np
import pytest


SPEC = importlib.util.spec_from_file_location("nav_demos_v5_report",
    Path(__file__).resolve().parents[1] / "nav_demos_v5_report.py")
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def write_recording(directory, family, index):
    directory.mkdir(parents=True)
    data = np.zeros(3, dtype=report.demos.RECORD_V3_DTYPE)
    data["time_ms"] = [1000, 1100, 1200]
    data["keys"]["forward"] = [1, -1, 0]
    data["keys"]["strafe"] = [0, 0, 1]
    data["keys"]["walk"] = 1
    data["keys"]["jump"] = [0, 1, 0]
    data["keys"]["climb"] = [0, 0, 1]
    data["previous_keys"][1:] = data["keys"][:-1]
    with gzip.open(directory / "samples.bin.gz", "wb") as stream:
        stream.write(report.demos.HEADER.pack(report.demos.MAGIC, 3,
            report.demos.RECORD_BYTES_BY_VERSION[3], report.demos.RAY_COUNT,
            report.demos.NEAR_SIZE, report.demos.FAR_SIZE, report.demos.PERIOD_MS))
        stream.write(data.tobytes())
    actions = np.zeros((4, 9))
    actions[:, 0] = [0, 100, 200, 250]
    for column, name in enumerate(("forward", "strafe", "turn", "walk", "jump", "climb",
                                   "head_yaw_deg", "head_pitch_deg"), 1):
        actions[:-1, column] = data["keys"][name]
    actions[-1, 4] = 1
    np.savetxt(directory / "actions.txt", actions)
    np.savetxt(directory / "trajectory.txt",
               [[0, 0, 0, 0, 0], [100, 30, 0, 0, 0], [200, 30, 0, 40, 0], [250, 60, 0, 80, 0]])
    metadata = dict(id=directory.name, goal_kind="follow" if family == "follow_below_gate" else
                    "point" if family in ("water", "jump") else family,
        split="train", start=f"S{index}", goal=f"G{index}", success=True,
        decisions=3, frames=3, elapsed_ms=250, navigation_memory_version=4,
        eye_layout=[[0, 0, 4000]] * report.demos.RAY_COUNT,
        physics_mode="cv", perception_mode="gaze", teacher_version=2 if family == "jump" else 3,
        teacher_variant="v2" if family == "jump" else "v3b", npc=f"NPC{index}",
        teacher_familiar_hint=False,
        local=index % 2 == 0, residency="local" if index % 2 == 0 else "stranger",
        knownroads={"known_nodes": 10 if index % 2 == 0 else 0},
        knownroads_selection={"source": "knownroads"}, training_eligible=True, detour_length_cm=100)
    if family == "object":
        metadata["place_id"] = "train-place"
    elif family in ("approach", "follow_below_gate"):
        metadata["region"] = "train-region"
    elif family == "jump":
        metadata.update(site_id="train-site", transition_id="train-transition")
    write_json(directory / "episode.json", metadata)
    frames = [dict(elapsed_ms=elapsed, water_depth_cm=0, deep_water_threshold_cm=120, swimming=False)
              for elapsed in (0, 100, 200, 250)]
    (directory / "perception-safety-frames.jsonl").write_text(
        "".join(json.dumps(frame) + "\n" for frame in frames))
    checked = report.demos.summarize(directory, validate=True)
    checksum = report.digest(directory / "samples.bin.gz")
    metrics = dict(id=directory.name, family=family, split="train", episode=str(directory),
        scenario={key: metadata[key] for key in ("id", "start", "goal", "split", "goal_kind",
                  "place_id", "region", "site_id", "transition_id") if key in metadata},
        decisions=3, teacher_goal_success=True, training_eligible=True, samples_sha256=checksum,
        validated=True, key_counts=checked["key_counts"], local=metadata["local"],
        residency=metadata["residency"], **report.physical_water(directory, 250))
    metadata.update(samples_sha256=checksum, collection_metrics=deepcopy(metrics))
    write_json(directory / "episode.json", metadata)
    return metrics


@pytest.fixture
def corpus(tmp_path, monkeypatch):
    monkeypatch.setattr(report, "QUOTAS", {family: 1 for family in report.QUOTAS})
    task, archive = tmp_path / "task", tmp_path / "storage"
    entries = [write_recording(archive / "recordings" / family / "train" / f"{family}-0", family, index)
               for index, family in enumerate(report.QUOTAS)]
    previous = tmp_path / "v4" / "point-0"
    previous.mkdir(parents=True)
    write_json(previous / "episode.json", dict(decisions=3, elapsed_ms=250, goal_kind="point", detour_length_cm=100))
    np.savetxt(previous / "trajectory.txt",
               [[0, 0, 0, 0, 0], [100, 40, 0, 0, 0], [200, 80, 0, 0, 0], [250, 100, 0, 0, 0]])
    holdout = lambda split: dict(id=f"heldout-{split}", split=split, start=f"{split}-S", goal=f"{split}-G",
        episode=None, recorded=False, decisions=0)
    v4 = dict(dataset="nav-demos-v4", root=str(tmp_path / "v4"),
        episodes=[dict(deepcopy(entries[0]), episode=str(previous)),
                  dict(deepcopy(entries[2]), episode="/never-open-moving-heldout")],
        splits=dict(train={"point": ["point-0"]},
                    validation={"point": [holdout("validation")]}, test={"point": [holdout("test")]}),
        split_policy=dict(scope="family_specific", globally_disjoint_spatial_regions=False,
            object=dict(group="place_id", groups=dict(train=["train-place"], validation=["heldout-place"], test=[])),
            moving=dict(group="region", groups=dict(train=["train-region"], validation=["heldout-region"], test=[])),
            jump=dict(group=["site_id", "transition_id"], groups=dict(train=["train-site"],
                validation=["heldout-site"], test=[]))))
    write_json(task / "input/v4-manifest.json", v4)
    for split in ("val", "test"):
        (task / "input" / f"detour-{split}.jsonl").write_text(json.dumps(
            dict(id=f"detour-{split}", start=f"detour-{split}-S", goal=f"detour-{split}-G")) + "\n")
    for name in ("holdout-cv", "ppo-validation"):
        (task / "input" / f"{name}.jsonl").write_text(json.dumps(
            dict(start=f"{name}-S", goal=f"{name}-G")) + "\n")
    shutil.copytree(task / "input", archive / "input")
    summary = dict(complete=True, navigation_memory_version=4, demo_version=3,
                   student_training_run=False, episodes=entries)
    write_json(archive / "recordings/collection-metrics.json", summary)
    return task, archive, summary, v4


def no_outputs(task, archive):
    assert not (archive / "report").exists()
    assert not (archive / "manifest.json").exists()
    assert not (task / "source" / report.REPORT).exists()


def test_finalization_checks_all_samples_and_keeps_heldout_unrecorded(corpus, monkeypatch):
    task, archive, summary, v4 = corpus
    checked = []
    summarize = report.demos.summarize

    def validate(path, validate=False):
        assert validate is True
        checked.append(str(path))
        return summarize(path, validate=validate)

    monkeypatch.setattr(report.demos, "summarize", validate)
    before = {path: (path.read_bytes(), path.stat().st_mtime_ns)
              for path in Path(v4["root"]).rglob("*") if path.is_file()}
    result = report.finalize(task, archive)
    assert result["episodes"] == result["training_episodes"] == 6
    assert set(checked) == {entry["episode"] for entry in summary["episodes"]}
    manifest = report.read_json(archive / "manifest.json")
    assert manifest["format"] == dict(name="NAVDEMO", version=3,
        navigation_memory_version=4, perception_mode="gaze", physics="cv")
    assert manifest["consumer"] == "nav-bc" and manifest["input_variant"] == "hd"
    assert manifest["splits"]["validation"]["point"] == v4["splits"]["validation"]["point"]
    assert manifest["splits"]["test"]["point"] == v4["splits"]["test"]["point"]
    for split in ("validation", "test"):
        assert len(manifest["splits"][split]["detour"]) == 1
        assert manifest["splits"][split]["detour"][0]["recorded"] is False
    assert all(Path(entry["episode"]).is_relative_to(archive) for entry in manifest["episodes"])
    assert manifest["by_family"]["point"]["backward_fraction"] == pytest.approx(1 / 3)
    assert manifest["by_family"]["point"]["jump"] == 1
    assert manifest["totals"]["local_episodes"] == 3
    for name in ("holdout_cv", "ppo_validation"):
        source = manifest["sources"][name]
        assert source["sha256"] == report.digest(source["path"])
    assert manifest["paired_v4"] == [dict(id="point-0", family="point", v4_travelled_cm=100,
        v5_travelled_cm=120, v4_length_over_detour=1, v5_length_over_detour=1.2)]
    assert {path: (path.read_bytes(), path.stat().st_mtime_ns) for path in before} == before
    for filename in ("manifest.json", "README.md"):
        assert (archive / "report" / filename).read_bytes() == (task / "source" / report.REPORT / filename).read_bytes()
    readme = (archive / "report/README.md").read_text()
    assert "6 train-эпизодов" in readme and "3/6 (50.00%)" in readme
    compact_path = archive / "report/manifest.json"
    compact = report.read_json(compact_path)
    assert compact["schema_version"] == 2
    assert compact["full_manifest"] == dict(path=str(archive / "manifest.json"),
        sha256=report.digest(archive / "manifest.json"))
    assert compact["full_manifest"]["sha256"] in readme
    assert compact_path.read_text().count("\n") == 1
    for entry in compact["episodes"]:
        assert set(entry) == {"id", "family", "split", "training_eligible", "decisions", "samples_sha256", "episode"}
        assert not Path(entry["episode"]).is_absolute()
        assert (Path(compact["root"]) / entry["episode"]).is_dir()
    assert compact["splits"]["validation"] == dict(manifest="full_manifest", pointer="/splits/validation")
    total_bytes = sum((archive / "report" / name).stat().st_size for name in ("README.md", "manifest.json"))
    assert total_bytes <= report.MAX_REPORT_BYTES
    before_artifacts = {path: path.read_bytes() for path in (compact_path, archive / "manifest.json", archive / "report/README.md")}
    monkeypatch.setattr(report, "MAX_REPORT_BYTES", total_bytes - 1)
    with pytest.raises(ValueError, match="report and compact manifest exceed 1.5 MB"):
        report.finalize(task, archive)
    assert {path: path.read_bytes() for path in before_artifacts} == before_artifacts


@pytest.mark.parametrize("location", ["summary", "metadata", "metadata_metrics"])
def test_checksum_mismatch_blocks_final_artifacts(corpus, location):
    task, archive, summary, _ = corpus
    entry = summary["episodes"][0]
    if location == "summary":
        entry["samples_sha256"] = "0" * 64
        write_json(archive / "recordings/collection-metrics.json", summary)
    else:
        source = Path(entry["episode"]) / "episode.json"
        metadata = report.read_json(source)
        target = metadata if location == "metadata" else metadata["collection_metrics"]
        target["samples_sha256"] = "0" * 64
        write_json(source, metadata)
    with pytest.raises(ValueError, match="checksum differs"):
        report.finalize(task, archive)
    no_outputs(task, archive)


def test_archived_episode_absent_from_summary_is_rejected(corpus):
    task, archive, summary, _ = corpus
    summary["episodes"].pop()
    write_json(archive / "recordings/collection-metrics.json", summary)
    with pytest.raises(ValueError, match="inventory differs"):
        report.finalize(task, archive)
    no_outputs(task, archive)


def test_water_requires_native_physical_trace(corpus):
    task, archive, summary, _ = corpus
    water = next(entry for entry in summary["episodes"] if entry["family"] == "water")
    (Path(water["episode"]) / "perception-safety-frames.jsonl").unlink()
    with pytest.raises(ValueError, match="Missing physical water evidence"):
        report.finalize(task, archive)
    no_outputs(task, archive)


@pytest.mark.parametrize("eligible", [True, False])
def test_deep_water_blocks_training_but_retains_ineligible_diagnostics(corpus, eligible):
    task, archive, summary, _ = corpus
    entry = summary["episodes"][0]
    directory = Path(entry["episode"])
    frames = report.rows(directory / "perception-safety-frames.jsonl")
    frames[1]["water_depth_cm"] = 119
    (directory / "perception-safety-frames.jsonl").write_text("".join(json.dumps(row) + "\n" for row in frames))
    entry.update(report.physical_water(directory, 250), training_eligible=eligible)
    metadata = report.read_json(directory / "episode.json")
    metadata["training_eligible"] = eligible
    write_json(directory / "episode.json", metadata)
    write_json(archive / "recordings/collection-metrics.json", summary)
    if eligible:
        with pytest.raises(ValueError, match="deep water or swam"):
            report.finalize(task, archive)
        no_outputs(task, archive)
    else:
        report.finalize(task, archive)
        manifest = report.read_json(archive / "manifest.json")
        assert manifest["safety"]["all"]["unsafe_episodes"] == 1
        assert manifest["safety"]["all"]["deep_water_entries"] == 1
        assert manifest["safety"]["all"]["target_met"] is False
        assert manifest["safety"]["train"]["target_met"] is True
        assert manifest["safety"]["water"]["target_met"] is True
        assert entry["id"] not in manifest["splits"]["train"]["point"]
        assert len(manifest["episodes"]) == 6 and directory.is_dir()
        assert "цель 0 глубоких не выполнена" in (archive / "report/README.md").read_text()


def test_unmet_eligible_quota_does_not_publish(corpus):
    task, archive, summary, _ = corpus
    entry = next(entry for entry in summary["episodes"] if entry["family"] == "object")
    entry["training_eligible"] = False
    source = Path(entry["episode"]) / "episode.json"
    metadata = report.read_json(source)
    metadata["training_eligible"] = False
    write_json(source, metadata)
    write_json(archive / "recordings/collection-metrics.json", summary)
    with pytest.raises(ValueError, match="Unmet object quota: 0, expected 1"):
        report.finalize(task, archive)
    no_outputs(task, archive)


@pytest.mark.parametrize("source", ["v4", "holdout-cv", "ppo-validation", "detour-val", "detour-test"])
def test_recorded_heldout_pair_rejected_before_opening_samples(corpus, source):
    task, archive, summary, v4 = corpus
    forbidden = (v4["splits"]["validation"]["point"][0] if source == "v4"
                 else report.rows(task / "input" / f"{source}.jsonl")[0])
    entry = summary["episodes"][0]
    entry["scenario"].update(start=forbidden["goal"], goal=forbidden["start"])
    source = Path(entry["episode"]) / "episode.json"
    metadata = report.read_json(source)
    metadata.update(start=forbidden["goal"], goal=forbidden["start"])
    write_json(source, metadata)
    write_json(archive / "recordings/collection-metrics.json", summary)
    with pytest.raises(ValueError, match="Held-out scenario was recorded"):
        report.finalize(task, archive)
    no_outputs(task, archive)


@pytest.mark.parametrize("overlap", ["moving", "object_alias"])
def test_v4_family_specific_membership_allows_cross_group_endpoint_reuse(corpus, overlap):
    task, archive, summary, v4 = corpus
    entry = next(entry for entry in summary["episodes"] if entry["family"] ==
                 ("point" if overlap == "moving" else "object"))
    scenario = entry["scenario"]
    family = "approach" if overlap == "moving" else "object"
    forbidden = dict(id="different-heldout-task", split="validation", episode=None, recorded=False,
        start=scenario["start"], goal=scenario["goal"], goal_kind=family,
        **(dict(region="heldout-region", route=[scenario["goal"], scenario["start"]], target="SCAVENGER")
           if overlap == "moving" else dict(place_id="heldout-place")))
    v4["splits"]["validation"][family] = [forbidden]
    for root in (task, archive):
        write_json(root / "input/v4-manifest.json", v4)
    report.finalize(task, archive)
    manifest = report.read_json(archive / "manifest.json")
    assert entry["id"] in manifest["splits"]["train"][entry["family"]]
    assert manifest["splits"]["validation"][family] == [forbidden]


@pytest.mark.parametrize("family,key,group", [("object", "place_id", "heldout-place"),
    ("approach", "region", "heldout-region"), ("jump", "site_id", "heldout-site"),
    ("jump", "transition_id", "heldout-transition")])
def test_same_family_heldout_groups_block_other_endpoints(corpus, family, key, group):
    task, archive, summary, v4 = corpus
    entry = next(entry for entry in summary["episodes"] if entry["family"] == family)
    entry["scenario"][key] = group
    metadata_path = Path(entry["episode"]) / "episode.json"
    metadata = report.read_json(metadata_path)
    metadata[key] = group
    write_json(metadata_path, metadata)
    write_json(archive / "recordings/collection-metrics.json", summary)
    if key == "transition_id":
        v4["splits"]["test"]["jump"] = [dict(id="different-jump", split="test", episode=None,
            recorded=False, start="different-S", goal="different-G", site_id="another-heldout-site",
            transition_id=group)]
        for root in (task, archive):
            write_json(root / "input/v4-manifest.json", v4)
    with pytest.raises(ValueError, match="Held-out group was recorded"):
        report.finalize(task, archive)
    no_outputs(task, archive)


def test_physical_water_counts_transitions_and_native_chest_threshold(tmp_path):
    frames = [dict(elapsed_ms=index * 100, water_depth_cm=depth, deep_water_threshold_cm=120, swimming=swim)
              for index, (depth, swim) in enumerate(((0, False), (11, False), (119, False),
                   (121, True), (0, False), (11, False)))]
    (tmp_path / "perception-safety-frames.jsonl").write_text("".join(json.dumps(row) + "\n" for row in frames))
    metrics = report.physical_water(tmp_path, 500)
    assert metrics == dict(physical_frames=6, entered_water_frames=4, deep_water_frames=2,
        swimming_frames=1, water_entries=2, deep_water_entries=1, max_water_depth_cm=121)


@pytest.mark.parametrize("change", ["failure", "different_goal", "moving", "no_detour"])
def test_v4_comparison_excludes_unpaired_or_undefined_ratios(corpus, change):
    _, _, summary, v4 = corpus
    entry = dict(summary["episodes"][0], travelled_cm=120, length_over_detour=1.2)
    if change == "failure":
        v4["episodes"][0]["teacher_goal_success"] = False
    elif change == "different_goal":
        v4["episodes"][0]["scenario"]["goal"] = "ANOTHER"
    elif change == "moving":
        entry["scenario"]["goal_kind"] = "approach"
    else:
        entry["length_over_detour"] = None
    assert report.compare_v4([entry], v4) == []
