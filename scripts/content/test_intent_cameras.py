"""Camera coordinates and visibility operate on the engine's world conventions."""

import csv
import json
import math
import sys

import numpy as np

import intent_cameras
from intent_cameras import Occluders, camera_commands


def test_world_camera_round_trip_preserves_eye_and_target():
    eye = np.array([121.5, 580.2, -732.1])
    hero, target, distance, height, yaw = camera_commands([40.4, 243.1, 54.8], eye)
    angle = math.radians(yaw)
    reconstructed_eye = np.asarray(hero) + [math.cos(angle) * distance, height, math.sin(angle) * distance]
    assert np.allclose(reconstructed_eye, eye)
    assert np.allclose(np.asarray(hero) + [0, 45, 0], target)
    assert np.linalg.norm(np.asarray(target) - [40.4, 243.1, 54.8]) < 1


def test_ray_rejects_occluder_but_allows_target_surface_endpoint():
    wall = [[0, -100, -100], [0, 100, -100], [0, 0, 100]]
    occluders = Occluders([wall])
    assert occluders.first_hit([-50, 0, 0], [50, 0, 0]) == 50
    assert occluders.first_hit([50, 0, 0], [-50, 0, 0]) == 50
    assert occluders.first_hit([-50, 0, 0], [0, 0, 0]) is None
    assert occluders.first_hit([-50, 0, 500], [50, 0, 500]) is None


def test_large_triangle_radius_keeps_far_centroid_in_ray_query():
    # The triangle centroid is far from the short ray, but its surface crosses it.
    occluders = Occluders([[[0, -100000, -100000], [0, 100000, -100000], [0, 0, 100000]]])
    assert occluders.first_hit([-50, 0, 0], [50, 0, 0]) == 50


def test_cli_uses_mapping_and_census_without_private_intermediates(tmp_path, monkeypatch):
    # A fresh committed census and CSV must suffice: no semantics.json or
    # waypoints.json is created in this fixture. Keep geometry work out of it.
    fields = ("object", "role", "depicts", "colour", "weathering", "orientation")
    semantic = dict(zip(fields, ("timber house", "infill bay", "pale plaster, dark edges", "cream", "dirty edges", "top to bottom")))
    mapping = tmp_path / "mapping.csv"
    with mapping.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=("texture", *fields))
        writer.writeheader()
        writer.writerow({"texture": "WALL.TGA", **semantic})
    waypoint = {"name": "MARKT", "position": [10, 20, 30]}
    (tmp_path / "context.json").write_text(json.dumps({"waypoints": [waypoint], "occluders": []}))
    (tmp_path / "intent.json").write_text(json.dumps({"textures": [{"texture": "WALL.TGA"}]}))
    metadata = tmp_path / "cameras.json"
    scenario = tmp_path / "locations.bench"
    reads, writes = [], []

    def small_json_items(path, prefix):
        reads.append((path.name, prefix))
        return iter(json.loads(path.read_text())[prefix.removesuffix(".item")])

    def choose(row, meaning, city_tree, city_positions, waypoints, occluders):
        assert meaning == semantic
        assert waypoints == [waypoint]
        assert np.array_equal(city_positions, [[10, 20, 30]])
        return {"texture": row["texture"], "semantic": meaning, "patch_index": 0,
                "selection": "city", "surface_samples_visible": 1.0}

    monkeypatch.setattr(intent_cameras, "json_items", small_json_items)
    monkeypatch.setattr(intent_cameras, "load_occluders", lambda directory: None)
    monkeypatch.setattr(intent_cameras, "choose_camera", choose)
    monkeypatch.setattr(intent_cameras, "write_scenario", lambda path, cameras: writes.append((path, cameras)))
    monkeypatch.setattr(sys, "argv", ["intent_cameras.py", "--input", str(tmp_path), "--mapping", str(mapping),
                                      "--scenario", str(scenario), "--metadata", str(metadata)])
    intent_cameras.main()
    assert json.loads(metadata.read_text())[0]["semantic"] == semantic
    assert reads == [("context.json", "waypoints.item"), ("intent.json", "textures.item")]
    assert writes[0][0] == scenario
