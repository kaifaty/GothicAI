"""Simulate a wall torch's flame in Mantaflow and render it from the front and the side for an atlas flipbook.

    blender -b --factory-startup -P torch_flame.py -- OUT_DIR [--scale 1] [--res 192] [--frames 320] [--start 97]
        [--fps 64] [--cache DIR] [--drift 1.5] [--vorticity 0.6] [--cpu] [--ramp 'LO HI GAMMA' ...] [--no-render]

reports/content/remaster/wall-torch/flame-sim.md. A torch head is a bundle of pitch-soaked tow 7 cm across and
10 cm tall on a stick; it burns over its whole skin, the flame is fuel-rich, 25-35 cm tall, and puffs at
5.5-7 Hz (f ~ 1.5 sqrt(g/D)), three times as fast as a brazier. The head's body is an obstacle and a shell 3 mm
outside it feeds fuel through drifting noise, so the flame wraps the head and leaves its top in torn tongues.
Mantaflow smooths small scenes, so --scale S builds everything S times larger: the puff rate then drops by about
sqrt(S), and the game sets it back with the flipbook's playback rate (flame_pulse.py measures it).
The domain is 0.3 x 0.3 x 0.6 m (x S), open on every side; the head spans z 0.02-0.12 (x S). Frames
START..FRAMES (64 per second by default; the first 1.5 s are the flame's build-up) are rendered as in
fire_flipbook.py: emission by the `flame` grid, its colour a black body by `temperature`, linear half-float EXR
on black, orthographic, 256 x 512 over the whole domain section. Camera 0 looks along +y (front), camera 1 along
-x (side): two views of one flame, uncorrelated enough for two cards. Frames to OUT_DIR/frames-<cam>-<ramp>/,
the sim cache to --cache (default OUT_DIR/cache), the scene to OUT_DIR/torch.blend.
"""
import argparse
import math
import sys
from pathlib import Path

import bpy

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
parser = argparse.ArgumentParser()
parser.add_argument("out", type=Path)
parser.add_argument("--scale", type=float, default=1.0)
parser.add_argument("--res", type=int, default=192)
parser.add_argument("--frames", type=int, default=320)
parser.add_argument("--start", type=int, default=97)
parser.add_argument("--fps", type=int, default=64)
parser.add_argument("--cache", type=Path)
# texture offset per second of the fuel noise (the hearth's logs drifted ~1.4/s): how fast fuel patches change
parser.add_argument("--drift", type=float, default=1.5)
# r1 (burning 0.75, fuel 1.2, lift 1.2): at scale 1 a 50 cm steady plume out of the frame top, puffing at 3.9 Hz
parser.add_argument("--burning-rate", type=float, default=0.95)
parser.add_argument("--fuel", type=float, default=1.0)
parser.add_argument("--beta", type=float, default=1.2)          # heat lift; the puff rate goes as its root
parser.add_argument("--flame-vorticity", type=float, default=2.0)
parser.add_argument("--vorticity", type=float, default=0.6)    # curls in the whole gas
# Forced puffing. A real torch flame is a continuous body only up to ~10-13 cm; above it tongues and blobs come and
# go (McCaffrey's zones). At a 3 mm grid Mantaflow's puffing is too weak to pinch the flame off (r1-r3: a solid
# body to 21-30 cm, brightness swing 2-3.5 %), so the fuel supply is modulated at the puff rate, as acoustic
# forcing does in lab flames: fuel x (1 - depth * (1 - cos phase) / 2), the phase advancing at --puff Hz with a
# slow random drift of +-jitter in rate. 0 keeps the supply steady.
parser.add_argument("--puff", type=float, default=0.0)
parser.add_argument("--puff-depth", type=float, default=0.8)
parser.add_argument("--puff-jitter", type=float, default=0.15)
parser.add_argument("--cpu", action="store_true")
parser.add_argument("--no-render", action="store_true")
# emission: "flame" as fire_flipbook.py (the `flame` grid alone), or "t4": the flame grid times (T / 2000 K)^4, as
# soot glows (Stefan-Boltzmann). r2: by `flame` alone the cooled top of the plume glowed as bright as the core,
# an even orange column; by T^4 a 1300 K tip is ~5x dimmer than an 1900 K core and the flame tapers to its tips.
parser.add_argument("--emission", choices=("flame", "t4"), default="t4")
# emission ramps over the `flame` grid, LO HI GAMMA each (fire_flipbook.py): one bake, frames per ramp
parser.add_argument("--ramp", action="append", default=[])
args = parser.parse_args(argv)
out = args.out.resolve()
cache = (args.cache or out / "cache").resolve()
S = args.scale

W, H = 0.3 * S, 0.6 * S             # domain section and height
HEAD_R, HEAD_Z0, HEAD_Z1 = 0.035 * S, 0.02 * S, 0.12 * S

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = args.fps
scene.frame_start, scene.frame_end = 1, args.frames

# --- domain: open sides and top, the gas leaves as in open air ----------------------------------------------
bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 0, H / 2))
domain = bpy.context.object
domain.name = "Domain"
domain.scale = (W, W, H)
bpy.ops.object.transform_apply(scale=True)
fluid = domain.modifiers.new("Fluid", 'FLUID')
fluid.fluid_type = 'DOMAIN'
ds = fluid.domain_settings
ds.domain_type = 'GAS'
ds.resolution_max = args.res
ds.cache_type = 'ALL'
ds.cache_directory = str(cache)
ds.cache_frame_start, ds.cache_frame_end = 1, args.frames
ds.use_adaptive_domain = False
for side in ("front", "back", "left", "right", "top", "bottom"):  # a closed floor 2 cm under the head pooled flame
    setattr(ds, f"use_collision_border_{side}", False)
ds.timesteps_max = 6        # a small cell at 64 fps: ~10 cells per frame at 2 m/s
ds.alpha = -0.2             # pitch soot is heavy: smoke lifts less than heat
ds.beta = args.beta         # heat lift
ds.vorticity = args.vorticity
ds.burning_rate = args.burning_rate  # how fast fuel burns away: the flame's length over the head
ds.flame_vorticity = args.flame_vorticity  # curls in the flame itself tear the tips
ds.flame_smoke = 1.0
ds.flame_ignition = 1.25
ds.flame_max_temp = 3.0
ds.use_dissolve_smoke = True
ds.dissolve_speed = 20
ds.use_noise = False

# --- the head: a solid core and a fuel shell round it ---------------------------------------------------------
mid = (HEAD_Z0 + HEAD_Z1) / 2
bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=HEAD_R, depth=HEAD_Z1 - HEAD_Z0, location=(0, 0, mid))
core = bpy.context.object
core.name = "Head"
mod = core.modifiers.new("Fluid", 'FLUID')
mod.fluid_type = 'EFFECTOR'
mod.effector_settings.effector_type = 'COLLISION'
core.is_holdout = True  # the game draws the head mesh there; r2: its outline in flame showed as a box

noise = bpy.data.textures.new("FuelNoise", 'CLOUDS')
noise.noise_scale = 0.12
noise.noise_depth = 2
pad = 0.003 * S
bpy.ops.mesh.primitive_cylinder_add(vertices=32, radius=HEAD_R + pad, depth=HEAD_Z1 - HEAD_Z0 + pad,
                                    location=(0, 0, mid + pad / 2))
shell = bpy.context.object
shell.name = "Fuel"
mod = shell.modifiers.new("Fluid", 'FLUID')
mod.fluid_type = 'FLOW'
fs = mod.flow_settings
fs.flow_type = 'FIRE'
fs.flow_behavior = 'INFLOW'
fs.flow_source = 'MESH'
fs.surface_distance = 1.0
bpy.context.preferences.edit.keyframe_new_interpolation_type = 'LINEAR'  # even keys: drift and puffs
fs.fuel_amount = args.fuel
if args.puff > 0:
    phase = 0.0
    for f in range(1, args.frames + 1):
        t = f / args.fps
        # rate and strength wander slowly (incommensurate sines), so no two puffs are the same
        rate = 1 + args.puff_jitter * (0.6 * math.sin(2 * math.pi * 0.37 * t) + 0.4 * math.sin(2 * math.pi * 0.83 * t + 1.3))
        depth = args.puff_depth * (0.8 + 0.2 * math.sin(2 * math.pi * 0.53 * t + 2.1))
        fs.fuel_amount = args.fuel * (1 - depth * (1 - math.cos(phase)) / 2)
        fs.keyframe_insert("fuel_amount", frame=f)
        phase += 2 * math.pi * args.puff * rate / args.fps
fs.temperature = 1.8
fs.use_texture = True
fs.noise_texture = noise
fs.texture_map_type = 'AUTO'
fs.texture_size = 0.6
fs.texture_offset = 0.0
fs.keyframe_insert("texture_offset", frame=1)
fs.texture_offset = args.drift * args.frames / args.fps  # the same drift per second at any fps
fs.keyframe_insert("texture_offset", frame=args.frames)
shell.hide_render = True

# --- flame material: emission by `flame`, colour by `temperature` (fire_flipbook.py) -------------------------
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
strength.inputs[1].default_value = 4.0 / S  # the same brightness per pixel at any scale: the depth summed grows by S
if args.emission == "t4":
    t4 = nodes.new('ShaderNodeMath')
    t4.operation = 'POWER'
    t4.inputs[1].default_value = 4.0
    per_k = nodes.new('ShaderNodeMath')
    per_k.operation = 'DIVIDE'
    per_k.inputs[1].default_value = 2000.0
    links.new(to_kelvin.outputs["Result"], per_k.inputs[0])
    links.new(per_k.outputs["Value"], t4.inputs[0])
    links.new(t4.outputs["Value"], strength.inputs[1])  # in place of the gain: fire_atlas.py sets the level
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
# Inflow from a closed mesh fills its inside too (r1: the head glowed as a hard-edged box). Inside the head there is
# no flame: the game draws the head mesh there, so the emission is masked by position.
geo = nodes.new('ShaderNodeNewGeometry')
xyz = nodes.new('ShaderNodeSeparateXYZ')
links.new(geo.outputs["Position"], xyz.inputs["Vector"])


def math_node(op, a, b):
    n = nodes.new('ShaderNodeMath')
    n.operation = op
    for i, v in enumerate((a, b)):
        if isinstance(v, float):
            n.inputs[i].default_value = v
        else:
            links.new(v, n.inputs[i])
    return n.outputs["Value"]


radius = math_node('SQRT', math_node('ADD', math_node('MULTIPLY', xyz.outputs["X"], xyz.outputs["X"]),
                                     math_node('MULTIPLY', xyz.outputs["Y"], xyz.outputs["Y"])), 0.0)
inside = math_node('MULTIPLY', math_node('LESS_THAN', radius, HEAD_R),
                   math_node('MULTIPLY', math_node('GREATER_THAN', xyz.outputs["Z"], HEAD_Z0),
                             math_node('LESS_THAN', xyz.outputs["Z"], HEAD_Z1)))
links.new(math_node('MULTIPLY', strength.outputs["Value"], math_node('SUBTRACT', 1.0, inside)),
          emission.inputs["Strength"])
links.new(emission.outputs["Emission"], out_node.inputs["Volume"])
domain.data.materials.append(mat)

# --- two orthographic cameras over the whole domain section ---------------------------------------------------
cams = []
for i, (loc, yaw) in enumerate((((0, -3.0 * S, H / 2), 0.0), ((3.0 * S, 0, H / 2), math.pi / 2))):
    data = bpy.data.cameras.new(f"Cam{i}")
    data.type = 'ORTHO'
    data.ortho_scale = H
    cam = bpy.data.objects.new(f"Cam{i}", data)
    scene.collection.objects.link(cam)
    cam.location = loc
    cam.rotation_euler = (math.pi / 2, 0, yaw)
    cams.append(cam)
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

out.mkdir(parents=True, exist_ok=True)
bpy.ops.wm.save_as_mainfile(filepath=str(out / "torch.blend"))
print(f"domain {W:.2f} x {W:.2f} x {H:.2f} m, res {args.res}: cell {H / args.res * 1000:.1f} mm, "
      f"{args.frames} frames at {args.fps} fps, scale {S}")

# --- bake the sim, then render both cameras -------------------------------------------------------------------
with bpy.context.temp_override(object=domain, active_object=domain, selected_objects=[domain]):
    bpy.ops.fluid.bake_data()
print("baked", args.frames, "frames at", args.res)
if args.no_render:
    sys.exit(0)

ramps = [tuple(float(v) for v in x.split()) for x in args.ramp] or [(0.0, 1.0, 1.0)]
for c in range(len(cams)):
    for i in range(len(ramps)):
        (out / f"frames-{c}-{i}").mkdir(parents=True, exist_ok=True)
for frame in range(args.start, args.frames + 1):
    scene.frame_set(frame)
    for c, cam in enumerate(cams):
        scene.camera = cam
        for i, (lo, hi, gamma) in enumerate(ramps):
            ramp.inputs["From Min"].default_value, ramp.inputs["From Max"].default_value = lo, hi
            sharp.inputs[1].default_value = gamma
            r.filepath = str(out / f"frames-{c}-{i}" / f"flame_{frame:04d}.exr")
            bpy.ops.render.render(write_still=True)
print("rendered", args.frames - args.start + 1, "frames x", len(cams), "cameras x", len(ramps), "ramps to", out)
