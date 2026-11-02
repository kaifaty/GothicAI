"""Simulate the fire under Edda's cauldron in Mantaflow and render each gap's tongue for a steady flipbook.

    blender -b --factory-startup -P fire_tongue.py -- OUT_DIR [--res 160] [--frames 200] [--start 41] [--cpu]
        [--ramp 'LO HI GAMMA' ...]

reports/content/remaster/city-hearth/fire-composition.md, step 4. The flame under a pot on a ring of stones is not
a campfire seen whole: it burns on the coals and the inner ends of three logs fed through gaps in the ring, hits
the pot's bottom and leaves through the gaps, where it curls up the pot's wall. So the sim has the pot and the
stones as obstacles and fuel only on the coal bed and the log ends; the gas can leave only through the gaps and
over the rim between pot and stones. Sizes from the hearth set: stones 0.16 m high on a ring of 0.45 m radius,
the pot 0.86 m across standing on them, logs 0.07 m thick burning from their tip under the pot out to the gap.
Each gap is filmed from outside along its axis (orthographic, 0.6 m wide, 256 x 256): the pot and the stones are
holdouts, so a tongue keeps only what is seen in front of them, and a game card standing in the gap draws no
flame through the pot. Camera i to OUT_DIR/frames-<i>-<ramp>/flame_NNNN.exr, linear half float as fire_flipbook.py;
fire_atlas.py packs each into an atlas, the vob layer puts one steady card per gap (particles.d flipbook "steady").
"""
import argparse
import math
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
parser.add_argument("--ramp", action="append", default=[])
args = parser.parse_args(argv)
out = args.out.resolve()

RING, STONE_H, POT_R, POT_BOTTOM = 0.45, 0.16, 0.43, 0.15
GAPS = (0.0, 2 * math.pi / 3, 4 * math.pi / 3)  # the logs' gaps, as in ground_fire.json (120 degrees apart)
CARD = 0.6  # metres the camera spans: the card's size in game

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.fps = 48
scene.frame_start, scene.frame_end = 1, args.frames

# --- domain: 1.5 x 1.5 x 1.0 m over the ring, gas leaves only at the top -------------------------------------
bpy.ops.mesh.primitive_cube_add(size=1.0, location=(0, 0, 0.5))
domain = bpy.context.object
domain.name = "Domain"
domain.scale = (1.5, 1.5, 1.0)
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
ds.use_collision_border_top = False
ds.alpha = -0.1
ds.beta = 1.2              # the pot holds the heat under it; the lift pushes it out through the gaps
ds.vorticity = 0.5
ds.burning_rate = 0.9
ds.flame_vorticity = 1.4
ds.flame_smoke = 0.4
ds.flame_ignition = 1.25
ds.flame_max_temp = 3.0
ds.use_dissolve_smoke = True
ds.dissolve_speed = 25
ds.use_noise = False


def effector(obj):
    mod = obj.modifiers.new("Fluid", 'FLUID')
    mod.fluid_type = 'EFFECTOR'
    mod.effector_settings.effector_type = 'COLLISION'
    obj.is_holdout = True  # seen by the camera as a cut-out: no flame drawn through it


# --- the pot: a squat sphere standing on the stones ----------------------------------------------------------
bpy.ops.mesh.primitive_uv_sphere_add(segments=48, ring_count=24, radius=POT_R, location=(0, 0, POT_BOTTOM + POT_R * 0.9))
pot = bpy.context.object
pot.name = "Pot"
pot.scale = (1, 1, 0.9)
bpy.ops.object.transform_apply(scale=True)
effector(pot)

# --- stones: nine round the ring with the three log gaps wider (ground_fire.json "widen" 1.7) ---------------
weights = [1.7 if k in (0, 3, 6) else 1.0 for k in range(9)]
total = sum(weights)
angles, at = [], -weights[0] / 2 / total * 2 * math.pi  # gap 0 (stones 0 and 1) centred on angle 0
for k in range(9):
    angles.append(at)
    at += weights[k] / total * 2 * math.pi
for k, a in enumerate(angles):
    bpy.ops.mesh.primitive_cube_add(size=1.0, location=(RING * math.cos(a), RING * math.sin(a), STONE_H / 2),
                                    rotation=(0, 0, a))
    stone = bpy.context.object
    stone.name = f"Stone{k}"
    stone.scale = (0.24, 0.27, STONE_H)
    bpy.ops.object.transform_apply(scale=True)
    effector(stone)

# --- fuel: the coal bed and each log's burning inner end, fed through drifting noise -------------------------
noise = bpy.data.textures.new("FuelNoise", 'CLOUDS')
noise.noise_scale = 0.05
noise.noise_depth = 2


def fuel(obj, amount, temperature, drift):
    mod = obj.modifiers.new("Fluid", 'FLUID')
    mod.fluid_type = 'FLOW'
    fs = mod.flow_settings
    fs.flow_type = 'FIRE'
    fs.flow_behavior = 'INFLOW'
    fs.flow_source = 'MESH'
    fs.surface_distance = 0.5
    fs.fuel_amount = amount
    fs.temperature = temperature
    fs.use_texture = True
    fs.noise_texture = noise
    fs.texture_map_type = 'AUTO'
    fs.texture_size = 0.5
    fs.texture_offset = 0.0
    fs.keyframe_insert("texture_offset", frame=1)
    fs.texture_offset = drift
    fs.keyframe_insert("texture_offset", frame=args.frames)
    obj.hide_render = True


bpy.ops.mesh.primitive_cylinder_add(vertices=24, radius=0.24, depth=0.04, location=(0, 0, 0.03))
bed = bpy.context.object
bed.name = "Coals"
fuel(bed, 0.5, 1.2, 5.0)
for i, a in enumerate(GAPS):
    mid = 0.265  # the burning part runs from the tip under the pot (0.13 m) out to the gap (0.40 m)
    bpy.ops.mesh.primitive_cylinder_add(vertices=12, radius=0.033, depth=0.27,
                                        location=(mid * math.cos(a), mid * math.sin(a), 0.08),
                                        rotation=(0, math.pi / 2, a))
    end = bpy.context.object
    end.name = f"LogEnd{i}"
    fuel(end, 1.0, 1.6, 6.0 + i)

# --- flame material: emission by `flame`, colour by `temperature` (as fire_flipbook.py) ----------------------
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

# --- one orthographic camera per gap, from outside along its axis, the card's bottom on the ground ----------
cams = []
for i, a in enumerate(GAPS):
    data = bpy.data.cameras.new(f"Cam{i}")
    data.type = 'ORTHO'
    data.ortho_scale = CARD
    cam = bpy.data.objects.new(f"Cam{i}", data)
    scene.collection.objects.link(cam)
    cam.location = (3.0 * math.cos(a), 3.0 * math.sin(a), CARD / 2)
    cam.rotation_euler = (math.pi / 2, 0, a + math.pi / 2)
    cams.append(cam)
world = bpy.data.worlds.new("Black")
world.use_nodes = True
world.node_tree.nodes["Background"].inputs["Color"].default_value = (0, 0, 0, 1)
scene.world = world

r = scene.render
r.engine = 'CYCLES'
r.resolution_x, r.resolution_y, r.resolution_percentage = 256, 256, 100
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

# --- bake the sim, then render every gap's camera -------------------------------------------------------------
with bpy.context.temp_override(object=domain, active_object=domain, selected_objects=[domain]):
    bpy.ops.fluid.bake_data()
print("baked", args.frames, "frames at", args.res)

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
print("rendered", args.frames - args.start + 1, "frames x", len(cams), "gaps x", len(ramps), "ramps to", out)
