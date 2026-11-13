"""Render side views of clusters of 3D grass: the pictures on the cards of the tufts along the rock edges.

    blender -b -P grass_cards.py -- --source grass_medium_01/grass_medium_01_4k.gltf --out work/grass-tufts \
        [--density 1024] [--seed 11]

Step 2 of reports/engine/2026-11-13-grass/README.md. A card is a flat picture of a few scanned tufts standing in a
row, seen from the side by an orthographic camera. The tufts are stretched to the card's leaf length (the scan's
leaves are 12-19 cm, a card's 25-100 cm) and fan out of a narrow root. A hanging card is the same picture turned
upside down by the builder (landscape_grass_tufts.py): a fan of leaves out of a root at the top, as the strokes
of the original's painted grass hang.

Two passes per card, written to `cards.npz` as `<name>` (RGBA: linear albedo, not premultiplied, and coverage) and
`<name>_height` (metres above the root, where covered), the top image row first; `cards.json` lists the cards.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix

sys.path.insert(0, str(Path(__file__).parent))
import grass_patch  # noqa: E402

# name: width and height of the picture in metres; leaf: length the tallest tuft is stretched to, as a share of
# the height; tufts: how many stand in the row; root: half width of the row of roots, m; fan: the widest tuft's
# width, m; kinds: which tufts of the scan; hang: the builder turns the picture upside down.
CARDS = dict(
    hang_long_a=dict(size=(.375, 1.), leaf=(.7, .97), tufts=5, root=.035, fan=(.2, .33), kinds=('large', 'mid'), hang=True),
    hang_long_b=dict(size=(.375, 1.), leaf=(.55, .97), tufts=4, root=.03, fan=(.16, .3), kinds=('large', 'mid'), hang=True),
    hang_mid_a=dict(size=(.375, .7), leaf=(.6, .96), tufts=5, root=.04, fan=(.22, .34), kinds=('large', 'mid'), hang=True),
    hang_mid_b=dict(size=(.375, .7), leaf=(.5, .96), tufts=3, root=.03, fan=(.18, .3), kinds=('mid',), hang=True),
    droop_a=dict(size=(.5, .5), leaf=(.6, .95), tufts=6, root=.09, fan=(.26, .42), kinds=('large', 'mid'), hang=True),
    droop_b=dict(size=(.5, .5), leaf=(.5, .95), tufts=4, root=.07, fan=(.22, .4), kinds=('large', 'mid'), hang=True),
    droop_thin=dict(size=(.5, .5), leaf=(.5, .9), tufts=2, root=.05, fan=(.2, .36), kinds=('mid', 'small'), hang=True),
    clump_a=dict(size=(.5, .5), leaf=(.55, .93), tufts=7, root=.13, fan=(.22, .36), kinds=('large', 'mid'), hang=False),
    clump_b=dict(size=(.5, .5), leaf=(.5, .93), tufts=6, root=.12, fan=(.2, .34), kinds=('large', 'mid', 'small'), hang=False),
    clump_low=dict(size=(.5, .5), leaf=(.35, .6), tufts=7, root=.15, fan=(.2, .32), kinds=('mid', 'small'), hang=False),
    clump_thin=dict(size=(.5, .5), leaf=(.5, .9), tufts=3, root=.08, fan=(.2, .34), kinds=('mid', 'small'), hang=False),
)
MARGIN = .012     # empty rim of a picture, m: a card's neighbour in the atlas does not leak into its mips
STRAW = .05       # share of straw blades: the layer's bake has 7 %, and its lit straw reads yellow in the game


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True, help='glTF of the tufts')
    parser.add_argument('--out', required=True)
    parser.add_argument('--density', type=int, default=1024, help='pixels per metre')
    parser.add_argument('--seed', type=int, default=11)
    parser.add_argument('--samples', type=int, default=32)
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:])


def main():
    args = arguments()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=args.source)
    scene = bpy.context.scene
    sources = {o.name: o for o in scene.objects if o.type == 'MESH'}
    source = Path(args.source)
    choose = grass_patch.grass_material(
        next(iter(sources.values())).data.materials[0],
        source.parent / 'textures' / source.name.replace('.gltf', '.png').replace('_4k', '_alpha_4k'))
    names = {key: sorted(n for n in sources if f'_{key}_' in n) for key in ('large', 'mid', 'small')}

    rng = np.random.default_rng(args.seed)
    stage = bpy.data.collections.new('cards')
    scene.collection.children.link(stage)
    frames = {}
    for number, (name, card) in enumerate(CARDS.items()):
        width, height = card['size']
        centre = number * 4.    # every card has its own place along X
        count = card['tufts']
        for index in range(count):
            tuft = sources[str(rng.choice([n for kind in card['kinds'] for n in names[kind]]))]
            # the first tuft takes the full leaf length, the rest fall off to the shorter end
            share = card['leaf'][1] - (card['leaf'][1] - card['leaf'][0]) * (index / max(count - 1, 1)) ** .7
            length = (height - 2 * MARGIN) * share * rng.uniform(.93, 1.)
            spread = rng.uniform(*card['fan'])
            x = rng.uniform(-card['root'], card['root']) if count > 1 else 0.
            lean = math.radians(rng.normal(0, 5)) + math.atan2(x, length) * .6   # outer tufts lean outward
            matrix = (Matrix.Translation((centre + x, rng.uniform(-.03, .03), 0))
                      @ Matrix.Rotation(lean, 4, 'Y')
                      @ Matrix.Diagonal((spread / tuft.dimensions.x, spread / tuft.dimensions.x,
                                         length / tuft.dimensions.z, 1))
                      @ Matrix.Rotation(rng.uniform(0, 2 * math.pi), 4, 'Z')
                      @ Matrix.Translation(-tuft.matrix_world.translation))
            copy = bpy.data.objects.new(tuft.name, tuft.data)
            copy.matrix_world = matrix @ tuft.matrix_world
            copy.color = (rng.uniform(.85, 1.15), STRAW, 0, 1)   # brightness of the tuft, its share of straw
            stage.objects.link(copy)
        frames[name] = (centre, width, height)
    for tuft in sources.values():
        bpy.data.objects.remove(tuft)

    camera = bpy.data.cameras.new('side')
    camera.type, camera.clip_end = 'ORTHO', 50
    eye = bpy.data.objects.new('side', camera)
    eye.rotation_euler = (math.pi / 2, 0, 0)
    scene.collection.objects.link(eye)
    scene.camera = eye
    world = bpy.data.worlds.new('dark')
    scene.world = world
    scene.render.engine = 'CYCLES'
    scene.render.film_transparent = True
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.render.image_settings.color_mode = 'RGBA'
    scene.view_settings.view_transform = 'Raw'
    cycles = scene.cycles
    cycles.device = 'CPU'
    cycles.samples = args.samples
    cycles.use_adaptive_sampling = False
    cycles.use_denoising = False
    cycles.max_bounces = 0
    cycles.transparent_max_bounces = 256
    cycles.pixel_filter_type = 'BOX'   # coverage of a texel, not a blur across its neighbours

    arrays, report = {}, []
    for name, (centre, width, height) in frames.items():
        columns, rows = round(width * args.density), round(height * args.density)
        scene.render.resolution_x, scene.render.resolution_y = columns, rows
        camera.ortho_scale = max(width, height)
        eye.location = (centre, -10, height / 2 - MARGIN)
        for kind in ('colour', 'height'):
            choose(kind)
            scene.render.filepath = str(out / f'{name}-{kind}.exr')
            bpy.ops.render.render(write_still=True)
            image = bpy.data.images.load(scene.render.filepath)
            pixels = np.empty(columns * rows * 4, np.float32)
            image.pixels.foreach_get(pixels)
            bpy.data.images.remove(image)
            Path(scene.render.filepath).unlink()
            pixels = pixels.reshape(rows, columns, 4)[::-1]    # top row first
            alpha = pixels[..., 3:4]
            straight = pixels[..., :3] / np.maximum(alpha, 1e-4)
            if kind == 'colour':
                arrays[name] = np.concatenate([straight, alpha], axis=2).astype(np.float16)
            else:
                arrays[f'{name}_height'] = straight[..., 0].astype(np.float16)
        report.append(dict(name=name, width_m=width, height_m=height, px=[columns, rows], hang=CARDS[name]['hang'],
                           coverage=round(float(arrays[name][..., 3].astype(np.float32).mean()), 4)))
        print(f'grass_cards: {name} {columns}x{rows}', flush=True)
    np.savez(out / 'cards.npz', **arrays)
    (out / 'cards.json').write_text(json.dumps(dict(source=Path(args.source).name, density=args.density,
                                                    seed=args.seed, margin_m=MARGIN, straw=STRAW,
                                                    cards=report), indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
