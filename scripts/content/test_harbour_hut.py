"""Connected wall fields, UV measurements, and strict repeated-block signatures."""

import math

import numpy as np
import pytest

import harbour_hut as hut


def triangle(points, uv=((0, 0), (1, 0), (0, 1))):
    points = np.asarray(points, dtype=float)
    cross = np.cross(points[1] - points[0], points[2] - points[0])
    return dict(points=points.tolist(), uv=np.asarray(uv, dtype=float).tolist(),
                texture='WALL.TGA', center=points.mean(axis=0).tolist(),
                normal=(cross / np.linalg.norm(cross)).tolist(),
                area_m2=float(np.linalg.norm(cross) / 20000))


def wall(x=0, width=250, height=200):
    a, b, c, d = ((x, 0, 0), (x + width, 0, 0),
                  (x + width, height, 0), (x, height, 0))
    return [triangle((a, b, c)), triangle((a, c, d))]


def fields(triangles, **kwargs):
    return {frozenset(group) for group in hut.coplanar_fields(triangles, **kwargs)}


def test_field_area_threshold_applies_to_connected_patch_not_each_triangle():
    # Each half is only 2.5 m², but their shared edge makes a 5 m² wall field.
    triangles = wall() + wall(1000, width=249.9)
    assert fields(triangles) == {frozenset((0, 1))}


def test_same_plane_separated_patches_and_opposite_faces_stay_distinct():
    first = wall()
    opposite = [triangle(t['points'][::-1]) for t in first]
    # The 2 cm gap is larger than the wall-field joining tolerance of 1 cm.
    triangles = first + wall(252) + opposite
    assert fields(triangles) == {frozenset((0, 1)), frozenset((2, 3)),
                                 frozenset((4, 5))}


def test_tilted_intersecting_face_is_not_coplanar_at_its_vertices():
    base = wall(width=400, height=600)
    tilted = triangle(((400, 0, -3), (800, 0, -3), (400, 600, 6)))
    # Its centroid lies exactly in the wall plane, its normal differs by less
    # than 1 degree, and it intersects the wall edge. Vertex distances reject it.
    assert tilted['center'][2] == 0
    assert np.dot(base[0]['normal'], tilted['normal']) > math.cos(math.radians(1))
    assert fields(base + [tilted]) == {frozenset((0, 1)), frozenset((2,))}


def test_touching_horizontal_surface_is_excluded_from_wall_fields():
    floor = [triangle(((0, 0, 0), (0, 0, 300), (300, 0, 300))),
             triangle(((0, 0, 0), (300, 0, 300), (300, 0, 0)))]
    assert fields(wall(width=300) + floor) == {frozenset((0, 1))}
    assert fields(floor, vertical_only=False) == {frozenset((0, 1))}


@pytest.mark.parametrize('angle_degrees', [35, -145, 90])
def test_affine_uv_scale_and_signed_u_direction_survive_world_rotation(angle_degrees):
    yaw, angle = math.radians(40), math.radians(angle_degrees)
    tangent = np.asarray([math.cos(yaw), 0., math.sin(yaw)])
    up = np.asarray([0., 1., 0.])
    world_u = 200 * (math.cos(angle) * tangent + math.sin(angle) * up)
    world_v = 120 * (-math.sin(angle) * tangent + math.cos(angle) * up)
    # A translated, skew UV triangle prevents an edge-length shortcut from
    # producing the correct centimetres per texture repeat.
    uv = np.asarray(((2, -3), (4, 0), (1, 2)), dtype=float)
    points = np.asarray([700., -20., 450.]) + uv @ np.stack((world_u, world_v))
    result = hut.uv_geometry(triangle(points, uv), tangent)
    assert result['uv_span'] == pytest.approx([3, 5])
    assert result['uv_area'] == pytest.approx(6.5)
    assert result['cm_per_uv'] == pytest.approx([200, 120])
    assert result['world_u_cm'] == pytest.approx(world_u)
    assert result['world_v_cm'] == pytest.approx(world_v)
    assert result['u_angle_degrees'] == pytest.approx(angle_degrees)


def test_degenerate_uv_keeps_extents_without_inventing_world_scale():
    face = triangle(((0, 0, 0), (300, 0, 0), (0, 200, 0)),
                    ((0, 1), (2, 3), (4, 5)))
    result = hut.uv_geometry(face, [1, 0, 0])
    assert result['degenerate_uv'] is True
    assert result['uv_span'] == [4, 4]
    assert result['uv_area'] == 0
    assert 'cm_per_uv' not in result
    assert 'u_angle_degrees' not in result


def test_texture_signature_requires_every_material_within_one_percent():
    reference = {'PLANKS.TGA': 100., 'GABLE.TGA': 20.}
    assert hut.area_signature_matches(reference, {'GABLE.TGA': 19.8, 'PLANKS.TGA': 101.})
    assert not hut.area_signature_matches(reference, {'PLANKS.TGA': 101.001, 'GABLE.TGA': 20.})
    assert not hut.area_signature_matches(reference, {'PLANKS.TGA': 100.})
    assert not hut.area_signature_matches(reference, dict(reference, **{'BEAM.TGA': .01}))
    # Equal totals do not turn a material substitution into a repeated block.
    assert not hut.area_signature_matches(reference, {'PLANKS.TGA': 90., 'GABLE.TGA': 30.})
