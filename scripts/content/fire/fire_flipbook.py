"""Simulate a small wood fire in Mantaflow and render its flame for an atlas flipbook (fire_atlas.py packs it).

    blender -b --factory-startup -P fire_flipbook.py -- OUT_DIR [--res 160] [--frames 200] [--start 41] [--cpu]
        [--ramp 'LO HI GAMMA' ...]

reports/engine/2026-11-01-fire-rendering.md, section 8. A gas domain 0.9 x 0.9 x 1.6 m over three crossed logs
of a cooking fire (0.44 m long); fuel enters from their skin through noise, so the tongues break up and flicker instead of
standing as one cone. 48 frames per second of sim, so pairs of frames average into motion blur at 24.
The flame is rendered alone on black (it adds onto the scene): emission from the `flame` grid, its colour that of
a black body by the `temperature` grid, 800-2000 K. Orthographic side view, 256 x 512 per frame, linear half-float
EXR, so the hot core keeps its range and fire_atlas.py picks the shoulder; frames START..FRAMES to OUT_DIR/frames-<ramp>/,
the sim cache stays in OUT_DIR/cache.
"""
import argparse
import sys
from pathlib import Path

import bpy

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
parser = argparse.ArgumentParser()
parser.add_argument("out", type=Path)
parser.add_argument("--res", type=int, default=160)
parser.add_argument("--frames", type=int, default=200)
parser.add_argument("--start", type=int, default=41)
parser.add_argument("--cpu", action="store_true")
# emission ramps over the `flame` grid, LO HI GAMMA each: flame below LO is dark, above HI full, so the haze of a
# little flame (r2: a soft orange fog up to the frame top) drops out and the tongues get an edge; one bake,
# frames of each ramp to OUT_DIR/frames-<i>/
parser.add_argument("--ramp", action="append", default=[])
args = parser.parse_args(argv)
out = args.out.resolve()

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 48
scene.frame_start, scene.frame_end = 1, args.frames

# --- domain: 0.9 x 0.9 x 1.6 m standing on z=0 -------------------------------------------------------------
bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 0, 0.8))
domain = bpy.context.object
domain.name = "Domain"
domain.scale = (0.9, 0.9, 1.6)
bpy.ops.object.transform_apply(scale=True)
fluid = domain.modifiers.new("Fluid", 'FLUID')
fluid.fluid_type = 'DOMAIN'
ds = fluid.domain_settings
ds.domain_type = 'GAS'
ds.resolution_max = args.res
ds.cache_type = 'ALL'
ds.cache_directory = str(out / "cache")
ds.cache_frame_start, ds.cache_frame_end = 1, args.frames
ds.use_adaptive_domain = False
ds.alpha = -0.1           # smoke density lifts a little (buoyancy by density)
ds.beta = 1.0             # heat lift: r2 at 1.6 ran the flame out of a 1.6 m frame over 0.44 m logs
ds.vorticity = 0.4        # curls that tear the tongues
ds.burning_rate = 0.95    # fuel burns away fast: tongues end in tips, not a column
ds.flame_vorticity = 1.2
ds.flame_smoke = 0.4
ds.flame_ignition = 1.25
ds.flame_max_temp = 3.0
ds.use_dissolve_smoke = True
ds.dissolve_speed = 25
ds.use_noise = False

# --- fuel: three logs crossed on the ground, fuel from their skin through moving noise ----------------------
# A disc fed fuel over its whole area burned as one slab (r1): seen from the side the emission of 0.36 m of dense
# flame summed into a flat white band. Logs leave gaps, so the base breaks into separate tongues.
noise = bpy.data.textures.new("FuelNoise", 'CLOUDS')
noise.noise_scale = 0.05
noise.noise_depth = 2
for i, (yaw, tilt) in enumerate(((0.0, 0.10), (2.09, -0.08), (4.19, 0.05))):
    bpy.ops.mesh.primitive_cylinder_add(vertices=12, radius=0.045, depth=0.44,
                                        location=(0, 0, 0.06 + 0.03 * i), rotation=(1.5708 + tilt, 0, yaw))
    log = bpy.context.object
    log.name = f"Log{i}"
    flow = log.modifiers.new("Fluid", 'FLUID')
    flow.fluid_type = 'FLOW'
    fs = flow.flow_settings
    fs.flow_type = 'FIRE'
    fs.flow_behavior = 'INFLOW'
    fs.flow_source = 'MESH'
    fs.surface_distance = 0.6
    fs.fuel_amount = 1.0
    fs.temperature = 1.6
    fs.use_texture = True
    fs.noise_texture = noise
    fs.texture_map_type = 'AUTO'
    fs.texture_size = 0.5
    fs.texture_offset = 0.0
    fs.keyframe_insert("texture_offset", frame=1)
    fs.texture_offset = 6.0 + i  # the noise drifts through the logs: fuel comes in moving patches
    fs.keyframe_insert("texture_offset", frame=args.frames)
    log.hide_render = True

# --- flame material: emission by `flame`, colour by `temperature` -----------------------------------------
mat = bpy.data.materials.new("Flame")
mat.use_nodes = True
nodes, links = mat.node_tree.nodes, mat.node_tree.links
nodes.clear()
out_node = nodes.new('ShaderNodeOutputMaterial')
emission = nodes.new('ShaderNodeEmission')
flame = nodes.new('ShaderNodeAttribute')
flame.attribute_name = "flame"
heat = nodes.new('ShaderNodeAttribute')
heat.attribute_name = "temperature"
to_kelvin = nodes.new('ShaderNodeMapRange')
to_kelvin.inputs["From Min"].default_value = 0.0
to_kelvin.inputs["From Max"].default_value = 3.0
to_kelvin.inputs["To Min"].default_value = 800.0
to_kelvin.inputs["To Max"].default_value = 2000.0
blackbody = nodes.new('ShaderNodeBlackbody')
strength = nodes.new('ShaderNodeMath')
strength.operation = 'MULTIPLY'
strength.inputs[1].default_value = 4.0
links.new(heat.outputs["Fac"], to_kelvin.inputs["Value"])
links.new(to_kelvin.outputs["Result"], blackbody.inputs["Temperature"])
links.new(blackbody.outputs["Color"], emission.inputs["Color"])
ramp = nodes.new('ShaderNodeMapRange')
ramp.clamp = True
sharp = nodes.new('ShaderNodeMath')
sharp.operation = 'POWER'
links.new(flame.outputs["Fac"], ramp.inputs["Value"])
links.new(ramp.outputs["Result"], sharp.inputs[0])
links.new(sharp.outputs["Value"], strength.inputs[0])
links.new(strength.outputs["Value"], emission.inputs["Strength"])
links.new(emission.outputs["Emission"], out_node.inputs["Volume"])
domain.data.materials.append(mat)

# --- camera and render --------------------------------------------------------------------------------------
cam_data = bpy.data.cameras.new("Cam")
cam_data.type = 'ORTHO'
cam_data.ortho_scale = 1.6
cam = bpy.data.objects.new("Cam", cam_data)
scene.collection.objects.link(cam)
cam.location = (0, -4.0, 0.8)
cam.rotation_euler = (1.5707963, 0, 0)
scene.camera = cam
world = bpy.data.worlds.new("Black")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0, 0, 0, 1)
scene.world = world

r = scene.render
r.engine = 'CYCLES'
r.resolution_x, r.resolution_y, r.resolution_percentage = 256, 512, 100
r.image_settings.file_format = 'OPEN_EXR'
r.image_settings.color_depth = '16'
r.image_settings.color_mode = 'RGB'
r.image_settings.exr_codec = 'ZIP'
scene.view_settings.view_transform = 'Standard'
scene.view_settings.look = 'None'
scene.cycles.samples = 48
scene.cycles.use_denoising = False
scene.cycles.volume_step_rate = 0.5
scene.cycles.volume_max_steps = 512
if not args.cpu:
    prefs = bpy.context.preferences.addons['cycles'].preferences
    for kind in ('OPTIX', 'CUDA'):
        try:
            prefs.compute_device_type = kind
            prefs.get_devices()
            if any(d.type == kind for d in prefs.devices):
                break
        except TypeError:
            continue
    for d in prefs.devices:
        d.use = d.type != 'CPU'
    scene.cycles.device = 'GPU'
    print(f"render: {prefs.compute_device_type}, {[d.name for d in prefs.devices if d.use]}")

# --- bake the sim, then render ------------------------------------------------------------------------------
with bpy.context.temp_override(object=domain, active_object=domain, selected_objects=[domain]):
    bpy.ops.fluid.bake_data()
print("baked", args.frames, "frames at", args.res)

ramps = [tuple(float(v) for v in r.split()) for r in args.ramp] or [(0.0, 1.0, 1.0)]
for i in range(len(ramps)):
    (out / f"frames-{i}").mkdir(parents=True, exist_ok=True)
for frame in range(args.start, args.frames + 1):
    scene.frame_set(frame)
    for i, (lo, hi, gamma) in enumerate(ramps):
        ramp.inputs["From Min"].default_value, ramp.inputs["From Max"].default_value = lo, hi
        sharp.inputs[1].default_value = gamma
        r.filepath = str(out / f"frames-{i}" / f"flame_{frame:04d}.exr")
        bpy.ops.render.render(write_still=True)
print("rendered", args.frames - args.start + 1, "frames x", len(ramps), "ramps to", out)
