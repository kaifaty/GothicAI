"""Preview a tiled roof or wall material as it reads from the street: albedo, tangent normal and
roughness on a pitched plane, a sky and a sun, Cycles GGX with the engine's fade of the highlight
with roughness.

    Blender -b --factory-startup -P material_preview.py -- ALBEDO NORMAL ROUGHNESS SIZE_M OUT_PREFIX

SIZE_M is the edge of one texture square in metres, or WxH for a texture that is not square. Two images are written: OUT_PREFIX-front.png with
the sun behind the viewer, and OUT_PREFIX-against.png with the sun high behind the roof, where wet,
smooth parts glint. The normal map is OpenGL style (X right, Y up), as the pipeline writes it.
"""
import math
import sys

import bmesh
import bpy

PITCH = math.radians(45)
TILES = 2
EAVES = 4.            # metres above the street
SAMPLES = 48

albedo, normal, roughness, size_m, out_prefix = sys.argv[sys.argv.index('--') + 1:]
size_x, size_y = (float(v) for v in size_m.split('x')) if 'x' in size_m else (float(size_m),) * 2
# a plane of TILES texture heights up the slope and as many texture widths as make it about square
tiles_x = max(1, round(TILES * size_y / size_x))
width, length = tiles_x * size_x, TILES * size_y

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.samples = SAMPLES
scene.cycles.use_denoising = True
scene.render.resolution_x, scene.render.resolution_y = 1280, 720
scene.view_settings.view_transform = 'AgX'

world = bpy.data.worlds.new('sky')
scene.world = world
world.use_nodes = True
wn, wl = world.node_tree.nodes, world.node_tree.links
coords = wn.new('ShaderNodeTexCoord')
split = wn.new('ShaderNodeSeparateXYZ')
ramp = wn.new('ShaderNodeValToRGB')
ramp.color_ramp.elements[0].color = (.55, .6, .66, 1)    # horizon
ramp.color_ramp.elements[1].color = (.22, .32, .5, 1)    # zenith
wl.new(coords.outputs['Generated'], split.inputs['Vector'])
wl.new(split.outputs['Z'], ramp.inputs['Fac'])
wl.new(ramp.outputs['Color'], wn['Background'].inputs['Color'])
wn['Background'].inputs['Strength'].default_value = 1.

mat = bpy.data.materials.new('material')
mat.use_nodes = True
nodes, links = mat.node_tree.nodes, mat.node_tree.links
bsdf = nodes['Principled BSDF']


def image(path, colour):
    tex = nodes.new('ShaderNodeTexImage')
    tex.image = bpy.data.images.load(path)
    tex.image.colorspace_settings.name = 'sRGB' if colour else 'Non-Color'
    tex.interpolation = 'Cubic'
    return tex


links.new(image(albedo, True).outputs['Color'], bsdf.inputs['Base Color'])
rough = image(roughness, False)
links.new(rough.outputs['Color'], bsdf.inputs['Roughness'])
# The engine fades a dielectric's highlight and sky reflection with roughness: full below 0.5,
# none above 0.8 (materialSpecularWeight). Do the same, or matte stone shows a grey sky film.
weight = nodes.new('ShaderNodeMapRange')
weight.clamp = True
weight.inputs['From Min'].default_value, weight.inputs['From Max'].default_value = .8, .5
weight.inputs['To Min'].default_value, weight.inputs['To Max'].default_value = 0., .5
links.new(rough.outputs['Color'], weight.inputs['Value'])
links.new(weight.outputs['Result'], bsdf.inputs['Specular IOR Level'])
nmap = nodes.new('ShaderNodeNormalMap')
links.new(image(normal, False).outputs['Color'], nmap.inputs['Color'])
links.new(nmap.outputs['Normal'], bsdf.inputs['Normal'])

bm = bmesh.new()
corners = [bm.verts.new(c) for c in ((0, 0, 0), (width, 0, 0), (width, length, 0), (0, length, 0))]
face = bm.faces.new(corners)
uv = bm.loops.layers.uv.new('UVMap')
for loop, coord in zip(face.loops, ((0, 0), (tiles_x, 0), (tiles_x, TILES), (0, TILES))):
    loop[uv].uv = coord
mesh = bpy.data.meshes.new('roof')
bm.to_mesh(mesh)
bm.free()
mesh.materials.append(mat)
roof = bpy.data.objects.new('roof', mesh)
roof.location = (-width / 2, 0, EAVES)
roof.rotation_euler = (PITCH, 0, 0)          # local +Y (texture up) runs up the slope to the ridge
scene.collection.objects.link(roof)

camera = bpy.data.cameras.new('eye')
camera.lens = 35
eye = bpy.data.objects.new('eye', camera)
eye.location = (1.5, -9, 1.7)
target = (0, length * math.cos(PITCH) * .35, EAVES + length * math.sin(PITCH) * .35)
direction = [t - e for t, e in zip(target, eye.location)]
eye.rotation_euler = (math.atan2(math.hypot(direction[0], direction[1]), -direction[2]), 0,
                      math.atan2(direction[1], direction[0]) - math.pi / 2)
scene.collection.objects.link(eye)
scene.camera = eye

sun = bpy.data.lights.new('sun', 'SUN')
sun.angle = math.radians(1)
sun_obj = bpy.data.objects.new('sun', sun)
scene.collection.objects.link(sun_obj)
for name, elevation, azimuth, energy in (('front', 40, 200, 3.), ('against', 62, 20, 3.5)):
    # azimuth 0: the sun stands behind the roof (towards +Y), 180: behind the viewer
    sun.energy = energy
    sun_obj.rotation_euler = (math.radians(90 - elevation), 0, math.radians(azimuth + 180))
    scene.render.filepath = f'{out_prefix}-{name}.png'
    bpy.ops.render.render(write_still=True)
print('material_preview: wrote', out_prefix)
