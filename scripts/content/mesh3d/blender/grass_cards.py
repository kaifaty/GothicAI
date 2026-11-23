"""Render side views of fans of single grass blades: the pictures on the cards of the tufts along the rock edges.

    blender -b -P grass_cards.py -- --source grass_medium_01/grass_medium_01_4k.gltf --out work/grass-tufts \
        [--density 1024] [--seed 11]

Step 2 of reports/engine/2026-11-13-grass/README.md. A card is a flat picture of a row of blades seen from the
side by an orthographic camera. The blades are the loose parts of the scanned tufts (a ribbon of some 24 vertices,
9 cm long), each taken alone and scaled whole to a leaf of 20-95 cm, so its width grows with it to the 1.5-3.4 cm
of a stroke of the original's painted grass. Whole tufts stretched to that length do not work: their hundreds of
blades stay 5 mm wide, merge into an opaque mass at the root and dissolve under the engine's hashed alpha further
down (round 1 of the step). A blade faces the camera, turned about its own axis by up to BLADE_TURN; the blades
at the back of the row are darker, the depth a flat card has no other way to show. A hanging card is the same
picture turned upside down by the builder (landscape_grass_tufts.py): leaves out of a root line at the top.

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

# name: width and height of the picture in metres; blades: how many; leaf: their lengths as shares of the height,
# shortest and longest; root: half width of the row of roots, m; fan: lean of the outermost blade, degrees;
# hang: the builder turns the picture upside down.
CARDS = dict(
    hang_long_a=dict(size=(.375, 1.), blades=22, leaf=(.45, .97), root=.14, fan=7, hang=True),
    hang_long_b=dict(size=(.375, 1.), blades=14, leaf=(.4, .97), root=.12, fan=9, hang=True),
    hang_mid_a=dict(size=(.375, .7), blades=22, leaf=(.45, .96), root=.14, fan=8, hang=True),
    hang_mid_b=dict(size=(.375, .7), blades=14, leaf=(.4, .96), root=.12, fan=10, hang=True),
    droop_a=dict(size=(.5, .5), blades=26, leaf=(.45, .95), root=.19, fan=20, hang=True),
    droop_b=dict(size=(.5, .5), blades=18, leaf=(.4, .95), root=.17, fan=22, hang=True),
    droop_thin=dict(size=(.5, .5), blades=10, leaf=(.4, .9), root=.14, fan=24, hang=True),
    clump_a=dict(size=(.5, .5), blades=30, leaf=(.45, .93), root=.19, fan=26, hang=False),
    clump_b=dict(size=(.5, .5), blades=20, leaf=(.4, .93), root=.17, fan=28, hang=False),
    clump_low=dict(size=(.5, .5), blades=26, leaf=(.3, .6), root=.2, fan=30, hang=False),
    clump_thin=dict(size=(.5, .5), blades=12, leaf=(.4, .9), root=.13, fan=28, hang=False),
)
KINDS = ('large', 'mid')  # the tufts whose blades are taken
MARGIN = .012             # empty rim of a picture, m: a card's neighbour in the atlas does not leak into its mips
STRAW = .05               # share of straw blades: the layer's bake has 7 %, and its lit straw reads yellow in the game
WIDTH = (.015, .034)      # a blade's ribbon across, m, at any length
BLADE_TURN = 35.          # a blade's turn about its own axis, degrees either way
BACK = .6                 # brightness of the blade furthest from the camera; the nearest has FRONT
FRONT = 1.1


def blades(tuft):
    """Loose parts of a tuft's mesh as single blades: vertices with the root at 0, the chord along +Z at length 1
    and the ribbon's face toward -Y; faces; UV per face corner; the ribbon's width as a share of the length."""
    mesh = tuft.data
    count = len(mesh.vertices)
    place = np.empty(count * 3, np.float32)
    mesh.vertices.foreach_get('co', place)
    place = place.reshape(count, 3).astype(np.float64)
    corner = np.empty(len(mesh.loops), np.int32)
    mesh.loops.foreach_get('vertex_index', corner)
    start, total = np.empty(len(mesh.polygons), np.int32), np.empty(len(mesh.polygons), np.int32)
    mesh.polygons.foreach_get('loop_start', start)
    mesh.polygons.foreach_get('loop_total', total)
    facing = np.empty(len(mesh.polygons) * 3, np.float32)
    mesh.polygons.foreach_get('normal', facing)
    facing = facing.reshape(-1, 3).astype(np.float64)
    uv = np.empty(len(mesh.loops) * 2, np.float32)
    mesh.uv_layers[0].uv.foreach_get('vector', uv)
    uv = uv.reshape(-1, 2)

    parent = list(range(count))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for first, length in zip(start, total):
        a = find(int(corner[first]))
        for other in corner[first + 1:first + length]:
            parent[find(int(other))] = a
    label = np.array([find(i) for i in range(count)])
    of_polygon = label[corner[start]]
    for island in np.unique(label):
        members = np.flatnonzero(label == island)
        polygons = np.flatnonzero(of_polygon == island)
        if len(members) < 12:
            continue
        points = place[members]
        root = points[np.argsort(points[:, 2])[:2]].mean(axis=0)
        if root[2] > .03:
            continue
        chord = points[np.argmax(np.linalg.norm(points - root, axis=1))] - root
        length = np.linalg.norm(chord)
        if length < .06:
            continue
        chord /= length
        normals = facing[polygons]
        normal = (normals * np.where(normals @ normals[0] < 0, -1., 1.)[:, None]).sum(axis=0)
        normal -= chord * (normal @ chord)
        if np.linalg.norm(normal) < 1e-6:
            continue
        normal /= np.linalg.norm(normal)
        across = np.cross(normal, chord)
        local = (points - root) @ np.stack([across, -normal, chord], axis=1) / length
        index = {int(member): number for number, member in enumerate(members)}
        faces = [[index[int(c)] for c in corner[first:first + n]] for first, n in zip(start[polygons], total[polygons])]
        corners = np.concatenate([uv[first:first + n] for first, n in zip(start[polygons], total[polygons])])
        area = sum(np.linalg.norm(np.cross(local[face[k]] - local[face[0]], local[face[k + 1]] - local[face[0]])) / 2
                   for face in faces for k in range(1, len(face) - 1))
        yield dict(place=local, faces=faces, uv=corners, width=float(area))     # of a ribbon of length 1


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

    rng = np.random.default_rng(args.seed)
    stage = bpy.data.collections.new('cards')
    scene.collection.children.link(stage)
    material = next(iter(sources.values())).data.materials[0]
    library = [blade for name in sorted(sources) if any(f'_{kind}_' in name for kind in KINDS)
               for blade in blades(sources[name])]
    widths = np.array([blade['width'] for blade in library])
    print(f'grass_cards: {len(library)} blades, width {np.percentile(widths, (10, 50, 90)).round(3)} of the length',
          flush=True)
    frames = {}
    for number, (name, card) in enumerate(CARDS.items()):
        width, height = card['size']
        centre = number * 4.    # every card has its own place along X
        count = card['blades']
        depth = rng.permutation(count) / max(count - 1, 1)      # 0: nearest the camera
        for index in range(count):
            # the first blade takes the full length, the rest fall off to the shorter end
            share = card['leaf'][1] - (card['leaf'][1] - card['leaf'][0]) * (index / max(count - 1, 1)) ** .7
            length = (height - 2 * MARGIN) * share
            for _ in range(40):     # a blade that leaves the picture is drawn again
                blade = library[rng.integers(len(library))]
                x = card['root'] * ((index + rng.random()) / count * 2 - 1)
                lean = math.radians(card['fan'] * x / card['root'] + rng.normal(0, 3))
                wide = float(np.clip(length * blade['width'], *WIDTH)) / blade['width']
                matrix = (Matrix.Translation((centre + x, .06 * (depth[index] - .5), 0))
                          @ Matrix.Rotation(lean, 4, 'Y')
                          @ Matrix.Rotation(math.radians(rng.uniform(-BLADE_TURN, BLADE_TURN)), 4, 'Z')
                          @ Matrix.Diagonal((wide, length, length, 1)))
                placed = blade['place'] @ np.array(matrix.to_3x3()).T + (x, 0, 0)
                if np.abs(placed[:, 0]).max() < width / 2 - .008 and placed[:, 2].max() < height - 2 * MARGIN:
                    break
            mesh = bpy.data.meshes.new(f'{name}-{index}')
            mesh.from_pydata(blade['place'].tolist(), [], blade['faces'])
            mesh.uv_layers.new().uv.foreach_set('vector', blade['uv'].ravel())
            mesh.materials.append(material)
            mesh.validate()
            mesh.update()
            copy = bpy.data.objects.new(mesh.name, mesh)
            copy.matrix_world = matrix
            # brightness of the blade by its depth, the share of straw
            copy.color = (FRONT - (FRONT - BACK) * depth[index], STRAW, 0, 1)
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
                                                    width_m=WIDTH, back=BACK,
                                                    cards=report), indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
