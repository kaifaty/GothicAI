"""Train water preparation isolates held-out sites before live physical probes."""
import importlib.util
from pathlib import Path

import pytest


SPEC = importlib.util.spec_from_file_location("nav_demos_v5_water", Path(__file__).resolve().parents[1] / "nav_demos_v5_water.py")
water = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(water)


def selection_inputs():
    positions = {}
    training = []
    for index, (region, (anchor, prefix, _)) in enumerate(water.REGIONS.items()):
        origin = index * 100000
        positions[anchor] = [origin, 0, 0]
        positions[prefix + "_A"] = [origin + 1000, 0, 0]
        positions[prefix + "_B"] = [origin + 2000, 0, 0]
        training.append(dict(region=region, start=anchor, goal=prefix + "_B"))
    manifest = dict(dataset="nav-demos-v4", complete=True,
        splits={split: dict(point=[], object=[], follow=[], approach=[], jump=[]) for split in ("validation", "test")},
        split_policy=dict(object=dict(groups=dict(train=["TRAIN"], validation=["HELD_OUT"], test=[]))))
    places = dict(places=[dict(id="TRAIN", anchor="unused", entrances=[]),
                          dict(id="HELD_OUT", anchor="unmapped-heldout-alias", entrances=[])], points={})
    fixtures = [dict(projected_start_cm=[-100000, 0, 0], goal_cm=[-100000, 0, 100], hazard_cm=[-100000, 0, 200])]
    return dict(training=training, positions=positions, places=places, manifest=manifest,
                detours=[], fixtures=fixtures, excluded=set())


def test_water_pool_is_train_only_named_points_with_frozen_districts():
    arguments = selection_inputs()
    first = water.candidates(**arguments)
    assert first == water.candidates(**arguments)
    assert len(first) == 24
    assert {value["region"] for value in first} == set(water.REGIONS)
    assert all(value["dataset_family"] == "water" and value["goal_kind"] == "point"
               and value["split"] == "train" for value in first)
    arguments["training"].pop()
    with pytest.raises(ValueError, match="lacks a water train district"):
        water.candidates(**arguments)


def test_place_aliases_heldout_pairs_and_coordinate_sites_cannot_enter_pool():
    arguments = selection_inputs()
    blocked = "NW_CITY_HABOUR_B"
    arguments["places"]["points"][blocked] = 1
    arguments["positions"]["unmapped-heldout-alias"] = [1500, 0, 100]
    arguments["manifest"]["splits"]["validation"]["point"] = [dict(
        goal_kind="point", start="cm:0,0,0", goal="cm:1000,0,0")]
    values = water.candidates(**arguments)
    assert all(blocked not in (value["start"], value["goal"]) for value in values)
    assert all("unmapped-heldout-alias" not in (value["start"], value["goal"]) for value in values)
    assert all(value["region"] != "harbour" for value in values)


def test_detour_sites_and_safety_fixture_segments_are_excluded_before_probes():
    arguments = selection_inputs()
    arguments["detours"] = [dict(goal_kind="point", split="test", start="NW_CITY_HABOUR_A", goal="NW_CITY_HABOUR_B")]
    arguments["fixtures"] = [dict(hazard_cm=[0, 0, 500])]
    values = water.candidates(**arguments)
    assert all(value["region"] != "harbour" for value in values)
    arguments["fixtures"] = []
    with pytest.raises(ValueError, match="safety-fixture"):
        water.candidates(**arguments)


def probe_cases(count=40):
    values, probes = [], []
    for index in range(count):
        values.append(dict(id=f"water-{index}", start=f"START-{index}", goal=f"GOAL-{index}"))
        probes.append(dict(label=f"water-{index}", start=f"START-{index}", goal=f"GOAL-{index}", qualified=True,
            start_dry_supported=True, goal_dry_supported=True, detour_complete=True, endpoints_on_surface=True,
            direct_water_samples=1, straight_blocked=True, consecutive_shore_samples=1, water_relation="crossing"))
    return values, probes


def test_native_crossing_and_three_consecutive_shore_samples_are_both_accepted():
    values, probes = probe_cases()
    probes[0].update(direct_water_samples=0, straight_blocked=False, consecutive_shore_samples=3, water_relation="along_shore")
    selected = water.qualified_candidates(values, probes)
    assert len(selected) == 40
    assert {value["water_qualification"]["water_relation"] for value in selected} == {"crossing", "along_shore"}


@pytest.mark.parametrize("change", [dict(goal_dry_supported=False), dict(detour_complete=False),
    dict(endpoints_on_surface=False), dict(direct_water_samples=0, consecutive_shore_samples=2),
    dict(direct_water_samples=1, straight_blocked=False, consecutive_shore_samples=0)])
def test_incomplete_water_evidence_does_not_count_toward_40(change):
    values, probes = probe_cases()
    probes[0].update(change)
    with pytest.raises(ValueError, match="Only 39 train water routes"):
        water.qualified_candidates(values, probes)


def test_probe_coverage_and_route_identity_must_match_exact_frozen_pool():
    values, probes = probe_cases()
    with pytest.raises(ValueError, match="exactly the frozen"):
        water.qualified_candidates(values, probes[:-1])
    probes[0]["goal"] = "OTHER"
    with pytest.raises(ValueError, match="changed a frozen route"):
        water.qualified_candidates(values, probes)
