"""Blender scene of a material made of pieces (scripts/content/piece_bake.py) and its passes.

    Blender -b --factory-startup --python-exit-code 1 -P piece_scene.py -- LAYOUT.json OUTDIR RESOLUTION SAMPLES \
        [NAME=SURFACE.png ...] [sun=E] [view=street]

Every piece of the layout becomes a solid: its top face is the top plane plus its terms, its side
walls go down to the bottom plane. Mesh points come from LAYOUT.npz. A piece with "grid" is a
surface instead: a regular grid of heights (LAYOUT.npz key h<id>, rows along y) over the top plane,
for a face shaped too finely for terms (hewn timber with its cracks). Copies across the period's edges
make the render tile. A surface NAME=PNG gives the colour of the pieces on that surface (UV in metres,
turned by each piece's grain lean); a surface without an image keeps its plain colour.

view=texture (default): an orthographic camera looks at one period up the slope, tilted by the
layout's camera tilt from straight down, the way the roof is seen from the street. The render is
cos(tilt) as tall as the period and still covers exactly one period, so it tiles once stretched back.
OUTDIR/passes.exr holds the colour (DiffCol), ambient occlusion at two distances (AO, crevice AOV),
the shading normal, the position (height in Z), the piece (IndexOB, id + 1), the light of the sky
over the pitched roof (DiffDir + DiffInd) and the lit model (Combined). No sun unless sun=E is given.

view=street: the model tiled three by three and tilted to the roof pitch, seen in perspective from
the street under sky and sun, wide and close (close=X,Y: the point of the square to look at):
OUTDIR/street-wide.png and street-close.png. For judging the block-out. pitch=DEGREES and eaves=METRES
override the tilt and the height of the model's lower edge (90: a wall, as for timber); stretch=K
stretches it along y as the game stretches a texture laid out in its own proportions.
"""
import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Vector, geometry

argv = sys.argv[sys.argv.index('--') + 1:]
layout_path = Path(argv[0])
spec = json.loads(layout_path.read_text())
out_dir = Path(argv[1])
resolution, samples = int(argv[2]), int(argv[3])
options = dict(a.split('=', 1) for a in argv[4:])
view = options.pop('view', 'texture')
sun_energy = float(options.pop('sun', 0))
close_at = options.pop('close', None)              # street view: x,y of the close shot on the roof
street_pitch = options.pop('pitch', None)          # street view: tilt of the model, degrees
street_eaves = float(options.pop('eaves', 4.))     # street view: height of the model's lower edge
street_stretch = float(options.pop('stretch', 1.))  # street view: the model stretched along y, as in game
images = options                                   # what is left: surface name -> image
npz = layout_path.with_suffix('.npz')
points = np.load(npz) if npz.exists() else {}
px, py = spec['period']
camera_spec, sky_spec = spec['camera'], spec['sky']
TILT = math.radians(camera_spec['tilt'])
PITCH = math.radians(sky_spec['pitch'])
AO_DISTANCE, CREVICE_DISTANCE = spec['ao']['distance'], spec['ao']['crevice']

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = 'CYCLES'
try:
    prefs = bpy.context.preferences.addons['cycles'].preferences
    prefs.compute_device_type = 'METAL' if sys.platform == 'darwin' else 'OPTIX'
    prefs.refresh_devices()
    for device in prefs.devices:
        device.use = True
    scene.cycles.device = 'GPU'
except Exception as error:   # no GPU backend: the CPU renders the same passes
    print('piece_scene: CPU render,', error)
    scene.cycles.device = 'CPU'
scene.cycles.samples = samples
scene.cycles.use_adaptive_sampling = False
scene.cycles.use_denoising = view == 'street'
scene.view_settings.view_transform = 'Standard' if view == 'texture' else 'AgX'

# Sky over a pitched roof: in the roof's own frame the zenith leans towards the ridge (+Y) by the
# pitch, and below the true horizon there is only dim ground. In the street view the model itself is
# tilted, so the sky is the true one.
world = bpy.data.worlds.new('sky')
scene.world = world
world.use_nodes = True
wn, wl = world.node_tree.nodes, world.node_tree.links
coords = wn.new('ShaderNodeTexCoord')
zenith = wn.new('ShaderNodeVectorMath')
zenith.operation = 'DOT_PRODUCT'
zenith.inputs[1].default_value = (0, math.sin(PITCH), math.cos(PITCH)) if view == 'texture' else (0, 0, 1)
wl.new(coords.outputs['Generated'], zenith.inputs[0])
sky = wn.new('ShaderNodeMath')
sky.operation = 'MAXIMUM'
sky.inputs[1].default_value = sky_spec['ground']
wl.new(zenith.outputs['Value'], sky.inputs[0])
wl.new(sky.outputs['Value'], wn['Background'].inputs['Strength'])
wn['Background'].inputs['Color'].default_value = (1, 1, 1, 1) if view == 'texture' else (.62, .7, .82, 1)
world.light_settings.distance = AO_DISTANCE

layer = scene.view_layers[0]
layer.use_pass_diffuse_color = True
layer.use_pass_ambient_occlusion = True
layer.use_pass_normal = True
layer.use_pass_position = True
layer.use_pass_object_index = True
layer.use_pass_diffuse_direct = True
layer.use_pass_diffuse_indirect = True
aov = layer.aovs.add()
aov.name, aov.type = 'crevice', 'VALUE'


def node(nodes, kind, **props):
    n = nodes.new(kind)
    for key, value in props.items():
        setattr(n, key, value)
    return n


def surface_material(surface):
    mat = bpy.data.materials.new(surface['name'])
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    bsdf = node(nodes, 'ShaderNodeBsdfDiffuse')
    out = node(nodes, 'ShaderNodeOutputMaterial')
    links.new(bsdf.outputs['BSDF'], out.inputs['Surface'])
    path = images.get(surface['name'])
    if path:
        tex = node(nodes, 'ShaderNodeTexImage', interpolation='Cubic', extension='REPEAT')
        tex.image = bpy.data.images.load(path)
        tex.image.colorspace_settings.name = 'sRGB'
        links.new(tex.outputs['Color'], bsdf.inputs['Color'])
        if surface.get('bump'):
            # relief of the surface from its brightness: dark grooves, light ridges of the grain
            bump = node(nodes, 'ShaderNodeBump')
            bump.inputs['Distance'].default_value = surface['bump']
            grey = node(nodes, 'ShaderNodeRGBToBW')
            links.new(tex.outputs['Color'], grey.inputs['Color'])
            links.new(grey.outputs['Val'], bump.inputs['Height'])
            links.new(bump.outputs['Normal'], bsdf.inputs['Normal'])
    else:
        bsdf.inputs['Color'].default_value = (*surface['colour'], 1)
    ao = node(nodes, 'ShaderNodeAmbientOcclusion', samples=16, only_local=False)
    ao.inputs['Distance'].default_value = CREVICE_DISTANCE
    aov_out = node(nodes, 'ShaderNodeOutputAOV', aov_name='crevice')
    links.new(ao.outputs['AO'], aov_out.inputs['Value'])
    return mat


materials = [surface_material(s) for s in spec['surfaces']]


def linear(form, x, y):
    return form[0] * x + form[1] * y + form[2]


def face_height(piece, xy):
    """Height of the top face: the top plane and the piece's terms.
    bow: a bulge across the piece (negative: cupped); ripple: waves of a direction, length and
    phase, the phase bent along the other axis (split grain, cleavage); twist: the corner where both ACROSS and ALONG reach 1 lifted by AMP;
    round: the edge falls by DROP metres over WIDTH, the hand-dressed or weathered edge.
    A term with a mask [mx, my, m0] applies where clip(mx·x + my·y + m0, 0, 1) says so."""
    x, y = xy[:, 0], xy[:, 1]
    a, bx, by = piece['top']
    z = a + bx * x + by * y
    for term in piece['terms']:
        mask = np.clip(linear(term['mask'], x, y), 0, 1) if term.get('mask') else 1.
        kind = term['kind']
        if kind == 'bow':
            s = np.clip(linear(term['across'], x, y), 0, 1)
            z = z + term['amp'] * (1 - (2 * s - 1) ** 2) * mask
        elif kind == 'ripple':
            dx, dy = term['dir']
            phase = term['phase'] + term.get('bend', 0.) * np.sin(
                2 * math.pi * (-dy * x + dx * y) / term.get('bend_length', 1.) + term.get('bend_phase', 0.))
            z = z + term['amp'] * np.sin(2 * math.pi * (dx * x + dy * y) / term['length'] + phase) * mask
        elif kind == 'twist':
            s = np.clip(linear(term['across'], x, y), 0, 1)
            t = np.clip(linear(term['along'], x, y), 0, 1)
            z = z + term['amp'] * s * t * mask
        elif kind == 'round':
            ring = np.asarray(piece['outline'])
            start, step = ring, np.roll(ring, -1, axis=0) - ring
            t = np.clip(((xy[:, None, :] - start[None]) * step[None]).sum(-1) / ((step ** 2).sum(-1)[None] + 1e-12), 0, 1)
            nearest = start[None] + t[..., None] * step[None]
            edge = np.sqrt(((xy[:, None, :] - nearest) ** 2).sum(-1)).min(axis=1)
            z = z - term['drop'] * np.clip(1 - edge / term['width'], 0, 1) ** 2 * mask
        else:
            raise ValueError(f'unknown term {kind}')
    return z


roof = bpy.data.objects.new('roof', None)          # the street view tilts the model by this
scene.collection.objects.link(roof)


def place(piece, bm):
    """UV of a piece's mesh by its surface and grain lean, and its objects with the copies."""
    surface = spec['surfaces'][piece['surface']]
    metres = surface['metres']
    ou, ov, lean = piece['uv']
    c, s = math.cos(lean), math.sin(lean)
    uv = bm.loops.layers.uv.new('UVMap')
    for face in bm.faces:
        for loop in face.loops:
            x, y = loop.vert.co.x, loop.vert.co.y
            loop[uv].uv = ((x * c + y * s) / metres + ou, (-x * s + y * c) / metres + ov)
    mesh = bpy.data.meshes.new(f'piece{piece["id"]}')
    bm.to_mesh(mesh)
    bm.free()
    mesh.validate()
    mesh.materials.append(materials[piece['surface']])
    # the street view shows the texture tiled three by three, as on a roof
    offsets = piece['copies'] if view == 'texture' else [(i * px, j * py) for i in (-1, 0, 1) for j in (-1, 0, 1)]
    for dx, dy in offsets:
        obj = bpy.data.objects.new(f'piece{piece["id"]}_{dx:+.2f}_{dy:+.2f}', mesh)
        obj.location = (dx, dy, 0)
        obj.pass_index = piece.get('index', piece['id'] + 1)
        obj.parent = roof
        scene.collection.objects.link(obj)


def grid_mesh(piece):
    """The top face of a grid piece: its heights over the top plane, rows along y."""
    heights = points[f'h{piece["id"]}'].astype(np.float64)
    x0, y0, dx, dy = piece['grid']
    rows, cols = heights.shape
    gx, gy = np.meshgrid(x0 + dx * np.arange(cols), y0 + dy * np.arange(rows))
    a, bx, by = piece['top']
    z = a + bx * gx + by * gy + heights
    bm = bmesh.new()
    verts = [bm.verts.new((x, y, h)) for x, y, h in zip(gx.ravel(), gy.ravel(), z.ravel())]
    for r in range(rows - 1):
        for c in range(cols - 1):
            i = r * cols + c
            f = bm.faces.new((verts[i], verts[i + 1], verts[i + cols + 1], verts[i + cols]))
            f.smooth = True
    return bm


for piece in spec['pieces']:
    if 'grid' in piece:
        bm = grid_mesh(piece)
        place(piece, bm)
        continue
    ring = piece['outline']
    n = len(ring)
    inner = points[f'p{piece["id"]}'].astype(np.float64) if f'p{piece["id"]}' in points else np.zeros((0, 2))
    flat = [Vector((x, y)) for x, y in ring] + [Vector((x, y)) for x, y in inner]
    coords, _edges, faces, origin, _oe, _of = geometry.delaunay_2d_cdt(flat, [], [list(range(n))], 1, 1e-7)
    xy = np.array([[c.x, c.y] for c in coords])
    z = face_height(piece, xy)
    bm = bmesh.new()
    top = [bm.verts.new((x, y, h)) for (x, y), h in zip(xy, z)]
    for face in faces:
        f = bm.faces.new([top[i] for i in face])
        f.smooth = True
    bm.normal_update()
    for f in bm.faces:
        if f.normal.z < 0:
            f.normal_flip()
    ring_top = {}
    for out_index, sources in enumerate(origin):
        for source in sources:
            if source < n:
                ring_top[source] = top[out_index]
    a, bx, by = piece['bottom']
    bottom = [bm.verts.new((x, y, a + bx * x + by * y)) for x, y in ring]
    for i in range(n):
        j = (i + 1) % n
        wall = (bottom[i], bottom[j], ring_top[j], ring_top[i])
        bm.faces.new(wall if ring_top[i] is not ring_top[j] else wall[:3])   # merged by the triangulation
    if piece.get('closed'):
        bm.faces.new(bottom[::-1])
    place(piece, bm)

void = spec.get('void')
if void:
    # the dark under the roof: what a hole shows past the laths and rafters
    mesh = bpy.data.meshes.new('void')
    m = 3. if view == 'texture' else px
    mesh.from_pydata([(-m, -m, void['z']), (px + m, -m, void['z']), (px + m, py + m, void['z']), (-m, py + m, void['z'])],
                     [], [(0, 1, 2, 3)])
    dark = bpy.data.materials.new('void')
    dark.use_nodes = True
    dark.node_tree.nodes['Principled BSDF'].inputs['Base Color'].default_value = (0, 0, 0, 1)
    dark.node_tree.nodes['Principled BSDF'].inputs['Roughness'].default_value = 1.
    mesh.materials.append(dark)
    obj = bpy.data.objects.new('void', mesh)
    obj.pass_index = 0
    obj.parent = roof
    scene.collection.objects.link(obj)

if sun_energy or view == 'street':
    sun = bpy.data.lights.new('sun', 'SUN')
    sun.energy, sun.angle = sun_energy or 3., math.radians(2)
    sun_obj = bpy.data.objects.new('sun', sun)
    # texture view: over the ridge, a little from the left; street view: behind the viewer, left
    sun_obj.rotation_euler = ((math.radians(32 - 90), 0, math.radians(35)) if view == 'texture'
                              else (math.radians(50), 0, math.radians(-30)))
    scene.collection.objects.link(sun_obj)


def shoot(name, location, target, lens):
    camera = bpy.data.cameras.new(name)
    camera.lens = lens
    eye = bpy.data.objects.new(name, camera)
    eye.location = location
    direction = Vector(target) - Vector(location)
    eye.rotation_euler = direction.to_track_quat('-Z', 'Y').to_euler()
    scene.collection.objects.link(eye)
    scene.camera = eye
    scene.render.filepath = str(out_dir / f'{name}.png')
    bpy.ops.render.render(write_still=True)


if view == 'texture':
    scene.render.resolution_x = resolution
    scene.render.resolution_y = round(resolution * py / px * math.cos(TILT))
    scene.render.resolution_percentage = 100
    camera = bpy.data.cameras.new('top')
    camera.type, camera.ortho_scale, camera.sensor_fit = 'ORTHO', px, 'HORIZONTAL'
    camera.clip_start, camera.clip_end = .01, 20
    cam = bpy.data.objects.new('top', camera)
    cam.location = (px / 2, py / 2 - 5 * math.sin(TILT), .05 + 5 * math.cos(TILT))
    cam.rotation_euler = (TILT, 0, 0)
    scene.collection.objects.link(cam)
    scene.camera = cam
    scene.render.image_settings.media_type = 'MULTI_LAYER_IMAGE'   # Blender 5: multilayer EXR
    scene.render.image_settings.file_format = 'OPEN_EXR_MULTILAYER'
    scene.render.image_settings.color_depth = '32'
    scene.render.filepath = str(out_dir / 'passes.exr')
    bpy.ops.render.render(write_still=True)
    print('piece_scene: wrote', out_dir / 'passes.exr')
else:
    # the eaves along X at 4 m, the slope rising away from the viewer
    eaves = street_eaves
    pitch = math.radians(float(street_pitch)) if street_pitch is not None else PITCH
    roof.location = (0, 0, eaves)
    roof.rotation_euler = (pitch, 0, 0)
    roof.scale = (1, street_stretch, 1)
    scene.render.resolution_x, scene.render.resolution_y = resolution, round(resolution * 9 / 16)
    scene.render.image_settings.file_format = 'PNG'
    up = (0, math.cos(pitch), math.sin(pitch))

    def on_roof(x, y):
        y *= street_stretch
        return (x, y * up[1], eaves + y * up[2])
    close = [float(v) for v in (close_at or f'{px / 3},{py / 2}').split(',')]
    shoot('street-wide', (px / 2, -6., 2.2), on_roof(px / 2, py * .5), 26)
    target = on_roof(*close)
    shoot('street-close', (close[0] + .8, target[1] - 4.2, target[2] - 2.2), target, 40)
    print('piece_scene: wrote', out_dir / 'street-wide.png')
