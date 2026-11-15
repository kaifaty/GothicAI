"""Checks for the historical random-branch fixture, without running an engine."""
import importlib.util
import json
from pathlib import Path
import sys

import pytest

spec = importlib.util.spec_from_file_location("body_headless_suite", Path(__file__).parents[1] / "body_headless_suite.py")
suite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suite)


def test_engine_library_set_binds_every_archive_and_ipc_expectation(tmp_path, monkeypatch):
    monkeypatch.syspath_prepend(str(Path(__file__).parents[1]))
    import body_headless_ipc_probe as probe

    first, second = tmp_path / "first", tmp_path / "second"
    names = ("libOpenGothic.a", "libengine-platform.a", "ai_bridge/libAiBridge.a")
    for directory in (first, second):
        for name in (*names, "lib/libdependency.a", "_deps/vendor/libvendor.a", "libminiz.a"):
            path = directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"original")
    baseline = suite.engine_library_provenance(first)
    assert baseline["engine_libraries"] == {Path(name).name: suite.sha256(first / name) for name in names}
    expected = baseline["engine_library_sha256"]
    assert expected != suite.sha256(first / "libOpenGothic.a")
    assert suite.engine_library_provenance(second) == baseline

    reference = tmp_path / "reference"
    (reference / "run").mkdir(parents=True)
    (reference / "run/Gothic.ini").write_text("settings")
    fixture = tmp_path / "fixture"
    fixture.write_text("fixture")
    monkeypatch.setattr(sys, "platform", "linux")

    def stop_before_engine(*args, **kwargs):
        raise RuntimeError("library check passed; engine must not run in this test")

    monkeypatch.setattr(probe.suite, "Runner", stop_before_engine)

    def ipc(directory, digest, label):
        output = tmp_path / label
        monkeypatch.setattr(sys, "argv", [str(probe.__file__), "--runner", str(fixture),
                            "--assets", str(tmp_path), "--save", str(fixture), "--scenario", str(fixture),
                            "--reference", str(reference), "--output", str(output),
                            "--engine-library", str(directory), "--expected-library-sha256", digest])
        assert probe.main() == 2
        return json.loads((output / "ipc-probe.json").read_text())

    accepted = ipc(first, expected, "accepted")
    assert accepted["error"].startswith("library check passed")
    assert accepted["provenance"]["engine_libraries"] == baseline["engine_libraries"]
    for index, name in enumerate(names):
        (second / name).write_bytes(b"changed")
        assert suite.engine_library_provenance(second)["engine_library_sha256"] != expected
        rejected = ipc(second, expected, f"changed-{index}")
        assert rejected["error"] == "engine library set differs from the recorded full mechanics build"
        (second / name).write_bytes(b"original")
    legacy = ipc(first, suite.sha256(first / "libOpenGothic.a"), "legacy")
    assert legacy["error"] == "engine library set differs from the recorded full mechanics build"
    (second / "libengine-platform.a").unlink()
    missing = ipc(second, expected, "missing")
    assert "engine library missing:" in missing["error"] and "libengine-platform.a" in missing["error"]


def write_trace(tmp_path, attempts):
    rows = [dict(kind="body_combat", phase="episode_start", seq=1, t_game=100,
                 seed=17, actor_id=3, target_id=4)]
    rows += [dict(kind="body_combat", phase="native_attempt", seq=index+2, t_game=200,
                  actor_id=3, target_id=4, actor="SAME_SYMBOL", target="SAME_SYMBOL",
                  accepted=True, pose_changed=True, requested_anim=35,
                  layers_after=[{"name": "T_1HPARADE_0_A3"}], **change)
             for index, change in enumerate(attempts)]
    rows.append(dict(kind="body_combat", phase="episode_end", seq=len(rows)+1, t_game=500))
    path = tmp_path / "events.jsonl"
    path.write_text("".join(json.dumps(row)+"\n" for row in rows))
    return path


def test_fixture_captures_only_random_clip_and_maps_lifetime_ids(tmp_path):
    path = write_trace(tmp_path, [{}])
    fixture = suite.animation_fixture(path)
    assert fixture["source_events_sha256"] == suite.sha256(path)
    assert fixture["entries"] == [dict(seed=17, role="actor", relative_time_ms=100,
                                       animation="T_1HPARADE_0_A3", variant=2, source_seq=2)]
    assert not set(fixture["entries"][0]) & {"hp", "outcome", "primitive", "requested_anim"}


def test_duplicate_random_opportunity_is_rejected(tmp_path):
    path = write_trace(tmp_path, [{}, {}])
    with pytest.raises(RuntimeError, match="duplicate branch"):
        suite.animation_fixture(path)


def test_fixture_cannot_use_incomplete_episode(tmp_path):
    path = write_trace(tmp_path, [{}])
    path.write_text("\n".join(path.read_text().splitlines()[:-1])+"\n")
    with pytest.raises(RuntimeError, match="incomplete"):
        suite.animation_fixture(path)
