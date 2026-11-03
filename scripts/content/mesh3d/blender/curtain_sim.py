"""Hang a side curtain of the master's canopy as cloth: pleated on the rail, drawn back to the tie on the head post.

blender -b --factory-startup -P curtain_sim.py -- OUT_DIR --side -1|1 [--frames 120] [--seed 3]

reports/content/remaster/city-furniture/mattress-fabric.md, «Шторы симуляцией»: the coded curtain (a bezier sweep
with sine folds) read too perfect even with irregular folds, and its shallow folds catch no light from the lamp; the
coverlet's folds read once they came from a simulation (the user, 2026-11-03: «складки получились хорошими»).
A linen sheet 1.25 x 2.10 m hangs flat by the bed; over the first frames its top row is pinned into pleats along the
rail (1.01 m) and a slanted line of it is pulled into the tie (the back edge 1.36 m below the rail, the front edge
1.65 m: the front sweeps diagonally), then it settles. The pins follow a shape key, as Blender's cloth lets pinned
vertices follow the mesh's own animation. Writes OUT_DIR/curtain<side>.npz: grid (rows down the cloth, columns from the
head post to the front; glTF metres) and rest (the flat sheet's (across, down) metres, for the UV), and look_*.png.
"""
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canopy  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:]
out = Path(args[0])
opt = dict(zip(args[1::2], args[2::2]))
side, frames, seed = int(opt["--side"]), int(opt.get("--frames", 120)), int(opt.get("--seed", 3))
out.mkdir(parents=True, exist_ok=True)
rng = np.random.default_rng([seed, side + 1])
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
b, lerp = canopy.b, canopy.lerp

WIDTH, LENGTH, CELL = 1.25, 2.10, 0.0149  # 85 x 141 points: (C-1) divisible by 3, (R-1) by 4 for the game grid
RAIL = (-1.300, -0.290, 2.122)           # the rail's x span and the cloth's top
TIE = (-1.215, -1.135, canopy.TIE_Y, 0.606)  # where the gathered linen sits inside the band: x span, y, |z|
DROP = (1.36, 1.65)                      # from the rail to the tie, at the back and the front edge
PLEATS, PLEAT_DEPTH = 10, 0.026


def box(name, x, y, z, outer=0.004):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = b(lerp(x[0], x[1], v.co.x + 0.5), lerp(y[0], y[1], v.co.z + 0.5), lerp(z[0], z[1], 0.5 - v.co.y))
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    scene.collection.objects.link(obj)
    obj.modifiers.new("collision", "COLLISION")
    obj.collision.thickness_outer, obj.collision.cloth_friction = outer, 5.0
    return obj


def span(a, c):
    return sorted((side * a, side * c))


# --- what the curtain can touch: the head post, its band, the bed's side rail and head board, the canopy rail, floor --
box("post", (-1.29, -1.13), (0.0, 2.11), span(0.42, 0.57))
box("rail", (-1.13, -0.29), (2.03, 2.11), span(0.49, 0.57))
box("bedrail", (-1.15, 1.01), (0.15, 0.34), span(0.465, 0.53))
box("head", (-1.23, -1.125), (0.0, 0.85), span(0.0, 0.57))
box("floor", (-3.0, 2.0), (-0.1, 0.0), (-2.0, 2.0))

# --- the sheet, flat by the bed, its top at the rail ------------------------------------------------------------------
C, R = round(WIDTH / CELL) + 1, round(LENGTH / CELL) + 1
across, down = np.linspace(0, WIDTH, C), np.linspace(0, LENGTH, R)
z0 = side * 0.60
flat = [(RAIL[0] + a, RAIL[2] - d, z0 + side * 0.004 * math.sin(7.3 * a + 13.1 * d + seed)) for d in down for a in across]
faces = [[i * C + j, i * C + j + 1, (i + 1) * C + j + 1, (i + 1) * C + j] for i in range(R - 1) for j in range(C - 1)]
me = bpy.data.meshes.new("curtain")
me.from_pydata([tuple(b(*p)) for p in flat], [], faces)
me.update()
cloth_obj = bpy.data.objects.new("curtain", me)
scene.collection.objects.link(cloth_obj)

# --- the pins and where they go: the top row into pleats on the rail, the tie line into the band ----------------------
pin = cloth_obj.vertex_groups.new(name="pin")
cloth_obj.shape_key_add(name="Basis")
tied = cloth_obj.shape_key_add(name="tied")
phase = rng.uniform(0, 2 * math.pi)
for j, a in enumerate(across):
    u = a / WIDTH
    x = lerp(RAIL[0] + 0.01, RAIL[1] - 0.01, u)
    pz = z0 + side * PLEAT_DEPTH * (0.5 + 0.5 * math.sin(2 * math.pi * PLEATS * u + phase))
    tied.data[j].co = b(x, RAIL[2], pz)
    pin.add([j], 1.0, "REPLACE")
    row = round(lerp(*DROP, u) / CELL)
    for k, dy in ((-1, 0.025), (0, 0.0), (1, -0.025)):  # three rows into the band, as a hand gathers them
        i = row + k
        tx = lerp(TIE[0], TIE[1], u) + rng.uniform(-0.004, 0.004)
        tz = side * (TIE[3] + 0.010 * math.sin(2 * math.pi * 9 * u + phase) + 0.004 * k)
        tied.data[i * C + j].co = b(tx, TIE[2] + dy, tz)
        pin.add([i * C + j], 1.0, "REPLACE")
tied.value = 0.0
tied.keyframe_insert("value", frame=1)
tied.value = 1.0
tied.keyframe_insert("value", frame=45)

cloth = cloth_obj.modifiers.new("cloth", "CLOTH")
st = cloth.settings
st.quality, st.mass, st.air_damping = 8, 0.3, 1.5
st.tension_stiffness, st.compression_stiffness = 15.0, 4.0  # linen: firm along the threads, gathers without fighting
st.shear_stiffness, st.bending_stiffness = 5.0, 0.8           # crisper than cotton (0.5), far softer than wool (6)
st.vertex_group_mass, st.pin_stiffness = "pin", 5.0
cs = cloth.collision_settings
cs.distance_min, cs.collision_quality = 0.005, 4
cs.use_self_collision, cs.self_distance_min = True, 0.004
cloth.point_cache.frame_start, cloth.point_cache.frame_end = 1, frames
scene.frame_start, scene.frame_end = 1, frames
for f in range(1, frames + 1):
    scene.frame_set(f)
    if f % 20 == 0:
        print("frame", f, flush=True)
ev = cloth_obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
settled = np.array([v.co[:] for v in ev.to_mesh().vertices])
ev.to_mesh_clear()
grid = np.stack([settled[:, 0], settled[:, 2], -settled[:, 1]], axis=1).reshape(R, C, 3)  # Blender -> glTF
rest = np.stack(np.meshgrid(across, down), axis=-1)
np.savez(out / f"curtain{side:+d}.npz", grid=grid, rest=rest)
print("SETTLED", side, "y", grid[..., 1].min().round(3), grid[..., 1].max().round(3),
      "|z|", np.abs(grid[..., 2]).min().round(3), np.abs(grid[..., 2]).max().round(3))

# --- look ---------------------------------------------------------------------------------------------------------------
cloth_obj.modifiers.remove(cloth)
cloth_obj.shape_key_clear()
cloth_obj.data.vertices.foreach_set("co", settled.astype(np.float32).ravel())
cloth_obj.data.update()
cloth_obj.data.shade_smooth()
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.show_cavity = True
scene.render.resolution_x, scene.render.resolution_y = 768, 768
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
scene.camera = cam
centre = b(-0.8, 1.2, side * 0.6)
for k, (az, el, dist) in enumerate(((90 * side, 10, 3.2), (40 * side, 20, 3.2), (0, 15, 3.0))):
    az, el = math.radians(az), math.radians(el)
    cam.location = centre + b(math.cos(az) * math.cos(el) * dist, math.sin(el) * dist, math.sin(az) * math.cos(el) * dist)
    cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
    scene.render.filepath = str(out / f"look{side:+d}_{k}.png")
    bpy.ops.render.render(write_still=True)
