import json
import math

from PIL import Image

import harbour_motion

CAMERAS = [
    {'name': 'ship', 'eye': [0, 0, 0], 'target': [1000, 500, 0], 'motion': True},
    {'name': 'quay', 'eye': [0, 0, 0], 'target': [0, 0, 1000]},
]


def test_the_scenario_draws_everything_and_pairs_moving_shots():
    day = harbour_motion.bench_lines(CAMERAS, night=False)
    assert 'view full' in day and not any(line.startswith('view hide') for line in day)
    assert day.count('set time 12 0') == 3  # the header and before each shot
    shots = [line[5:] for line in day if line.startswith('shot ')]
    assert shots == ['ship', 'ship-b', 'quay']
    i = day.index('shot ship')
    assert day[i + 1] == f'wait {harbour_motion.PAIR_MS}' and day[i + 2] == 'shot ship-b'


def test_the_night_scenario_names_its_shots():
    night = harbour_motion.bench_lines(CAMERAS, night=True)
    assert 'set time 12 0' not in night and night.count('set time 0 0') == 3
    assert [line[5:] for line in night if line.startswith('shot ')] == ['ship-night', 'ship-night-b', 'quay-night']
    assert 'midnight' in night[0] and 'harbour_motion.py' in night[1]


def test_a_reshoot_scenario_takes_only_the_named_cameras(tmp_path, monkeypatch):
    cameras = tmp_path / 'cameras.json'
    cameras.write_text(json.dumps({'cameras': CAMERAS}))
    (tmp_path / 'scripts/content').mkdir(parents=True)
    monkeypatch.setattr(harbour_motion, 'CAMERAS', cameras)
    monkeypatch.setattr(harbour_motion, 'ROOT', tmp_path)
    args = harbour_motion.parse_args(['bench', '--only', 'quay', '--name', 'reshoot'])
    assert args.func(args) == 0
    shots = [line for line in (tmp_path / 'scripts/content/reshoot-night.bench').read_text().splitlines() if line.startswith('shot ')]
    assert shots == ['shot quay-night']
    args = harbour_motion.parse_args(['bench', '--only', 'quay,mole'])
    try:
        args.func(args)
    except SystemExit as e:
        assert 'mole' in str(e)
    else:
        raise AssertionError('an unknown camera must stop the scenario')


def test_a_smoke_scenario_waits_out_a_particle_life_and_turns_to_night():
    lines = harbour_motion.together_lines(CAMERAS, CAMERAS[:1], settle=12000)
    shots = [line[5:] for line in lines if line.startswith('shot ')]
    assert shots == ['ship', 'ship-b', 'quay', 'ship-night', 'ship-night-b']
    assert lines.count('wait 12000') == 3 and 'wait 6000' not in lines
    # midnight before the first night camera settles, and again right before its shot
    turn = lines.index('set time 0 0')
    night_goto = next(i for i in range(turn, len(lines)) if lines[i].startswith('goto pos'))
    assert lines.index('shot quay') < turn < night_goto < lines.index('wait 12000', night_goto) < lines.index('shot ship-night')
    assert lines[lines.index('shot ship-night') - 1] == 'set time 0 0'


def test_a_clip_follows_the_shots_of_each_camera():
    lines = harbour_motion.together_lines(CAMERAS, CAMERAS[1:], settle=13000, clip=15000)
    recs = [line[4:] for line in lines if line.startswith('rec ')]
    assert recs == ['ship-clip', 'quay-clip', 'quay-night-clip']
    i = lines.index('rec ship-clip')
    assert lines[i - 1] == 'shot ship-b' and lines[i + 1:i + 3] == ['wait 15000', 'stop']
    assert not any(line.startswith('rec ') for line in harbour_motion.bench_lines(CAMERAS, night=False))


def test_the_smoke_sprite_is_soft_white_and_has_no_edge(tmp_path):
    import numpy as np
    alpha = np.full((16, 16), 0.8)
    rgba = harbour_motion.smoke_sprite(alpha)
    assert rgba.shape == (16, 16, 4) and (rgba[..., :3] == 1).all()
    assert rgba[..., 3].max() == 0.8 and rgba[0, :, 3].max() == 0 and rgba[:, -1, 3].max() == 0
    path = tmp_path / 'own-hut-smoke/albedo.dds'
    harbour_motion.write_soft_dds(path, rgba)
    data = path.read_bytes()
    assert data[:4] == b'DDS ' and data[84:88] == b'DXT5'
    assert len(data) == 128 + 16 * (16 + 4 + 1 + 1 + 1)  # 16x16, 8x8, 4x4, 2x2, 1x1 in 4x4 blocks
    with Image.open(path) as image:
        back = np.asarray(image.convert('RGBA'), float) / 255
    assert np.abs(back[..., 3] - rgba[..., 3]).max() < 0.04  # a soft alpha, not hardened to a cutout


def test_a_tilted_emitter_leans_with_the_wind():
    smoke, = harbour_motion.layer_entries([
        {'key': 'smoke', 'kind': 'emitter', 'pfx': 'OWN_SMOKE', 'position': [1, 2, 3], 'tilt': 40}])
    assert 'yaw_deg' not in smoke
    up = [row[1] for row in smoke['rotation']]  # the local up axis in the world
    assert up[0] == 0 and up[2] > 0 and abs(up[1] ** 2 + up[2] ** 2 - 1) < 1e-5
    assert abs(math.degrees(math.atan2(up[2], up[1])) - 40) < 1e-3
    try:
        harbour_motion.layer_entries([{'key': 'lamp', 'kind': 'light', 'position': [0, 0, 0], 'range': 1,
                                       'color': [1, 1, 1], 'tilt': 10}])
    except ValueError as e:
        assert 'tilt' in str(e)
    else:
        raise AssertionError('only an emitter leans')


def test_own_particles_keep_only_the_format_fields():
    doc = harbour_motion.particle_document([
        {'name': 'OWN_SMOKE', 'base': 'LIGHTSMOKE', 'set': {'visAlphaStart': 200.0}, 'why': 'denser'}])
    assert doc == {'format': 1, 'effects': [{'name': 'OWN_SMOKE', 'base': 'LIGHTSMOKE', 'set': {'visAlphaStart': 200.0}}]}
    for bad in ([{'name': 'own_smoke', 'base': 'LIGHTSMOKE', 'set': {}}],
                [{'name': 'A', 'base': 'LIGHTSMOKE', 'set': {}}, {'name': 'A', 'base': 'A', 'set': {}}]):
        try:
            harbour_motion.particle_document(bad)
        except ValueError:
            continue
        raise AssertionError(bad)


def test_emitters_and_lights_keep_their_positions():
    entries = harbour_motion.layer_entries([
        {'key': 'smoke', 'kind': 'emitter', 'pfx': 'LIGHTSMOKE', 'position': [1, 2.04, 3]},
        {'key': 'lamp', 'kind': 'light', 'position': [4, 5, 6], 'range': 600, 'color': [255, 150, 80]},
        {'key': 'post', 'kind': 'mesh', 'visual': 'FIREPLACE_NW_LAMP_01.ASC', 'position': [7, 8, 9], 'yaw': 30},
    ])
    smoke, lamp, post = entries
    assert smoke == {'key': 'smoke', 'kind': 'emitter', 'position': [1.0, 2.0, 3.0], 'yaw_deg': 0.0, 'pfx': 'LIGHTSMOKE'}
    assert lamp['range'] == 600.0 and lamp['color'] == [255, 150, 80] and 'pfx' not in lamp
    assert post['visual'] == 'FIREPLACE_NW_LAMP_01.ASC' and post['collision'] == 'mesh' and post['yaw_deg'] == 30.0
    json.dumps(entries)


def test_duplicate_keys_are_refused():
    item = {'key': 'a', 'kind': 'emitter', 'pfx': 'FIRE_SMOKE', 'position': [0, 0, 0]}
    try:
        harbour_motion.layer_entries([item, dict(item)])
    except ValueError as error:
        assert 'duplicate' in str(error)
    else:
        raise AssertionError('duplicate keys passed')


def test_motion_counts_changed_pixels_per_region():
    a = Image.new('RGB', (20, 10), (100, 100, 100))
    b = a.copy()
    for x in range(10, 20):
        for y in range(5):
            b.putpixel((x, y), (140, 140, 140))  # a moving patch: 50 of the right half's 100 pixels
    result = harbour_motion.pair_motion(a, b, {'cloth': [10, 0, 20, 10], 'still': [0, 0, 10, 10]})
    assert result['still'] == {'mean': 0.0, 'changed': 0.0}
    assert result['cloth']['changed'] == 0.5 and abs(result['cloth']['mean'] - 20.0) < 1e-6


def test_the_camera_aims_at_the_target_wherever_the_hero_stands():
    import math
    for hero in ('near', 'far'):
        camera = {'name': 'quay', 'eye': [100, 50, 200], 'target': [1100, 450, 2200], 'hero': hero}
        lines = harbour_motion.bench_lines([camera], night=False)
        look = float(next(line for line in lines if line.startswith('cam look'))[9:])
        spot = [float(v) for v in next(line for line in lines if line.startswith('goto pos'))[9:].split()]
        dist, dy, yaw = (float(v) for v in next(line for line in lines if line.startswith('cam world'))[10:].split())
        eye = [spot[0] + dist * math.cos(math.radians(yaw)), spot[1] + dy, spot[2] + dist * math.sin(math.radians(yaw))]
        assert all(abs(eye[i] - camera['eye'][i]) < 0.01 for i in range(3))
        aim = [spot[0] - eye[0], spot[1] + look - eye[1], spot[2] - eye[2]]
        ray = [camera['target'][i] - camera['eye'][i] for i in range(3)]
        cross = [aim[1] * ray[2] - aim[2] * ray[1], aim[2] * ray[0] - aim[0] * ray[2], aim[0] * ray[1] - aim[1] * ray[0]]
        assert max(abs(c) for c in cross) < 1e-3 * math.dist(aim, [0, 0, 0]) * math.dist(ray, [0, 0, 0])
    far = harbour_motion.hero_spot({'eye': [0, 0, 0], 'target': [0, 0, 1000]})
    assert far == [0.0, 2000.0, 6000.0]


def test_an_attached_item_follows_its_parent_turn():
    entries = harbour_motion.layer_entries([
        {'key': 'post', 'kind': 'mesh', 'visual': 'OC_LOB_LIGHTER_HUGE3', 'position': [100, 0, 200], 'yaw': 90},
        {'key': 'fire', 'kind': 'emitter', 'pfx': 'FIRE_MEDIUM', 'attach': 'post', 'offset': [10, 324, 0]},
    ])
    # yaw 90: local +x goes to world -z (rows [[c, 0, s], [0, 1, 0], [-s, 0, c]])
    assert entries[1]['position'] == [100.0, 324.0, 190.0]
