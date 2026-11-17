"""The atlas of spruce boughs: every bough built as geometry and photographed from above.

    blender -b --factory-startup -P fir_boughs.py -- --out DIR [--config fir.json] [--seed N] [--samples 48]
        [--only KIND,KIND]

Each cell of the config's atlas gets one bough at the size of the card that shows it (fir_tree.py): the woody
axis from the stem at the middle of the cell's left edge to its right edge, side shoots in a herringbone, their
own shoots. The bough has depth: a side shoot leaves the axis downward and hangs, its end turns up again, the
shoots of the next order hang from it, short shoots stand on the axis and cover it. A shoot is a bottle brush:
needles 13 to 19 mm long stand all round it, leaning to its end, none in the quarter that faces down. A bough is
grown inside the outline its card is cut to, a margin inside it, so the card loses nothing. The kinds differ in
habit:

    fan      the end of a branch seen from above: wide herringbone
    hang     a spray that hangs from a branch: long shoots that turn along the axis and hang deep
    side     a shorter herringbone to the side of a branch
    sparse   the shaded inside of the crown: thin, with bare and dry shoots
    twig     a small end shoot
    top      the short shoots along the top of a branch's axis
    shoot    a long narrow shoot of a hanging cluster: side shoots close to the axis, uneven, hanging deep
    sprig    a shorter shoot of a cluster
    tuft     the slender short shoot of a cluster

A cell with `"turn": true` stands on the sheet: its bough's axis runs down the picture from the middle of the
cell's upper edge. The picture inside is the same bough turned a quarter.

One orthographic camera looks straight down at all cells; three renders give the passes: colour (flat, the
needle's own, no light), the share of the sky a point sees straight above itself within SKY_REACH (the shade
between the shoots and under them, whatever way the needle itself faces) and height. DIR/boughs.npz holds them
as float arrays, row 0 is the top of the atlas: colour (linear, premultiplied by alpha), alpha, ao (the sky's
share), height in metres over the plane of the axis. The normal map is made of the height by fir_set.py. The
concept of the bough is a reference for the habit only; no picture is used.
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

# Linear albedo; fir_set.py brings the mean of the sheet to the palette, so only the differences matter here.
# The year's growth at the shoot ends is a little lighter, the needles deep on an old shoot darker. A twig is
# bark in the crown's shade, darker than a needle in the sun.
NEEDLE = np.array((.018, .028, .014))
NEEDLE_YOUNG = NEEDLE * (1.28, 1.32, 1.15)
NEEDLE_OLD = NEEDLE * .78
NEEDLE_DRY = np.array((.060, .040, .020))
TWIG = np.array((.020, .015, .011))
TWIG_DRY = np.array((.050, .043, .036))
SKY_REACH = .10             # metres: how far above itself a point looks for what shades it
HEIGHT = (-.5, .5)          # metres the height pass spans

# nodes: metres between side shoots; angle: of a side shoot to the axis at the stem and at the tip, degrees;
# curve: its turn over its length; reach: its length as a share of the room to the outline; droop: degrees a
# side shoot leaves the axis downward; needles: per metre of a shoot; top: chance of a standing shoot at a node
HABIT = dict(
    fan=dict(nodes=(.050, .078), angle=(60, 42), curve=-10, reach=(.78, .98), missing=.05, sub=(.040, .062),
             sub_share=.42, droop=(16, 32), needles=1700, axis_radius=.006, dry=0, top=.8),
    hang=dict(nodes=(.050, .075), angle=(44, 24), curve=-22, reach=(.70, .98), missing=.05, sub=(.040, .060),
              sub_share=.40, droop=(30, 50), needles=1700, axis_radius=.005, dry=0, top=.5),
    side=dict(nodes=(.048, .074), angle=(58, 40), curve=-12, reach=(.75, .98), missing=.07, sub=(.038, .056),
              sub_share=.42, droop=(16, 34), needles=1700, axis_radius=.0045, dry=0, top=.7),
    sparse=dict(nodes=(.065, .100), angle=(58, 38), curve=-8, reach=(.55, .95), missing=.15, sub=(.050, .080),
                sub_share=.32, droop=(20, 40), needles=1000, axis_radius=.005, dry=3, top=.3),
    twig=dict(nodes=(.042, .064), angle=(54, 38), curve=-10, reach=(.75, .98), missing=.06, sub=(.036, .050),
              sub_share=.38, droop=(14, 30), needles=1750, axis_radius=.0035, dry=0, top=.7),
    top=dict(nodes=(.042, .060), angle=(46, 30), curve=-8, reach=(.80, .98), missing=.04, sub=(.036, .050),
             sub_share=.40, droop=(10, 24), needles=1750, axis_radius=.004, dry=0, top=1.),
    shoot=dict(nodes=(.048, .072), angle=(40, 22), curve=-20, reach=(.55, .98), missing=.10, sub=(.038, .058),
               sub_share=.40, droop=(26, 46), needles=1700, axis_radius=.004, dry=0, top=.5),
    sprig=dict(nodes=(.042, .064), angle=(46, 28), curve=-14, reach=(.60, .98), missing=.10, sub=(.036, .050),
               sub_share=.38, droop=(18, 36), needles=1750, axis_radius=.0035, dry=0, top=.6),
    tuft=dict(nodes=(.040, .060), angle=(36, 22), curve=-12, reach=(.60, .98), missing=.08, sub=(.034, .048),
              sub_share=.36, droop=(16, 32), needles=1750, axis_radius=.003, dry=0, top=.6))


def smooth(x: float) -> float:
    x = min(max(x, 0.), 1.)
    return x * x * (3 - 2 * x)


class Outline:
    """The card's outline in the bough's own metres: x along the axis from the stem, y across."""

    def __init__(self, cell: dict, px_per_m: float):
        _, _, w, h = cell['cell']
        if cell.get('turn'):
            w, h = h, w
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

    def shoot(self, start, heading: float, length: float, turn: float, order: int, pitch=(0., 0.),
              dry: bool = False, wobble: float = .08, bare: bool = False) -> dict | None:
        """A polyline from `start`: its heading in the picture's plane turns by `turn` over its length, its pitch
        (up is positive) goes from pitch[0] to pitch[1]. It stops at the outline."""
        rng = self.rng
        steps = max(3, int(math.ceil(length / .015)))
        step = length / steps
        phase, wave = rng.uniform(0, 6.28), rng.uniform(1.5, 4.)
        points = [np.asarray(start, np.float64)]
        for i in range(steps):
            t = (i + .5) / steps
            a = heading + turn * t + wobble * math.sin(phase + wave * t * 3.)
            p = pitch[0] + (pitch[1] - pitch[0]) * smooth(t)
            nxt = points[-1] + np.array([math.cos(a) * math.cos(p), math.sin(a) * math.cos(p), math.sin(p)]) * step
            if self.outline.depth(nxt) < (self.margin if order else .002) or nxt[0] > self.outline.length - .02:
                break
            points.append(nxt)
        if len(points) < 3:
            return None
        shoot = dict(points=np.array(points), order=order, dry=dry, bare=bare, length=step * (len(points) - 1))
        self.shoots.append(shoot)
        return shoot

    def children(self, parent: dict, spacing, angle, curve: float, reach, order: int, share: float, droop,
                 start: float = .1, missing: float = 0., top: float = 0.) -> list[dict]:
        rng, points = self.rng, parent['points']
        along = np.concatenate([[0.], np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))])
        total, out = along[-1], []
        s, side = total * start + rng.uniform(0, spacing[0]), rng.choice([-1., 1.])
        while s < total * .97:
            i = int(np.clip(np.searchsorted(along, s) - 1, 0, len(points) - 2))
            base = points[i] + (points[i + 1] - points[i]) * (s - along[i]) / max(along[i + 1] - along[i], 1e-9)
            tangent = (points[i + 1] - points[i]) / max(along[i + 1] - along[i], 1e-9)
            heading, lie = math.atan2(tangent[1], tangent[0]), math.asin(float(np.clip(tangent[2], -1, 1)))
            t = s / total
            for turn in ((side, -side) if order == 1 else (side,)):
                if rng.uniform() < missing:
                    continue
                a = math.radians(angle[0] + (angle[1] - angle[0]) * t + rng.uniform(-8, 8))
                down = math.radians(rng.uniform(*droop))
                if order == 1:
                    length = self.room(base, heading + turn * a) * rng.uniform(*reach) / math.cos(down * .6)
                    if rng.uniform() < .1:
                        length *= rng.uniform(.35, .6)
                    pitch = (-down, -down + math.radians(rng.uniform(22, 44)))      # hangs, the end turns up
                else:
                    length = parent['length'] * share * (1 - .65 * t) * rng.uniform(.6, 1.15)
                    pitch = (lie - down * .7, lie - down * .7 + math.radians(rng.uniform(15, 35)))
                if length < .03:
                    continue
                child = self.shoot(base, heading + turn * a, length, turn * math.radians(curve) * rng.uniform(.5, 1.3),
                                   order, pitch, parent['dry'])
                if child:
                    out.append(child)
            if rng.uniform() < top and not parent['dry']:
                # a short shoot that stands on the parent and leans along it: what covers an axis from above
                self.shoot(base, heading + math.radians(rng.uniform(-28, 28)), rng.uniform(.04, .09), 0., 3,
                           (lie + math.radians(rng.uniform(22, 48)), lie + math.radians(rng.uniform(0, 18))))
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
        axis = self.shoot((.004, 0., 0.), 0., self.outline.length, rng.uniform(-.04, .04), 0, wobble=.04)
        first = self.children(axis, h['nodes'], h['angle'], h['curve'], h['reach'], 1, 0., h['droop'], start=.04,
                              missing=h['missing'], top=h['top'])
        for index in rng.permutation(len(first))[:h['dry']]:
            first[index]['dry'] = True
        for shoot in first:
            if shoot['length'] < .08:
                continue
            for second in self.children(shoot, h['sub'], (50, 36), h['curve'] * .5, None, 2, h['sub_share'],
                                        (10, 28), start=.12, top=h['top'] * .35):
                if second['length'] > .07 and not second['dry']:
                    self.children(second, (h['sub'][0] * .8, h['sub'][1] * .8), (48, 38), 0., None, 3, .4, (8, 22), start=.2)

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
                radius = (.0015 if order == 1 else .0011 if order == 2 else .0009) * (1 - .5 * share) \
                    * (1 + 1.5 * shoot['length'] * (order == 1))
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
            tone = (TWIG_DRY if shoot['dry'] else TWIG) * rng.uniform(.8, 1.15)
            push(tube.reshape(-1, 3), tone, tris)
            if shoot['dry'] and rng.uniform() < .7:
                continue
            # needles: all round the shoot but its underside, leaning to its end; a brush of them closes the end
            lengths = np.concatenate([[0.], np.cumsum(np.linalg.norm(np.diff(points, axis=0), axis=1))])
            total = lengths[-1]
            start = total * (.06 if order == 0 else .04)
            m = int((total - start) * h['needles'] * (.35 if shoot['dry'] else 1.))
            if m <= 0:
                continue
            s = np.sort(rng.uniform(start, total + .003, m))
            i = np.clip(np.searchsorted(lengths, s) - 1, 0, n - 2)
            w = ((s - lengths[i]) / np.maximum(lengths[i + 1] - lengths[i], 1e-9))[:, None]
            base = points[i] * (1 - w) + points[np.minimum(i + 1, n - 1)] * w
            around = np.radians(rng.uniform(-135, 135, m))           # from the shoot's top
            lean = np.where(np.abs(around) < math.radians(45), rng.uniform(28, 52, m), rng.uniform(42, 68, m))
            lean = np.radians(np.where(s > total - .012, rng.uniform(8, 36, m), lean))
            radial = np.cos(around)[:, None] * up[i] + np.sin(around)[:, None] * across[i]
            direction = np.cos(lean)[:, None] * tangent[i] + np.sin(lean)[:, None] * radial
            age = np.clip((s - start) / max(total - start, 1e-6), 0, 1)        # 1 at the shoot's end
            long = rng.uniform(.013, .019, m) * np.where(age > .9, .85, 1.)
            wide = rng.uniform(.0020, .0026, m)
            flat = np.cross(direction, (0, 0, 1.))
            short = np.linalg.norm(flat, axis=1) < 1e-3
            flat[short] = across[i][short]
            flat /= np.linalg.norm(flat, axis=1, keepdims=True)
            rise = np.cross(flat, direction)
            roll = np.radians(rng.uniform(-35, 35, m))
            edge = (flat * np.cos(roll)[:, None] + rise * np.sin(roll)[:, None]) * (wide / 2)[:, None]
            base = base + radial * radius[i][:, None]
            mid, tip = base + direction * (long * .45)[:, None], base + direction * long[:, None]
            quads = np.stack([base, mid - edge, tip, mid + edge], axis=1)
            young = np.clip((age - .6) / .4, 0, 1) * (order >= 1) * rng.uniform(.3, 1., m)
            old = np.clip(1. - age * 2.5, 0, 1) * rng.uniform(.3, 1., m) * (order < 2)
            tone = NEEDLE + (NEEDLE_YOUNG - NEEDLE) * young[:, None]
            tone = tone + (NEEDLE_OLD - tone) * old[:, None]
            tone = tone * rng.uniform(.86, 1.14, (m, 1)) * (1 + rng.uniform(-.04, .04, (m, 3)))
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


def passes(height_range: tuple[float, float], ao_distance: float, above: bool = False) -> dict:
    """The four materials of the bake: colour, normal, occlusion, height. With `above` the occlusion is the share
    of the sky a point sees straight over itself, whatever way its own face looks."""
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
        if above:
            up = nodes.new('ShaderNodeCombineXYZ')
            up.inputs['Z'].default_value = 1.
            links.new(up.outputs[0], node.inputs['Normal'])
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
         transparent: bool = True, above: bool = False, skip=()) -> dict:
    """Photograph the object from above: x 0..width_m to the right, y 0..-height_m down the picture. The passes
    named in `skip` are not rendered."""
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
    sc.render.pixel_aspect_x, sc.render.pixel_aspect_y = (1., height_m / width_m) if height_m > width_m \
        else (width_m / height_m, 1.)
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
    for name, mat in passes(height_range, ao_distance, above).items():
        if name in skip:
            continue
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
    parser.add_argument('--only', default='', help='bake these cells only, a look at a habit: fan,top')
    parser.add_argument('--blend', action='store_true', help='keep the scene as DIR/boughs.blend')
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    atlas = json.loads(args.config.read_text())['atlas']
    size, ppm = atlas['size'], atlas['px_per_m']
    args.out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    only = set(filter(None, args.only.split(',')))
    position, colour, index, count, report = [], [], [], 0, {}
    for number, (kind, cell) in enumerate(atlas['cells'].items()):
        if only and kind not in only:
            continue
        outline = Outline(cell, ppm)
        geometry = Bough(kind, outline, np.random.default_rng([args.seed, number]), margin=.022).geometry()
        x, y, w, h = cell['cell']
        if cell.get('turn'):
            place = geometry['position'][:, (1, 0, 2)] * (1, -1, 1) + np.array([(x + w / 2) / ppm, -y / ppm, 0.])
            w, h = h, w
        else:
            place = geometry['position'] + np.array([x / ppm, -(y + h / 2) / ppm, 0.])
        position.append(place)
        colour.append(geometry['colour'])
        index.append(geometry['index'] + count)
        count += len(place)
        report[kind] = dict(shoots=geometry['shoots'], twig_m=geometry['twig_m'],
                            triangles=int(len(geometry['index'])),
                            size_m=[round(outline.length, 3), round(h / ppm, 3)],
                            height_m=[round(float(v), 3) for v in (place[:, 2].min(), place[:, 2].max())])
    obj = mesh('boughs', np.concatenate(position), np.concatenate(colour), np.concatenate(index))
    extent = size / ppm
    baked = bake(obj, size, extent, extent, args.out, args.samples, HEIGHT, SKY_REACH, above=True,
                 skip=('normal',))
    if args.blend:
        bpy.ops.wm.save_as_mainfile(filepath=str(args.out / 'boughs.blend'))

    def plain(name: str, empty: float) -> np.ndarray:
        """A pass without the film's alpha in it: its value where the bough is, `empty` elsewhere."""
        value, alpha = baked[name][..., 0], baked[name][..., 3]
        return np.where(alpha > 1e-3, value / np.maximum(alpha, 1e-3), empty)

    np.savez_compressed(args.out / 'boughs.npz', colour=baked['colour'][..., :3].astype(np.float16),
                        alpha=baked['colour'][..., 3].astype(np.float16),
                        ao=np.clip(plain('ao', 1.), 0, 1).astype(np.float16),
                        height=(plain('height', .5) * (HEIGHT[1] - HEIGHT[0]) + HEIGHT[0]).astype(np.float16))
    report = dict(seed=args.seed, size=size, px_per_m=ppm, triangles=int(sum(len(i) for i in index)), cells=report)
    (args.out / 'boughs.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
