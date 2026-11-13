"""Bake a tiling patch of 3D grass, seen straight down, into the arrays the fringe layer's maps are made of.

    blender -b -P grass_patch.py -- --source grass_medium_01/grass_medium_01_4k.gltf --ground sparse_grass \
        --out work/grass-layer [--size 4096] [--tile 4] [--seed 7]

The patch is a torus of `tile` metres: tufts of the scanned set are scattered by three fields that repeat with the
tile (vigour: tall clumps against a short sward; dry: straw blades; bare: spots with no tuft), every tuft near a
border is repeated on the other side, and an orthographic camera looks down on exactly one tile. Tufts lean, more
often toward the image's bottom than elsewhere: the layer is projected with texture v down a wall, so there the long
leaves hang.

Five passes, each a render of its own, written to `bake.npz` with the top image row first:
  colour  linear albedo, with straw blades (per blade, by mesh island) and a brightness of the tuft
  height  metres above the ground
  normal  unit, +X right, +Y up the image, +Z to the camera
  rough   the scan's roughness
  light   the share of a white sky a blade receives (diffuse, with bounces): the shadow between the blades
`patch.json` beside it lists what was scattered.
"""
import argparse
import json
import math
import sys
import time
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

LEAF = .45        # diffuse albedo of a blade in the light pass
THROUGH = .2      # and the share it lets through
# straw against the luma of the green blade it replaces
STRAW = (1.5, 1.28, .6)


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True, help='glTF of the tufts')
    parser.add_argument('--ground', required=True, help='folder of the ground scan (sparse_grass)')
    parser.add_argument('--out', required=True)
    parser.add_argument('--size', type=int, default=4096)
    parser.add_argument('--tile', type=float, default=4.)
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--samples', type=int, default=4, help='samples of the data passes')
    parser.add_argument('--light-samples', type=int, default=16)
    parser.add_argument('--device', choices=('cpu', 'gpu'), default='gpu')
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:])


class Field:
    """Smooth noise that repeats with the tile, as a percentile 0..1: `field(x, y) > .9` is a tenth of the area."""

    def __init__(self, rng, tile, waves):
        vectors = np.array([(i, j) for i in range(-waves, waves + 1) for j in range(waves + 1)
                            if (i, j) != (0, 0) and not (j == 0 and i < 0)], np.float64)
        self.vectors = vectors * 2 * math.pi / tile
        self.amplitude = rng.normal(size=len(vectors)) / np.linalg.norm(vectors, axis=1) ** 1.3
        self.phase = rng.uniform(0, 2 * math.pi, len(vectors))
        axis = (np.arange(256) + .5) / 256 * tile
        self.sorted = np.sort(self.raw(*np.meshgrid(axis, axis)).ravel())

    def raw(self, x, y):
        x, y = np.asarray(x, np.float64), np.asarray(y, np.float64)
        return (self.amplitude * np.cos(x[..., None] * self.vectors[:, 0] + y[..., None] * self.vectors[:, 1]
                                        + self.phase)).sum(axis=-1)

    def __call__(self, x, y):
        return float(np.searchsorted(self.sorted, self.raw(x, y)) / len(self.sorted))


def scatter(rng, tile, tufts):
    """Tufts of the patch: dicts with kind, position, scales, lean, tint. Dart throwing on the torus."""
    vigour, dry, bare, dark = (Field(rng, tile, waves) for waves in (3, 2, 6, 2))
    placed, points, radii = [], np.zeros((0, 2)), np.zeros(0)

    def free(point, radius, overlap):
        if not len(points):
            return True
        delta = np.abs(points - point)
        delta = np.minimum(delta, tile - delta)
        return bool(np.all(np.hypot(delta[:, 0], delta[:, 1]) > overlap * (radii + radius)))

    def lean(low, high, share_far, down=.6):
        angle = rng.uniform(math.radians(45), math.radians(68)) if rng.random() < share_far else \
            rng.uniform(math.radians(low), math.radians(high))
        # more often toward -Y, the bottom of the image
        direction = rng.normal(-math.pi / 2, math.radians(35)) if rng.random() < down else rng.uniform(0, 2 * math.pi)
        return angle, direction

    def add(kind, point, scale, stretch, tilt, dryness, brightness):
        placed.append(dict(kind=kind, x=float(point[0]), y=float(point[1]), scale=float(scale),
                           stretch=float(stretch), yaw=float(rng.uniform(0, 2 * math.pi)), lean=float(tilt[0]),
                           lean_to=float(tilt[1]), dry=float(dryness), brightness=float(brightness)))

    def tint(point, straw_share=.025):
        d = dry(*point)
        dryness = .07 + .3 * min(max((d - .72) / .25, 0.), 1.)   # every fourteenth blade anywhere, a third in a dry patch
        if rng.random() < straw_share:
            dryness = rng.uniform(.35, .7)
        brightness = (.62 + .5 * dark(*point)) * rng.uniform(.85, 1.15)
        return dryness, brightness

    names = {key: sorted(n for n in tufts if f'_{key}_' in n) for key in ('large', 'mid', 'small', 'tall', 'tiny')}
    for _ in range(int(7500 * tile * tile)):
        point = rng.uniform(0, tile, 2)
        v, b = vigour(*point), bare(*point)
        if b > .965:     # a few bare spots, 3-4 % of the tile
            continue
        edge = b > .92   # their rims: short and thin
        if v > .6 and not edge:        # tall clumps with long leaves that lie down the image
            kind = rng.choice(names['large'] + names['mid'][:1])
            scale, stretch = rng.uniform(1.5, 2.3) * (.8 + .5 * v), rng.uniform(1.3, 2.)
            tilt, overlap = lean(15, 45, .2, down=.75), .34
        elif v > .25 and not edge:
            kind = rng.choice(names['large'] + names['mid'] * 2 + names['small'])
            scale, stretch = rng.uniform(1.05, 1.7), rng.uniform(1., 1.5)
            tilt, overlap = lean(8, 32, .1), .38
        else:                          # short sward
            kind = rng.choice(names['small'] * 2 + names['mid'])
            scale, stretch = rng.uniform(.75, 1.2), rng.uniform(.7, 1.1)
            tilt, overlap = lean(0, 22, .04), .34
        radius = tufts[kind] * scale
        if not free(point, radius, overlap):
            continue
        points, radii = np.vstack([points, point]), np.append(radii, radius)
        add(kind, point, scale, stretch, tilt, *tint(point))
    for _ in range(int(55 * tile * tile / 16)):        # seed stems: straw, half of them lying
        point = rng.uniform(0, tile, 2)
        if bare(*point) > .965:
            continue
        lying = rng.random() < .5
        tilt = lean(50, 78, 0.) if lying else lean(5, 30, 0.)
        add(rng.choice(names['tall']), point, rng.uniform(1., 1.6), 1., tilt, 1., rng.uniform(.7, 1.))
    for _ in range(int(1500 * tile * tile / 16)):       # single leaves between the tufts
        point = rng.uniform(0, tile, 2)
        if bare(*point) > .975:
            continue
        add(rng.choice(names['tiny']), point, rng.uniform(1., 2.2), 1., lean(0, 30, .1), *tint(point, .08))
    return placed


def link(tree, source, target):
    tree.links.new(source, target)


def grass_material(material, alpha: Path):
    """Rewire the imported material: one emission (or diffuse) surface whose colour is chosen by `passes`.
    The glTF names the colour JPEG alone; the cut-out of the seed heads and leaf tips lies beside it, in `alpha`."""
    tree = material.node_tree
    nodes = tree.nodes
    colour, arm = nodes['Image Texture'], nodes['Image Texture.001']
    normal_map = nodes['Normal Map']
    opacity = nodes.new('ShaderNodeTexImage')
    opacity.image = bpy.data.images.load(str(alpha))
    opacity.image.colorspace_settings.name = 'Non-Color'
    for wire in colour.inputs['Vector'].links:
        tree.links.new(wire.from_socket, opacity.inputs['Vector'])
    for node in ('Principled BSDF', 'Math', 'Separate Color'):
        nodes.remove(nodes[node])

    def math_node(operation, a=None, b=None):
        node = nodes.new('ShaderNodeMath')
        node.operation = operation
        for socket, value in zip(node.inputs, (a, b)):
            if value is None:
                continue
            if isinstance(value, (int, float)):
                socket.default_value = value
            else:
                link(tree, value, socket)
        return node.outputs[0]

    def vector_node(operation, a, b=None, scale=None):
        node = nodes.new('ShaderNodeVectorMath')
        node.operation = operation
        for socket, value in zip(node.inputs, (a, b)):
            if value is None:
                continue
            if isinstance(value, tuple):
                socket.default_value = value
            else:
                link(tree, value, socket)
        if isinstance(scale, float):
            node.inputs['Scale'].default_value = scale
        elif scale is not None:
            link(tree, scale, node.inputs['Scale'])
        return node.outputs[0]

    geometry, info = nodes.new('ShaderNodeNewGeometry'), nodes.new('ShaderNodeObjectInfo')
    tuft = nodes.new('ShaderNodeSeparateColor')     # object colour: brightness, dryness of the tuft
    link(tree, info.outputs['Color'], tuft.inputs[0])
    island = geometry.outputs['Random Per Island']
    # a blade is straw when its random falls under the tuft's dryness
    straw_blade = math_node('LESS_THAN', island, tuft.outputs[1])
    luma = nodes.new('ShaderNodeRGBToBW')
    link(tree, colour.outputs['Color'], luma.inputs[0])
    straw = vector_node('SCALE', STRAW, scale=luma.outputs[0])
    mixed = nodes.new('ShaderNodeMix')
    mixed.data_type = 'RGBA'
    link(tree, straw_blade, mixed.inputs[0])              # the sockets of the colour mix: factor 0, A 6, B 7
    link(tree, colour.outputs['Color'], mixed.inputs[6])
    link(tree, straw, mixed.inputs[7])
    # the blade's own brightness, .82..1.18, from the same random spread wider
    blade = math_node('ADD', math_node('MULTIPLY', math_node('FRACT', math_node('MULTIPLY', island, 7.31)), .36), .82)
    tinted = vector_node('SCALE', mixed.outputs[2], scale=math_node('MULTIPLY', blade, tuft.outputs[0]))

    position = nodes.new('ShaderNodeSeparateXYZ')
    link(tree, geometry.outputs['Position'], position.inputs[0])
    height = nodes.new('ShaderNodeCombineXYZ')
    for socket in height.inputs:
        link(tree, position.outputs['Z'], socket)

    # seen from above either side of a blade may face the camera: turn the normal up
    shading = nodes.new('ShaderNodeSeparateXYZ')
    link(tree, normal_map.outputs['Normal'], shading.inputs[0])
    up = vector_node('SCALE', normal_map.outputs['Normal'], scale=math_node('SIGN', shading.outputs['Z']))
    packed = vector_node('ADD', vector_node('SCALE', up, scale=.5), (.5, .5, .5))

    rough = nodes.new('ShaderNodeSeparateColor')
    link(tree, arm.outputs['Color'], rough.inputs[0])
    roughness = nodes.new('ShaderNodeCombineXYZ')
    for socket in roughness.inputs:
        link(tree, rough.outputs[1], socket)

    emission, transparent = nodes.new('ShaderNodeEmission'), nodes.new('ShaderNodeBsdfTransparent')
    diffuse, through = nodes.new('ShaderNodeBsdfDiffuse'), nodes.new('ShaderNodeBsdfTranslucent')
    diffuse.inputs['Color'].default_value = (LEAF,) * 3 + (1,)
    through.inputs['Color'].default_value = (THROUGH,) * 3 + (1,)
    leaf = nodes.new('ShaderNodeAddShader')
    link(tree, diffuse.outputs[0], leaf.inputs[0])
    link(tree, through.outputs[0], leaf.inputs[1])
    cut = nodes.new('ShaderNodeMixShader')
    link(tree, math_node('GREATER_THAN', opacity.outputs['Color'], .5), cut.inputs[0])
    link(tree, transparent.outputs[0], cut.inputs[1])
    link(tree, cut.outputs[0], nodes['Material Output'].inputs['Surface'])
    sources = dict(colour=tinted, height=height.outputs[0], normal=packed, rough=roughness.outputs[0])

    def choose(name):
        if name == 'light':
            link(tree, leaf.outputs[0], cut.inputs[2])
        else:
            link(tree, sources[name], emission.inputs['Color'])
            link(tree, emission.outputs[0], cut.inputs[2])
    return choose


def ground_material(folder: Path, tile_m: float, relief_m: float, shade: float):
    """The ground under the tufts: the flat grass scan at its real size, darker, with a few centimetres of relief."""
    material = bpy.data.materials.new('ground')
    material.use_nodes = True
    tree = material.node_tree
    nodes = tree.nodes
    nodes.remove(nodes['Principled BSDF'])
    geometry, mapping = nodes.new('ShaderNodeNewGeometry'), nodes.new('ShaderNodeMapping')
    mapping.inputs['Scale'].default_value = (1 / tile_m,) * 3
    link(tree, geometry.outputs['Position'], mapping.inputs['Vector'])

    def texture(suffix, data):
        node = nodes.new('ShaderNodeTexImage')
        node.image = bpy.data.images.load(str(folder / f'{folder.name}_{suffix}'))
        if data:
            node.image.colorspace_settings.name = 'Non-Color'
        link(tree, mapping.outputs[0], node.inputs['Vector'])
        return node.outputs['Color']

    def scaled(colour, factor):
        node = nodes.new('ShaderNodeVectorMath')
        node.operation = 'SCALE'
        link(tree, colour, node.inputs[0])
        node.inputs['Scale'].default_value = factor
        return node.outputs[0]

    rough = nodes.new('ShaderNodeSeparateColor')
    link(tree, texture('arm_4k.jpg', True), rough.inputs[0])
    roughness = nodes.new('ShaderNodeCombineXYZ')
    for socket in roughness.inputs:
        link(tree, rough.outputs[1], socket)
    # the plane lies in XY with the image's axes: the scan's normal map is already the pass
    sources = dict(colour=scaled(texture('diff_4k.jpg', False), shade), height=scaled(texture('disp_4k.png', True), relief_m),
                   normal=texture('nor_gl_4k.png', True), rough=roughness.outputs[0])
    emission, diffuse = nodes.new('ShaderNodeEmission'), nodes.new('ShaderNodeBsdfDiffuse')
    diffuse.inputs['Color'].default_value = (LEAF,) * 3 + (1,)
    surface = nodes['Material Output'].inputs['Surface']

    def choose(name):
        if name == 'light':
            link(tree, diffuse.outputs[0], surface)
        else:
            link(tree, sources[name], emission.inputs['Color'])
            link(tree, emission.outputs[0], surface)
    return material, choose


def main():
    args = arguments()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    tile = args.tile
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=args.source)
    scene = bpy.context.scene
    sources = {o.name: o for o in scene.objects if o.type == 'MESH'}
    tufts = {name: max(o.dimensions.x, o.dimensions.y) / 2 for name, o in sources.items()}
    source = Path(args.source)
    choose_grass = grass_material(next(iter(sources.values())).data.materials[0],
                                  source.parent / 'textures' / source.name.replace('.gltf', '.png').replace('_4k', '_alpha_4k'))

    rng = np.random.default_rng(args.seed)
    placed = scatter(rng, tile, tufts)
    patch = bpy.data.collections.new('patch')
    scene.collection.children.link(patch)
    copies = 0
    for tuft in placed:
        source = sources[tuft['kind']]
        to = tuft['lean_to']
        matrix = (Matrix.Rotation(tuft['lean'], 4, Vector((-math.sin(to), math.cos(to), 0)))
                  @ Matrix.Rotation(tuft['yaw'], 4, 'Z')
                  @ Matrix.Diagonal((tuft['scale'], tuft['scale'], tuft['scale'] * tuft['stretch'], 1)))
        reach = tufts[tuft['kind']] * tuft['scale'] + source.dimensions.z * tuft['scale'] * tuft['stretch'] + .3
        for dx in (-tile, 0, tile):
            for dy in (-tile, 0, tile):
                x, y = tuft['x'] + dx, tuft['y'] + dy
                if not (-reach < x < tile + reach and -reach < y < tile + reach):
                    continue
                copy = bpy.data.objects.new(source.name, source.data)
                copy.matrix_world = Matrix.Translation((x, y, 0)) @ matrix
                copy.color = (tuft['brightness'], tuft['dry'], 0, 1)
                patch.objects.link(copy)
                copies += 1
    for source in sources.values():   # the originals lie in a row along X: keep only the copies
        bpy.data.objects.remove(source)

    ground_tile = 2.   # the scan's real size
    ground, choose_ground = ground_material(Path(args.ground), ground_tile, relief_m=.03, shade=.55)
    mesh = bpy.data.meshes.new('ground')
    low, high = -tile, 2 * tile
    mesh.from_pydata([(low, low, 0), (high, low, 0), (high, high, 0), (low, high, 0)], [], [(0, 1, 2, 3)])
    mesh.materials.append(ground)
    patch.objects.link(bpy.data.objects.new('ground', mesh))

    camera = bpy.data.cameras.new('down')
    camera.type, camera.ortho_scale, camera.clip_end = 'ORTHO', tile, 50
    eye = bpy.data.objects.new('down', camera)
    eye.location = (tile / 2, tile / 2, 10)
    scene.collection.objects.link(eye)
    scene.camera = eye

    world = bpy.data.worlds.new('sky')
    world.use_nodes = True
    scene.world = world
    sky = world.node_tree.nodes['Background'].inputs
    scene.render.engine = 'CYCLES'
    scene.render.resolution_x = scene.render.resolution_y = args.size
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = 'OPEN_EXR'
    scene.render.image_settings.color_depth = '32'
    scene.render.image_settings.color_mode = 'RGB'
    scene.view_settings.view_transform = 'Raw'
    cycles = scene.cycles
    cycles.use_adaptive_sampling = False
    cycles.transparent_max_bounces = 256
    cycles.device = 'CPU'
    if args.device == 'gpu':
        preferences = bpy.context.preferences.addons['cycles'].preferences
        for kind in ('METAL', 'OPTIX', 'CUDA'):
            try:
                preferences.compute_device_type = kind
            except TypeError:
                continue
            preferences.refresh_devices()
            devices = [d for d in preferences.devices if d.type == kind]
            if devices:
                for device in preferences.devices:
                    device.use = device.type == kind
                cycles.device = 'GPU'
                break

    arrays, seconds = {}, {}
    for name in ('colour', 'height', 'normal', 'rough', 'light'):
        choose_grass(name)
        choose_ground(name)
        lit = name == 'light'
        sky[0].default_value = (1, 1, 1, 1) if lit else (0, 0, 0, 1)
        cycles.samples = args.light_samples if lit else args.samples
        cycles.max_bounces = cycles.diffuse_bounces = 3 if lit else 0
        cycles.transmission_bounces = 3 if lit else 0
        cycles.use_denoising = lit
        scene.render.filepath = str(out / f'{name}.exr')
        started = time.monotonic()
        bpy.ops.render.render(write_still=True)
        seconds[name] = round(time.monotonic() - started, 1)
        print(f'grass_patch: {name} {seconds[name]} s', flush=True)
        image = bpy.data.images.load(scene.render.filepath)
        pixels = np.empty(args.size * args.size * 4, np.float32)
        image.pixels.foreach_get(pixels)
        bpy.data.images.remove(image)
        pixels = pixels.reshape(args.size, args.size, 4)[::-1, :, :3]   # top row first
        if name == 'normal':
            pixels = pixels * 2 - 1
            pixels = pixels / np.maximum(np.linalg.norm(pixels, axis=2, keepdims=True), 1e-6)
        elif name == 'light':
            pixels = pixels[..., 0] / LEAF
        elif name != 'colour':
            pixels = pixels[..., 0]
        arrays[name] = pixels.astype(np.float16)
        Path(scene.render.filepath).unlink()
    np.savez(out / 'bake.npz', **arrays)
    kinds = {}
    for tuft in placed:
        key = tuft['kind'].split('_')[-3]
        kinds[key] = kinds.get(key, 0) + 1
    report = dict(source=Path(args.source).name, ground=Path(args.ground).name, tile_m=tile, size=args.size,
                  seed=args.seed, tufts=len(placed), kinds=kinds, objects=copies,
                  samples=args.samples, light_samples=args.light_samples, device=cycles.device, seconds=seconds,
                  leaf=LEAF, through=THROUGH, straw=STRAW)
    (out / 'patch.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
