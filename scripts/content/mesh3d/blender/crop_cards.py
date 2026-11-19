"""Render side views of rows of rye, of its stubble and of a bound sheaf: the pictures on the cards of a crop field.

    blender -b -P crop_cards.py -- --out work/crop-cards [--density 512] [--seed 7]

reports/content/2026-11-19-crop-fields (intent.md). Everything is built here from numbers, no scan and no pixel of
the original: a stalk is a flat ribbon along a curve, seen from the side by an orthographic camera, as the cards of
the grass tufts are (grass_cards.py). A stalk of rye is a stem that bows a little, a neck that goes over and a
bearded ear that nods from it, with one or two dry leaves; the stalks at the back of a row are darker, the depth a
flat card has no other way to show, and every stalk darkens to its root. Stems are drawn wider than life (7 mm at
the root): a ribbon of two texels dissolves under the engine's hashed alpha from a few metres.

One pass per card, written to `cards.npz` as `<name>` (RGBA: linear albedo, not premultiplied, and coverage), the
top image row first; `cards.json` lists the cards.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np

# name: kind, width and height of the picture in metres, how many stalks
CARDS = dict(
    rye_a=dict(kind='rye', size=(1.5, 1.625), stalks=120),
    rye_b=dict(kind='rye', size=(1.5, 1.625), stalks=120),
    rye_c=dict(kind='rye', size=(1.5, 1.625), stalks=105),
    rye_thin=dict(kind='rye', size=(1.5, 1.625), stalks=48),
    stubble_a=dict(kind='stubble', size=(1.5, .34375), stalks=120),
    stubble_b=dict(kind='stubble', size=(1.5, .34375), stalks=85),
    sheaf=dict(kind='sheaf', size=(.5625, 1.3125), stalks=64),
)
MARGIN = .012
BACK, FRONT = .5, 1.08      # brightness of the stalk furthest from the camera and of the nearest
FLOOR, CURVE = .5, 1.3      # a stalk's brightness at its root, and how it rises with the height share


def linear(code: str) -> np.ndarray:
    c = np.array([int(code[i:i + 2], 16) for i in (1, 3, 5)]) / 255
    return np.where(c <= .04045, c / 12.92, ((c + .055) / 1.055) ** 2.4)


STEM_LOW, STEM_HIGH = linear('#8f8160'), linear('#b0935a')
EAR, EAR_DARK, AWN = linear('#a66a30'), linear('#784b22'), linear('#b98f52')
LEAF = linear('#97804c')
STUBBLE, STUBBLE_ROOT = linear('#a79a72'), linear('#6f6347')
BAND = linear('#7f6a3e')


class Sheet:
    """Ribbons of one card: quads that face the camera (-Y), a colour per vertex."""

    def __init__(self):
        self.points, self.colours, self.faces = [], [], []

    def ribbon(self, line, widths, colours, depth):
        """line: (n, 2) of x and z; widths: n; colours: (n, 3); depth: y, the further the later drawn over."""
        line = np.asarray(line, np.float64)
        along = np.gradient(line, axis=0)
        along /= np.maximum(np.linalg.norm(along, axis=1, keepdims=True), 1e-9)
        across = np.stack([along[:, 1], -along[:, 0]], axis=1)
        first = len(self.points)
        for point, side, width, colour in zip(line, across, widths, colours):
            for sign in (-1, 1):
                x, z = point + side * sign * width / 2
                self.points.append((x, depth, z))
                self.colours.append((*colour, 1.))
        for k in range(len(line) - 1):
            a = first + 2 * k
            self.faces.append((a, a + 1, a + 3, a + 2))

    def mesh(self, name):
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata(self.points, [], self.faces)
        layer = mesh.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
        layer.data.foreach_set('color', np.asarray(self.colours, np.float32).ravel())
        mesh.validate()
        mesh.update()
        return mesh


def curve(start, heading, lengths, turns, steps=6):
    """A line from `start` going `heading` (radians from up, to +x), piece by piece: each piece its length and
    the turn it makes along itself."""
    points, at, angle = [np.asarray(start, np.float64)], np.asarray(start, np.float64), heading
    for length, turn in zip(lengths, turns):
        for _ in range(steps):
            angle += turn / steps
            at = at + np.array((math.sin(angle), math.cos(angle))) * length / steps
            points.append(at)
    return np.array(points), angle


def shade(z, top, depth):
    return (FLOOR + (1 - FLOOR) * np.clip(z / top, 0, 1) ** CURVE) * (FRONT - (FRONT - BACK) * depth)


def rye_stalk(sheet, rng, x, top, depth, heading=None, stem=None, nod=None):
    """A stalk whose ear's bend is at `top` metres: stem, neck, ear, awns, leaves."""
    y = .08 * (depth - .5)
    stem = top - rng.uniform(.02, .06) if stem is None else stem
    side = rng.choice((-1., 1.), p=(.35, .65))
    heading = math.radians(rng.normal(0, 2.5)) if heading is None else heading
    line, angle = curve((x, 0.), heading, (stem * .55, stem * .45), (math.radians(rng.normal(0, 2)),
                                                                      side * math.radians(rng.uniform(1, 6))), steps=5)
    tone = shade(line[:, 1], top, depth)[:, None]
    share = np.clip(line[:, 1] / top, 0, 1)[:, None]
    sheet.ribbon(line, np.linspace(.0075, .005, len(line)), (STEM_LOW * (1 - share) + STEM_HIGH * share) * tone, y)
    # the neck goes over, the ear hangs from it
    nod = math.radians(rng.uniform(35, 95)) if nod is None else nod
    neck, angle = curve(line[-1], angle, (rng.uniform(.07, .12),), (side * nod * .7,), steps=5)
    sheet.ribbon(neck, np.full(len(neck), .0045), np.tile(STEM_HIGH * shade(top, top, depth), (len(neck), 1)), y)
    length = rng.uniform(.09, .125)
    ear, _ = curve(neck[-1], angle, (length,), (side * nod * .3,), steps=10)
    profile = np.array((.008, .016, .021, .023, .023, .022, .021, .019, .016, .012, .006))
    grain = np.where(np.arange(len(ear)) % 2 == 0, 1., 0.)[:, None]
    light = shade(top, top, depth)
    sheet.ribbon(ear, profile, (EAR * grain + EAR_DARK * (1 - grain)) * light, y - .001)
    along = np.gradient(ear, axis=0)
    along /= np.linalg.norm(along, axis=1, keepdims=True)
    for k in range(1, len(ear) - 1):
        for sign in (-1, 1):
            spread = math.radians(rng.uniform(14, 26)) * sign
            way = np.array((along[k, 0] * math.cos(spread) - along[k, 1] * math.sin(spread),
                            along[k, 0] * math.sin(spread) + along[k, 1] * math.cos(spread)))
            tip = ear[k] + way * rng.uniform(.05, .085)
            sheet.ribbon((ear[k], (ear[k] + tip) / 2, tip), (.003, .0024, .0012), np.tile(AWN * light, (3, 1)),
                         y - .002)
    for _ in range(rng.integers(0, 3)):
        at = rng.uniform(.25, .8)
        root = line[int(at * (len(line) - 1))]
        way = rng.choice((-1., 1.))
        blade, _ = curve(root, way * math.radians(rng.uniform(15, 50)), (rng.uniform(.08, .2), rng.uniform(.05, .18)),
                         (way * math.radians(rng.uniform(15, 90)), way * math.radians(rng.uniform(0, 80))), steps=4)
        tone = shade(blade[:, 1], top, depth)[:, None] * rng.uniform(.8, 1.)
        sheet.ribbon(blade, np.linspace(.013, .003, len(blade)), LEAF * tone, y + .001)


def rye(sheet, rng, width, height, count):
    depth = rng.permutation(count) / max(count - 1, 1)
    level = height - MARGIN - .19                        # where the ears bend over: the field's level top
    for index in np.argsort(-depth):                     # the back ones first
        x = (width - .12) * ((index + rng.random()) / count - .5)
        low = rng.random() < .1                          # a shorter stalk here and there
        top = level - (rng.uniform(.12, .3) if low else abs(rng.normal(0, .035)))
        rye_stalk(sheet, rng, x, top, depth[index])


def stubble(sheet, rng, width, height, count):
    depth = rng.permutation(count) / max(count - 1, 1)
    top = height - MARGIN - .02
    for index in np.argsort(-depth):
        x = (width - .06) * ((index + rng.random()) / count - .5)
        y = .08 * (depth[index] - .5)
        broken = rng.random() < .16
        length = rng.uniform(.13, .2) if broken else rng.uniform(.15, top)
        lean = math.radians(rng.normal(0, 30 if broken else 9))
        line, angle = curve((x, 0.), lean, (length,), (math.radians(rng.normal(0, 8)),), steps=3)
        if broken:                                       # snapped: the top lies over
            more, _ = curve(line[-1], angle + rng.choice((-1, 1)) * math.radians(rng.uniform(70, 120)),
                            (rng.uniform(.06, .14),), (0.,), steps=2)
            line = np.concatenate([line, more[1:]])
        tone = (shade(line[:, 1], top * .8, depth[index]) * rng.uniform(.85, 1.05))[:, None]
        share = np.clip(line[:, 1] / (top * .6), 0, 1)[:, None]
        sheet.ribbon(line, np.full(len(line), rng.uniform(.0065, .009)),
                     (STUBBLE_ROOT * (1 - share) + STUBBLE * share) * tone, y)


def sheaf(sheet, rng, width, height, count):
    """Butts spread on the ground, bound at 0.56 of the height, ears fanned out above."""
    depth = rng.permutation(count) / max(count - 1, 1)
    band, top = .7, height - MARGIN - .2
    for index in np.argsort(-depth):
        share = (index + rng.random()) / count * 2 - 1           # -1..1 across the sheaf
        y = .08 * (depth[index] - .5)
        foot, waist = share * .19, share * .06
        lean = math.atan2(waist - foot, band)
        fan = math.radians(share * 20 + rng.normal(0, 4))
        length = (top - band) * rng.uniform(.8, 1.) / max(math.cos(fan), .5)
        low, angle = curve((foot, 0.), lean, (band / math.cos(lean),), (0.,), steps=4)
        high, angle = curve(low[-1], lean, (length,), (fan - lean,), steps=5)
        line = np.concatenate([low, high[1:]])
        tone = (FLOOR + (1 - FLOOR) * np.clip(line[:, 1] / top, 0, 1) ** .8)[:, None] * (FRONT - (FRONT - BACK) * depth[index])
        mix = np.clip(line[:, 1] / top, 0, 1)[:, None]
        sheet.ribbon(line, np.linspace(.008, .005, len(line)), (STEM_LOW * (1 - mix) + STEM_HIGH * mix) * tone, y)
        side = 1. if share > 0 else -1.
        if rng.random() < .15:
            side = -side
        nod = math.radians(rng.uniform(30, 80))
        neck, angle = curve(line[-1], angle, (.07,), (side * nod * .7,), steps=4)
        light = (FRONT - (FRONT - BACK) * depth[index])
        sheet.ribbon(neck, np.full(len(neck), .0045), np.tile(STEM_HIGH * light, (len(neck), 1)), y)
        ear, _ = curve(neck[-1], angle, (rng.uniform(.09, .12),), (side * nod * .3,), steps=10)
        profile = np.array((.008, .016, .021, .023, .023, .022, .021, .019, .016, .012, .006))
        grain = np.where(np.arange(len(ear)) % 2 == 0, 1., 0.)[:, None]
        sheet.ribbon(ear, profile, (EAR * grain + EAR_DARK * (1 - grain)) * light, y - .001)
        along = np.gradient(ear, axis=0)
        along /= np.linalg.norm(along, axis=1, keepdims=True)
        for k in range(1, len(ear) - 1, 2):
            for sign in (-1, 1):
                spread = math.radians(rng.uniform(14, 26)) * sign
                way = np.array((along[k, 0] * math.cos(spread) - along[k, 1] * math.sin(spread),
                                along[k, 0] * math.sin(spread) + along[k, 1] * math.cos(spread)))
                tip = ear[k] + way * rng.uniform(.05, .08)
                sheet.ribbon((ear[k], (ear[k] + tip) / 2, tip), (.003, .0024, .0012), np.tile(AWN * light, (3, 1)),
                             y - .002)
    # the band: a twist of straw round the waist, two turns
    for turn in range(2):
        z = band - .012 + turn * .024
        line = np.array([(-.075, z + .008), (-.03, z - .003), (.03, z + .004), (.075, z - .006)])
        sheet.ribbon(line, np.full(4, .02), np.tile(BAND * (.95 - .15 * turn), (4, 1)), -.05)


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True)
    parser.add_argument('--density', type=int, default=512, help='pixels per metre')
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--samples', type=int, default=16)
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:])


def main():
    args = arguments()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    rng = np.random.default_rng(args.seed)

    material = bpy.data.materials.new('crop')
    material.use_nodes = True
    nodes, links = material.node_tree.nodes, material.node_tree.links
    nodes.clear()
    colour = nodes.new('ShaderNodeVertexColor')
    colour.layer_name = 'Col'
    emission = nodes.new('ShaderNodeEmission')
    output = nodes.new('ShaderNodeOutputMaterial')
    links.new(colour.outputs['Color'], emission.inputs['Color'])
    links.new(emission.outputs['Emission'], output.inputs['Surface'])

    frames = {}
    builders = dict(rye=rye, stubble=stubble, sheaf=sheaf)
    for number, (name, card) in enumerate(CARDS.items()):
        width, height = card['size']
        sheet = Sheet()
        builders[card['kind']](sheet, rng, width, height, card['stalks'])
        mesh = sheet.mesh(name)
        mesh.materials.append(material)
        placed = bpy.data.objects.new(name, mesh)
        placed.location = (number * 4., 0, 0)
        scene.collection.objects.link(placed)
        frames[name] = (number * 4., width, height)

    camera = bpy.data.cameras.new('side')
    camera.type, camera.clip_end = 'ORTHO', 50
    eye = bpy.data.objects.new('side', camera)
    eye.rotation_euler = (math.pi / 2, 0, 0)
    scene.collection.objects.link(eye)
    scene.camera = eye
    scene.world = bpy.data.worlds.new('dark')
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
    cycles.transparent_max_bounces = 64
    cycles.pixel_filter_type = 'BOX'

    arrays, report = {}, []
    for name, (centre, width, height) in frames.items():
        columns, rows = round(width * args.density), round(height * args.density)
        scene.render.resolution_x, scene.render.resolution_y = columns, rows
        camera.ortho_scale = max(width, height)
        eye.location = (centre, -10, height / 2 - MARGIN)
        scene.render.filepath = str(out / f'{name}.exr')
        bpy.ops.render.render(write_still=True)
        image = bpy.data.images.load(scene.render.filepath)
        pixels = np.empty(columns * rows * 4, np.float32)
        image.pixels.foreach_get(pixels)
        bpy.data.images.remove(image)
        Path(scene.render.filepath).unlink()
        pixels = pixels.reshape(rows, columns, 4)[::-1]
        alpha = pixels[..., 3:4]
        arrays[name] = np.concatenate([pixels[..., :3] / np.maximum(alpha, 1e-4), alpha], axis=2).astype(np.float16)
        report.append(dict(name=name, kind=CARDS[name]['kind'], width_m=width, height_m=height, px=[columns, rows],
                           coverage=round(float(alpha.mean()), 4)))
        print(f'crop_cards: {name} {columns}x{rows}', flush=True)
    np.savez(out / 'cards.npz', **arrays)
    (out / 'cards.json').write_text(json.dumps(dict(density=args.density, seed=args.seed, margin_m=MARGIN,
                                                    cards=report), indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
