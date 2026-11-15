"""Bake a tiling patch of the meadow's ground, seen straight down: the sward under the grass or the bare earth.

    blender -b -P meadow_patch.py -- --layer sward --tufts grass_medium_01/grass_medium_01_4k.gltf \
        --rocks rock_moss_set_01/rock_moss_set_01_4k.gltf --ground sparse_grass --out work/meadow-sward \
        [--size 4096] [--tile 4] [--seed 11]

The two layers of reports/engine/2026-11-14-meadow-ground, by the recipe of grass_patch.py (a torus of `tile`
metres, an orthographic camera on exactly one tile, a pass per render), with its materials and passes:

  sward  what lies under the blades the engine draws: the soil of the scan, on it a low growth of 2-5 cm (the
         scanned tufts at a third of their size, pressed down) and a thatch of dry stems of 3-9 cm that lie across
         it. No clump stands out of it: a feature of the tile is what the eye finds again 4 m further;
  earth  where the meadow is bare: the soil of the scan, in it pebbles of 2-6 cm sunk by a half, gravel between
         them in drifts, a few dry stems and leaves.

Six passes in `bake.npz`, top image row first: colour, height, normal, rough, light as grass_patch.py writes them,
and `kind`: red the growth, green a stone, blue a dry stem, black the soil; the maps are toned by it
(landscape_meadow_ground.py). `patch.json` lists what was scattered.
"""
import argparse
import importlib.util
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Euler, Matrix, Vector

KIND_STONE, KIND_STEM = (0, 1, 0), (0, 0, 1)


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--layer', choices=('sward', 'earth'), required=True)
    parser.add_argument('--tufts', required=True, help='glTF of the tufts')
    parser.add_argument('--rocks', required=True, help='glTF of the rocks the pebbles are made of')
    parser.add_argument('--ground', required=True, help='folder of the ground scan (sparse_grass)')
    parser.add_argument('--out', required=True)
    parser.add_argument('--size', type=int, default=4096)
    parser.add_argument('--tile', type=float, default=4.)
    parser.add_argument('--seed', type=int, default=11)
    parser.add_argument('--samples', type=int, default=4, help='samples of the data passes')
    parser.add_argument('--light-samples', type=int, default=16)
    parser.add_argument('--device', choices=('cpu', 'gpu'), default='gpu')
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:])


def grass_patch():
    path = Path(__file__).with_name('grass_patch.py')
    spec = importlib.util.spec_from_file_location('grass_patch', path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def stone_material(patch, material):
    """Rewire the rocks' material as grass_patch.grass_material does the tufts'."""
    tree = material.node_tree
    nodes = tree.nodes
    images = {}
    for node in nodes:
        if node.type == 'TEX_IMAGE':
            images['rough' if 'rough' in node.image.name else 'colour' if 'diff' in node.image.name else 'normal'] = node
    normal_map = next(node for node in nodes if node.type == 'NORMAL_MAP')
    geometry = nodes.new('ShaderNodeNewGeometry')
    position = nodes.new('ShaderNodeSeparateXYZ')
    patch.link(tree, geometry.outputs['Position'], position.inputs[0])
    height = nodes.new('ShaderNodeCombineXYZ')
    for socket in height.inputs:
        patch.link(tree, position.outputs['Z'], socket)
    packed = nodes.new('ShaderNodeVectorMath')
    packed.operation = 'MULTIPLY_ADD'
    patch.link(tree, normal_map.outputs['Normal'], packed.inputs[0])
    packed.inputs[1].default_value = packed.inputs[2].default_value = (.5, .5, .5)
    emission, diffuse = nodes.new('ShaderNodeEmission'), nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.inputs['Color'].default_value = (patch.LEAF,) * 3 + (1,)
    sources = dict(colour=images['colour'].outputs['Color'], height=height.outputs[0], normal=packed.outputs[0],
                   rough=images['rough'].outputs['Color'], kind=patch.kind_node(nodes, KIND_STONE))
    surface = next(node for node in nodes if node.type == 'OUTPUT_MATERIAL').inputs['Surface']

    def choose(name):
        if name == 'light':
            patch.link(tree, diffuse.outputs[0], surface)
        else:
            patch.link(tree, sources[name], emission.inputs['Color'])
            patch.link(tree, emission.outputs[0], surface)
    return choose


def stem_material(patch):
    """Dry stems: the colour is the stem's own, an attribute of its corners; matte."""
    material = bpy.data.materials.new('stems')
    material.use_nodes = True
    tree = material.node_tree
    nodes = tree.nodes
    nodes.remove(nodes['Principled BSDF'])
    geometry, tint = nodes.new('ShaderNodeNewGeometry'), nodes.new('ShaderNodeAttribute')
    tint.attribute_name = 'tint'
    position, facing = nodes.new('ShaderNodeSeparateXYZ'), nodes.new('ShaderNodeSeparateXYZ')
    patch.link(tree, geometry.outputs['Position'], position.inputs[0])
    patch.link(tree, geometry.outputs['Normal'], facing.inputs[0])
    height = nodes.new('ShaderNodeCombineXYZ')
    for socket in height.inputs:
        patch.link(tree, position.outputs['Z'], socket)
    sign = nodes.new('ShaderNodeMath')      # either side may face the camera
    sign.operation = 'SIGN'
    patch.link(tree, facing.outputs['Z'], sign.inputs[0])
    up = nodes.new('ShaderNodeVectorMath')
    up.operation = 'SCALE'
    patch.link(tree, geometry.outputs['Normal'], up.inputs[0])
    patch.link(tree, sign.outputs[0], up.inputs['Scale'])
    packed = nodes.new('ShaderNodeVectorMath')
    packed.operation = 'MULTIPLY_ADD'
    patch.link(tree, up.outputs[0], packed.inputs[0])
    packed.inputs[1].default_value = packed.inputs[2].default_value = (.5, .5, .5)
    rough = nodes.new('ShaderNodeRGB')
    rough.outputs[0].default_value = (.9, .9, .9, 1)
    emission, diffuse = nodes.new('ShaderNodeEmission'), nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.inputs['Color'].default_value = (patch.LEAF,) * 3 + (1,)
    sources = dict(colour=tint.outputs['Color'], height=height.outputs[0], normal=packed.outputs[0],
                   rough=rough.outputs[0], kind=patch.kind_node(nodes, KIND_STEM))
    surface = nodes['Material Output'].inputs['Surface']

    def choose(name):
        if name == 'light':
            patch.link(tree, diffuse.outputs[0], surface)
        else:
            patch.link(tree, sources[name], emission.inputs['Color'])
            patch.link(tree, emission.outputs[0], surface)
    return material, choose


# dry stems from pale straw to the grey of last year's
STEM_COLOURS = ((.46, .37, .17), (.36, .29, .14), (.30, .25, .15), (.52, .44, .24))


def stems(rng, tile, count, length, width, lift, keep, material):
    """One mesh of `count` lying stems: ribbons of three links, bent a little, each repeated across the borders.
    length, width, lift: ranges in metres; keep(x, y): the share of stems that stay at a place."""
    vertices, faces, tints = [], [], []
    for _ in range(count):
        x, y = rng.uniform(0, tile, 2)
        if rng.random() > keep(x, y):
            continue
        long, wide = rng.uniform(*length), rng.uniform(*width)
        yaw, bend = rng.uniform(0, 2 * math.pi), rng.normal(0, .35)
        base, rise = rng.uniform(*lift), rng.uniform(-.3, .3) * long
        colour = np.array(STEM_COLOURS[rng.integers(len(STEM_COLOURS))]) * rng.uniform(.75, 1.2)
        spine = []
        for link in range(4):
            t = link / 3
            angle = yaw + bend * (t - .5)
            spine.append((x + math.cos(angle) * long * (t - .5), y + math.sin(angle) * long * (t - .5),
                          max(base + rise * (t - .5) + .004 * math.sin(math.pi * t), .001),
                          -math.sin(angle), math.cos(angle), wide * (1 - .6 * t)))
        for dx in (-tile, 0, tile):
            for dy in (-tile, 0, tile):
                if not (-long < x + dx < tile + long and -long < y + dy < tile + long):
                    continue
                first = len(vertices)
                for px, py, pz, sx, sy, half in spine:
                    vertices.append((px + dx - sx * half / 2, py + dy - sy * half / 2, pz))
                    vertices.append((px + dx + sx * half / 2, py + dy + sy * half / 2, pz + half * .3))
                for link in range(3):
                    faces.append((first + 2 * link, first + 2 * link + 1, first + 2 * link + 3, first + 2 * link + 2))
                    tints.extend([colour] * 4)
    mesh = bpy.data.meshes.new('stems')
    mesh.from_pydata(vertices, [], faces)
    attribute = mesh.color_attributes.new('tint', 'FLOAT_COLOR', 'CORNER')
    attribute.data.foreach_set('color', np.hstack([np.array(tints), np.ones((len(tints), 1))]).ravel())
    mesh.materials.append(material)
    mesh.validate()
    mesh.update()
    return bpy.data.objects.new('stems', mesh), len(faces) // 3


def copies(collection, source, tile, x, y, matrix, reach, colour=None):
    """The object and its repeats across the tile's borders."""
    count = 0
    for dx in (-tile, 0, tile):
        for dy in (-tile, 0, tile):
            px, py = x + dx, y + dy
            if not (-reach < px < tile + reach and -reach < py < tile + reach):
                continue
            copy = bpy.data.objects.new(source.name, source.data)
            copy.matrix_world = Matrix.Translation((px, py, 0)) @ matrix
            if colour:
                copy.color = colour
            collection.objects.link(copy)
            count += 1
    return count


def sward(patch, rng, tile, tufts, collection, stem):
    """The low growth, pressed down, and the thatch over it."""
    thin, dry, dark = (patch.Field(rng, tile, waves) for waves in (9, 5, 7))
    kinds = {key: sorted(n for n in tufts if f'_{key}_' in n) for key in ('large', 'mid', 'small', 'tiny')}
    step, placed, objects = .03, 0, 0
    cells = int(round(tile / step))
    for i in range(cells):
        for j in range(cells):
            x, y = (i + rng.uniform(.1, .9)) * tile / cells, (j + rng.uniform(.1, .9)) * tile / cells
            sparse = thin(x, y)
            if rng.random() < .08 + .5 * max(sparse - .8, 0) / .2:     # the soil shows between the tufts
                continue
            source = tufts[rng.choice(kinds['small'] + kinds['mid'] * 2 + kinds['large'] * 2)]
            tall = rng.uniform(.022, .05)
            scale = tall / source.dimensions.z
            wide = scale * rng.uniform(1.2, 1.8)       # trodden: wider than it is tall
            to, lean = rng.uniform(0, 2 * math.pi), rng.uniform(math.radians(5), math.radians(40))
            matrix = (Matrix.Rotation(lean, 4, Vector((-math.sin(to), math.cos(to), 0)))
                      @ Matrix.Rotation(rng.uniform(0, 2 * math.pi), 4, 'Z') @ Matrix.Diagonal((wide, wide, scale, 1)))
            d = dry(x, y)
            dryness = .22 + .4 * min(max((d - .6) / .35, 0.), 1.)
            brightness = (.72 + .32 * dark(x, y)) * rng.uniform(.85, 1.15)
            objects += copies(collection, source, tile, x, y, matrix, .15, (brightness, dryness, 0, 1))
            placed += 1
    leaves = 0
    for _ in range(int(260 * tile * tile)):       # single leaves in the gaps
        x, y = rng.uniform(0, tile, 2)
        source = tufts[rng.choice(kinds['tiny'])]
        scale = rng.uniform(.5, 1.)
        to, lean = rng.uniform(0, 2 * math.pi), rng.uniform(math.radians(20), math.radians(60))
        matrix = (Matrix.Rotation(lean, 4, Vector((-math.sin(to), math.cos(to), 0)))
                  @ Matrix.Rotation(rng.uniform(0, 2 * math.pi), 4, 'Z') @ Matrix.Diagonal((scale,) * 3 + (1,)))
        objects += copies(collection, source, tile, x, y, matrix, .1, (rng.uniform(.7, 1.1), rng.uniform(.3, 1.), 0, 1))
        leaves += 1
    thatch, lying = stems(rng, tile, int(520 * tile * tile), (.03, .09), (.0016, .003), (.004, .028),
                          lambda x, y: .55 + .45 * dry(x, y), stem)
    collection.objects.link(thatch)
    return dict(tufts=placed, leaves=leaves, stems=lying, objects=objects + 1)


def earth(patch, rng, tile, tufts, rocks, collection, stem):
    """Pebbles sunk into the soil, gravel in drifts, a few dry stems and leaves."""
    drift = patch.Field(rng, tile, 4)
    placed, objects, points = dict(pebbles=0, gravel=0, leaves=0), 0, []

    def stone(x, y, size, sunk):
        source = rocks[rng.choice(sorted(rocks))]
        turn = Euler(rng.uniform(0, 2 * math.pi, 3)).to_matrix().to_4x4()
        scale = size / max(source.dimensions)
        flat = rng.uniform(.55, .85)
        matrix = Matrix.Diagonal((1, 1, flat, 1)) @ turn @ Matrix.Diagonal((scale,) * 3 + (1,))
        # the rock's origin is near its middle: lower it so that `sunk` of its height is under the soil
        tall = size * flat * .7
        return copies(collection, source, tile, x, y, Matrix.Translation((0, 0, tall * (.5 - sunk))) @ matrix, size)

    for _ in range(int(22 * tile * tile)):         # 2-6 cm, most of them small
        x, y = rng.uniform(0, tile, 2)
        size = .02 + .04 * rng.random() ** 1.8
        if any(math.hypot(min(abs(x - px), tile - abs(x - px)), min(abs(y - py), tile - abs(y - py))) < (size + ps) * .7
               for px, py, ps in points):
            continue
        points.append((x, y, size))
        objects += stone(x, y, size, rng.uniform(.4, .7))
        placed['pebbles'] += 1
    for _ in range(int(900 * tile * tile)):        # 5-18 mm, in drifts
        x, y = rng.uniform(0, tile, 2)
        if rng.random() > max(drift(x, y) - .35, .03) ** 1.6:
            continue
        objects += stone(x, y, .006 + .016 * rng.random() ** 2, rng.uniform(.3, .6))
        placed['gravel'] += 1
    kinds = sorted(n for n in tufts if '_tiny_' in n)
    for _ in range(int(14 * tile * tile)):
        x, y = rng.uniform(0, tile, 2)
        to = rng.uniform(0, 2 * math.pi)
        matrix = (Matrix.Rotation(rng.uniform(math.radians(55), math.radians(80)), 4, Vector((-math.sin(to), math.cos(to), 0)))
                  @ Matrix.Rotation(rng.uniform(0, 2 * math.pi), 4, 'Z') @ Matrix.Diagonal((rng.uniform(.6, 1.2),) * 3 + (1,)))
        objects += copies(collection, tufts[rng.choice(kinds)], tile, x, y, matrix, .1, (rng.uniform(.8, 1.1), 1., 0, 1))
        placed['leaves'] += 1
    lying, count = stems(rng, tile, int(34 * tile * tile), (.03, .09), (.0016, .003), (.002, .006), lambda x, y: 1., stem)
    collection.objects.link(lying)
    return dict(placed, stems=count, objects=objects + 1)


def main():
    args = arguments()
    patch = grass_patch()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    tile = args.tile
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene

    def imported(path):
        before = set(scene.objects)
        bpy.ops.import_scene.gltf(filepath=path)
        return {o.name: o for o in scene.objects if o not in before and o.type == 'MESH'}

    tufts, rocks = imported(args.tufts), imported(args.rocks)
    source = Path(args.tufts)
    choosers = [patch.grass_material(next(iter(tufts.values())).data.materials[0],
                                     source.parent / 'textures' / source.name.replace('.gltf', '.png').replace('_4k', '_alpha_4k')),
                stone_material(patch, next(iter(rocks.values())).data.materials[0])]
    stem, choose_stem = stem_material(patch)
    choosers.append(choose_stem)

    rng = np.random.default_rng(args.seed)
    collection = bpy.data.collections.new('patch')
    scene.collection.children.link(collection)
    if args.layer == 'sward':
        placed = sward(patch, rng, tile, tufts, collection, stem)
    else:
        placed = earth(patch, rng, tile, tufts, rocks, collection, stem)
    for source in (*tufts.values(), *rocks.values()):    # the originals lie in rows: keep only the copies
        bpy.data.objects.remove(source)

    ground, choose_ground = patch.ground_material(Path(args.ground), 2., relief_m=.03, shade=1.)   # the scan's real size
    choosers.append(choose_ground)
    mesh = bpy.data.meshes.new('ground')
    low, high = -tile, 2 * tile
    mesh.from_pydata([(low, low, 0), (high, low, 0), (high, high, 0), (low, high, 0)], [], [(0, 1, 2, 3)])
    mesh.materials.append(ground)
    collection.objects.link(bpy.data.objects.new('ground', mesh))

    sky, cycles = patch.stage(scene, tile, args.size, args.device)
    arrays, seconds = patch.render_passes(scene, sky, cycles, out, args.size,
                                          ('colour', 'height', 'normal', 'rough', 'kind', 'light'), choosers,
                                          args.samples, args.light_samples)
    np.savez(out / 'bake.npz', **arrays)
    report = dict(layer=args.layer, tufts=Path(args.tufts).name, rocks=Path(args.rocks).name, ground=Path(args.ground).name,
                  tile_m=tile, size=args.size, seed=args.seed, placed=placed, samples=args.samples,
                  light_samples=args.light_samples, device=cycles.device, seconds=seconds)
    (out / 'patch.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
