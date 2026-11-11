"""Check packed engine formats against independently assembled byte streams."""
from pathlib import Path
import struct
import sys

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from navsim.formats import read_rays, read_static


def test_engine_snapshots_are_unaligned_little_endian(tmp_path):
    static = tmp_path / "static.bin"
    vertices = tuple(float(i * 100 - 500) for i in range(36))
    static.write_bytes(b"NAVSTAT1" + struct.pack("<II36f4B", 4, 0, *vertices, 1, 2, 3, 4))
    mesh = read_static(static)
    np.testing.assert_array_equal(mesh.vertices_cm.ravel(), vertices)
    np.testing.assert_array_equal(mesh.categories, [1, 2, 3, 4])

    rays = tmp_path / "rays.bin"
    payload = bytearray(b"NAVRAYS1" + struct.pack("<IIf", 238, 2, 1000))
    for update in range(2):
        payload += struct.pack("<Q3fB", 100 + update * 100, 101.25, -12.5, 2000, 1 - update)
        for index in range(238):
            payload += struct.pack("<5fBfBB", 1, 0, 0, 10000, index + .5, index % 6, -.75, 1, 1 - update)
    rays.write_bytes(payload)
    eye = read_rays(rays)
    assert rays.stat().st_size == 20 + 2 * (21 + 238 * 27)
    assert eye.near_range_cm == 1000
    np.testing.assert_array_equal(eye.updates["elapsed_ms"], [100, 200])
    np.testing.assert_array_equal(eye.updates["far_updated"], [1, 0])
    np.testing.assert_array_equal(eye.updates[1]["origin"], [101.25, -12.5, 2000])
    for update in range(2):
        ray = eye.updates[update]["rays"][237]
        np.testing.assert_array_equal(ray["direction"], [1, 0, 0])
        assert (ray["max_range_cm"], ray["distance_cm"], ray["kind"], ray["normal_y"],
                ray["hit"], ray["complete"]) == (10000, 237.5, 3, -.75, 1, 1 - update)


def test_episode_package_preserves_engine_values_and_verifies_files(tmp_path):
    import hashlib
    import json

    import pytest
    from navsim.formats import read_package

    # OGNAV001's native 64-bit Detour tile references and unaligned tile header.
    tile = b"opaque Detour tile bytes"
    checksum = 14695981039346656037
    for value in tile:
        checksum = ((checksum ^ value) * 1099511628211) & ((1 << 64) - 1)
    mesh = struct.pack("<8sIIQd3f2f2iQ", b"OGNAV001", 2, 0x01020304, 123, .25,
                       -20, 0, 30, 25.6, 25.6, 1, 32768, 1)
    mesh += struct.pack("<QIQ", 1 << 40, len(tile), checksum) + tile
    (tmp_path / "navmesh.navmesh").write_bytes(mesh)
    documents = {
        "navmesh-params.json": {"nearest_horizontal_m": 1, "nearest_vertical_m": 2},
        "links.json": [{"start_m": [0, 0, 0], "end_m": [2, -1, 0], "radius_m": .6,
                        "type": 3, "bidirectional": False, "id": 42}],
        "roads-graph.json": {"nodes": [["HOME", 100, 200, 300], ["GOAL", 500, 200, 300]],
                             "edges": [[0, 1, 400, True]]},
        "places.json": {"world": "NEWWORLD", "places": [{"id": "CITY", "center": [100, 200, 300]}],
                        "points": {"HOME": 0, "GOAL": 0}},
    }
    for name, value in documents.items():
        (tmp_path / name).write_text(json.dumps(value), encoding="utf-8")
    actor = dict(instance="CITIZEN", home_cm=[100, 200, 300], stranger=False,
                 known_node_ids=[0, 1], known_edge_ids=[0])
    (tmp_path / "actors.jsonl").write_text(json.dumps(actor) + "\n", encoding="utf-8")
    pair = dict(start="HOME", goal="GOAL", region="city", teacher_success=True)
    for split in ("train", "validation", "test"):
        (tmp_path / f"{split}.jsonl").write_text(json.dumps(pair) + "\n", encoding="utf-8")
    check = dict(split="validation", pair_index=0, start="HOME", goal="GOAL", start_m=[1, 2, 3],
                 end_m=[5, 2, 3], allow_transitions=True, complete=True, length_m=4, reason="ok")
    (tmp_path / "path-checks.jsonl").write_text(json.dumps(check) + "\n", encoding="utf-8")
    files = [dict(path=p.name, size=p.stat().st_size, sha256=hashlib.sha256(p.read_bytes()).hexdigest())
             for p in sorted(tmp_path.iterdir())]
    manifest = dict(format="navsim-package-v1", world="NEWWORLD", engine_revision="abc123", files=files)
    (tmp_path / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")

    package = read_package(tmp_path)
    assert package.manifest == manifest
    assert package.navmesh.key == 123
    assert package.navmesh.origin_m == (-20, 0, 30)
    assert package.navmesh.tiles[0].reference == 1 << 40
    assert bytes(package.navmesh.tiles[0].data) == tile
    assert not package.navmesh.tiles[0].data.flags.writeable
    assert package.links == documents["links.json"]
    assert package.roads == documents["roads-graph.json"]
    assert package.actors == [actor]
    assert package.places == documents["places.json"]
    assert package.pairs == {split: [pair] for split in ("train", "validation", "test")}
    assert package.path_checks == [check]
    (tmp_path / "train.jsonl").write_text("{}\n", encoding="utf-8")
    with pytest.raises(ValueError, match="Package size/SHA-256 mismatch: train.jsonl"):
        read_package(tmp_path)


def test_population_reward_comparison_matches_order_and_all_path_fields(tmp_path):
    import json
    from navsim.population_runs import comparison

    roots = [tmp_path / name for name in ("first", "second")]
    sample = dict(time_ms=100, next_time_ms=200, elapsed_ms=0, action_indices=[1, 0],
                  rewards=dict(progress_cm=50, blocked=0, time_ms=100), path_before_cm=500,
                  path_after_cm=450, path_valid=True, terminated=False, truncated=False)
    # Sequence one reuses slot zero: episode identity is not the population slot.
    for root in roots:
        for sequence in range(2):
            directory = root / "episodes" / str(sequence)
            directory.mkdir(parents=True)
            metadata = dict(start=f"FROM{sequence}", goal="TO", population_index=0,
                            pair_index=sequence, episode_index=sequence)
            (directory / "episode.json").write_text(json.dumps(metadata))
            (directory / "samples.jsonl").write_text(json.dumps(dict(sample, logits=[sequence])) + "\n")
            (directory / "trajectory.txt").write_text("0 10 20 30 40\n100 50 60 70 80\n")
    other = roots[1] / "episodes/1/samples.jsonl"
    other.write_text(json.dumps(dict(sample, logits=[999])) + "\n")
    equal = comparison(*roots)
    assert equal["deterministic"] and equal["compared_samples"] == 2
    other.write_text(json.dumps(dict(sample, path_valid=False, rewards=dict(sample["rewards"], blocked=1))) + "\n")
    changed = comparison(*roots)
    assert not changed["deterministic"] and changed["differing_samples"] == 1
    assert changed["examples"][0]["episode"] == 1
    assert changed["examples"][0]["fields"] == ["path_valid", "rewards"]


def test_package_launcher_isolates_duplicate_jobs_and_refuses_published_revision(tmp_path):
    import json
    import os
    import shutil
    import subprocess

    project = tmp_path / "project"
    launcher = project / "scripts/navsim/package-remote.sh"
    launcher.parent.mkdir(parents=True)
    shutil.copyfile(Path(__file__).resolve().parents[1] / "navsim/package-remote.sh", launcher)
    tools = tmp_path / "tools"
    tools.mkdir()
    stub = tools / "stub"
    stub.write_text("""#!/usr/bin/env python3
import json
import os
from pathlib import Path
import subprocess
import sys
name, args = Path(sys.argv[0]).name, sys.argv[1:]
if name == 'git':
    if 'rev-parse' in args:
        print(('1' if args[-1] == 'HEAD' else '2') * 40)
    elif 'status' not in args:
        sys.exit('Unexpected git operation: ' + str(args))
elif name == 'ssh':
    environment = dict(os.environ, HOME=os.environ['MOCK_REMOTE_HOME'])
    command = args[-1].replace('/mnt/shared', os.environ['MOCK_NAS'])
    sys.exit(subprocess.run(['bash', '-c', command], env=environment).returncode)
elif name == 'gq':
    fd = os.open(os.environ['MOCK_GQ_LOG'], os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    os.write(fd, (json.dumps(args) + '\\n').encode())
    os.close(fd)
else:
    sys.exit('Unexpected tool: ' + name)
""")
    stub.chmod(0o755)
    for name in ("git", "ssh", "rsync"):
        (tools / name).symlink_to(stub)
    (project / "scripts/gq").symlink_to(stub)
    remote = tmp_path / "remote"
    task = remote / "gothic-ai/nav-sim-stand-package"
    revision, baseline = "1" * 40, "2" * 40
    for path in (task / "source" / revision, task / "main" / baseline):
        path.mkdir(parents=True)
    submissions = tmp_path / "submissions.jsonl"
    nas = tmp_path / "nas"
    environment = dict(os.environ, PATH=f"{tools}{os.pathsep}{os.environ['PATH']}",
                       MOCK_REMOTE_HOME=str(remote), MOCK_NAS=str(nas), MOCK_GQ_LOG=str(submissions))
    processes = [subprocess.Popen(["bash", str(launcher)], env=environment, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True) for _ in range(2)]
    for process in processes:
        stdout, stderr = process.communicate(timeout=10)
        assert process.returncode == 0, stdout + stderr
    jobs = [json.loads(line) for line in submissions.read_text().splitlines()]
    assert len(jobs) == 2
    archives = [job[job.index("--archive") + 1] for job in jobs]
    assert archives[0] != archives[1]
    for job, archive in zip(jobs, archives):
        assert job[-5:] == ["run", str(task), revision, baseline, archive]
        assert Path(archive).parent == task and Path(archive).name.startswith(f"raw-{revision}-")
    for kind in ("package", "determinism", "reference"):
        published = nas / "gothic-ai/nav-sim-research/stand" / kind / revision
        published.mkdir(parents=True)
        rejected = subprocess.run(["bash", str(launcher)], env=environment, capture_output=True, text=True, timeout=10)
        assert rejected.returncode != 0, f"Existing {kind}/{revision} was submitted"
        assert len(submissions.read_text().splitlines()) == 2
        published.rmdir()
