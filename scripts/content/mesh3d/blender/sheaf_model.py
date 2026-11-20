"""A bound sheaf of rye for the crop fields: a low body with its maps baked from a sheaf modelled straw by straw.

    blender -b -P sheaf_model.py -- --out work/sheaf [--size 1024] [--seed 7]

reports/content/2026-11-19-crop-fields/sheaf.md. The sheaf is modelled as it is bound: some 900 straws from the
splayed butts through the waist under a band of twisted straw to the flared top, each ending in its neck and its
ear, over a dark core. The game's mesh is one surface round that model: a lathe with a cap at each end up to where
the straws part, and the same lathe going on as an open collar through the ears, with a second collar inside it.
Colour, relief and the shade between the straws are baked on it from the straws; over the collars the bake's
coverage is the cut-out. So the ears stand on the straws the body shows, in one texture (the user, 2026-11-20:
a ring of separate cards of ears read as another thing stuck on the body).

Writes `sheaf.npz`: `position`, `normal` (metres, Z up, the sheaf's foot at 0), `uv` (the image's, v down),
`index`; `colour` (linear), `alpha`, `tangent_normal` (0..1), `ao`, top row first; and `sheaf.json`.
"""
import argparse
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).parent))
import crop_cards as cc  # noqa: E402

# height in metres, radius: the butts splay on the ground, the band sits at the waist; from BODY_TOP the surface
# is a collar through the ears
PROFILE = ((0., .245), (.12, .215), (.4, .15), (.64, .098), (.72, .086), (.8, .098), (.95, .14), (1.04, .165),
           (1.12, .205), (1.21, .25), (1.3, .285), (1.38, .3))
BODY_TOP = 1.04
INNER = ((1.04, .07), (1.14, .1), (1.24, .135), (1.33, .15))     # the collar inside: the ears of the middle
INNER_SIDES = 8
BAND = .72
SIDES = 14
STRAWS = 900
STUBS = 420                 # short ends that fill the butt and the top between the straws: what the caps show
STRAW_RADIUS = .0026
BODY = (0., .75)            # the surface's share of the atlas along u
CAPS = ((.875, .875), (.875, .625), .115)     # centres of the butt's and the top's discs, their radius, Blender's uv
INNER_CELL = ((.775, .01), (.22, .46))        # the inner collar's low corner and size, Blender's uv
EAR_TONE = cc.linear('#9a7a45')               # the ear of a stalk on the field (crop_field_life.STAND_MEAN's note)
DARK = cc.linear('#3a2f1c')


def radius(z):
    points = np.asarray(PROFILE)
    return np.interp(z, points[:, 0], points[:, 1])


def tube(points, colours, faces, line, thick, colour, sides=5, capped=True):
    """A tube along `line` (n, 3) with a colour per ring; thick: its radius, one or per ring."""
    line = np.asarray(line, np.float64)
    along = np.gradient(line, axis=0)
    along /= np.linalg.norm(along, axis=1, keepdims=True)
    first = len(points)
    for at, way, tone, wide in zip(line, along, colour, np.broadcast_to(thick, len(line))):
        a = np.cross(way, (.3, .5, .81))
        a /= np.linalg.norm(a)
        b = np.cross(way, a)
        for k in range(sides):
            angle = 2 * math.pi * k / sides
            points.append(tuple(at + (a * math.cos(angle) + b * math.sin(angle)) * wide))
            colours.append((*tone, 1.))
    for ring in range(len(line) - 1):
        for k in range(sides):
            a, b = first + ring * sides + k, first + ring * sides + (k + 1) % sides
            faces.append((a, b, b + sides, a + sides))
    if capped:
        faces.append(tuple(first + k for k in range(sides))[::-1])
        faces.append(tuple(first + (len(line) - 1) * sides + k for k in range(sides)))


def straws(rng):
    points, colours, faces = [], [], []
    top = BODY_TOP
    for _ in range(STRAWS):
        share, angle = rng.random() ** .3, rng.uniform(0, 2 * math.pi)     # most of them near the outside
        end = top + rng.uniform(-.06, .15)        # a ragged head: the straws are of many lengths
        heights = np.array((rng.uniform(0, .035) * share, .12, .4, .64, BAND, .8, .98, end))
        twist = angle + rng.normal(0, .05) + (heights - BAND) * rng.normal(0, .5)
        reach = radius(heights) * share * (1 + rng.normal(0, .025, len(heights)))
        reach[3:6] = np.minimum(reach[3:6], radius(heights[3:6]))          # the band holds them in
        line = np.stack([np.cos(twist) * reach, np.sin(twist) * reach, heights], axis=1)
        tone = rng.uniform(.72, 1.12) * (.8 if rng.random() < .12 else 1.)  # a weathered straw here and there
        rise = np.clip(heights / top, 0, 1)[:, None]
        colour = (cc.STEM_LOW * (1 - rise) + cc.STEM_HIGH * rise) * tone * 1.25
        colour[0] *= 1.25                                                  # the cut end is paler
        tube(points, colours, faces, line, STRAW_RADIUS * rng.uniform(.8, 1.2), colour)
        # its neck goes over, outwards for the outer straws, and the ear hangs from it
        way = line[-1] - line[-2]
        way /= np.linalg.norm(way)
        turn = twist[-1] + rng.normal(0, .5 + 2.5 * (1 - share))
        aim = np.array((math.cos(turn), math.sin(turn), rng.uniform(-.5, .3)))
        aim /= np.linalg.norm(aim)
        nod, at, head = rng.uniform(.25, .85) * (.5 + .5 * share), line[-1].copy(), [line[-1].copy()]
        for k in range(1, 15):
            bend = nod * min(k / 6, 1.) + (.12 * (k - 6) / 8 if k > 6 else 0.)
            ahead = way * (1 - bend) + aim * bend
            at = at + ahead / np.linalg.norm(ahead) * (rng.uniform(.07, .1) / 6 if k <= 6 else rng.uniform(.095, .125) / 8)
            head.append(at.copy())
        head = np.array(head)
        ear = (cc.STEM_HIGH + EAR_TONE) / 2 * tone * 1.6
        tube(points, colours, faces, head[:7], STRAW_RADIUS * .8, np.tile(cc.STEM_HIGH * tone * 1.25, (7, 1)), capped=False)
        grain = np.where(np.arange(9) % 2 == 0, 1., .8)[:, None]
        tube(points, colours, faces, head[6:], np.array((.0025, .0048, .0058, .006, .006, .0056, .005, .004, .002)),
             ear * grain, sides=4)
        for k in range(7, 14):
            along = head[k + 1] - head[k]
            along /= np.linalg.norm(along)
            for _ in range(2):
                out = np.cross(along, rng.normal(0, 1, 3))
                tip = head[k] + (along + out / np.linalg.norm(out) * rng.uniform(.3, .5)) * rng.uniform(.04, .075)
                tube(points, colours, faces, (head[k], tip), np.array((.0011, .0006)), np.tile(ear * 1.08, (2, 1)),
                     sides=3, capped=False)
    for _ in range(STUBS):
        share, angle = math.sqrt(rng.random()) * .88, rng.uniform(0, 2 * math.pi)
        for low, high in ((rng.uniform(0, .03), .09), (top - .09, top - rng.uniform(.005, .05))):
            reach = radius(np.array((low, high))) * share
            line = np.stack([np.cos(angle) * reach, np.sin(angle) * reach, (low, high)], axis=1)
            tube(points, colours, faces, line, STRAW_RADIUS, np.tile(cc.STEM_HIGH * rng.uniform(.8, 1.3), (2, 1)))
    # the band: two twists of straw round the waist
    for turn in range(2):
        angles = np.linspace(0, 2 * math.pi, 40) + turn
        reach = radius(BAND) + .012
        line = np.stack([np.cos(angles) * reach, np.sin(angles) * reach,
                         BAND - .011 + turn * .022 + .006 * np.sin(angles * 3 + turn)], axis=1)
        for strand in range(3):
            shift = np.stack([np.cos(angles) * .004 * math.cos(strand * 2.1), np.sin(angles) * .004 * math.cos(strand * 2.1),
                              .005 * np.sin(angles * 9 + strand * 2.1)], axis=1)
            tube(points, colours, faces, line + shift, .006, np.tile(cc.BAND * (1. - .12 * strand), (len(line), 1)),
                 capped=False)
    # the dark between the straws
    rings = [(z, radius(z) * .8) for z, _ in PROFILE if z <= BODY_TOP]
    first = len(points)
    for z, reach in rings:
        for k in range(24):
            points.append((math.cos(2 * math.pi * k / 24) * reach, math.sin(2 * math.pi * k / 24) * reach,
                           float(np.clip(z, .05, top - .05))))     # behind the stubs of both ends
            colours.append((*DARK, 1.))
    for ring in range(len(rings) - 1):
        for k in range(24):
            a, b = first + ring * 24 + k, first + ring * 24 + (k + 1) % 24
            faces.append((a, b, b + 24, a + 24))
    faces.append(tuple(first + k for k in range(24))[::-1])
    faces.append(tuple(first + (len(rings) - 1) * 24 + k for k in range(24)))
    return points, colours, faces


def body():
    """The game's mesh: position, normal, uv (Blender's), triangles."""
    position, normal, uv, index = [], [], [], []
    top = PROFILE[-1][0]

    def lathe(rings, sides, corner, size):
        rings = np.asarray(rings)
        first, slope = len(position), np.gradient(rings[:, 1], rings[:, 0])
        for (z, reach), lean in zip(rings, slope):
            for k in range(sides + 1):
                angle = 2 * math.pi * k / sides
                out = np.array((math.cos(angle), math.sin(angle), -lean))
                position.append((math.cos(angle) * reach, math.sin(angle) * reach, z))
                normal.append(tuple(out / np.linalg.norm(out)))
                uv.append((corner[0] + size[0] * k / sides,
                           corner[1] + size[1] * (z - rings[0, 0]) / (rings[-1, 0] - rings[0, 0])))
        for ring in range(len(rings) - 1):
            for k in range(sides):
                a, b = first + ring * (sides + 1) + k, first + ring * (sides + 1) + k + 1
                index.extend([(a, b, b + sides + 1), (a, b + sides + 1, a + sides + 1)])

    lathe(PROFILE, SIDES, (BODY[0], 0.), (BODY[1] - BODY[0], 1.))
    lathe(INNER, INNER_SIDES, *INNER_CELL)
    for z, centre, up in ((0., CAPS[0], -1.), (BODY_TOP, CAPS[1], 1.)):
        first, reach = len(position), float(radius(z))
        position.append((0., 0., z))
        normal.append((0., 0., up))
        uv.append(centre)
        for k in range(SIDES):
            angle = 2 * math.pi * k / SIDES
            position.append((math.cos(angle) * reach, math.sin(angle) * reach, z))
            normal.append((0., 0., up))
            uv.append((centre[0] + math.cos(angle) * CAPS[2], centre[1] + math.sin(angle) * CAPS[2]))
        for k in range(SIDES):
            a, b = first + 1 + k, first + 1 + (k + 1) % SIDES
            index.append((first, a, b) if up > 0 else (first, b, a))
    return np.array(position), np.array(normal), np.array(uv), np.array(index)


def placed(name, points, faces, colours=None, uv=None):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata([tuple(p) for p in points], [], [tuple(int(i) for i in f) for f in faces])
    if colours is not None:
        layer = mesh.color_attributes.new('Col', 'FLOAT_COLOR', 'POINT')
        layer.data.foreach_set('color', np.asarray(colours, np.float32).ravel())
    if uv is not None:
        layer = mesh.uv_layers.new(name='UVMap')
        loops = np.empty(len(mesh.loops), np.int32)
        mesh.loops.foreach_get('vertex_index', loops)
        layer.data.foreach_set('uv', np.asarray(uv, np.float32)[loops].ravel())
    mesh.validate()
    mesh.update()
    made = bpy.data.objects.new(name, mesh)
    bpy.context.scene.collection.objects.link(made)
    return made


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', required=True)
    parser.add_argument('--size', type=int, default=1024)
    parser.add_argument('--seed', type=int, default=7)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    rng = np.random.default_rng(args.seed)

    def emission(name, white):
        material = bpy.data.materials.new(name)
        material.use_nodes = True
        nodes, links = material.node_tree.nodes, material.node_tree.links
        nodes.clear()
        shader, output = nodes.new('ShaderNodeEmission'), nodes.new('ShaderNodeOutputMaterial')
        if not white:
            colour = nodes.new('ShaderNodeVertexColor')
            colour.layer_name = 'Col'
            links.new(colour.outputs['Color'], shader.inputs['Color'])
        links.new(shader.outputs['Emission'], output.inputs['Surface'])
        return material

    coloured, white = emission('coloured', False), emission('white', True)
    points, colours, faces = straws(rng)
    high = [placed('straws', points, faces, colours)]
    for polygon in high[0].data.polygons:
        polygon.use_smooth = True
    position, normal, uv, index = body()
    low_points, low_faces, low_uv = [tuple(p) for p in position], [tuple(t) for t in index], [tuple(t) for t in uv]
    low = placed('low', low_points, low_faces, uv=low_uv)
    for polygon in low.data.polygons:
        polygon.use_smooth = True
    for made in high:
        made.data.materials.append(coloured)

    target = bpy.data.materials.new('target')
    target.use_nodes = True
    picture = target.node_tree.nodes.new('ShaderNodeTexImage')
    target.node_tree.nodes.active = picture
    low.data.materials.append(target)
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.world = bpy.data.worlds.new('white')
    bake = scene.render.bake
    bake.use_selected_to_active = True
    bake.cage_extrusion = .035
    bake.max_ray_distance = .09      # a collar takes the ears of its own depth: the head stays loose
    bake.margin = 8
    bake.normal_space = 'TANGENT'
    bpy.ops.object.select_all(action='DESELECT')
    for made in high:
        made.select_set(True)
    low.select_set(True)
    bpy.context.view_layer.objects.active = low

    def baked(kind, samples, material=None):
        image = bpy.data.images.new(kind, args.size, args.size, alpha=False, float_buffer=True)
        image.colorspace_settings.name = 'Non-Color'
        picture.image = image
        scene.cycles.samples = samples
        if material is not None:
            for made in high:
                made.data.materials[0] = material
        bpy.ops.object.bake(type='EMIT' if material is not None else kind)
        pixels = np.empty(args.size * args.size * 4, np.float32)
        image.pixels.foreach_get(pixels)
        bpy.data.images.remove(image)
        print(f'sheaf_model: baked {kind}', flush=True)
        return pixels.reshape(args.size, args.size, 4)[::-1, :, :3]

    colour = baked('colour', 4, coloured)
    bake.margin = 0             # the margin would grow the ears
    alpha = baked('cover', 4, white)[..., 0]
    bake.margin = 8
    tangent = baked('NORMAL', 4)
    ao = baked('AO', 96)[..., 0]

    # the surface is opaque up to where the straws part, and on its caps; over the collars the bake's cover cuts it
    rows = 1 - (np.arange(args.size)[:, None] + .5) / args.size        # Blender's v of a row
    columns = (np.arange(args.size)[None, :] + .5) / args.size
    solid = ((columns < BODY[1]) & (rows < BODY_TOP / PROFILE[-1][0])) | ((columns > BODY[1] + .005) & (rows > .5))
    alpha = np.where(solid, 1., alpha)
    # the head is open to the sky: the ears shade each other less than the bake of so dense a model says, and
    # their relief is too fine for the light to follow
    # (rising over the last of the body, so no line shows where the surface opens)
    top = PROFILE[-1][0]
    head = np.clip((rows - (BODY_TOP - .14) / top) / (.18 / top), 0, 1) * np.ones_like(columns)
    head = np.where(columns < BODY[1], head * head * (3 - 2 * head), np.where(solid, 0., 1.))
    ao = ao + (1 - ao) * .45 * head
    tangent = tangent + (np.array((.5, .5, 1.)) - tangent) * .6 * head[..., None]

    np.savez(out / 'sheaf.npz', position=position, normal=normal, uv=np.stack([uv[:, 0], 1 - uv[:, 1]], axis=1),
             index=index, colour=colour.astype(np.float16), alpha=alpha.astype(np.float16),
             tangent_normal=tangent.astype(np.float16), ao=ao.astype(np.float16))
    (out / 'sheaf.json').write_text(json.dumps(dict(size=args.size, seed=args.seed, profile=PROFILE, band_m=BAND,
                                                    straws=STRAWS + 2 * STUBS, triangles=len(index), body_top_m=BODY_TOP), indent=2) + '\n')
    preview = bpy.data.images.new('preview', args.size, args.size, alpha=True)
    lit = np.clip(colour * (.45 + .55 * ao[..., None]), 0, 1) ** (1 / 2.2)
    preview.pixels.foreach_set(np.concatenate([lit, alpha[..., None]], axis=2)[::-1].astype(np.float32).ravel())
    preview.filepath_raw, preview.file_format = str(out / 'preview.png'), 'PNG'
    preview.save()


if __name__ == '__main__':
    main()
