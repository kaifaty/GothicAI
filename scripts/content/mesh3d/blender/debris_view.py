"""Looks at a model.debris.glb with its atlas: the pieces spread apart, and close-ups of the torn boards.

blender -b --factory-startup -P debris_view.py -- DEBRIS.glb ALBEDO.png OUT_DIR

exploded.png: every piece moved away from the centre of the whole thing (by 0.7 of its offset) and
seen from a three-quarter view above. closeup-<board>.png, one per board split into "-strip-" pieces or
broken into "-half-" pieces: only that board, in its place, its pieces drawn 3 cm apart, seen close
and at a slant where they part, so the split faces or the torn edge face the camera.
Lit by a sun and a grey sky; base colour only, the atlas albedo, as the game's FractureView draws it.
"""
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector
import numpy as np

argv = sys.argv[sys.argv.index('--') + 1:]
debris, albedo, out = Path(argv[0]), Path(argv[1]), Path(argv[2])
out.mkdir(parents=True, exist_ok=True)

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=str(debris))
image = bpy.data.images.load(str(albedo))
for material in bpy.data.materials:
    material.use_nodes = True
    nodes = material.node_tree.nodes
    bsdf = next(n for n in nodes if n.type == 'BSDF_PRINCIPLED')
    tex = nodes.new('ShaderNodeTexImage')
    tex.image = image
    tex.interpolation = 'Linear'
    material.node_tree.links.new(tex.outputs['Color'], bsdf.inputs['Base Color'])
    bsdf.inputs['Roughness'].default_value = 0.8
    bsdf.inputs['Metallic'].default_value = 0.0

pieces = [o for o in bpy.context.scene.objects if o.type == 'MESH']


def centre(obj):
    points = [obj.matrix_world @ v.co for v in obj.data.vertices]
    return sum(points, Vector()) / len(points)


whole = sum((centre(o) for o in pieces), Vector()) / len(pieces)
home = {o.name: o.location.copy() for o in pieces}

scene = bpy.context.scene
scene.render.engine = 'BLENDER_EEVEE' if 'BLENDER_EEVEE' in {e.identifier for e in
                                                             bpy.types.RenderSettings.bl_rna.properties['engine'].enum_items} \
    else 'BLENDER_EEVEE_NEXT'
scene.eevee.taa_render_samples = 32
scene.render.resolution_x, scene.render.resolution_y = 1600, 1100
scene.view_settings.view_transform = 'Standard'
world = bpy.data.worlds.new('sky')
world.use_nodes = True
world.node_tree.nodes['Background'].inputs['Color'].default_value = (0.35, 0.37, 0.4, 1)
world.node_tree.nodes['Background'].inputs['Strength'].default_value = 1.2
scene.world = world
sun = bpy.data.objects.new('sun', bpy.data.lights.new('sun', 'SUN'))
sun.data.energy = 3.5
sun.rotation_euler = (math.radians(50), 0, math.radians(35))
scene.collection.objects.link(sun)
camera = bpy.data.objects.new('camera', bpy.data.cameras.new('camera'))
scene.collection.objects.link(camera)
scene.camera = camera
bpy.ops.mesh.primitive_plane_add(size=20, location=(0, 0, 0))
ground = bpy.context.object
ground_mat = bpy.data.materials.new('ground')
ground_mat.use_nodes = True
ground_mat.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value = (0.12, 0.11, 0.1, 1)
ground.data.materials.append(ground_mat)


def look(target, distance, azimuth, elevation, lens=50):
    direction = Vector((math.cos(math.radians(elevation)) * math.cos(math.radians(azimuth)),
                        math.cos(math.radians(elevation)) * math.sin(math.radians(azimuth)),
                        math.sin(math.radians(elevation))))
    camera.location = target + direction * distance
    camera.rotation_euler = (target - camera.location).to_track_quat('-Z', 'Y').to_euler()
    camera.data.lens = lens


# 1. exploded: each piece away from the centre, lifted so that nothing goes under the ground
for obj in pieces:
    offset = centre(obj) - whole
    obj.location = home[obj.name] + offset * 0.7 + Vector((0, 0, 0.45))
ground.location.z = min((o.matrix_world @ v.co).z for o in pieces for v in o.data.vertices) - 0.01
look(whole + Vector((0, 0, 0.5)), 4.6, -55, 32)
scene.render.filepath = str(out / 'exploded.png')
bpy.ops.render.render(write_still=True)

# 2. close-ups: each torn board alone, in its place, its pieces 3 cm apart
for obj in pieces:
    obj.location = home[obj.name]
bpy.context.view_layer.update()
boards = {}
for obj in pieces:
    for mark in ('-half-', '-strip-'):
        if mark in obj.name:
            boards.setdefault(obj.name.split(mark)[0], []).append(obj)
ground.hide_render = True
for board, group in sorted(boards.items()):
    for obj in pieces:
        obj.hide_render = obj not in group
    points = [o.matrix_world @ v.co for o in group for v in o.data.vertices]
    middle = sum(points, Vector()) / len(points)
    # the board's axes: length, width and thickness from the spread of its points
    array = np.array([tuple(p - middle) for p in points])
    _, _, axes = np.linalg.svd(array, full_matrices=False)
    length, width, normal = (Vector(tuple(a)) for a in axes)
    light = sun.matrix_world.to_3x3() @ Vector((0, 0, 1))   # toward the sun: look at the lit side
    normal = normal if normal.dot(light) > 0 else -normal
    width = width if width.dot(light) > 0 else -width
    centres = {o.name: centre(o) for o in group}
    for obj in group:
        away = centres[obj.name] - middle
        away = away - normal * away.dot(normal)
        obj.location = home[obj.name] + away.normalized() * 0.015
    bpy.context.view_layer.update()
    if '-half-' in group[0].name:   # look at the tear: where the halves meet along the length
        first = [((group[0].matrix_world @ v.co) - middle).dot(length) for v in group[0].data.vertices]
        second = [((group[1].matrix_world @ v.co) - middle).dot(length) for v in group[1].data.vertices]
        tear = (max(first) + min(second)) / 2 if sum(first) < sum(second) else (min(first) + max(second)) / 2
        target = middle + length * tear
        distance, aim = 0.55, normal + width * 0.5 + length * 0.35
    else:                            # look along the strips from one end
        along = [((o.matrix_world @ v.co) - middle).dot(length) for o in group for v in o.data.vertices]
        target = middle + length * (max(along) - 0.15)
        length = length if length.dot(target - middle) > 0 else -length
        distance, aim = 0.5, normal * 0.7 + length * 0.8 + width * 0.6
    aim.normalize()
    camera.location = target + aim * distance
    camera.rotation_euler = (target - camera.location).to_track_quat('-Z', 'Y').to_euler()
    camera.data.lens = 35
    scene.render.filepath = str(out / f'closeup-{board}.png')
    bpy.ops.render.render(write_still=True)
    for obj in group:
        obj.location = home[obj.name]
print('DEBRIS_VIEW', out)
