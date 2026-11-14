"""The trial torch's layer and effects in git agree with each other and with the set script's numbers."""
import importlib.util
import json
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('wall_torch_set', HERE / 'wall_torch_set.py')
torch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(torch)


def test_layer_swaps_the_mesh_in_place_and_names_only_effects_of_its_file():
    layer = json.loads(torch.LAYER.read_text())
    effects = {e['name']: e for e in json.loads(torch.PARTICLES.read_text())['effects']}
    # the lamp hangs under the mesh vob, so the mesh is swapped in place, never removed
    assert layer['visual'] == [{'id': torch.MESH_ID, 'visual': torch.VISUAL}]
    assert torch.MESH_ID not in layer['remove'] and sorted(layer['remove']) == sorted([torch.EFFECT_ID, torch.FLARE_ID])
    emitters = {e['key']: e for e in layer['add']}
    assert all(e['kind'] == 'emitter' and e['pfx'] in effects for e in emitters.values())
    assert {e['pfx'] for e in emitters.values()} == set(effects), 'an effect of the file that no emitter uses'
    lamp, = layer['light']
    assert lamp['id'] == torch.LAMP_ID and 'flicker' in emitters[lamp['flicker_from']]
    assert all(abs(v) <= 100 for v in lamp['offset_cm'])


def test_the_fire_sits_on_the_wrap_and_the_light_in_the_flame():
    layer = json.loads(torch.LAYER.read_text())
    cfg = json.loads((torch.BLENDER_DIR / torch.CFG).read_text())
    at = {e['key']: np.array(e['position']) for e in layer['add']}
    flame = at['wall-torch-fire-flame']
    card = float(next(e for e in json.loads(torch.PARTICLES.read_text())['effects']
                      if e['name'] == 'OWN_TORCH_FLAME')['set']['visSizeStart_S'].split()[1])
    assert card == torch.CARD[1]
    # the club's tip in the world; the wrap ends a few cm past it
    tip = np.array(torch.POSE['rotation']) @ np.array(cfg['tip']) + np.array(torch.POSE['position'])
    low = flame[1] - card / 2
    assert tip[1] - 12 < low < tip[1] + 2, 'the flame card starts on the wrap, under its top'
    light = torch.LAMP_AT + np.array(layer['light'][0]['offset_cm'])
    assert low < light[1] < flame[1] + card / 2 and np.hypot(*(light - flame)[[0, 2]]) < 8, 'the light sits inside the flame'
    assert at['wall-torch-fire-drops'][1] < at['wall-torch-fire-licks'][1] < at['wall-torch-fire-sparks'][1] < at['wall-torch-fire-soot'][1]


def test_world_maps_the_model_to_the_base_pose():
    # the loader mirrors X: the model's origin is the vob's place, +y of the model runs up the vob's own Y axis
    assert np.allclose(torch.world([0, 0, 0]), torch.POSE['position'])
    up = torch.world([0, 1, 0]) - torch.world([0, 0, 0])
    assert np.allclose(up, np.array(torch.POSE['rotation'])[:, 1] * 100)
    side = torch.world([1, 0, 0]) - torch.world([0, 0, 0])
    assert np.allclose(side, -np.array(torch.POSE['rotation'])[:, 0] * 100)


def test_every_torch_of_a_place_gets_the_model_its_fire_and_one_light_in_the_flame():
    effects = {e['name']: e for e in json.loads(torch.PARTICLES.read_text())['effects']}
    for name, place in torch.PLACES.items():
        layer = json.loads(torch.LAYER.with_name(f'wall-torch-{name}.json').read_text())
        ids = [e['id'] for e in layer['visual']]
        assert len(ids) == len(set(ids)) == place['count'] and all(e['visual'] == torch.VISUAL for e in layer['visual'])
        # the mesh and the lamp stay in the base's tree; three vobs of the original's fire go at most
        assert not set(layer['remove']) & (set(ids) | {lamp['id'] for lamp in layer['light']})
        assert len(layer['remove']) == len(set(layer['remove'])) <= 3 * len(ids)
        added = {e['key']: e for e in layer['add']}
        assert len(added) == len(layer['add'])
        lights = [e for e in layer['add'] if e['kind'] == 'light']
        followed = [e['flicker_from'] for e in layer['light'] + lights]
        assert sorted(followed) == sorted(f'wall-torch-{name}-{mesh}-flame' for mesh in ids), 'one light a flame'
        for mesh in ids:
            fire = [added[f'wall-torch-{name}-{mesh}-{end}'] for end in ('flame', 'flame-a', 'flame-b', 'licks', 'sparks', 'drops', 'soot')]
            assert all(e['kind'] == 'emitter' and e['pfx'] in effects for e in fire) and 'flicker' in fire[0]
        seeds = [e['flicker']['seed'] for e in layer['add'] if 'flicker' in e]
        assert len(seeds) == len(set(seeds)) == 3 * len(ids), 'no two flames pulse in step'
        assert all(max(abs(v) for v in lamp['offset_cm']) <= 100 for lamp in layer['light'])
        keys = [e['key'] for e in layer['add']]
        for light in lights:
            flame = np.array(added[light['flicker_from']]['position'])
            assert keys.index(light['flicker_from']) < keys.index(light['key']), 'the signal is known before its light'
            assert light['range'] == place['range'] and light['color'] == torch.LAMP['color']
            gap = np.array(light['position']) - flame
            assert abs(gap[1]) < torch.CARD[1] / 2 and np.hypot(gap[0], gap[2]) < 8, 'the light sits inside the flame'
