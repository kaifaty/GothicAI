"""Acceptance measurements preserve checker U/V and cylindrical tolerances."""
import json

import numpy as np
import pytest

from synthetic import assert_uv_quality, uv_quality


def cylinder_profile(radii=(.92, .96, 1.04, 1.08, 1.04, .96, .92), sides=128):
    radii = np.asarray(radii)
    heights = np.arange(len(radii), dtype=float) * .3
    angle = np.arange(sides) * 2 * np.pi / sides
    vertices = np.array([[radius * np.cos(theta), height, radius * np.sin(theta)]
                         for height, radius in zip(heights, radii) for theta in angle])
    faces, coords = [], []
    distance = np.r_[0., np.cumsum(np.hypot(np.diff(heights), np.diff(radii)))]
    for ring in range(len(radii) - 1):
        for index in range(sides):
            next_index = (index + 1) % sides
            a, b = ring * sides + index, ring * sides + next_index
            c, d = a + sides, b + sides
            faces.extend([[a, b, c], [b, d, c]])
            ta, tb = index * 2 * np.pi / sides, (index + 1) * 2 * np.pi / sides
            coords.extend([[[ta, distance[ring]], [tb, distance[ring]], [ta, distance[ring + 1]]],
                           [[tb, distance[ring]], [tb, distance[ring + 1]], [ta, distance[ring + 1]]]])
    faces, coords = np.asarray(faces), np.asarray(coords)
    triangles = vertices[faces]
    area = np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0],
                                   triangles[:, 2] - triangles[:, 0]), axis=1) * .5
    face_radius = np.linalg.norm(triangles[:, :, [0, 2]], axis=2).mean(axis=1)
    coords[:, :, 0] *= np.average(face_radius, weights=area)
    return vertices, faces, coords


def plane():
    vertices = np.array([[0., 0., 0.], [1., 0., 0.], [0., 1., 0.], [1., 1., 0.]])
    faces = np.array([[0, 1, 2], [1, 3, 2]])
    return vertices, faces, vertices[faces, :2].copy()


def test_bulged_cylinder_accepts_fixed_angle_u_and_metric_meridian_v():
    vertices, faces, uv = cylinder_profile()
    stats = uv_quality(vertices, faces, uv, "cylinder")
    assert_uv_quality(stats)
    assert stats["u_meters_per_uv"]["min"] < .97
    assert stats["u_meters_per_uv"]["max"] > 1.05
    assert stats["v_meters_per_uv"]["min"] == pytest.approx(1, abs=.001)
    assert stats["v_meters_per_uv"]["max"] == pytest.approx(1, abs=.001)
    assert stats["cylindrical"]["u_at_mean_section_meters_per_uv"]["mean"] == pytest.approx(1, abs=.001)
    json.dumps(stats, allow_nan=False)


def test_cylinder_rejects_six_percent_v_error():
    vertices, faces, uv = cylinder_profile()
    uv[:, :, 1] /= 1.06
    stats = uv_quality(vertices, faces, uv, "cylinder")
    with pytest.raises(AssertionError, match="V mean .*outside 1m"):
        assert_uv_quality(stats)


def test_cylinder_rejects_six_percent_u_error_at_mean_section():
    vertices, faces, uv = cylinder_profile()
    uv[:, :, 0] /= 1.06
    stats = uv_quality(vertices, faces, uv, "cylinder")
    assert stats["u_meters_per_uv"]["max"] < 1.15
    with pytest.raises(AssertionError, match="U at mean section"):
        assert_uv_quality(stats)


def test_cylinder_rejects_one_sixteen_percent_u_face_without_averaging_it_away():
    vertices, faces, uv = cylinder_profile((1., 1., 1.))
    uv[0, :, 0] /= 1.16
    stats = uv_quality(vertices, faces, uv, "cylinder")
    assert stats["cylindrical"]["u_at_mean_section_meters_per_uv"]["mean"] == pytest.approx(1, abs=.001)
    with pytest.raises(AssertionError, match="U .*outside 1m"):
        assert_uv_quality(stats)


@pytest.mark.parametrize("axis", [0, 1])
def test_box_rejects_six_percent_in_either_checker_direction(axis):
    vertices, faces, uv = plane()
    assert_uv_quality(uv_quality(vertices, faces, uv, "box"))
    uv[:, :, axis] /= 1.06
    with pytest.raises(AssertionError, match=f"{'U' if axis == 0 else 'V'} .*outside 1m"):
        assert_uv_quality(uv_quality(vertices, faces, uv, "box"))


def test_directional_vectors_do_not_mix_u_with_v_like_singular_values():
    vertices, faces, uv = plane()
    uv[:, :, 0] -= uv[:, :, 1]
    stats = uv_quality(vertices, faces, uv, "box")
    assert stats["u_meters_per_uv"]["max"] == pytest.approx(1)
    assert stats["v_meters_per_uv"]["min"] == pytest.approx(np.sqrt(2))


@pytest.mark.parametrize("radial_face", [False, True])
def test_cylinder_planar_caps_and_stave_thickness_keep_five_percent_limit(radial_face):
    vertices, faces, uv = plane()
    if radial_face:
        # Vertical radial plane: its normal is the angular tangent.
        vertices[:, 0] += 1
    else:
        # Horizontal cap: the normal is +Y.
        vertices = vertices[:, [0, 2, 1]]
    # Keep the cylinder axis away from this standalone surface's centre.
    vertices = np.vstack((vertices, [-2., -1., -2.], [2., 2., 2.]))
    uv[:, :, 0] /= 1.06
    stats = uv_quality(vertices, faces, uv, "cylinder")
    assert stats["planar"]["triangles"] == 2
    with pytest.raises(AssertionError, match="U .*outside 1m"):
        assert_uv_quality(stats)


def test_degenerate_uv_fails_explicitly_instead_of_being_dropped():
    vertices, faces, uv = plane()
    uv[0] = 0
    with pytest.raises(ValueError, match="nondegenerate UV"):
        uv_quality(vertices, faces, uv, "box")


def elliptical_mean_section(angular_u=False):
    """Known ellipse arc, calculated by quadrature without the UV implementation."""
    vertices, faces, uv = cylinder_profile((.96, 1.02, 1.04, 1.02, .96))
    parameter = uv[:, :, 0].copy()
    # Recover the original circle's parameter from its test reference radius.
    triangles = vertices[faces]
    area = np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0],
                                   triangles[:, 2] - triangles[:, 0]), axis=1) * .5
    reference = np.average(np.linalg.norm(triangles[:, :, [0, 2]], axis=2).mean(axis=1), weights=area)
    parameter /= reference
    vertices[:, 0] *= .47
    vertices[:, 2] *= .38
    triangles = vertices[faces]
    area = np.linalg.norm(np.cross(triangles[:, 1] - triangles[:, 0],
                                   triangles[:, 2] - triangles[:, 0]), axis=1) * .5
    radial_scale = np.linalg.norm(triangles[:, :, [0, 2]] / [.47, .38], axis=2).mean(axis=1)
    reference = np.average(radial_scale, weights=area)
    grid = np.linspace(0, 2 * np.pi, 32769)
    if angular_u:
        angle = np.unwrap(np.arctan2(.38 * np.sin(grid), .47 * np.cos(grid)))
        physical_radius = np.linalg.norm(triangles[:, :, [0, 2]], axis=2).mean(axis=1)
        uv[:, :, 0] = np.interp(parameter, grid, angle) * np.average(physical_radius, weights=area)
    else:
        speed = np.hypot(.47 * np.sin(grid), .38 * np.cos(grid))
        arc = np.r_[0., np.cumsum((speed[:-1] + speed[1:]) * np.diff(grid) * .5)]
        uv[:, :, 0] = np.interp(parameter, grid, arc) * reference
    # The geometric bow has a small slope, so height itself is within the 5% V
    # tolerance; this avoids sharing the implementation's meridian calculation.
    uv[:, :, 1] = vertices[faces, 1]
    return vertices, faces, uv


def test_ellipse_arc_has_metric_mean_section_in_every_occupied_angle_bin():
    vertices, faces, uv = elliptical_mean_section()
    stats = uv_quality(vertices, faces, uv, "cylinder", np.ones(len(faces), dtype=int))
    assert_uv_quality(stats)
    reference = stats["cylindrical"]["u_at_mean_section_meters_per_uv"]
    assert reference["occupied_bins"] == 64
    assert reference["min"] == pytest.approx(1, abs=.01)
    assert reference["max"] == pytest.approx(1, abs=.01)


def test_ellipse_angular_u_cannot_hide_mean_section_distortion_in_global_average():
    vertices, faces, uv = elliptical_mean_section(angular_u=True)
    stats = uv_quality(vertices, faces, uv, "cylinder", np.ones(len(faces), dtype=int))
    assert stats["u_meters_per_uv"]["min"] > .85
    assert stats["u_meters_per_uv"]["max"] < 1.15
    reference = stats["cylindrical"]["u_at_mean_section_meters_per_uv"]
    assert reference["mean"] == pytest.approx(1, abs=.04)
    assert reference["min"] < .95
    assert reference["max"] > 1.05
    with pytest.raises(AssertionError, match="U at mean section"):
        assert_uv_quality(stats)


def test_mean_section_checks_each_actual_chart_instead_of_cancelling_scale_errors():
    vertices, faces, uv = cylinder_profile((1., 1., 1.))
    charts = (vertices[faces].mean(axis=1)[:, 2] >= 0).astype(int)
    uv[charts == 0, :, 0] /= 1.06
    uv[charts == 1, :, 0] /= .94
    stats = uv_quality(vertices, faces, uv, "cylinder", patch_ids=charts)
    assert stats["cylindrical"]["u_at_mean_section_meters_per_uv"]["charts"] == 2
    with pytest.raises(AssertionError, match="U at mean section"):
        assert_uv_quality(stats)


def test_mean_section_u_does_not_mix_in_meridian_v_shear():
    vertices, faces, uv = elliptical_mean_section()
    types = np.ones(len(faces), dtype=int)
    baseline = uv_quality(vertices, faces, uv, "cylinder", types)
    sheared = uv.copy()
    sheared[:, :, 1] += .6 * sheared[:, :, 0]
    distorted = uv_quality(vertices, faces, sheared, "cylinder", types)
    assert distorted["cylindrical"]["u_at_mean_section_meters_per_uv"] == baseline["cylindrical"]["u_at_mean_section_meters_per_uv"]
    # The original checker measurement still sees the physical shear.
    assert distorted["u_meters_per_uv"]["max"] > baseline["u_meters_per_uv"]["max"] + .05
