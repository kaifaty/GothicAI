"""Small deterministic geometry checks; no Blender or model runtime needed."""

import json
from pathlib import Path

import numpy as np
import pytest

from geometry import (
    box_uv, budget_ratio, choose_clusters, classify_zones, cylinder_uv,
    face_adjacency, fit_bbox, kmeans, linear_to_oklab, select_uv_projection, simplify_loop,
    smooth_labels, triangle_areas,
)


def cuboid(size=(2., 3., 1.)):
    vertices = np.array([[x, y, z] for x in (0., size[0]) for y in (0., size[1]) for z in (0., size[2])])
    faces = np.array([
        [0, 1, 3], [0, 3, 2], [4, 6, 7], [4, 7, 5],
        [0, 4, 5], [0, 5, 1], [2, 3, 7], [2, 7, 6],
        [0, 2, 6], [0, 6, 4], [1, 5, 7], [1, 7, 3],
    ])
    return vertices, faces


def test_oklab_reference_values_are_linear_rgb():
    result = linear_to_oklab(np.array([[0., 0., 0.], [1., 1., 1.], [1., 0., 0.]]))
    np.testing.assert_allclose(result[0], [0., 0., 0.], atol=1e-7)
    np.testing.assert_allclose(result[1], [1., 0., 0.], atol=1e-7)
    np.testing.assert_allclose(result[2], [0.62795536, 0.22486306, 0.12584630], atol=1e-7)
    with pytest.raises(ValueError):
        linear_to_oklab([[-.1, 0., 0.]])


@pytest.mark.parametrize("count", [1, 2, 3])
def test_cluster_selection_finds_visible_colors_without_splitting_noise(count):
    rng = np.random.default_rng(9)
    colors = np.vstack([rng.normal([.35 + index * .18, .04, .08], .001, (40, 3)) for index in range(count)])
    labels, centres = choose_clusters(colors)
    assert len(centres) == count
    for index in range(count):
        assert len(np.unique(labels[index * 40:(index + 1) * 40])) == 1
    labels_again, centres_again = choose_clusters(colors)
    np.testing.assert_array_equal(labels, labels_again)
    np.testing.assert_allclose(centres, centres_again)


def test_weighted_kmeans_ignores_zero_weight_outlier():
    labels, centres = kmeans(np.array([[.4, .1, .1], [.4, .1, .1], [1., 0., 0.]]), 3, [1., 1., 0.])
    assert len(centres) == 1
    np.testing.assert_allclose(centres[0], [.4, .1, .1])
    assert np.all(labels == 0)


def separated_triangles(colors, metallic):
    vertices = np.array([[i * 2., 0., 0.] if corner == 0 else [i * 2. + (corner == 1), float(corner == 2), 0.] for i in range(len(colors)) for corner in range(3)])
    faces = np.arange(len(vertices)).reshape(-1, 3)
    return vertices, faces, np.repeat(colors, 3, axis=0), None if metallic is None else np.repeat(metallic, 3)


ZONES = [{"key": "wood", "group": "WOOD", "role": "body"}, {"key": "iron", "group": "METAL", "role": "bands"}]


def test_explicit_metallic_overrides_vertex_color_and_threshold_is_strict():
    inputs = separated_triangles([[.5, .2, .06], [.5, .2, .06], [.02, .02, .02]], [1., .5, 0.])
    np.testing.assert_array_equal(classify_zones(*inputs, ZONES), [1, 0, 0])


def test_fallback_recognises_dark_neutral_iron_and_keeps_brown_wood():
    inputs = separated_triangles([[.02, .02, .02], [.3, .12, .035], [.15, .06, .015]], None)
    np.testing.assert_array_equal(classify_zones(*inputs, ZONES), [1, 0, 0])


@pytest.mark.parametrize("target_id", ["crate", "chest"])
@pytest.mark.parametrize("explicit_metallic", [True, False])
def test_real_pilot_targets_accept_multiple_metal_zones(target_id, explicit_metallic):
    from parametric import build

    targets_path = Path(__file__).resolve().parents[4] / "data/content/mesh3d-pilot.json"
    target = next(item for item in json.loads(targets_path.read_text())
                  if item["id"] == target_id)
    zones = target["zones"]
    metal_slots = [index for index, zone in enumerate(zones) if zone["group"] == "METAL"]
    assert len(metal_slots) == 2
    vertices, faces, color, metallic = build("crate")
    labels = classify_zones(vertices, faces, color,
                            metallic if explicit_metallic else None, zones)
    is_metal = metallic[faces].mean(axis=1) > 0.5
    np.testing.assert_array_equal(labels[is_metal], metal_slots[0])
    assert all(zones[index]["group"] == "WOOD" for index in labels[~is_metal])


def test_smoothing_replaces_isolated_wood_cluster_but_keeps_metal_boundaries():
    # A central face has one neighbour across each edge.
    faces = np.array([[0, 1, 2], [1, 0, 3], [2, 1, 4], [0, 2, 5]])
    np.testing.assert_array_equal(smooth_labels(faces, [0, 1, 1, 1], np.ones(4), iterations=1), [1, 1, 1, 1])
    np.testing.assert_array_equal(smooth_labels(faces, [2, 1, 1, 1], np.ones(4), locked_labels=[2]), [2, 1, 1, 1])
    np.testing.assert_array_equal(smooth_labels(faces, [0, 2, 2, 2], np.ones(4), locked_labels=[2]), [0, 2, 2, 2])


def test_smoothing_votes_by_surface_area_not_triangle_count():
    faces = np.array([[0, 1, 2], [1, 0, 3], [2, 1, 4], [0, 2, 5]])
    labels = smooth_labels(faces, [0, 1, 1, 1], [10., .1, .1, .1], iterations=1)
    assert labels[0] == 0


@pytest.mark.parametrize("axis", ["+X", "-X", "+Y", "-Y", "+Z", "-Z"])
def test_orientation_fits_declared_up_and_exact_anchored_target_bbox(axis):
    vertices, _ = cuboid()
    # Encode the source's upward direction explicitly; add arbitrary yaw.
    up_index = "XYZ".index(axis[1])
    source = vertices.copy()
    source[:, [1, up_index]] = source[:, [up_index, 1]]
    source[:, up_index] *= 1 if axis[0] == "+" else -1
    original_height = source[:, up_index] * (1 if axis[0] == "+" else -1)
    target = {"bbox_min": [-.7, 0., -.2], "bbox_max": [.7, 1.9, .2]}
    output = fit_bbox(source, {"up_axis": axis, "meters_per_unit": .01}, target)
    np.testing.assert_allclose(output.min(axis=0), target["bbox_min"], atol=1e-6)
    np.testing.assert_allclose(output.max(axis=0), target["bbox_max"], atol=1e-6)
    np.testing.assert_allclose(output[original_height == 0, 1], 0., atol=1e-7)


def test_horizontal_obb_removes_generator_yaw_before_scaling():
    vertices, _ = cuboid()
    angle = .61
    rotation = np.array([[np.cos(angle), 0., np.sin(angle)], [0., 1., 0.], [-np.sin(angle), 0., np.cos(angle)]])
    target = {"bbox_min": [0., 0., 0.], "bbox_max": [2., 3., 1.]}
    output = fit_bbox(vertices @ rotation.T + [10., -5., 1.], {"up_axis": "+Y", "meters_per_unit": 1.}, target)
    # Correct OBB keeps each box corner on a corner, not somewhere along a face.
    for dimension, extent in enumerate([2., 3., 1.]):
        assert np.all(np.minimum(abs(output[:, dimension]), abs(output[:, dimension] - extent)) < 1e-5)


def test_budget_ratio_is_strict_and_does_not_expand_small_meshes():
    assert budget_ratio(300_000, 1_000) * 300_000 < 1_000
    assert budget_ratio(500, 1_000) == 1.
    with pytest.raises(ValueError):
        budget_ratio(0, 1_000)


def loop_deviation(points, indices):
    start = points[indices]
    direction = np.roll(start, -1, axis=0) - start
    relative = points[:, None] - start
    fraction = np.clip(np.einsum("pij,ij->pi", relative, direction)
                       / np.einsum("ij,ij->i", direction, direction), 0, 1)
    distance = np.linalg.norm(relative - fraction[:, :, None] * direction, axis=2)
    return distance.min(axis=1).max()


def test_closed_loop_simplification_retains_square_corners_and_removes_dense_sides():
    vertices = np.array([[0., 0., 0.], [1., 0., 0.], [1., 1., 0.], [0., 1., 0.]])
    points = np.vstack([start + np.arange(20)[:, None] / 20 * (finish - start)
                        for start, finish in zip(vertices, np.roll(vertices, -1, axis=0))])
    indices = simplify_loop(points, .001)
    np.testing.assert_array_equal(indices, [0, 20, 40, 60])
    assert loop_deviation(points, indices) < 1e-12


@pytest.mark.parametrize("zigzag", [False, True])
def test_closed_loop_simplification_reduces_circle_within_metric_tolerance(zigzag):
    angle = np.arange(512) * 2 * np.pi / 512
    radius = .5 + (np.arange(len(angle)) % 2) * .0008 * zigzag
    points = np.column_stack((radius * np.cos(angle), .0006 * np.sin(angle * 17) * zigzag,
                              radius * np.sin(angle)))
    indices = simplify_loop(points, .005)
    assert 12 <= len(indices) <= 64
    assert np.all(np.diff(indices) > 0)
    assert loop_deviation(points, indices) <= .005 + 1e-12


def test_closed_loop_supports_repeated_endpoints_and_never_removes_all_corners():
    points = np.array([[0., 0., 0.], [1., 0., 0.], [1., 1., 0.], [0., 1., 0.], [0., 0., 0.]])
    indices = simplify_loop(points, 10.)
    assert len(indices) == 3
    assert np.all(indices < 4)
    with pytest.raises(ValueError):
        simplify_loop(points, -.1)


def test_box_uv_keeps_world_metre_scale_on_rotated_faces_and_long_grain():
    vertices, faces = cuboid()
    angle = .36
    rotation = np.array([[np.cos(angle), -np.sin(angle), 0.], [np.sin(angle), np.cos(angle), 0.], [0., 0., 1.]])
    vertices = vertices @ rotation.T
    uv = box_uv(vertices, faces)
    for a, b in [(0, 1), (1, 2), (2, 0)]:
        world_length = np.linalg.norm(vertices[faces[:, a]] - vertices[faces[:, b]], axis=1)
        uv_length = np.linalg.norm(uv[:, a] - uv[:, b], axis=1)
        np.testing.assert_allclose(uv_length, world_length, rtol=.00001)
    # Every face's V aligns with its longer box dimension.
    assert np.all(np.ptp(uv[:, :, 1], axis=1) >= np.ptp(uv[:, :, 0], axis=1))


def physical_checker_scale(vertices, faces, uv):
    triangles = vertices[faces]
    physical = np.linalg.solve(uv[:, 1:] - uv[:, :1], triangles[:, 1:] - triangles[:, :1])
    return np.linalg.norm(physical, axis=2)


def test_box_uv_is_metric_in_both_directions_on_sloped_faces():
    vertices, faces = cuboid()
    vertices[7] += [.18, -.23, .12]
    uv = box_uv(vertices, faces)
    # Every face participates, including the small oblique faces.
    scale = physical_checker_scale(vertices, faces, uv)
    np.testing.assert_allclose(scale, 1., rtol=.05)
    assert_no_patch_uv_jumps(faces, *box_uv(vertices, faces, return_patches=True)[:2])


def cylinder(segments=96, radius=.6, height=1.4):
    angles = np.arange(segments) * (2 * np.pi / segments)
    vertices = np.array([[radius * np.cos(angle), y, radius * np.sin(angle)] for y in (0., height) for angle in angles])
    faces = []
    for index in range(segments):
        following = (index + 1) % segments
        faces.extend([[index, following, following + segments], [index, following + segments, index + segments]])
    return vertices, np.asarray(faces)


@pytest.mark.parametrize("projection", ["cylinder", "box"])
def test_uv_projection_explicit_target_override_wins_over_geometry(projection):
    vertices, faces = cuboid() if projection == "cylinder" else cylinder()
    assert select_uv_projection(vertices, faces, {"uv_projection": projection}) == projection


def test_uv_projection_rejects_unsupported_explicit_override():
    vertices, faces = cylinder()
    with pytest.raises(ValueError, match="uv_projection"):
        select_uv_projection(vertices, faces, {"uv_projection": "sphere"})


def test_uv_projection_detects_low_poly_circular_sections_using_faces():
    vertices, faces = cylinder(segments=8)
    assert select_uv_projection(vertices, faces, {}) == "cylinder"


def noisy_bowed_barrel():
    sides, rings = 48, 17
    theta, height = np.meshgrid(np.arange(sides) * 2 * np.pi / sides, np.linspace(0, 1.2, rings))
    radius = .36 + .075 * np.sin(np.pi * height / 1.2)
    radius += .003 * np.sin(theta * 13 + height * 17)
    noisy_height = height + .006 * np.sin(theta * 7 + height * 19)
    vertices = np.stack((radius * np.cos(theta), noisy_height, radius * np.sin(theta)), axis=-1).reshape(-1, 3)
    first = (np.arange(rings - 1)[:, None] * sides + np.arange(sides)).ravel()
    following = (first // sides) * sides + (first + 1) % sides
    faces = np.vstack((np.column_stack((first, first + sides, following)),
                       np.column_stack((following, first + sides, following + sides))))
    return vertices, faces


def test_uv_projection_detects_noisy_bowed_barrel_independent_of_target_kind():
    vertices, faces = noisy_bowed_barrel()
    assert select_uv_projection(vertices, faces, {"id": "test", "kind": "static_mrm"}) == "cylinder"


@pytest.mark.parametrize("size", [(1., 1., 1.), (2., 3., 1.)])
def test_uv_projection_rejects_cuboids_even_when_target_is_named_barrel(size):
    vertices, faces = cuboid(size)
    assert select_uv_projection(vertices, faces, {"id": "barrel", "kind": "barrel"}) == "box"


def test_uv_projection_uses_geometry_for_actual_pr312_static_mrm_barrel_target():
    path = Path(__file__).resolve().parents[4] / "data/content/mesh3d-pilot.json"
    target = next(row for row in json.loads(path.read_text()) if row["id"] == "barrel")
    assert target["kind"] == "static_mrm"
    vertices, faces = cylinder()
    vertices = fit_bbox(vertices, {"up_axis": "+Y", "meters_per_unit": 1.}, target)
    assert select_uv_projection(vertices, faces, target) == "cylinder"


def test_cylinder_uv_has_metric_scale_including_seam():
    vertices, faces = cylinder()
    uv = cylinder_uv(vertices, faces)
    for a, b in [(0, 1), (1, 2), (2, 0)]:
        world_length = np.linalg.norm(vertices[faces[:, a]] - vertices[faces[:, b]], axis=1)
        uv_length = np.linalg.norm(uv[:, a] - uv[:, b], axis=1)
        np.testing.assert_allclose(uv_length, world_length, rtol=.001)
    assert np.max(np.ptp(uv[:, :, 0], axis=1)) < .05


def test_cylinder_shared_meridian_and_mean_section_arc_keep_metric_checker():
    vertices, faces = noisy_bowed_barrel()
    vertices *= [1.18, 1., .93]
    uv, patches, types = cylinder_uv(vertices, faces, return_patches=True)
    scale = physical_checker_scale(vertices, faces, uv)
    np.testing.assert_allclose(scale[:, 1], 1., rtol=.05)
    np.testing.assert_allclose(scale[:, 0], 1., rtol=.15)
    assert np.all(types == 1)
    assert_no_patch_uv_jumps(faces, uv, patches)


def test_cylinder_uses_shared_binned_cone_meridian_and_mean_radius_u():
    sides, rings = 96, 65
    theta, height = np.meshgrid(np.arange(sides) * 2 * np.pi / sides,
                               np.linspace(0., 1.2, rings))
    radius = .5 + .125 * height
    vertices = np.stack((radius * np.cos(theta), height, radius * np.sin(theta)), axis=-1).reshape(-1, 3)
    first = (np.arange(rings - 1)[:, None] * sides + np.arange(sides)).ravel()
    following = first // sides * sides + (first + 1) % sides
    faces = np.vstack((np.column_stack((first, first + sides, following)),
                       np.column_stack((following, first + sides, following + sides))))
    uv, patches, types = cylinder_uv(vertices, faces, return_patches=True)
    assert np.all(types == 1)
    expected_length = 1.2 * np.sqrt(1 + .125 ** 2)
    assert expected_length > 1.2
    all_triangles = vertices[faces]
    expected_radius = np.average(np.linalg.norm(all_triangles[:, :, [0, 2]], axis=2).mean(axis=1),
                                 weights=triangle_areas(vertices, faces))
    for patch in np.unique(patches):
        selected = patches == patch
        np.testing.assert_allclose(np.ptp(uv[selected, :, 1]), expected_length, rtol=.05)
        triangles = vertices[faces[selected]]
        angle = np.arctan2(triangles[:, :, 2], triangles[:, :, 0])
        # Differentiating within each triangle removes the half-chart's seam
        # phase while checking a single mean-radius multiplier everywhere.
        angle = np.unwrap(angle, axis=1)
        np.testing.assert_allclose(np.diff(uv[selected, :, 0], axis=1),
                                   np.diff(angle, axis=1) * expected_radius,
                                   rtol=1e-5, atol=1e-6)
        for level in np.unique(height):
            at_level = triangles[:, :, 1] == level
            assert np.ptp(uv[selected, :, 1][at_level]) < 1e-6
    assert_no_patch_uv_jumps(faces, uv, patches)


def test_cylinder_mean_ellipse_section_arc_has_physical_metre_scale():
    vertices, faces = cylinder(segments=128, radius=1., height=1.2)
    vertices *= [.47, 1., .38]
    uv, patches, types = cylinder_uv(vertices, faces, return_patches=True)
    assert np.all(types == 1)
    scale = physical_checker_scale(vertices, faces, uv)
    # Every horizontal section is the mean section on this straight ellipse.
    np.testing.assert_allclose(scale[:, 0], 1., rtol=.05)
    np.testing.assert_allclose(scale[:, 1], 1., rtol=.05)
    phi = np.linspace(0., 2 * np.pi, 16385)
    outline = np.column_stack((.47 * np.cos(phi), .38 * np.sin(phi)))
    perimeter = np.linalg.norm(np.diff(outline, axis=0), axis=1).sum()
    measured = sum(np.ptp(uv[patches == patch, :, 0]) for patch in np.unique(patches))
    np.testing.assert_allclose(measured, perimeter, rtol=.05)
    assert_no_patch_uv_jumps(faces, uv, patches)


def test_cylinder_meridian_ignores_uneven_angular_height_sampling():
    sides, rings = 48, 17
    theta, height = np.meshgrid(np.arange(sides) * 2 * np.pi / sides,
                               np.linspace(0., 1.2, rings))
    # An extruded ellipse has straight vertical meridians even when its vertex
    # rows follow different heights at different angles.
    height += .35 * np.sin(np.pi * height / 1.2) ** 2 * np.cos(2 * theta)
    vertices = np.stack((.47 * np.cos(theta), height, .38 * np.sin(theta)), axis=-1).reshape(-1, 3)
    first = (np.arange(rings - 1)[:, None] * sides + np.arange(sides)).ravel()
    following = first // sides * sides + (first + 1) % sides
    faces = np.vstack((np.column_stack((first, first + sides, following)),
                       np.column_stack((following, first + sides, following + sides))))
    uv, patches, types = cylinder_uv(vertices, faces, return_patches=True)
    assert np.all(types == 1)
    scale = physical_checker_scale(vertices, faces, uv)
    np.testing.assert_allclose(scale[:, 0], 1., rtol=.05)
    np.testing.assert_allclose(scale[:, 1], 1., rtol=.05)
    for patch in np.unique(patches):
        np.testing.assert_allclose(np.ptp(uv[patches == patch, :, 1]), 1.2, rtol=.05)
    assert_no_patch_uv_jumps(faces, uv, patches)


def test_cylinder_caps_have_planar_metric_uv():
    vertices = np.array([[0., 1., 0.], [.6, 1., 0.], [0., 1., .6]])
    faces = np.array([[0, 1, 2]])
    uv = cylinder_uv(vertices, faces)
    np.testing.assert_allclose(np.linalg.norm(uv[0, 1] - uv[0, 2]), np.linalg.norm(vertices[1] - vertices[2]), rtol=1e-6)


def test_cylinder_stave_radial_thickness_faces_have_nondegenerate_metric_uv():
    vertices, faces = cylinder()
    offset = len(vertices)
    vertices = np.vstack((vertices, [[.55, 0., 0.], [.6, 0., 0.], [.6, 1.4, 0.], [.55, 1.4, 0.]]))
    faces = np.vstack((faces, np.array([[0, 1, 2], [0, 2, 3]]) + offset))
    uv = cylinder_uv(vertices, faces)
    for a, b in [(0, 1), (1, 2), (2, 0)]:
        world_length = np.linalg.norm(vertices[faces[-2:, a]] - vertices[faces[-2:, b]], axis=1)
        uv_length = np.linalg.norm(uv[-2:, a] - uv[-2:, b], axis=1)
        np.testing.assert_allclose(uv_length, world_length, rtol=.00001)


def test_cylinder_lid_plank_chords_use_planar_metric_uv():
    vertices, faces = cylinder(radius=.47, height=1.2)
    vertices[:, 1] -= .08
    # Rounded coordinates of two failed processed control triangles: a long
    # plank edge across the lid and a short oblique plank end, both vertical.
    planks = np.array([
        [[.3309, 1.0624, .0913], [-.0693, 1.0984, -.2761], [.3309, 1.0984, .0913]],
        [[-.0433, 1.0984, .2795], [.0210, 1.0624, .2340], [.0210, 1.0984, .2340]],
    ])
    extra_faces = np.arange(6).reshape(-1, 3) + len(vertices)
    vertices = np.vstack((vertices, planks.reshape(-1, 3)))
    faces = np.vstack((faces, extra_faces))
    uv = cylinder_uv(vertices, faces)
    np.testing.assert_allclose(physical_checker_scale(vertices, faces[-2:], uv[-2:]), 1., rtol=1e-5)


def assert_no_patch_uv_jumps(faces, uv, patches):
    checked = 0
    for a, b in face_adjacency(faces):
        if patches[a] != patches[b]:
            continue
        common = np.intersect1d(faces[a], faces[b])
        first = uv[a, [list(faces[a]).index(vertex) for vertex in common]]
        second = uv[b, [list(faces[b]).index(vertex) for vertex in common]]
        np.testing.assert_allclose(first, second, atol=1e-5, rtol=0)
        checked += 1
    assert checked > 0


def test_generated_barrel_control_checks_all_charts_and_lid_diagonals():
    from parametric import build_barrel

    vertices, faces, _, _ = build_barrel()
    target = {"bbox_min": [-.47, -.08, -.38], "bbox_max": [.47, 1.12, .38]}
    vertices = fit_bbox(vertices, {"up_axis": "+Y", "meters_per_unit": 1.}, target)
    uv, patches, types = cylinder_uv(vertices, faces, return_patches=True)
    scale = physical_checker_scale(vertices, faces, uv)
    assert np.all((scale[:, 0] >= .85) & (scale[:, 0] <= 1.15))
    assert np.all((scale[:, 1] >= .95) & (scale[:, 1] <= 1.05))
    np.testing.assert_allclose(scale[types != 1], 1., rtol=.05)
    assert_no_patch_uv_jumps(faces, uv, patches)
    # The same lid board used to choose cylinder/plane across this diagonal.
    assert patches[2358] == patches[2359]
    assert types[2358] == types[2359] == 0


def test_dense_bowed_barrel_uses_continuous_shared_charts():
    vertices, faces = noisy_bowed_barrel()
    target = {"bbox_min": [-.47, -.08, -.38], "bbox_max": [.47, 1.12, .38]}
    vertices = fit_bbox(vertices.astype(np.float32), {"up_axis": "+Y", "meters_per_unit": 1.}, target)
    uv, patches, types = cylinder_uv(vertices, faces, return_patches=True)
    assert np.all(types == 1)
    assert_no_patch_uv_jumps(faces, uv, patches)
    scale = physical_checker_scale(vertices, faces, uv)
    np.testing.assert_allclose(scale[:, 0], 1., rtol=.15)
    np.testing.assert_allclose(scale[:, 1], 1., rtol=.05)


def test_triangle_areas_use_actual_surface_area():
    vertices, faces = cuboid()
    assert triangle_areas(vertices, faces).sum() == 22.
