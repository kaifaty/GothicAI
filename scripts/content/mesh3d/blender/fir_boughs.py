"""The atlas of spruce boughs: every bough built as geometry and photographed from above.

    blender -b --factory-startup -P fir_boughs.py -- --out DIR [--config fir.json] [--seed N] [--samples 48]

Each cell of the config's atlas gets one bough laid flat: the woody axis from the stem at the middle of the cell's
left edge to its right edge, side shoots in a herringbone, their own shoots, and needles 1.5 to 2 cm long on all
of them. A bough is grown inside the outline its card is cut to (fir_tree.py), a margin inside it, so the card
loses nothing. The kinds differ in habit:

    fan      the end of a branch seen from above: wide herringbone
    hang     a spray that hangs from a branch: long shoots that turn along the axis
    side     a shorter herringbone to the side of a branch
    sparse   the shaded inside of the crown: thin, with bare and dry shoots
    twig     a small end shoot

One orthographic camera looks straight down at all cells; four renders give the passes: colour (flat, the
needle's own, no light), normal, ambient occlusion over a few centimetres and height. DIR/boughs.npz holds them
as float arrays, row 0 is the top of the atlas: colour (linear), alpha, normal (x right, y up the picture, z to
the viewer), ao, height in metres. The concept of the bough is a reference for the habit only; no picture is used.
"""
from __future__ import annotations

import argparse
import json
import math
import sys
import time
from pathlib import Path

import bpy
import numpy as np

HERE = Path(__file__).resolve().parent

# Linear albedo. Needles: dark cold olive, the texture of the original averages (33, 40, 8) in sRGB bytes and is
# yellow; ours keeps that darkness and loses the yellow. Young growth at the shoot ends is lighter.
NEEDLE_OLD = np.array((.010, .015, .009))
NEEDLE = np.array((.018, .028, .013))
NEEDLE_YOUNG = np.array((.036, .055, .024))
NEEDLE_DRY = np.array((.060, .040, .020))
TWIG = np.array((.070, .050, .036))
TWIG_YOUNG = np.array((.085, .055, .030))

HABIT = dict(
    fan=dict(nodes=(.05, .075), angle=(64, 40), curve=-10, reach=(.74, .98), missing=.06, sub=(.03, .045),
             sub_share=.36, needles=900, axis_radius=.011, dry=1),
    hang=dict(nodes=(.045, .07), angle=(46, 20), curve=-26, reach=(.6, .97), missing=.05, sub=(.03, .045),
              sub_share=.32, needles=900, axis_radius=.006, dry=0),
    side=dict(nodes=(.045, .065), angle=(60, 40), curve=-12, reach=(.7, .97), missing=.08, sub=(.03, .042),
              sub_share=.36, needles=900, axis_radius=.006, dry=0),
    sparse=dict(nodes=(.07, .11), angle=(58, 38), curve=-8, reach=(.5, .95), missing=.28, sub=(.05, .08),
                sub_share=.30, needles=520, axis_radius=.007, dry=4),
    twig=dict(nodes=(.04, .055), angle=(56, 38), curve=-10, reach=(.7, .97), missing=.06, sub=(.028, .04),
              sub_share=.36, needles=920, axis_radius=.004, dry=0))


class Outline:
    """The card's outline in the bough's own metres: x along the axis from the stem, y across."""

    def __init__(self, cell: dict, px_per_m: float):
        _, _, w, h = cell['cell']
        self.length, half = w / px_per_m, h / px_per_m / 2
        (t1, h1), (t2, h2) = cell['sections']
        top = [(0., 0.), (t1 * self.length, h1 * half), (t2 * self.length, h2 * half), (self.length, 0.)]
        self.points = np.array(top + [(x, -y) for x, y in top[-2:0:-1]])
        nxt = np.roll(self.points, -1, axis=0)
        edge = nxt - self.points
        self.normal = np.stack([edge[:, 1], -edge[:, 0]], axis=1)      # inward: the outline runs clockwise
        self.normal /= np.linalg.norm(self.normal, axis=1, keepdims=True)

    def depth(self, point) -> float:
        """How far inside the outline the point is, negative outside."""
        return float(np.min(np.einsum('ij,ij->i', np.asarray(point)[None, :2] - self.points, self.normal)))


class Bough:
    def __init__(self, kind: str, outline: Outline, rng: np.random.Generator, margin: float):
        self.habit = HABIT[kind.split('_')[0]]
        self.outline, self.rng, self.margin = outline, rng, margin
        self.shoots: list[dict] = []
        self.grow()

    def shoot(self, start, angle: float, length: float, curve: float, order: int, z: float, dry: bool = False,
              wobble: float = .08) -> dict | None:
        """A polyline that turns by `curve` over its length and stops at the outline."""
        rng = self.rng
        steps = max(2, int(math.ceil(length / .02)))
        step = length / steps
        phase, wave = rng.uniform(0, 6.28), rng.uniform(1.5, 4.)
        points = [np.array([start[0], start[1], z])]
        for i in range(steps):
            t = (i + .5) / steps
            a = angle + curve * t + wobble * math.sin(phase + wave * t * 3.)
            nxt = points[-1] + np.array([math.cos(a), math.sin(a), -.06 * (order > 0)]) * step
            if self.outline.depth(nxt) < (self.margin if order else .002) or nxt[0] > self.outline.length - .02:
                break
            points.append(nxt)
        if len(points) < 3:
            return None
        shoot = dict(points=np.array(points), order=order, dry=dry, length=step * (len(points) - 1))
        self.shoots.append(shoot)
        return shoot

    def children(self, parent: dict, spacing, angle, curve: float, reach, order: int, share: float,
                 start: float = .1, missing: float = 0.) -> list[dict]:
        rng, points = self.rng, parent['points']
        along = np.concatenate([[0.], np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))])
        total, out = along[-1], []
        s, side = total * start + rng.uniform(0, spacing[0]), rng.choice([-1., 1.])
        while s < total * .97:
            i = int(np.clip(np.searchsorted(along, s) - 1, 0, len(points) - 2))
            base = points[i] + (points[i + 1] - points[i]) * (s - along[i]) / max(along[i + 1] - along[i], 1e-9)
            heading = math.atan2(*(points[i + 1] - points[i])[1::-1])
            t = s / total
            for turn in ((side, -side) if order == 1 else (side,)):
                if rng.uniform() < missing:
                    continue
                a = math.radians(angle[0] + (angle[1] - angle[0]) * t + rng.uniform(-8, 8))
                if order == 1:
                    length = self.room(base, heading + turn * a) * rng.uniform(*reach)
                    if rng.uniform() < .1:
                        length *= rng.uniform(.35, .6)
                else:
                    length = parent['length'] * share * (1 - .65 * t) * rng.uniform(.6, 1.15)
                if length < .03:
                    continue
                child = self.shoot(base, heading + turn * a, length, turn * math.radians(curve) * rng.uniform(.5, 1.3),
                                   order, base[2] - .004 * order + rng.uniform(-.003, .003), parent['dry'])
                if child:
                    out.append(child)
            s += rng.uniform(*spacing)
            side = -side
        return out

    def room(self, start, angle: float) -> float:
        """The length of a straight shoot from here to the outline."""
        direction = np.array([math.cos(angle), math.sin(angle)])
        low, high = 0., self.outline.length
        for _ in range(18):
            mid = (low + high) / 2
            if self.outline.depth(np.asarray(start)[:2] + direction * mid) >= self.margin:
                low = mid
            else:
                high = mid
        return low

    def grow(self) -> None:
        h, rng = self.habit, self.rng
        axis = self.shoot((.004, 0.), 0., self.outline.length, rng.uniform(-.04, .04), 0, .03, wobble=.04)
        first = self.children(axis, h['nodes'], h['angle'], h['curve'], h['reach'], 1, 0., start=.05,
                              missing=h['missing'])
        for index in rng.permutation(len(first))[:h['dry']]:
            first[index]['dry'] = True
        for shoot in first:
            if shoot['length'] < .1:
                continue
            for second in self.children(shoot, h['sub'], (50, 36), h['curve'] * .5, None, 2, h['sub_share'], start=.12):
                if second['length'] > .07 and not second['dry']:
                    self.children(second, (.025, .036), (48, 38), 0., None, 3, .4, start=.2)

    def geometry(self) -> dict:
        """Twigs as thin tubes and needles as flat spindles: position, colour, triangles."""
        h, rng = self.habit, self.rng
        position, colour, index, count = [], [], [], 0

        def push(p, c, tri):
            nonlocal count
            position.append(p)
            colour.append(np.broadcast_to(c, (len(p), 3)) if np.ndim(c) == 1 else c)
            index.append(np.asarray(tri) + count)
            count += len(p)

        for shoot in self.shoots:
            points, order = shoot['points'], shoot['order']
            n = len(points)
            share = np.linspace(0, 1, n)
            if order == 0:
                radius = h['axis_radius'] * (1 - share) ** .8 + .0022
            else:
                radius = (.0026 if order == 1 else .0016 if order == 2 else .0012) * (1 - .6 * share) \
                    * (1 + 1.2 * shoot['length'] * (order == 1))
            tangent = np.gradient(points, axis=0)
            tangent /= np.linalg.norm(tangent, axis=1, keepdims=True)
            across = np.cross(tangent, (0, 0, 1.))
            across /= np.linalg.norm(across, axis=1, keepdims=True)
            up = np.cross(across, tangent)
            angles = np.linspace(0, 2 * math.pi, 5, endpoint=False)
            ring = (np.cos(angles)[None, :, None] * across[:, None] + np.sin(angles)[None, :, None] * up[:, None])
            tube = points[:, None] + radius[:, None, None] * ring
            tris = []
            for i in range(n - 1):
                for k in range(5):
                    a, b = i * 5 + k, i * 5 + (k + 1) % 5
                    tris += [(a, b, b + 5), (a, b + 5, a + 5)]
            tone = (TWIG if order < 2 else TWIG_YOUNG) * rng.uniform(.8, 1.15)
            if shoot['dry']:
                tone = np.array((.085, .075, .065)) * rng.uniform(.8, 1.1)
            push(tube.reshape(-1, 3), tone, tris)
            if shoot['dry'] and rng.uniform() < .7:
                continue
            # needles
            lengths = np.concatenate([[0.], np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))])
            total = lengths[-1]
            start = total * (.3 if order == 0 else .04)
            density = h['needles'] * (.35 if shoot['dry'] else 1.)
            m = int((total - start) * density)
            if m <= 0:
                continue
            s = np.sort(rng.uniform(start, total + .004, m))
            i = np.clip(np.searchsorted(lengths, s) - 1, 0, n - 2)
            w = ((s - lengths[i]) / np.maximum(lengths[i + 1] - lengths[i], 1e-9))[:, None]
            base = points[i] * (1 - w) + points[np.minimum(i + 1, n - 1)] * w
            heading = np.arctan2(tangent[i, 1], tangent[i, 0])
            top = rng.uniform(size=m) < .3
            side = np.where(rng.uniform(size=m) < .5, -1., 1.)
            spread = np.where(top, rng.uniform(0, 28, m), rng.uniform(42, 74, m))
            spread = np.where(s > total, rng.uniform(0, 40, m), spread)        # the bud's brush at the very end
            a = heading + side * np.radians(spread)
            lift = np.where(top, rng.uniform(.25, .7, m), rng.uniform(-.25, .3, m))
            direction = np.stack([np.cos(a), np.sin(a), lift], axis=1)
            direction /= np.linalg.norm(direction, axis=1, keepdims=True)
            age = np.clip((s - start) / max(total - start, 1e-6), 0, 1)        # 1 at the shoot's end
            long = rng.uniform(.016, .022, m) * np.where(age > .9, .8, 1.) * (1.1 if order == 0 else 1.)
            wide = rng.uniform(.0022, .0028, m)
            roll = np.radians(rng.uniform(-40, 40, m))
            flat = np.cross(direction, (0, 0, 1.))
            flat /= np.linalg.norm(flat, axis=1, keepdims=True)
            rise = np.cross(flat, direction)
            edge = (flat * np.cos(roll)[:, None] + rise * np.sin(roll)[:, None]) * (wide / 2)[:, None]
            base = base + np.array([0, 0, .002]) + rise * (radius[i] * .6)[:, None]
            mid, tip = base + direction * (long * .45)[:, None], base + direction * long[:, None]
            quads = np.stack([base, mid - edge, tip, mid + edge], axis=1)
            young = np.clip((age - .55) / .45, 0, 1) * (order >= 2) + np.clip((age - .8) / .2, 0, 1) * (order < 2)
            young = np.clip(young * rng.uniform(.5, 1., m), 0, 1)[:, None]
            old = np.clip(1.2 - age * 2.2, 0, 1)[:, None] * rng.uniform(.3, 1., (m, 1)) * (order < 3)
            tone = NEEDLE * (1 - young) + NEEDLE_YOUNG * young
            tone = tone * (1 - old) + NEEDLE_OLD * old
            tone = tone * rng.uniform(.78, 1.22, (m, 1)) * (1 + rng.uniform(-.06, .06, (m, 3)))
            if shoot['dry']:
                tone = NEEDLE_DRY * rng.uniform(.6, 1.1, (m, 1))
            tris = np.array([(0, 1, 2), (0, 2, 3)])[None] + (np.arange(m) * 4)[:, None, None]
            push(quads.reshape(-1, 3), np.repeat(tone, 4, axis=0), tris.reshape(-1, 3))
        return dict(position=np.concatenate(position), colour=np.concatenate(colour), index=np.concatenate(index),
                    shoots=len(self.shoots), twig_m=round(float(sum(s['length'] for s in self.shoots)), 2))


def emission(name: str, build) -> bpy.types.Material:
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    out, glow = nodes.new('ShaderNodeOutputMaterial'), nodes.new('ShaderNodeEmission')
    links.new(build(nodes, links), glow.inputs['Color'])
    links.new(glow.outputs[0], out.inputs['Surface'])
    return mat


def passes(height_range: tuple[float, float], ao_distance: float) -> dict:
    """The four materials of the bake: colour, normal, occlusion, height."""
    def colour(nodes, links):
        node = nodes.new('ShaderNodeAttribute')
        node.attribute_name = 'Col'
        return node.outputs['Color']

    def normal(nodes, links):
        geometry = nodes.new('ShaderNodeNewGeometry')
        half = nodes.new('ShaderNodeVectorMath')
        half.operation = 'MULTIPLY_ADD'
        half.inputs[1].default_value = half.inputs[2].default_value = (.5, .5, .5)
        links.new(geometry.outputs['Normal'], half.inputs[0])
        return half.outputs[0]

    def occlusion(nodes, links):
        node = nodes.new('ShaderNodeAmbientOcclusion')
        node.samples = 16
        node.inputs['Distance'].default_value = ao_distance
        return node.outputs['AO']

    def height(nodes, links):
        geometry = nodes.new('ShaderNodeNewGeometry')
        split = nodes.new('ShaderNodeSeparateXYZ')
        links.new(geometry.outputs['Position'], split.inputs[0])
        scale = nodes.new('ShaderNodeMapRange')
        scale.inputs['From Min'].default_value, scale.inputs['From Max'].default_value = height_range
        links.new(split.outputs['Z'], scale.inputs['Value'])
        return scale.outputs['Result']

    return dict(colour=emission('colour', colour), normal=emission('normal', normal),
                ao=emission('ao', occlusion), height=emission('height', height))


def bake(obj, size: int, width_m: float, height_m: float, out: Path, samples: int, height_range, ao_distance=.03,
         transparent: bool = True) -> dict:
    """Photograph the object from above: x 0..width_m to the right, y 0..-height_m down the picture."""
    sc = bpy.context.scene
    data = bpy.data.cameras.new('top')
    data.type = 'ORTHO'
    data.ortho_scale = max(width_m, height_m)
    data.clip_start, data.clip_end = .1, 20
    cam = bpy.data.objects.new('top', data)
    cam.location = (width_m / 2, -height_m / 2, 10)
    sc.collection.objects.link(cam)
    sc.camera = cam
    sc.render.engine = 'CYCLES'
    sc.cycles.samples = samples
    sc.cycles.use_denoising = False
    sc.cycles.max_bounces = 0
    sc.cycles.pixel_filter_type = 'BLACKMAN_HARRIS'
    sc.cycles.filter_width = 1.5
    sc.render.film_transparent = transparent
    sc.render.resolution_x = sc.render.resolution_y = size
    sc.render.pixel_aspect_x, sc.render.pixel_aspect_y = (height_m / width_m, 1.) if height_m > width_m \
        else (1., width_m / height_m)
    sc.render.resolution_percentage = 100
    sc.view_settings.view_transform = 'Raw'
    sc.render.image_settings.file_format = 'OPEN_EXR'
    sc.render.image_settings.color_mode = 'RGBA'
    sc.render.image_settings.color_depth = '32'
    try:
        prefs = bpy.context.preferences.addons['cycles'].preferences
        if sys.platform == 'darwin':
            prefs.compute_device_type = 'METAL'
            prefs.get_devices()
            for device in prefs.devices:
                device.use = True
            sc.cycles.device = 'GPU'
    except Exception as error:
        print('cycles device:', error)
    result = {}
    for name, mat in passes(height_range, ao_distance).items():
        sc.cycles.samples = samples if name == 'colour' else max(8, samples // 4)
        obj.data.materials.clear()
        obj.data.materials.append(mat)
        sc.render.filepath = str(out / f'pass-{name}.exr')
        started = time.time()
        bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(sc.render.filepath)
        pixels = np.empty(size * size * 4, np.float32)
        img.pixels.foreach_get(pixels)
        result[name] = pixels.reshape(size, size, 4)[::-1].copy()
        bpy.data.images.remove(img)
        Path(sc.render.filepath).unlink()
        print(f'pass {name}: {time.time() - started:.1f} s')
    return result


def mesh(name: str, position: np.ndarray, colour: np.ndarray, index: np.ndarray):
    data = bpy.data.meshes.new(name)
    data.vertices.add(len(position))
    data.vertices.foreach_set('co', position.astype(np.float32).reshape(-1))
    data.loops.add(len(index) * 3)
    data.loops.foreach_set('vertex_index', index.astype(np.int32).reshape(-1))
    data.polygons.add(len(index))
    data.polygons.foreach_set('loop_start', np.arange(0, len(index) * 3, 3, dtype=np.int32))
    data.polygons.foreach_set('loop_total', np.full(len(index), 3, np.int32))
    attribute = data.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
    attribute.data.foreach_set('color', np.concatenate([colour, np.ones((len(colour), 1))], axis=1)
                               .astype(np.float32).reshape(-1))
    data.update(calc_edges=True)
    obj = bpy.data.objects.new(name, data)
    bpy.context.scene.collection.objects.link(obj)
    return obj


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--config', type=Path, default=HERE / 'fir.json')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seed', type=int, default=11)
    parser.add_argument('--samples', type=int, default=48)
    parser.add_argument('--blend', action='store_true', help='keep the scene as DIR/boughs.blend')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    atlas = json.loads(args.config.read_text())['atlas']
    size, ppm = atlas['size'], atlas['px_per_m']
    args.out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    rng = np.random.default_rng(args.seed)
    position, colour, index, count, report = [], [], [], 0, {}
    for kind, cell in atlas['cells'].items():
        outline = Outline(cell, ppm)
        geometry = Bough(kind, outline, rng, margin=.03).geometry()
        x, y, _, h = cell['cell']
        place = geometry['position'] + np.array([x / ppm, -(y + h / 2) / ppm, 0.])
        position.append(place)
        colour.append(geometry['colour'])
        index.append(geometry['index'] + count)
        count += len(place)
        report[kind] = dict(shoots=geometry['shoots'], twig_m=geometry['twig_m'],
                            triangles=int(len(geometry['index'])),
                            size_m=[round(outline.length, 3), round(h / ppm, 3)])
    obj = mesh('boughs', np.concatenate(position), np.concatenate(colour), np.concatenate(index))
    extent = size / ppm
    baked = bake(obj, size, extent, extent, args.out, args.samples, (-.08, .08))
    if args.blend:
        bpy.ops.wm.save_as_mainfile(filepath=str(args.out / 'boughs.blend'))
    np.savez_compressed(args.out / 'boughs.npz', colour=baked['colour'][..., :3].astype(np.float16),
                        alpha=baked['colour'][..., 3].astype(np.float16),
                        normal=(baked['normal'][..., :3] * 2 - 1).astype(np.float16),
                        ao=baked['ao'][..., 0].astype(np.float16),
                        height=(baked['height'][..., 0] * .16 - .08).astype(np.float16))
    report = dict(seed=args.seed, size=size, px_per_m=ppm, triangles=int(sum(len(i) for i in index)), cells=report)
    (args.out / 'boughs.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
