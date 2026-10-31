"""Asset-free checks for held-out detour geometry preparation."""
from copy import deepcopy
import importlib.util
import json
import math
from pathlib import Path
from types import SimpleNamespace

import pytest


SPEC = importlib.util.spec_from_file_location(
    "nav_dagger3_detours", Path(__file__).resolve().parents[1] / "nav_dagger3_detours.py")
detours = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(detours)

MESH = dict(cell_size_m=.2, cell_height_m=.1, climb_m=.6)


def geometry(kind="point"):
    value = dict(id="detour-case", goal_kind=kind, start="START", goal="GOAL")
    identity = dict(label=value["id"], start=value["start"], goal=value["goal"])
    probe = dict(identity, start_supported=True, detour_complete=True, straight_blocked=True,
                 visible=True, visible_centre=False, visible_edge=True,
                 visibility_criterion="centre_or_opening_edge_v1")
    plan = dict(identity, start_cm=[0., 0., 0.], goal_cm=[1000., 0., 0.], length_cm=1300.,
                complete=True, straight_walkable=False, start_snap_cm=0., end_snap_cm=0.,
                corners_cm=[[0., 0., 0.], [500., 0., math.sqrt(650 ** 2 - 500 ** 2)],
                            [1000., 0., 0.]])
    return value, probe, plan


@pytest.mark.parametrize("target,changes", [
    ("plan", dict(length_cm=1299.)),
    ("plan", dict(straight_walkable=True)),
    ("plan", dict(complete=False)),
    ("probe", dict(straight_blocked=False)),
    ("probe", dict(detour_complete=False)),
    ("probe", dict(start_supported=False)),
    ("plan", dict(length_cm=float("nan"))),
    ("plan", dict(start_snap_cm=float("inf"))),
    ("plan", dict(corners_cm=[])),
])
def test_qualification_requires_every_geometry_gate(target, changes):
    value, probe, plan = geometry()
    assert detours.qualified(value, probe, plan, MESH)
    (probe if target == "probe" else plan).update(changes)
    assert not detours.qualified(value, probe, plan, MESH)


@pytest.mark.parametrize("target", ["probe", "plan"])
@pytest.mark.parametrize("field", ["label", "start", "goal"])
def test_native_geometry_must_match_the_exact_candidate(target, field):
    value, probe, plan = geometry()
    (probe if target == "probe" else plan)[field] = "ANOTHER"
    with pytest.raises(ValueError, match="geometry identity"):
        detours.qualified(value, probe, plan, MESH)


def test_object_requires_the_original_visibility_witness_but_point_does_not():
    value, probe, plan = geometry("object")
    assert detours.qualified(value, probe, plan, MESH)  # Opening edge alone is valid.
    assert not detours.qualified(value, dict(probe, visible=False), plan, MESH)
    assert not detours.qualified(value, dict(probe, visibility_criterion="centre_only"), plan, MESH)
    assert detours.qualified(dict(value, goal_kind="point"), dict(probe, visible=False), plan, MESH)


@pytest.mark.parametrize("endpoint", [0, -1])
@pytest.mark.parametrize("offset,expected", [
    ([0., -46., 0.], True),
    ([0., -80., 0.], True),
    ([0., -81., 0.], False),
    ([20., 0., 0.], True),
    ([21., 0., 0.], False),
])
def test_both_endpoints_use_separate_horizontal_and_vertical_surface_limits(endpoint, offset, expected):
    value, probe, plan = geometry()
    plan["corners_cm"][endpoint] = [a + b for a, b in zip(plan["corners_cm"][endpoint], offset)]
    assert detours.qualified(value, probe, plan, MESH) is expected


def test_surface_limits_come_from_the_actual_mesh_parameters():
    value, probe, plan = geometry()
    plan["corners_cm"][0][1] = -46.
    assert detours.qualified(value, probe, plan, MESH)
    assert not detours.qualified(value, probe, plan, dict(MESH, climb_m=.2))
    plan["corners_cm"][0] = [21., 0., 0.]
    assert not detours.qualified(value, probe, plan, MESH)
    assert detours.qualified(value, probe, plan, dict(MESH, cell_size_m=.3))


def polygon(x, y=0., z=0.):
    return dict(vertices=[[x - .1, y, z - .1], [x + .1, y, z - .1],
                          [x + .1, y, z + .1], [x - .1, y, z + .1]])


def corpus_fixture(tmp_path, monkeypatch):
    corpus = tmp_path / "corpus"
    (corpus / "input").mkdir(parents=True)
    points = {split: [dict(id=f"{split}-{index}", goal_kind="point", split=split, region="region",
                          start=f"{split}-S{index}", goal=f"{split}-G{index}", seed=1)
                      for index in range(60)] for split in detours.SPLITS}
    for split, values in points.items():
        (corpus / f"input/{split}.jsonl").write_text(
            "".join(json.dumps(value) + "\n" for value in values), encoding="utf-8")
    objects = [dict(id=f"object-{split}", goal_kind="object", split=split, region="region",
                    place_id=f"place-{split}", start=f"{split}-OBJECT-START",
                    goal=f"cm:{index * 20000},0,0", target_position=[index * 20000, 0., 0.],
                    target_type="house", entrance=0, entrance_in=f"{split}-IN",
                    entrance_out=f"{split}-OUT", goal_aliases=[f"{split}-ALIAS"], seed=1)
               for index, split in enumerate((*detours.SPLITS, "train"))]
    detours.write_rows(corpus / "input/object-candidates.jsonl", objects)
    manifest = dict(dataset="nav-demos-v4", complete=True, root=str(corpus),
                    splits={split: dict(point=values) for split, values in points.items()},
                    split_policy=dict(point={f"{split}_source": f"input/{split}.jsonl"
                                             for split in detours.SPLITS},
                                      object=dict(groups={split: [f"place-{split}"]
                                                          for split in (*detours.SPLITS, "train")})))
    excluded = set()
    monkeypatch.setattr(detours, "module", lambda name: SimpleNamespace(
        exclusion_pairs=lambda holdout, validation: excluded))
    return corpus, manifest, objects, excluded


def test_candidates_keep_frozen_points_and_expand_only_heldout_object_places(tmp_path, monkeypatch):
    corpus, manifest, objects, excluded = corpus_fixture(tmp_path, monkeypatch)
    polygons = [polygon(6.), polygon(206.), polygon(406.)]
    result = detours.candidates(manifest, corpus, polygons)
    for split in detours.SPLITS:
        actual = [value for value in result if value["goal_kind"] == "point" and value["split"] == split]
        assert len(actual) == 60
        assert {(value["start"], value["goal"], value["seed"]) for value in actual} == {
            (value["start"], value["goal"], value["seed"]) for value in manifest["splits"][split]["point"]}
    selected_objects = [value for value in result if value["goal_kind"] == "object"]
    assert {value["place_id"] for value in selected_objects} == {"place-validation", "place-test"}
    assert len(selected_objects) == 4
    assert {value["start"] for value in selected_objects if value["start"].startswith("cm:")} == {
        "cm:600,0,0", "cm:20600,0,0"}
    assert objects[2]["id"] not in {value["id"] for value in result}


@pytest.mark.parametrize("change", ["missing", "another_pair"])
def test_candidates_reject_changed_frozen_point_sets(tmp_path, monkeypatch, change):
    corpus, manifest, _, _ = corpus_fixture(tmp_path, monkeypatch)
    points = manifest["splits"]["validation"]["point"]
    if change == "missing":
        points.pop()
    else:
        points[0]["goal"] = "ANOTHER"
    with pytest.raises(ValueError, match="frozen pairs"):
        detours.candidates(manifest, corpus, [])


def test_candidates_reject_a_heldout_object_reassigned_to_a_train_place(tmp_path, monkeypatch):
    corpus, manifest, objects, _ = corpus_fixture(tmp_path, monkeypatch)
    objects[0]["place_id"] = "place-train"
    (corpus / "input/object-candidates.jsonl").write_text(
        "".join(json.dumps(value) + "\n" for value in objects), encoding="utf-8")
    with pytest.raises(ValueError, match="place split"):
        detours.candidates(manifest, corpus, [])


def test_polygon_generation_and_candidate_selection_ignore_input_order(tmp_path, monkeypatch):
    corpus, manifest, objects, _ = corpus_fixture(tmp_path, monkeypatch)
    polygons = [polygon(6.05), polygon(6.25), polygon(7.1), polygon(206.)]
    assert detours.polygon_starts(polygons) == detours.polygon_starts(list(reversed(polygons)))
    assert len(detours.polygon_starts(polygons)) == 3
    result = detours.candidates(manifest, corpus, polygons)
    (corpus / "input/object-candidates.jsonl").write_text(
        "".join(json.dumps(value) + "\n" for value in reversed(objects)), encoding="utf-8")
    reordered = deepcopy(manifest)
    for split in detours.SPLITS:
        reordered["splits"][split]["point"].reverse()
    assert detours.candidates(reordered, corpus, list(reversed(polygons))) == result


@pytest.mark.parametrize("alias_field", ["goal", "goal_aliases", "entrance_in", "entrance_out"])
@pytest.mark.parametrize("reverse", [False, True])
def test_object_aliases_exclude_both_directions_of_historical_pairs(tmp_path, monkeypatch, alias_field, reverse):
    corpus, manifest, objects, excluded = corpus_fixture(tmp_path, monkeypatch)
    original = objects[0]
    goal = original[alias_field]
    if isinstance(goal, list):
        goal = goal[0]
    pair = (original["start"], goal)
    excluded.add(tuple(reversed(pair)) if reverse else pair)
    result = detours.candidates(manifest, corpus, [])
    assert original["id"] not in {value["id"] for value in result}
    assert objects[1]["id"] in {value["id"] for value in result}
    assert sum(value["goal_kind"] == "point" for value in result) == 120


def test_profiles_preserve_scenario_mapping_and_sample_floor_every_50_cm():
    values = [dict(id="first", geometry=dict(start_cm=[0., 0., 0.], goal_cm=[120., 60., 160.])),
              dict(id="second", geometry=dict(start_cm=[1000., 0., 0.], goal_cm=[1000., 0., 0.]))]
    queries = list(detours.profile_queries(values))
    assert len(queries) == 8 + 5
    first = [(sample, query) for case, sample, query in queries if case == "first"]
    assert [sample for sample, _ in first[:3]] == ["horizontal-40", "horizontal-100", "horizontal-180"]
    assert first[1][1] == [0., 100., 0., 120., 160., 160., 0]
    floor = first[3:]
    assert [sample for sample, _ in floor] == [f"floor-{index}-of-4" for index in range(5)]
    assert floor[0][1] == [0., 200., 0., 0., -1000., 0., 0]
    assert floor[-1][1] == [120., 260., 160., 120., -940., 160., 0]
    assert all(math.hypot(a[1][0] - b[1][0], a[1][2] - b[1][2]) == 50.
               for a, b in zip(floor, floor[1:]))
    assert [sample for case, sample, _ in queries if case == "second" and sample.startswith("floor")] == [
        "floor-0-of-1", "floor-1-of-1"]


def floor_ray(x, y, normal_y=1., hit=True):
    return dict(point=[x, y, 0], normal=[0, normal_y, 0], hit=hit, sector="")


def test_obstacle_classification_requires_positive_material_or_floor_witness():
    samples = [("horizontal-100", dict(floor_ray(0, 0), material=2)),
               ("floor-0-of-1", floor_ray(0, 0)), ("floor-1-of-1", floor_ray(50, -100))]
    assert detours.obstacle_type(samples)[0] == "unknown"  # STONE and a 1-m step are insufficient.
    samples[-1] = ("floor-1-of-1", floor_ray(50, -101))
    assert detours.obstacle_type(samples)[0] == "cliff_or_ledge"
    samples[0] = ("horizontal-100", dict(floor_ray(0, 0), sector="S:HAFEN_HOLZWAND_01"))
    assert detours.obstacle_type(samples)[0] == "wall_or_fence"


@pytest.mark.parametrize("bad_floor", [floor_ray(50, -150, normal_y=.6), floor_ray(50, -150, hit=False),
                                      floor_ray(51, -150)])
def test_missing_steep_or_distant_floor_samples_do_not_establish_a_ledge(bad_floor):
    assert detours.obstacle_type([("floor-0-of-1", floor_ray(0, 0)),
                                 ("floor-1-of-1", bad_floor)])[0] == "unknown"


def test_validation_site_count_merges_neighbourhoods_and_existing_object_sites():
    def site(region):
        return detours.site_id(dict(goal_kind="point", split="validation", region=region))
    assert site("harbour") == site("port_streets")
    assert site("onar") == "BIGFARM_HOUSE"
    assert site("upper_streets") == "CITY_UPTOWN_HUT#2"
    assert len({site(region) for region in ("harbour", "market", "lobart")}) == 3
    assert detours.site_id(dict(goal_kind="point", split="test", region="market")) is None
