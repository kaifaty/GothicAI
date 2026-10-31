"""Budget and exact historical-link coverage for the CV probe collector."""
import importlib.util
import json
import os
import shlex
import subprocess
import sys
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location("nav_jumps", Path(__file__).resolve().parents[1] / "nav_jumps.py")
jumps = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(jumps)


def test_links_preserve_uint32_ids_and_reject_duplicate_ids(tmp_path):
    path = tmp_path / "links.txt"
    source = [dict(start=(1., 2., 3.), end=(4., 5., 6.), type=2, id=4294967295)]
    jumps.write_links(path, source)
    assert jumps.links(path) == source
    path.write_text(path.read_text() * 2)
    with pytest.raises(ValueError, match="Duplicate"):
        jumps.links(path)


def test_batch_requires_every_original_link_and_qualifies_success_cv(tmp_path):
    expected = [dict(start=(0., 0., 0.), end=(1., 0., 0.), type=1, id=42)]
    jumps.write_links(tmp_path / "accepted-links.txt", expected)
    path = tmp_path / "transitions.jsonl"
    row = dict(link_id=42, accepted=True, type="jump", reason="landed_without_damage", frames=10, cv_frames=4,
               expect_cv=True, forced_cv_frames=10, preparation_frames=4, preparation_cv_frames=4)
    path.write_text(json.dumps(row) + "\n")
    assert jumps.verify_batch(tmp_path, expected)[1] == expected
    row["preparation_cv_frames"] = 3
    path.write_text(json.dumps(row) + "\n")
    outcomes, accepted = jumps.verify_batch(tmp_path, expected)
    assert accepted == []
    assert outcomes[0]["accepted"] is False
    assert outcomes[0]["reason"] == "accepted_without_cv_support_evidence"
    row["preparation_cv_frames"] = 4
    row["link_id"] = 41
    path.write_text(json.dumps(row) + "\n")
    with pytest.raises(RuntimeError, match="Incomplete/duplicate"):
        jumps.verify_batch(tmp_path, expected)


@pytest.mark.parametrize("reason", ["cv_lost_during_preparation", "cv_not_active", "takeoff_no_ground",
                                   "takeoff_did_not_settle", "damage", "cv_forcing_lost"])
def test_rejected_candidate_never_aborts_the_batch(tmp_path, reason):
    expected = [dict(start=(0., 0., 0.), end=(1., 0., 0.), type=1, id=42)]
    jumps.write_links(tmp_path / "accepted-links.txt", [])
    row = dict(link_id=42, accepted=False, type="jump", reason=reason, frames=0,
               cv_frames=0, expect_cv=True, forced_cv_frames=0, preparation_frames=0, preparation_cv_frames=0)
    (tmp_path / "transitions.jsonl").write_text(json.dumps(row) + "\n")
    assert jumps.verify_batch(tmp_path, expected) == ([row], [])


def test_missing_runner_outcomes_still_abort(tmp_path):
    jumps.write_links(tmp_path / "accepted-links.txt", [])
    (tmp_path / "transitions.jsonl").write_text("")
    expected = [dict(start=(0., 0., 0.), end=(1., 0., 0.), type=1, id=42)]
    with pytest.raises(RuntimeError, match="Incomplete/duplicate"):
        jumps.verify_batch(tmp_path, expected)


def test_runner_cannot_change_candidate_type(tmp_path):
    expected = [dict(start=(0., 0., 0.), end=(1., 0., 0.), type=1, id=42)]
    jumps.write_links(tmp_path / "accepted-links.txt", [])
    row = dict(link_id=42, accepted=False, type="climb", reason="native_has_no_climb")
    (tmp_path / "transitions.jsonl").write_text(json.dumps(row) + "\n")
    with pytest.raises(RuntimeError, match="changed a candidate link type"):
        jumps.verify_batch(tmp_path, expected)


def test_reject_is_followed_by_a_valid_transition(tmp_path):
    expected = [dict(start=(0., 0., 0.), end=(1., 0., 0.), type=1, id=42),
                dict(start=(3., 0., 0.), end=(4., 0., 0.), type=2, id=43)]
    jumps.write_links(tmp_path / "accepted-links.txt", expected[1:])
    rejected = dict(link_id=42, accepted=False, type="jump", reason="cv_lost_during_preparation", frames=2, cv_frames=1)
    passed = dict(link_id=43, accepted=True, type="climb", reason="landed_without_damage", frames=60,
                  expect_cv=True, forced_cv_frames=60, preparation_frames=13, preparation_cv_frames=13,
                  candidate_preparation=True, preparation_qualified_ms=400)
    (tmp_path / "transitions.jsonl").write_text(json.dumps(rejected) + "\n" + json.dumps(passed) + "\n")
    outcomes, accepted = jumps.verify_batch(tmp_path, expected)
    assert outcomes == [rejected, passed]
    assert accepted == expected[1:]


def test_pilot_keeps_the_three_diagnostic_locations():
    values = [dict(start=(float(i), 0., 0.), end=(float(i) + 1., 0., 0.), type=kind,
                   id=(jumps.PILOT_CASES[kind - 1] if i == 7 else kind * 10000 + i))
              for kind in jumps.TYPES for i in range(100)]
    selected = jumps.pilot_links(values)
    assert set(jumps.PILOT_CASES) <= {row["id"] for row in selected}
    assert jumps.Counter(row["type"] for row in selected) == {1: 16, 2: 16, 3: 16}


def test_pilot_has_48_unfiltered_candidates_across_types_and_space():
    values = [dict(start=(float(i), 0., 0.), end=(float(i) + 1., 0., 0.), type=kind,
                   id=kind * 10000 + i) for kind in jumps.TYPES for i in range(100)]
    selected = jumps.pilot_links(values)
    assert len(selected) == len({row["id"] for row in selected}) == 48
    assert selected == jumps.pilot_links(list(reversed(values)))
    assert jumps.Counter(row["type"] for row in selected) == {1: 16, 2: 16, 3: 16}
    for kind in jumps.TYPES:
        group = [row for row in selected if row["type"] == kind]
        assert min(row["start"][0] for row in group) == 0.
        assert max(row["start"][0] for row in group) == 99.


def test_rejection_reasons_are_counted_by_type():
    counts = jumps.summary([dict(type="climb", accepted=False, reason="native_has_no_climb"),
                            dict(type="jump", accepted=True, reason="landed_without_damage")])
    assert counts["climb"] == dict(attempts=1, accepted=0, rejected_reasons={"native_has_no_climb": 1})
    assert counts["jump"]["accepted"] == 1
    assert counts["drop"]["attempts"] == 0


def test_discovery_counts_new_directed_links_only():
    old = dict(start=(0., 0., 0.), end=(1., 0., 0.), type=1, id=1)
    repeated = dict(old, start=(.2, 0., 0.), id=2)
    reverse = dict(old, start=old["end"], end=old["start"], id=3)
    climb = dict(old, type=2, id=4)
    assert jumps.novel_links([repeated, reverse, climb, climb], [old]) == [reverse, climb]


def test_remote_launch_requires_setup_before_directories_or_queue(tmp_path, monkeypatch):
    tools = tmp_path / "bin"
    tools.mkdir()
    calls = tmp_path / "ssh-calls"
    ssh = tools / "ssh"
    ssh.write_text(f"#!{sys.executable}\n" + """import os
from pathlib import Path
import subprocess
import sys
with Path(os.environ["SSH_CALLS"]).open("a") as stream:
    stream.write("ssh\\n")
command = sys.argv[-1]
if '"$HOME/gothic-ai/bin/gq"' in command:
    Path(os.environ["SSH_QUEUE"]).write_text(command)
    raise SystemExit(0)
remote_env = {key: value for key, value in os.environ.items()
              if key != "GOTHIC_DIR" and not key.startswith("OPENGOTHIC_BASE")
              and key != "OPENGOTHIC_VIDEOS_FINGERPRINT"}
raise SystemExit(subprocess.run(["bash", "-c", command], env=remote_env).returncode)
""")
    ssh.chmod(0o755)
    rsync = tools / "rsync"
    rsync.write_text("#!/bin/sh\nexit 0\n")
    rsync.chmod(0o755)
    home = tmp_path / "home"
    home.mkdir()
    for key in tuple(os.environ):
        if key == "GOTHIC_DIR" or key.startswith("OPENGOTHIC_BASE") or key == "OPENGOTHIC_VIDEOS_FINGERPRINT":
            monkeypatch.delenv(key)
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.setenv("PATH", str(tools) + os.pathsep + os.environ["PATH"])
    monkeypatch.setenv("SSH_CALLS", str(calls))
    queue = tmp_path / "queue-command"
    monkeypatch.setenv("SSH_QUEUE", str(queue))
    for filename, action in (("nav-jumps-remote.sh", "build"), ("ryzen.sh", "pytest")):
        calls.write_text("")
        monkeypatch.setenv("GOTHIC_DIR", "/Volumes/x")
        script = Path(__file__).resolve().parents[1] / filename
        result = subprocess.run(["bash", str(script), action], capture_output=True, text=True)
        assert result.returncode == 1
        assert "Run scripts/ryzen.sh setup first" in result.stderr
        assert calls.read_text().splitlines() == ["ssh"]
        assert not (home / "gothic-ai").exists()

    settings = home / "gothic-ai/game-settings"
    (settings / "system").mkdir(parents=True)
    for name in ("Gothic.ini", "SystemPack.ini"):
        (settings / "system" / name).write_text("")
    fingerprints = {"base": "a" * 64, "videos": "b" * 64}
    (home / "gothic-ai/game-base.json").write_text(json.dumps(fingerprints))
    monkeypatch.setenv("GOTHIC_DIR", "/Volumes/x")
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", "/custom/base-cache")
    script = Path(__file__).resolve().parents[1] / "nav-jumps-remote.sh"
    result = subprocess.run(["bash", str(script), "recheck"], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    payload = shlex.split(queue.read_text())
    assert str(home / "gothic-ai/nav-jumps/source/scripts/nav_job.py") in payload
    assert payload[payload.index("--max-seconds") + 1] == "7200"
    assert "/Volumes/x" not in queue.read_text()
    assert not any(value.startswith("GOTHIC_DIR=") for value in payload)
    assert f"OPENGOTHIC_BASE_FINGERPRINT={fingerprints['base']}" in payload
    assert f"OPENGOTHIC_VIDEOS_FINGERPRINT={fingerprints['videos']}" in payload
    assert "OPENGOTHIC_BASE_CACHE=/custom/base-cache" in payload
    assert payload[payload.index("--assets") + 1] == str(settings)
