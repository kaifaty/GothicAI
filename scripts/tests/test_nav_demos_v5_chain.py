"""V5 phases preserve quotas and evacuate only checksum-verified recordings."""
import importlib.util
import json
from pathlib import Path
import shutil
from types import SimpleNamespace

import pytest


SPEC = importlib.util.spec_from_file_location("nav_demos_v5_chain",
    Path(__file__).resolve().parents[1] / "nav_demos_v5_chain.py")
chain = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(chain)


def summary(count=1, *, complete=False):
    groups = {name: dict(episodes=target, training_eligible=target, complete=True)
              for name, target in chain.TARGETS.items()} if complete else {}
    return dict(episodes=[dict(id=f"saved-{index}") for index in range(count)], by_family=groups)


@pytest.fixture
def setup(tmp_path, monkeypatch):
    task, archive = tmp_path / "task", tmp_path / "archive"
    task.mkdir()
    calls = dict(collector=[], archive=[], submit=[], finalized=[], provenance=[])
    monkeypatch.setattr(chain, "require_room", lambda *args: 0)
    def collect(options):
        calls["collector"].append(options)
        return summary()
    monkeypatch.setattr(chain.collect, "run", collect)
    monkeypatch.setattr(chain, "archive_phase", lambda *args:
        calls["archive"].append(args) or dict(archived_directories=1, corpus_bytes=100))
    monkeypatch.setattr(chain, "submit_next", lambda *args, **kwargs:
        calls["submit"].append((args, kwargs)) or "012345abcdef")
    monkeypatch.setattr(chain, "module", lambda name: SimpleNamespace(finalize=lambda *args:
        calls["finalized"].append(args)))
    return task, archive, calls


def test_bounded_phase_preserves_fixed_targets_and_queues_one_continuation(setup):
    task, archive, calls = setup
    result = chain.run(task, archive)
    assert result["status"] == "continued" and result["next_job"] == "012345abcdef"
    assert calls["archive"] == [(task, archive)] and len(calls["submit"]) == 1
    options = calls["collector"][0]
    assert options.point_limit == 400 and options.max_bytes == 30_000_000_000
    assert options.corpus_version == 5 and options.archive_root == archive
    assert options.include_follow == "no" and options.follow_below_gate
    assert options.wall_minutes == 80 and options.limit_ms == 240000
    assert not result["student_training_run"] and not result["archive_pending"]
    assert json.loads((archive / "collection-chain.json").read_text())["next_job"] == "012345abcdef"


def test_prepare_uses_a_separate_allocation_and_never_records(setup, monkeypatch):
    task, archive, calls = setup
    monkeypatch.setattr(chain, "module", lambda name: SimpleNamespace(prepare=lambda *args:
        calls["provenance"].append((name, args))))
    monkeypatch.setattr(chain.collect, "prepare_residency", lambda options:
        calls["provenance"].append(("residency", options.task)), raising=False)
    monkeypatch.setattr(chain.collect, "prepare_v5_selections", lambda task:
        calls["provenance"].append(("selections", task)))
    monkeypatch.setattr(chain, "archive_provenance", lambda *args:
        calls["provenance"].append(("archive", args)))
    assert chain.run(task, archive, stage="prepare")["status"] == "continued"
    assert [value[0] for value in calls["provenance"]] == ["nav_demos_v5_water", "residency", "selections", "archive"]
    assert not calls["collector"] and not calls["archive"]
    assert len(calls["submit"]) == 1


def test_complete_corpus_finalizes_then_removes_only_own_build(setup, monkeypatch):
    task, archive, calls = setup
    build = task / "build"
    build.mkdir()
    (task / "input").mkdir()
    monkeypatch.setattr(chain.collect, "run", lambda options: summary(complete=True))
    assert chain.run(task, archive)["status"] == "complete"
    assert calls["finalized"] == [(task, archive)] and not calls["submit"]
    assert not build.exists() and (task / "input").is_dir()


def test_failed_finalization_keeps_build_and_never_queues(setup, monkeypatch):
    task, archive, calls = setup
    (task / "build").mkdir()
    monkeypatch.setattr(chain.collect, "run", lambda options: summary(complete=True))
    def fail(*args):
        raise ValueError("Missing sample hash")
    monkeypatch.setattr(chain, "module", lambda name: SimpleNamespace(finalize=fail))
    with pytest.raises(ValueError, match="Missing sample hash"):
        chain.run(task, archive)
    assert (task / "build").is_dir() and not calls["submit"]
    assert json.loads((task / "collection-chain.json").read_text())["status"] == "failed"


@pytest.mark.parametrize("result, expected", [
    (dict(summary(), stop_reason="disk_budget_exhausted"), "limit_reached"),
    (dict(summary(), by_family={"water": dict(candidate_pool_exhausted=True)}), "limit_reached"),
    (summary(0), "no_progress"),
])
def test_stopped_or_unproductive_phase_archives_and_does_not_continue(setup, monkeypatch, result, expected):
    task, archive, calls = setup
    monkeypatch.setattr(chain.collect, "run", lambda options: result)
    assert chain.run(task, archive)["status"] == expected
    assert calls["archive"] == [(task, archive)] and not calls["submit"]


def test_failed_archival_is_recovered_before_next_collection(setup, monkeypatch):
    task, archive, calls = setup
    chain.v4.atomic_json(task / "collection-chain.json", dict(status="failed", archive_pending=True))
    assert chain.run(task, archive)["status"] == "continued"
    assert calls["archive"] == [(task, archive), (task, archive)]


def copying_rsync(command, **kwargs):
    assert command[:3] == ["rsync", "-a", "--no-links"] and kwargs == dict(check=True)
    source, target = map(Path, command[-2:])
    for path in source.iterdir():
        if path.name.startswith("collection") and "--exclude=/collection*" in command:
            continue
        destination = target / path.name
        if path.is_dir():
            shutil.copytree(path, destination, dirs_exist_ok=True)
        else:
            shutil.copy2(path, destination)


def recording(tmp_path):
    task, archive = tmp_path / "task", tmp_path / "archive"
    episode = task / "recordings/water/train/water-001"
    episode.mkdir(parents=True)
    (episode / "samples.bin.gz").write_bytes(b"sample bytes")
    (episode / "episode.json").write_text('{"id":"water-001"}\n')
    chain.v4.atomic_json(task / "recordings/collection-metrics.json",
                         dict(episodes=[dict(episode=str(episode))]))
    chain.v4.atomic_json(task / "recordings/collection-state.json",
                         dict(rejected_candidates={"water/train/refused": dict(directory=str(
                             task / "recordings/water/train/refused"))}))
    return task, archive, episode


def test_verified_archive_deletes_local_episodes_and_retargets_controls(tmp_path, monkeypatch):
    task, archive, episode = recording(tmp_path)
    original = (episode / "episode.json").read_bytes()
    monkeypatch.setattr(chain.subprocess, "run", copying_rsync)
    result = chain.archive_phase(task, archive)
    saved = archive / "recordings/water/train/water-001"
    assert result["archived_directories"] == 1 and not episode.exists()
    assert (saved / "episode.json").read_bytes() == original
    for root in (task, archive):
        metrics = json.loads((root / "recordings/collection-metrics.json").read_text())
        state = json.loads((root / "recordings/collection-state.json").read_text())
        assert metrics["episodes"][0]["episode"] == str(saved)
        assert state["rejected_candidates"]["water/train/refused"]["directory"].startswith(str(archive))


def test_checksum_mismatch_retains_entire_local_phase(tmp_path, monkeypatch):
    task, archive, episode = recording(tmp_path)
    def corrupt(command, **kwargs):
        copying_rsync(command, **kwargs)
        (archive / "recordings/water/train/water-001/samples.bin.gz").write_bytes(b"corrupted")
    monkeypatch.setattr(chain.subprocess, "run", corrupt)
    with pytest.raises(ValueError, match="checksum mismatch"):
        chain.archive_phase(task, archive)
    assert (episode / "samples.bin.gz").read_bytes() == b"sample bytes"


def test_symlinks_are_rejected_before_any_transfer_or_deletion(tmp_path, monkeypatch):
    task, archive, episode = recording(tmp_path)
    (episode / "link").symlink_to(episode / "episode.json")
    calls = []
    monkeypatch.setattr(chain.subprocess, "run", lambda *args, **kwargs: calls.append(args))
    with pytest.raises(ValueError, match="Symlink is forbidden"):
        chain.archive_phase(task, archive)
    assert episode.exists() and not calls


def test_storage_admission_retains_20_gib_and_enforces_30_billion_byte_cap(tmp_path, monkeypatch):
    task, archive = tmp_path / "task", tmp_path / "archive"
    monkeypatch.setattr(chain.v4, "disk_check", lambda: None)
    monkeypatch.setattr(chain, "used_bytes", lambda *args: 10)
    monkeypatch.setattr(chain.shutil, "disk_usage", lambda path:
                        SimpleNamespace(free=chain.LOCAL_FREE_BYTES - 1))
    with pytest.raises(ValueError, match="local_floor"):
        chain.require_room(task, archive)
    monkeypatch.setattr(chain.shutil, "disk_usage", lambda path:
                        SimpleNamespace(free=chain.LOCAL_FREE_BYTES))
    assert chain.require_room(task, archive) == 10
    monkeypatch.setattr(chain, "used_bytes", lambda *args: chain.MAX_BYTES)
    with pytest.raises(ValueError, match="max=30000000000"):
        chain.require_room(task, archive)


def test_continuation_keeps_v5_owner_script_archive_and_stage(tmp_path, monkeypatch):
    task, archive = tmp_path / "task", tmp_path / "archive"
    monkeypatch.setattr(chain, "require_room", lambda *args: 0)
    calls = []
    monkeypatch.setattr(chain.v4, "submit_next", lambda *args, **kwargs:
                        calls.append((args, kwargs)) or "abcdef012345")
    assert chain.submit_next(task, archive, stage="prepare") == "abcdef012345"
    args, kwargs = calls[0]
    assert args == (task,) and kwargs["owner"] == "nav-demos-v5"
    assert Path(kwargs["script"]).name == "nav_demos_v5_chain.py"
    assert kwargs["extra"] == ("--archive-root", str(archive), "--stage", "prepare")
