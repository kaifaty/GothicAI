"""Transition route qualification uses the selected v2 plan and location splits."""
import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

import pytest


SPEC = importlib.util.spec_from_file_location("nav_jump_routes", Path(__file__).resolve().parents[1] / "nav_jump_routes.py")
routes = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(routes)


def link(identifier, start=(0., 0., 0.), end=(2., 1., 0.), kind="jump"):
    return dict(id=identifier, start=start, end=end, type=kind)


def candidate(identifier=7, split="train"):
    return dict(label=f"link-{identifier}-00", transition_id=identifier, type="jump", split=split,
                cluster=f"site-{identifier}", start="cm:-100,0,0", goal="cm:300,100,0",
                region="city", approach_m=1., beyond_m=1., lateral_m=0.)


def plan(value, identifiers=None):
    identifiers = identifiers if identifiers is not None else [value["transition_id"]]
    return dict(label=value["label"], start=value["start"], goal=value["goal"],
                complete=True, start_supported=True, direct_walkable=False, beyond_known=True,
                start_snap_m=0., end_snap_m=0.,
                corners_cm=[[-100., 0., 0.], [0., 0., 0.], [200., 100., 0.], [300., 100., 0.]],
                transitions=[dict(id=identifier, type="jump", corner_index=1,
                                  start_cm=[0., 0., 0.], end_cm=[200., 100., 0.])
                             for identifier in identifiers])


def test_spatial_union_uses_both_endpoints_and_transitive_neighbours():
    # A's landing is close to B's launch; B's landing is close to C's launch.
    # A and C themselves are >30 m apart. All three must stay in one location.
    values = [link(1, (0., 0., 0.), (4., 0., 0.)),
              link(2, (33., 0., 0.), (37., 0., 0.)),
              link(3, (66., 0., 0.), (70., 0., 0.))]
    assigned = routes.spatial_splits(values)
    assert len({item["cluster"] for item in assigned.values()}) == 1
    assert len({item["split"] for item in assigned.values()}) == 1


def test_split_assignment_is_order_independent_and_about_70_15_15():
    values = [link(index + 1, (index * 100., 0., 0.), (index * 100. + 2., 0., 0.)) for index in range(20)]
    assigned = routes.spatial_splits(values, seed=2)
    assert assigned == routes.spatial_splits(list(reversed(values)), seed=2)
    assert {key: sum(value["split"] == key for value in assigned.values())
            for key in routes.SPLITS} == dict(train=14, validation=3, test=3)


def test_candidate_variants_share_one_transition_split_and_use_centimetres():
    places = [dict(id="CITY_ROOF", ru="крыша", center=[0., 0., 0.])]
    values = routes.route_candidates([link(7)], places)
    assert len(values) == 48
    assert len({value["label"] for value in values}) == len(values)
    assert {value["split"] for value in values} == {"train"}
    assert all(value["transition_id"] == 7 and value["region"] == "city" for value in values)
    assert values[0]["start"] == "cm:-80.000000,0.000000,0.000000"
    assert values[0]["goal"] == "cm:320.000000,100.000000,0.000000"


def test_route_requires_actual_transition_id_in_selected_teacher_path():
    value = candidate()
    selected, counts = routes.qualify([value], [plan(value)])
    assert len(selected) == 1
    assert selected[0]["planned_transition_ids"] == [7]
    assert counts["by_type"] == {"jump": 1}
    # Same corners and location but another transition id is insufficient.
    selected, counts = routes.qualify([value], [plan(value, [8])])
    assert selected == []
    assert counts["rejected"] == {"transition_not_in_teacher_path": 1}


@pytest.mark.parametrize("change,reason", [
    (dict(complete=False), "incomplete_path"),
    (dict(direct_walkable=True), "direct_walk_has_no_transition"),
    (dict(start_supported=False), "unsupported_start"),
    (dict(start_snap_m=.31), "excessive_mesh_snap"),
    (dict(end_snap_m=.61), "excessive_mesh_snap"),
])
def test_rejects_incomplete_synthetic_or_misplaced_route(change, reason):
    value = candidate()
    selected, counts = routes.qualify([value], [dict(plan(value), **change)])
    assert selected == []
    assert counts["rejected"] == {reason: 1}


def test_known_road_plan_is_allowed_when_its_actual_carrot_plan_has_link():
    value = candidate()
    selected, _ = routes.qualify([value], [dict(plan(value), beyond_known=False)])
    assert len(selected) == 1


def test_route_cannot_visit_a_transition_assigned_to_another_split():
    a, b = candidate(7, "train"), candidate(8, "test")
    selected, counts = routes.qualify([a, b], [plan(a, [7, 8]), plan(b)])
    assert [value["transition_id"] for value in selected] == [8]
    assert counts["rejected"] == {"path_crosses_another_split": 1}


def test_qualifier_rejects_mismatched_pair_or_missing_batch_result():
    value = candidate()
    with pytest.raises(ValueError, match="identity mismatch"):
        routes.qualify([value], [dict(plan(value), goal="another_goal")])
    with pytest.raises(ValueError, match="cover exactly"):
        routes.qualify([value], [])
    with pytest.raises(ValueError, match="Duplicate plans"):
        routes.qualify([value], [plan(value), plan(value)])


def test_qualifier_checks_link_endpoints_are_explicit_corners():
    value = candidate()
    planned = plan(value)
    planned["transitions"][0]["corner_index"] = 2
    with pytest.raises(ValueError, match="endpoint does not match"):
        routes.qualify([value], [planned])


def test_pair_file_is_the_native_batch_api(tmp_path):
    path = tmp_path / "pairs.txt"
    value = candidate()
    routes.write_pair_lines(path, [value])
    assert path.read_text() == "link-7-00 cm:-100,0,0 cm:300,100,0\n"
    with pytest.raises(ValueError, match="whitespace"):
        routes.write_pair_lines(path, [dict(value, start="cm:0 0 0")])


def test_exactly_30m_is_a_different_location():
    values = [link(1, (0., 0., 0.), (1., 0., 0.)),
              link(2, (31., 0., 0.), (32., 0., 0.))]
    result = routes.spatial_splits(values)
    assert result[1]["cluster"] != result[2]["cluster"]


def test_selection_covers_approaches_before_duplicate_lateral_views():
    a = candidate()
    values = [dict(a, label=f"link-7-{index:02d}", approach_m=approach, lateral_m=lateral)
              for index, (approach, lateral) in enumerate(((1., 0.), (1., .35), (2., 0.), (3., 0.)))]
    selected, _ = routes.qualify(values, [plan(value) for value in values], per_link=3)
    assert {value["approach_m"] for value in selected} == {1., 2., 3.}


def test_three_variants_cover_centre_both_sides_and_different_distances():
    places = [dict(id="FARM1_BANDITS", ru="пещера", center=[0., 0., 0.])]
    values = routes.route_candidates([link(7)], places)
    selected, _ = routes.qualify(values, [plan(value) for value in values], per_link=3)
    assert len(selected) == 3
    assert selected[0]["lateral_m"] == 0.
    assert {value["lateral_m"] for value in selected} == {0., -.35, .35}
    assert len({value["approach_m"] for value in selected}) == 3
    assert selected == routes.qualify(list(reversed(values)), [plan(value) for value in reversed(values)], per_link=3)[0]


def test_side_diversity_uses_only_verified_paths_and_keeps_available_count():
    a = candidate()
    values = [dict(a, label=f"link-7-{index:02d}", approach_m=approach, lateral_m=lateral)
              for index, (approach, lateral) in enumerate(((1., 0.), (2., -.35), (3., .35)))]
    plans = [plan(value) for value in values]
    plans[-1]["transitions"] = []
    selected, counts = routes.qualify(values, plans, per_link=3)
    assert len(selected) == 2
    assert {value["lateral_m"] for value in selected} == {0., -.35}
    assert counts["rejected"] == {"transition_not_in_teacher_path": 1}


def test_prepare_accepts_the_version2_places_book_schema(tmp_path):
    source = tmp_path / "places.json"
    source.write_text(json.dumps(dict(version=2, world="NEWWORLD.ZEN", points={}, places=[
        dict(id="CITY_ROOF", ru="крыша", center=[10000., 200., 0.]),
        dict(id="MONASTERY", ru="монастырь", center=[0., 50., 0.])
    ])))
    links = tmp_path / "links.txt"
    links.write_text("0 0 0 2 1 0 1 7\n")
    output = tmp_path / "routes"
    values = routes.prepare(SimpleNamespace(places=source, links=links, output=output, seed=19))
    assert len(values) == 48
    assert {value["region"] for value in values} == {"monastery"}
    assert {value["place_name"] for value in values} == {"монастырь"}
    assert len(list(routes.rows(output / "candidate-routes.jsonl"))) == 48
    assert len((output / "candidate-pairs.txt").read_text().splitlines()) == 48


def test_link_reader_preserves_native_type_and_identifier(tmp_path):
    source = tmp_path / "links.txt"
    source.write_text("0 0 0 1 0.5 0 2 4294967295\n")
    assert routes.read_links(source) == [link(4294967295, (0., 0., 0.), (1., .5, 0.), "climb")]
