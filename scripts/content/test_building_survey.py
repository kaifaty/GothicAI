"""Geometry-only building census, exclusions, and ground-level camera framing."""

import csv
import json
from types import SimpleNamespace

import numpy as np
import pytest

import building_survey as survey


def triangle(points, texture='WALL.TGA'):
    points = np.asarray(points, dtype=float)
    cross = np.cross(points[1] - points[0], points[2] - points[0])
    return dict(points=points.tolist(), texture=texture, visual='',
                center=points.mean(axis=0).tolist(),
                normal=(cross / np.linalg.norm(cross)).tolist(),
                area_m2=float(np.linalg.norm(cross) / 20000))


def quad(a, b, c, d, texture='WALL.TGA'):
    return [triangle((a, b, c), texture), triangle((a, c, d), texture)]


def house(x=0, front='WALL.TGA'):
    # A 4 x 4 x 5 m shell, with no floor. Adjacent x positions share a wall.
    return (quad((x, 0, 0), (x + 400, 0, 0), (x + 400, 400, 0), (x, 400, 0), front)
            + quad((x, 0, 500), (x, 400, 500), (x + 400, 400, 500), (x + 400, 0, 500))
            + quad((x, 0, 0), (x, 400, 0), (x, 400, 500), (x, 0, 500))
            + quad((x + 400, 0, 0), (x + 400, 0, 500), (x + 400, 400, 500), (x + 400, 400, 0))
            + quad((x, 400, 0), (x + 400, 400, 0), (x + 400, 400, 500), (x, 400, 500)))


def components(triangles, **kwargs):
    return {frozenset(group) for group in survey.cluster_triangles(triangles, **kwargs)}


def test_two_houses_with_shared_wall_are_one_connected_shell():
    # This census counts connected structures: a party wall joins both houses.
    triangles = house() + house(400)
    assert components(triangles) == {frozenset(range(20))}


@pytest.mark.parametrize('gap_cm, expected', [(50, 1), (50.01, 2), (175, 2)])
def test_face_gap_threshold_between_separate_houses(gap_cm, expected):
    triangles = house() + house(400 + gap_cm)
    groups = components(triangles)
    assert len(groups) == expected
    assert set().union(*groups) == set(range(20))
    if expected == 2:
        assert groups == {frozenset(range(10)), frozenset(range(10, 20))}


def test_centimeters_round_shared_vertices_before_face_proximity():
    first = triangle(((0, 0, 0), (100, 0, 0), (0, 100, 0)))
    second = triangle(((0, 0, .49), (100, 0, .49), (0, 100, .49)))
    assert components([first, second], gap_cm=0) == {frozenset((0, 1))}


@pytest.mark.parametrize('gap_cm, expected', [(0, 1), (49.99, 1), (50, 1), (50.01, 2)])
def test_mid_edge_face_proximity_does_not_depend_on_vertices_or_centroids(gap_cm, expected):
    # Crossed long edges are closest in their middles. Every vertex pair and
    # both centroids remain farther apart than the allowed 50 cm.
    a = triangle(((-1000, 0, 0), (1000, 0, 0), (0, -100, 0)))
    b = triangle(((0, -1000, gap_cm), (0, 1000, gap_cm), (200, 0, gap_cm)))
    pa, pb = np.asarray(a['points']), np.asarray(b['points'])
    assert np.linalg.norm(pa[:, None] - pb[None, :], axis=2).min() > 50
    assert np.linalg.norm(pa.mean(axis=0) - pb.mean(axis=0)) > 50
    assert survey.face_distance(pa[None], pb[None])[0] == pytest.approx(gap_cm)
    assert len(components([a, b])) == expected


def test_edge_piercing_triangle_interior_has_zero_distance():
    floor = triangle(((-1000, 0, -1000), (1000, 0, -1000), (0, 0, 1000)))
    crossing = triangle(((-100, -100, 0), (100, -100, 0), (0, 100, 0)))
    # No vertex lies on either triangle, and the small triangle pierces the
    # large face away from all three large boundary edges.
    assert survey.face_distance([floor['points']], [crossing['points']])[0] == 0
    assert components([floor, crossing], gap_cm=0) == {frozenset((0, 1))}


def test_shell_selection_keeps_architecture_and_rejects_non_shell_roles():
    with (survey.ROOT / 'data/content/city-materials.csv').open() as stream:
        rows = list(csv.DictReader(stream))
    selected = survey.shell_rows(rows)
    assert {
        'NW_CITY_WALL_03.TGA', 'NW_CITY_WINDOW_03.TGA',
        'NW_CITY_WOODBEAM_02.TGA', 'NW_CITY_FACHWERKBASEMENT_EMPTY_01.TGA',
        'NW_CITY_HAFEN_GIEBEL_01.TGA', 'NW_CITY_HAFEN_HOLZWAND_01.TGA',
        'NW_CITY_HAFEN_HOLZWAND_02.TGA', 'NW_CITY_ROOF_01.TGA',
        'NW_CITY_ROOF_04.TGA', 'NW_CITY_STONEWALL_02.TGA',
        'NW_MISC_STRONG_STONEWALL_01.TGA',
    } <= selected.keys()
    assert not ({
        'NW_CITY_DIRTYGROUND_02.TGA', 'NW_CITY_DIRTYGROUND_WEG_02.TGA',
        'NW_CITY_BODENDIELEN_01.TGA', 'NW_CITY_INSIDEFLOOR_11.TGA',
        'NW_CITY_DUNGEONFLOOR_02.TGA', 'NW_CITY_PFLASTERSTEIN_01.TGA',
        'NW_CITY_HAFENKAI_BODEN_01.TGA', 'NW_CITY_HAFENKAI_BODEN_TRANS_01.TGA',
        'NW_CITY_INSIDEWALL_11.TGA', 'NW_CITY_INSIDEWALL_12.TGA',
        'NW_CITY_INSIDEWALL_13.TGA', 'NW_CITY_BARREL_01.TGA',
        'NW_HARBOUR_WOODCRATE_01.TGA', 'NW_HARBOUR_CANON_01.TGA',
        'NW_HARBOUR_FISHINGNET_01.TGA', 'NW_CITY_ROPE_01.TGA',
        'NW_HARBOUR_ROPEWALL_01.TGA', 'NW_CITY_IVY_FULL_01.TGA',
        'NW_CITY_GITTERZAUN_01.TGA', 'NW_HARBOUR_WOODENGATE_01.TGA',
    } & selected.keys())


def test_exclusion_decision_overrides_an_architectural_class():
    rows = [dict(texture='WALL.TGA', decision='include', **{'class': 'planks_wall'}),
            dict(texture='ALPHA.TGA', decision='exclude', **{'class': 'planks_wall'}),
            dict(texture='NW_CITY_INSIDEWALL_TEST.TGA', decision='module',
                 **{'class': 'framed_masonry_tile'})]
    assert set(survey.shell_rows(rows)) == {'WALL.TGA'}


def test_props_reusing_shell_materials_do_not_enter_building_census():
    texture = 'NW_CITY_WOODBEAM_02.TGA'
    rows = survey.shell_rows([
        dict(texture=texture, decision='module', **{'class': 'bound_timber_tile'})])
    surface = triangle(((0, 0, 0), (100, 0, 0), (0, 100, 0)), texture)
    building_visuals = ('', 'NW_CITY_BEAM_01.MRM', 'NW_MISC_POOR_HUT_01.MRM')
    prop_visuals = ('NW_HARBOUR_CANON_01.MRM', 'NW_HARBOUR_FISHINGNET_01.MRM',
                    'NW_CITY_SHELF_01.MRM', 'NW_CITY_SIGN_01.MRM',
                    'NW_HARBOUR_SHIP_01.MRM', 'LOOSE_BOARDS.MRM')
    selected = {visual for visual in building_visuals + prop_visuals
                if survey.shell_triangle(dict(surface, visual=visual), rows)}
    assert selected == set(building_visuals)
    assert not survey.shell_triangle(dict(surface, area_m2=0), rows)
    assert not survey.shell_triangle(dict(surface, texture='GROUND.TGA'), rows)


def test_ground_does_not_weld_disconnected_house_shells_after_filtering():
    triangles = house() + house(800)
    # One ground triangle touches a vertex on each house and would bridge them.
    triangles += [triangle(((0, 0, 0), (1200, 0, 0), (0, 0, 1000)), 'GROUND.TGA')]
    assert len(components(triangles)) == 1
    rows = [dict(texture='WALL.TGA', decision='include', **{'class': 'masonry_rubble'}),
            dict(texture='GROUND.TGA', decision='include', **{'class': 'mud'})]
    selected = survey.shell_rows(rows)
    shell = [t for t in triangles if t['texture'] in selected]
    assert len(shell) == 20
    assert components(shell) == {frozenset(range(10)), frozenset(range(10, 20))}


def test_camera_is_outside_largest_shutter_wall_at_ground_plus_170_and_frames_facade():
    triangles = house(front=survey.HARBOUR[1])
    # The archive stores oriented exterior normals for this front wall.
    for t in triangles[:2]:
        t['normal'] = [0, 0, -1]
    cluster = survey.describe(triangles, list(range(len(triangles))),
                              [dict(name='ENTRY', position=[0, 0, 0])])
    cluster['id'] = 'B001'
    ground = [t['points'] for t in quad((-100000, 25, -100000), (100000, 25, -100000),
                                       (100000, 25, 100000), (-100000, 25, 100000))]
    result = survey.camera(cluster, triangles, ground)
    eye, target = np.asarray(result['eye_cm']), np.asarray(result['target_cm'])
    assert result['ground_y_cm'] == 25
    assert eye[1] == 195
    assert eye[2] < 0
    np.testing.assert_allclose(result['outward'], [0, 0, -1])
    np.testing.assert_allclose(target, [200, 200, 0])
    assert result['face_area_m2'] == pytest.approx(16)
    face = survey.facade(cluster, triangles)
    points = np.concatenate((face['points'], face['frame']))
    xy, depth = survey.project(points, eye, target)
    assert depth.min() > 0
    assert np.max(np.abs(xy)) <= .96


def test_camera_chooses_largest_connected_wall_not_largest_single_triangle():
    texture = survey.HARBOUR[1]
    front = [t for x in range(0, 600, 150)
             for t in quad((x, 0, 0), (x, 400, 0), (x + 150, 400, 0),
                           (x + 150, 0, 0), texture)]
    side = quad((600, 0, 0), (600, 400, 0), (600, 400, 400),
                (600, 0, 400), texture)
    assert max(t['area_m2'] for t in front) < min(t['area_m2'] for t in side)
    triangles = front + side
    cluster = survey.describe(triangles, list(range(len(triangles))),
                              [dict(name='ENTRY', position=[0, 0, 0])])
    cluster['id'] = 'B001'
    ground = [t['points'] for t in quad((-100000, 0, -100000), (100000, 0, -100000),
                                       (100000, 0, 100000), (-100000, 0, 100000))]
    result = survey.camera(cluster, triangles, ground)
    assert result['face_area_m2'] == pytest.approx(24)
    np.testing.assert_allclose(result['outward'], [0, 0, -1])


def test_ground_height_uses_highest_surface_below_camera_limit():
    grounds = [t['points'] for y in (-20, 40, 900)
               for t in quad((-100, y, -100), (100, y, -100),
                             (100, y, 100), (-100, y, 100))]
    assert survey.ground_height(grounds, 10, 20, below=200) == 40
    with pytest.raises(ValueError, match='No ground below camera'):
        survey.ground_height(grounds, 500, 20, below=200)


def test_walking_ground_accepts_streets_and_terrain_but_rejects_other_surfaces():
    with (survey.ROOT / 'data/content/city-materials.csv').open() as stream:
        mapping = list(csv.DictReader(stream))
    accepted = ['NW_CITY_DIRTYGROUND_02.TGA', 'NW_CITY_DIRTYGROUND_WEG_02.TGA',
                'NW_CITY_DIRTYGROUND_WEG_END_01.TGA', 'NW_CITY_PFLASTERSTEIN_01.TGA',
                'NW_CITY_PFLASTERSTEIN_BORDER_01.TGA', 'NW_CITY_HAFENKAI_BODEN_01.TGA',
                'NW_CITY_HAFENKAI_BODEN_TRANS_01.TGA', 'NW_NATURE_WALDBODEN_01.TGA',
                'NW_NATURE_PATH_01.TGA', 'NW_NATURE_DARKWOOD_FELS_01.TGA', 'OW_SURFACE_GRASS.TGA']
    rejected = ['NW_CITY_ROOF_01.TGA', 'NW_CITY_BODENDIELEN_01.TGA', 'NW_CITY_INSIDEFLOOR_11.TGA',
                'NW_NATURE_BARK_01.TGA', 'NW_CITY_IVY_FULL_01.TGA', 'NW_NATURE_GREATSEA_WATER_01.TGA']
    names = accepted+rejected
    points = [[[i*100, 0, 0], [i*100+50, 0, 0], [i*100, 0, 50]] for i in range(len(names))]
    result = survey.walking_ground(dict(ground_triangles=points, ground_textures=names), mapping)
    np.testing.assert_array_equal(result, points[:len(accepted)])


def test_camera_moves_along_facade_to_clear_occluder_on_walking_ground():
    from intent_cameras import Occluders

    triangles = house(front=survey.HARBOUR[1])
    block = survey.describe(triangles, list(range(len(triangles))),
                            [dict(name='ENTRY', position=[0, 0, 0])])
    block['id'] = 'B001'
    ground = [t['points'] for t in quad((-1000, 20, -1000), (1000, 20, -1000),
                                       (1000, 20, -200), (-1000, 20, -200))]
    obstacle = quad((100, 0, -150), (100, 1000, -150), (300, 1000, -150), (300, 0, -150))
    occluders = Occluders([t['points'] for t in obstacle])
    face = survey.facade(block, triangles)
    centered_eye = face['target']+face['outward']*300
    centered_eye[1] = 190
    assert all(occluders.first_hit(centered_eye, p, endpoint_margin=50) is not None
               for p in face['samples'])
    result = survey.camera(block, triangles, ground, occluders=occluders)
    eye, target = np.asarray(result['eye_cm']), np.asarray(result['target_cm'])
    assert result['tangent_offset_cm'] != 0
    assert result['facade_visible_fraction'] == 1
    assert result['ground_y_cm'] == 20
    assert eye[1] == 190
    assert (eye-target) @ face['outward'] > 0
    assert (eye-target) @ face['side'] == pytest.approx(0)
    assert all(occluders.first_hit(eye, p, endpoint_margin=50) is None for p in face['samples'])


def test_market_validation_accepts_complete_reference_inside_a_larger_block(tmp_path):
    reference = [triangle(((0, 0, 0), (100, 0, 0), (0, 100, 0)), survey.MARKET[0]),
                 triangle(((0, 0, 0), (0, 100, 0), (0, 0, 100)), survey.MARKET[1])]
    # Vertex order is irrelevant; extra connected surfaces and their areas do
    # not change whether this block contains the reference geometry.
    triangles = [dict(t, points=t['points'][::-1]) for t in reference]
    triangles += house(400, survey.MARKET[0])
    measurement = tmp_path / 'market.json'
    measurement.write_text(json.dumps(dict(candidate_house_triangles=reference)))
    blocks = [dict(id='B001', triangle_indices=list(range(len(triangles))))]
    result = survey.validate_market(blocks, triangles, measurement, [])
    assert result == dict(block_id='B001', waypoint='NW_CITY_MERCHANT_HUT_02_ENTRY',
                          reference_triangles=2, matched_triangles=2, missing_triangles=0)


def test_market_validation_requires_each_reference_triangle_in_one_block(tmp_path):
    first = triangle(((0, 0, 0), (100, 0, 0), (0, 100, 0)), survey.MARKET[0])
    other = triangle(((0, 0, 0), (0, 100, 0), (0, 0, 100)), survey.MARKET[1])
    measurement = tmp_path / 'market.json'
    # Duplicates count separately. Another block cannot supply a missing face.
    measurement.write_text(json.dumps(dict(candidate_house_triangles=[first, first, other])))
    blocks = [dict(id='B001', triangle_indices=[0, 1]),
              dict(id='B002', triangle_indices=[2])]
    with pytest.raises(ValueError, match='Market validation failed: 2/3 triangles'):
        survey.validate_market(blocks, [first, other, first], measurement, [])


def test_candidates_exclude_blocks_over_40m_and_fill_all_six_slots():
    def block(identifier, total, x=40, z=40, shutter=10):
        return dict(id=identifier, size_m=[x, 99, z],
                    textures_m2={survey.HARBOUR[0]: total-shutter,
                                 survey.HARBOUR[1]: shutter})

    blocks = [block(f'B{i:03d}', 100-i) for i in range(1, 8)]
    blocks += [block('B050', 1000, x=40.01), block('B051', 900, z=40.01),
               block('B052', 800, shutter=9.99)]
    assert [b['id'] for b in survey.select_candidates(blocks)] == [f'B{i:03d}' for i in range(1, 7)]


def test_facade_selects_50cm_plane_slab_and_adds_3m_on_both_sides():
    triangles = quad((0, 0, 0), (0, 400, 0), (600, 400, 0),
                     (600, 0, 0), survey.HARBOUR[1])
    triangles += [triangle(((-100, 400, 50), (700, 400, 50), (300, 700, 50)), survey.HARBOUR[2]),
                  triangle(((-200, 0, -50), (-100, 400, -50), (-100, 0, -50))),
                  triangle(((-500, 0, 50.01), (-500, 400, 50.01), (-400, 0, 50.01)))]
    triangles += quad((600, 0, 0), (600, 400, 0), (600, 400, 400), (600, 0, 400))
    block = survey.describe(triangles, list(range(len(triangles))),
                            [dict(name='ENTRY', position=[0, 0, 0])])
    face = survey.facade(block, triangles)
    assert set(face['triangle_indices']) == {0, 1, 2, 3}
    assert face['face_area_m2'] == pytest.approx(24)
    along_points = (face['points']-face['anchor']) @ face['side']
    along_frame = (face['frame']-face['anchor']) @ face['side']
    assert along_points.min()-along_frame.min() == pytest.approx(300)
    assert along_frame.max()-along_points.max() == pytest.approx(300)
    assert (face['frame'][:, 1].min(), face['frame'][:, 1].max()) == (0, 700)


def test_inward_patch_normal_flips_away_from_block_center_to_exterior_skin():
    triangles = house(front=survey.HARBOUR[1])
    for t in triangles[:2]:
        t['normal'] = [0, 0, 1]
    triangles += quad((0, 0, -25), (0, 400, -25), (400, 400, -25),
                      (400, 0, -25), survey.HARBOUR[0])
    block = survey.describe(triangles, list(range(len(triangles))),
                            [dict(name='ENTRY', position=[0, 0, 0])])
    face = survey.facade(block, triangles)
    assert np.dot(face['anchor']-block['center_cm'], face['normal']) < 0
    np.testing.assert_allclose(face['outward'], [0, 0, -1])
    assert np.dot(face['anchor']-block['center_cm'], face['outward']) > 0
    # Both skins lie in the 50 cm band. Aim at the outer skin so it does not
    # obscure the reference field on the inner side of the wall thickness.
    assert face['target'][2] == -25
    np.testing.assert_allclose(face['frame'][:, 2], -25)


@pytest.mark.parametrize('roof_texture', ['NW_CITY_ROOF_01.TGA', 'NW_CITY_ROOF_04.TGA', survey.HARBOUR[2]])
def test_roof_ridge_sets_facade_height_without_unrelated_tall_stonework(roof_texture):
    triangles = house(front=survey.HARBOUR[1])
    # The ridge lies outside the facade plane band and still belongs in frame.
    triangles += [triangle(((0, 400, 250), (400, 400, 250), (200, 650, 250)), roof_texture),
                  triangle(((800, 0, 200), (900, 0, 200), (850, 4370, 200)),
                           'NW_CITY_STONEWALL_02.TGA')]
    block = survey.describe(triangles, list(range(len(triangles))),
                            [dict(name='ENTRY', position=[0, 0, 0])])
    face = survey.facade(block, triangles)
    assert block['bounds_cm'][1][1] == 4370
    assert face['points'][:, 1].max() == 400
    assert face['frame'][:, 1].max() == 650
    assert face['target'][1] == 325


@pytest.mark.parametrize('angle', [45, -45])
def test_oblique_camera_stays_outside_at_45_degrees_and_frames_facade(angle):
    triangles = house(front=survey.HARBOUR[1])
    for t in triangles[:2]:
        t['normal'] = [0, 0, -1]
    block = survey.describe(triangles, list(range(len(triangles))),
                            [dict(name='ENTRY', position=[0, 0, 0])])
    block['id'] = 'B001'
    ground = [t['points'] for t in quad((-100000, 20, -100000), (100000, 20, -100000),
                                       (100000, 20, 100000), (-100000, 20, 100000))]
    result = survey.camera(block, triangles, ground, angle=angle)
    face = survey.facade(block, triangles)
    eye, target = np.asarray(result['eye_cm']), np.asarray(result['target_cm'])
    offset = eye-target
    normal_distance = offset @ face['outward']
    side_distance = offset @ face['side']
    assert normal_distance > 0
    assert side_distance/normal_distance == pytest.approx(np.sign(angle))
    assert eye[1] == 190
    assert result['angle_degrees'] == angle
    xy, depth = survey.project(np.concatenate((face['points'], face['frame'])), eye, target)
    assert depth.min() > 0
    assert np.abs(xy).max() <= .96


def test_scenario_has_front_and_three_quarter_for_six_blocks(tmp_path):
    candidates = [dict(id=f'B{i:03d}', bounds_cm=[[0, 0, 0], [400, 700, 500]],
                       cameras={view: dict(eye_cm=[0, 170, -1000], target_cm=[200, 350, 0])
                                for view in ('front', 'three-quarter')})
                  for i in range(1, 7)]
    path = tmp_path / 'harbour-house.bench'
    survey.write_scenario(path, candidates)
    lines = path.read_text().splitlines()
    assert [line for line in lines if line.startswith('shot ')] == [
        f'shot B{i:03d}-{view}' for i in range(1, 7) for view in ('front', 'three-quarter')]
    assert lines.count('cam hold') == 12
    assert 'set time 12 0' in lines
    assert 'view hide animated morph pfx' in lines


def test_sheet_contains_six_rows_of_two_640x360_panels_below_1mb(tmp_path):
    from PIL import Image

    candidates = [dict(id=f'B{i:03d}', nearest=f'HARBOUR_{i}',
                       textures_m2={name: i*10 for name in survey.HARBOUR}) for i in range(1, 7)]
    metadata = tmp_path / 'metadata.json'
    metadata.write_text(json.dumps(dict(candidates=candidates)))
    captures = tmp_path / 'captures'
    (captures / 'original').mkdir(parents=True)
    colors = {}
    for i, block in enumerate(candidates):
        for side, view in enumerate(('front', 'three-quarter')):
            color = (30+i*30, 50+side*120, 80)
            colors[i, side] = color
            Image.new('RGB', (1280, 720), color).save(captures / 'original' / f"{block['id']}-{view}.png")
    output = tmp_path / 'sheet.jpg'
    survey.sheet(SimpleNamespace(metadata=metadata, captures=captures, output=output))
    assert output.stat().st_size <= 1_000_000
    with Image.open(output) as sheet:
        assert sheet.format == 'JPEG'
        assert sheet.size == (1280, 6*(360+64))
        for (row, side), color in colors.items():
            np.testing.assert_allclose(sheet.getpixel((side*640+320, row*424+64+180)), color, atol=3)
