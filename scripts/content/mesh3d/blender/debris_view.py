"""Two looks at a model.debris.glb with its atlas: the pieces spread apart, and a close-up of the torn ones.

blender -b --factory-startup -P debris_view.py -- DEBRIS.glb ALBEDO.png OUT_DIR [--focus NAME ...]

exploded.png: every piece moved away from the centre of the whole thing (by 0.7 of its offset) and
seen from a three-quarter view above. closeup.png: the pieces whose names hold a --focus part
(default "-half-" and "-strip-") laid side by side on the ground, their new faces toward the camera.
Lit by a sun and a grey sky; base colour only, the atlas albedo, as the game's FractureView draws it.
"""
import math
import sys
from pathlib import Path

import bpy
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:]
debris, albedo, out = Path(argv[0]), Path(argv[1]), Path(argv[2])
focus = argv[argv.index('--focus') + 1:] if '--focus' in argv else ['-half-', '-strip-']
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
    tex.interpolation = 'Closest'
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

# 2. close-up: the split and broken pieces in a row on the ground, the rest hidden
chosen = [o for o in pieces if any(f in o.name for f in focus)]
chosen.sort(key=lambda o: o.name)
for obj in pieces:
    obj.hide_render = obj not in chosen
x = 0.0
for obj in chosen:
    obj.location = home[obj.name]
    points = [obj.matrix_world @ v.co for v in obj.data.vertices]
    lo = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
    hi = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
    size = hi - lo
    obj.rotation_mode = 'XYZ'
    # lay each piece flat with its long side along x: its thinnest extent up
    order = sorted(range(3), key=lambda k: size[k])
    if order[0] == 0:
        obj.rotation_euler = (0, math.radians(90), 0)
    elif order[0] == 1:
        obj.rotation_euler = (math.radians(90), 0, 0)
    bpy.context.view_layer.update()
    points = [obj.matrix_world @ v.co for v in obj.data.vertices]
    lo = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
    hi = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
    if hi.y - lo.y > hi.x - lo.x:
        obj.rotation_euler.z += math.radians(90)
        bpy.context.view_layer.update()
        points = [obj.matrix_world @ v.co for v in obj.data.vertices]
        lo = Vector((min(p.x for p in points), min(p.y for p in points), min(p.z for p in points)))
        hi = Vector((max(p.x for p in points), max(p.y for p in points), max(p.z for p in points)))
    obj.location += Vector((-lo.x - (hi.x - lo.x) / 2, x - lo.y, -lo.z))
    x += (hi.y - lo.y) + 0.05
bpy.context.view_layer.update()
ground.location.z = -0.001
points = [o.matrix_world @ v.co for o in chosen for v in o.data.vertices]
middle = Vector((sum(p.x for p in points) / len(points), sum(p.y for p in points) / len(points), 0))
look(middle, 2.2, -20, 45, lens=40)
scene.render.filepath = str(out / 'closeup.png')
bpy.ops.render.render(write_still=True)
print('DEBRIS_VIEW', out)
