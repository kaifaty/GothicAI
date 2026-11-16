"""Render a tree's GLB for the acceptance sheet: the concept's viewpoint, a side view, the lower boughs up close.

    blender -b --factory-startup -P fir_views.py -- --glb TREE.glb --out DIR
        [--boughs ATLAS.png] [--boughs-normal N.png] [--bark BARK.png] [--bark-normal N.png]
        [--views concept,side,close,foot] [--width 936] [--samples 96]

compare.py cannot do this: it has no cutout, aims at the middle of the bounding box and lights like a studio.
Here the tree stands on a meadow under a noon sun from the upper left, as in the concept, with a 1.8 m figure at
its foot; the camera of `concept` is 30 m away at eye height, `side` is the same turned a quarter, `close` looks
at the lower boughs from 3 m, `foot` at the foot of the trunk from 3 m.

The cards are shaded as the game will shade them: one side drawn twice, both sides with the authored normal (the
crown's hull), the cutout by the atlas's alpha at one half. Without --boughs the cards are flat olive polygons:
the check of the silhouette before the atlas exists. View transform Standard, so colours can be compared with
the scene measurement of the intent sheet.
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fir  # noqa: E402
import tree_glb  # noqa: E402

GRASS = (0.078, 0.066, 0.030)      # the meadow of the scene measurement, #4f472e, as albedo
SKY = (0.10, 0.19, 0.42)


def image(path: Path, colour: bool):
    img = bpy.data.images.load(str(path))
    img.colorspace_settings.name = 'sRGB' if colour else 'Non-Color'
    return img


def material(name: str, colour: Path | None, normal: Path | None, flat, cutout: bool) -> None:
    mat = bpy.data.materials.get(name) or bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    out = nodes.new('ShaderNodeOutputMaterial')
    tex = None
    if colour:
        tex = nodes.new('ShaderNodeTexImage')
        tex.image = image(colour, True)
    if not cutout:
        bsdf = nodes.new('ShaderNodeBsdfPrincipled')
        bsdf.inputs['Roughness'].default_value = .9
        bsdf.inputs['Specular IOR Level'].default_value = .1
        bsdf.inputs['Base Color'].default_value = (*flat, 1)
        if tex:
            links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
        if normal:
            relief = nodes.new('ShaderNodeTexImage')
            relief.image = image(normal, False)
            bump = nodes.new('ShaderNodeNormalMap')
            links.new(relief.outputs['Color'], bump.inputs['Color'])
            links.new(bump.outputs['Normal'], bsdf.inputs['Normal'])
        links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
        return
    # The game lights a card by its authored normal whichever side is seen and wherever the sun is: a diffuse
    # lobe for light on the seen side and a translucent one for light from behind, both about that normal.
    geometry = nodes.new('ShaderNodeNewGeometry')
    sign = nodes.new('ShaderNodeMapRange')
    sign.inputs['To Min'].default_value, sign.inputs['To Max'].default_value = 1., -1.
    links.new(geometry.outputs['Backfacing'], sign.inputs['Value'])
    authored = nodes.new('ShaderNodeVectorMath')
    authored.operation = 'SCALE'
    links.new(geometry.outputs['Normal'], authored.inputs[0])
    links.new(sign.outputs['Result'], authored.inputs['Scale'])
    behind = nodes.new('ShaderNodeVectorMath')
    behind.operation = 'SCALE'
    behind.inputs['Scale'].default_value = -1.
    links.new(authored.outputs[0], behind.inputs[0])
    diffuse, through = nodes.new('ShaderNodeBsdfDiffuse'), nodes.new('ShaderNodeBsdfTranslucent')
    links.new(authored.outputs[0], diffuse.inputs['Normal'])
    links.new(behind.outputs[0], through.inputs['Normal'])
    for lobe in (diffuse, through):
        lobe.inputs['Color'].default_value = (*flat, 1)
        if tex:
            links.new(tex.outputs['Color'], lobe.inputs['Color'])
    both = nodes.new('ShaderNodeAddShader')
    links.new(diffuse.outputs[0], both.inputs[0])
    links.new(through.outputs[0], both.inputs[1])
    surface = both.outputs[0]
    if tex:
        cut = nodes.new('ShaderNodeMath')
        cut.operation = 'GREATER_THAN'
        cut.inputs[1].default_value = .5
        links.new(tex.outputs['Alpha'], cut.inputs[0])
        mix = nodes.new('ShaderNodeMixShader')
        links.new(cut.outputs[0], mix.inputs[0])
        links.new(nodes.new('ShaderNodeBsdfTransparent').outputs[0], mix.inputs[1])
        links.new(surface, mix.inputs[2])
        surface = mix.outputs[0]
    links.new(surface, out.inputs['Surface'])


def flat(name: str, colour, roughness: float = .9):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    bsdf = mat.node_tree.nodes['Principled BSDF']
    bsdf.inputs['Base Color'].default_value = (*colour, 1)
    bsdf.inputs['Roughness'].default_value = roughness
    bsdf.inputs['Specular IOR Level'].default_value = .1
    return mat


def scene(args) -> dict:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    parts = [p | dict(cutout=p['material'].get('alphaMode') == 'MASK') for p in tree_glb.read(args.glb)]
    for part in parts:
        if part['cutout']:
            material(part['name'], args.boughs, args.boughs_normal, (.016, .026, .013), True)
        else:
            material(part['name'], args.bark, args.bark_normal, (.11, .085, .07), False)
    tree = fir.mesh_object(parts, 'tree')
    position = np.concatenate([p['position'] for p in parts])

    bpy.ops.mesh.primitive_plane_add(size=600)
    bpy.context.object.data.materials.append(flat('meadow', GRASS))
    # a man 1.8 m tall at the foot of the tree, as in the concept
    dark = flat('figure', (.02, .018, .016))
    bpy.ops.mesh.primitive_cylinder_add(radius=.2, depth=1.55, location=(-1.3, -1.2, .775))
    bpy.context.object.data.materials.append(dark)
    bpy.ops.mesh.primitive_uv_sphere_add(radius=.125, location=(-1.3, -1.2, 1.675))
    bpy.context.object.data.materials.append(dark)

    sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
    sun.data.energy, sun.data.angle, sun.data.color = 4.2, math.radians(.6), (1., .95, .86)
    sun.rotation_euler = Vector((.55, .42, -.72)).to_track_quat('-Z', 'Y').to_euler()
    sc.collection.objects.link(sun)
    world = bpy.data.worlds.new('sky')
    world.use_nodes = True
    background = world.node_tree.nodes['Background']
    background.inputs['Color'].default_value = (*SKY, 1)
    background.inputs['Strength'].default_value = 1.
    sc.world = world

    sc.render.engine = 'CYCLES'
    sc.cycles.samples = args.samples
    sc.cycles.use_denoising = True
    sc.cycles.max_bounces = 6
    sc.cycles.transparent_max_bounces = 48
    sc.view_settings.view_transform = 'Standard'
    sc.view_settings.look = 'None'
    sc.render.image_settings.file_format = 'PNG'
    try:
        prefs = bpy.context.preferences.addons['cycles'].preferences
        prefs.compute_device_type = 'METAL' if sys.platform == 'darwin' else 'NONE'
        prefs.get_devices()
        for device in prefs.devices:
            device.use = True
        if sys.platform == 'darwin':
            sc.cycles.device = 'GPU'
    except Exception as error:  # no device: the CPU renders the same picture
        print('cycles device:', error)
    return dict(tree=tree, top=float(position[:, 2].max()),
                reach=float(np.hypot(position[:, 0], position[:, 1]).max()))


def shot(name: str, place, target, fov_deg: float, size, out: Path) -> None:
    sc = bpy.context.scene
    data = bpy.data.cameras.new(name)
    data.sensor_fit = 'VERTICAL'
    data.angle = math.radians(fov_deg)
    data.clip_end = 2000
    cam = bpy.data.objects.new(name, data)
    cam.location = place
    cam.rotation_euler = (Vector(target) - Vector(place)).to_track_quat('-Z', 'Y').to_euler()
    sc.collection.objects.link(cam)
    sc.camera = cam
    sc.render.resolution_x, sc.render.resolution_y = size
    sc.render.filepath = str(out / f'{name}.png')
    bpy.ops.render.render(write_still=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--glb', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--boughs', type=Path)
    parser.add_argument('--boughs-normal', type=Path)
    parser.add_argument('--bark', type=Path)
    parser.add_argument('--bark-normal', type=Path)
    parser.add_argument('--views', default='concept,side,close,foot')
    parser.add_argument('--width', type=int, default=936)
    parser.add_argument('--samples', type=int, default=96)
    args = parser.parse_args(sys.argv[sys.argv.index('--') + 1:])
    args.out.mkdir(parents=True, exist_ok=True)
    info = scene(args)
    top, reach = info['top'], info['reach']
    tall = (args.width, round(args.width * 1024 / 936))
    wide = (round(args.width * 1.25), round(args.width * 1024 / 936))
    views = dict(
        concept=((1.5, -30., 1.7), (.6, 0., top * .47), 37., tall),
        side=((30., 1.5, 1.7), (0., -.6, top * .47), 37., tall),
        close=((reach * .55, -(reach * .8 + 3.), 1.7), (reach * .1, -reach * .45, 1.9), 62., wide),
        foot=((2.4, -2.4, 1.6), (0., 0., 1.05), 52., tall))
    for name in args.views.split(','):
        shot(name, *views[name], args.out)


if __name__ == '__main__':
    main()
