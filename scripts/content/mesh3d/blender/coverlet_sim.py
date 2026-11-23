"""Drop a wool coverlet on the master's mattress with Blender's cloth simulation; keep its form for the game.

blender -b --factory-startup -P coverlet_sim.py -- OUT_DIR [--bed BED.json] [--frames 90] [--seed 3] [--tris 4200]

reports/content/remaster/city-furniture/mattress-fabric.md, «Покрывало»: the original's mattress reads as cloth by the
drape painted into its texture (creases dark, bends light, the sheet hanging over the sides); our taut tick read flat.
Here a coverlet of thick wool falls on the mattress (canopy.mattress(), the side rails and the boards as colliders)
from a slightly skewed, gently waved start, so its folds are not a grid's. Writes OUT_DIR:
  coverlet.npz       the game mesh: vertices (glTF metres), faces, uv per corner; the sheet's UV (across, along), as
                     the original's texture lies (side - top - side, the woven borders at the ends)
  folds.normal.png   the fine folds of the full simulation baked onto it (tangent, OpenGL)
  folds.ao.png       their occlusion
  look_*.png         workbench views
BED.json (another bed, the plain and the rich one): {"mattress": canopy.mattress keywords, "colliders": [{"x", "y",
"az"}: a box on each side, or {"x", "y", "z"}: one box], "sheet": {"x": [x0, x1], "width": w}, "cloth": {cloth
settings by name}}; what it leaves out is the master's bed.
"""
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import canopy  # noqa: E402

args = sys.argv[sys.argv.index("--") + 1:]
out = Path(args[0])
opt = dict(zip(args[1::2], args[2::2]))
frames, seed, tris = int(opt.get("--frames", 90)), int(opt.get("--seed", 3)), int(opt.get("--tris", 4200))
MASTER = {"mattress": {}, "sheet": {"x": [-1.12, 0.98], "width": 1.38},
          "colliders": [{"x": [-1.15, 1.01], "y": [0.15, 0.34], "az": [0.465, 0.53]},
                        {"x": [-1.23, -1.125], "y": [0.0, 0.85], "z": [-0.57, 0.57]},
                        {"x": [1.01, 1.13], "y": [0.0, 0.79], "z": [-0.6, 0.6]}],
          "cloth": {}}
bed = {**MASTER, **(json.loads(Path(opt["--bed"]).read_text()) if "--bed" in opt else {})}
out.mkdir(parents=True, exist_ok=True)
rng = np.random.default_rng(seed)
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
b = canopy.b


def collider(obj, outer=0.004):
    obj.modifiers.new("collision", "COLLISION")
    obj.collision.thickness_outer = outer
    obj.collision.cloth_friction = 8.0  # wool on wool and on wood does not slide off
    return obj


def box(name, x, y, z):
    import bmesh
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = b(canopy.lerp(x[0], x[1], v.co.x + 0.5), canopy.lerp(y[0], y[1], v.co.z + 0.5),
                 canopy.lerp(z[0], z[1], 0.5 - v.co.y))
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    obj = bpy.data.objects.new(name, me)
    scene.collection.objects.link(obj)
    return obj


# --- the bed as the generator built it, where the cloth can touch it (glTF metres, measured on its mesh) -------------
core = canopy.mattress(**bed["mattress"])
collider(core)
for k, c in enumerate(bed["colliders"]):
    if "az" in c:
        for s in (-1, 1):
            collider(box(f"part{k}{s:+d}", c["x"], c["y"], sorted((s * c["az"][0], s * c["az"][1]))))
    else:
        collider(box(f"part{k}", c["x"], c["y"], c["z"]))

# --- the coverlet: 1.5 cm cells, the mattress's length, its top and a hang of about 20 cm each side ------------------
(X0, X1), W, CELL = bed["sheet"]["x"], bed["sheet"]["width"], 0.015
top = max(v.co.z for v in core.data.vertices)  # Blender Z is glTF y
nx, nz = round((X1 - X0) / CELL) + 1, round(W / CELL) + 1
phase = rng.uniform(0, 2 * math.pi, 4)
shift, skew = rng.uniform(-0.03, 0.03), math.radians(rng.uniform(-2, 2))
grid = np.zeros((nx, nz, 3))
for i in range(nx):
    for j in range(nz):
        x, z = canopy.lerp(X0, X1, i / (nx - 1)), canopy.lerp(-W / 2, W / 2, j / (nz - 1)) + shift
        x, z = x * math.cos(skew) - z * math.sin(skew), x * math.sin(skew) + z * math.cos(skew)
        y = top + 0.07 + 0.03 * math.sin(2 * math.pi * x / 0.55 + phase[0]) * math.sin(2 * math.pi * z / 0.7 + phase[1]) \
            + 0.02 * math.sin(2 * math.pi * (x + z) / 0.35 + phase[2]) + 0.01 * math.sin(2 * math.pi * x / 0.21 + phase[3])
        grid[i, j] = (x, y, z)
faces = [[i * nz + j, i * nz + j + 1, (i + 1) * nz + j + 1, (i + 1) * nz + j] for i in range(nx - 1) for j in range(nz - 1)]
uv = [[(j / (nz - 1), i / (nx - 1)) for i, j in ((i, j), (i, j + 1), (i + 1, j + 1), (i + 1, j))]
      for i in range(nx - 1) for j in range(nz - 1)]
sheet = canopy._object("coverlet", "tick", [b(*p) for p in grid.reshape(-1, 3)], faces, uv)
cloth = sheet.modifiers.new("cloth", "CLOTH")
st = cloth.settings
st.quality, st.mass, st.air_damping = 8, 0.6, 1.0
st.tension_stiffness = st.compression_stiffness = 30.0  # thick wool: firm, heavy, broad folds (Blender's denim
st.shear_stiffness, st.bending_stiffness = 20.0, 6.0    # preset is 40/40/40/10, its cotton 15/15/15/0.5)
for key, value in bed["cloth"].items():
    setattr(st, key, value)
cs = cloth.collision_settings
cs.distance_min, cs.collision_quality = 0.005, 4
cs.use_self_collision, cs.self_distance_min = True, 0.004
cloth.point_cache.frame_start, cloth.point_cache.frame_end = 1, frames
scene.frame_start, scene.frame_end = 1, frames
for f in range(1, frames + 1):
    scene.frame_set(f)
ev = sheet.evaluated_get(bpy.context.evaluated_depsgraph_get())
settled = np.array([v.co[:] for v in ev.to_mesh().vertices])
sheet.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh_clear()
sheet.modifiers.remove(cloth)
sheet.data.vertices.foreach_set("co", settled.astype(np.float32).ravel())
sheet.data.update()
print("SETTLED", settled[:, 2].min().round(3), settled[:, 2].max().round(3))

# --- the game mesh: the same sheet decimated to the budget, its UVs kept -------------------------------------------
low = sheet.copy()
low.data = sheet.data.copy()
low.name = "coverlet-low"
scene.collection.objects.link(low)
dec = low.modifiers.new("decimate", "DECIMATE")
dec.ratio = tris / (2 * len(sheet.data.polygons))
dec.use_collapse_triangulate = True
bpy.context.view_layer.objects.active = low
for o in scene.objects:
    o.select_set(o is low)
bpy.ops.object.modifier_apply(modifier="decimate")
low.data.shade_smooth()
me = low.data
V = np.array([(v.co.x, v.co.z, -v.co.y) for v in me.vertices])  # Blender -> glTF
F = np.array([list(p.vertices) for p in me.polygons])
UV = np.array([[me.uv_layers.active.data[li].uv[:] for li in p.loop_indices] for p in me.polygons])
np.savez(out / "coverlet.npz", vertices=V, faces=F, uv=UV)
print("LOW", len(F), "triangles")

# --- bake the full simulation's folds onto it -------------------------------------------------------------------------
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
world = bpy.data.worlds.new("w")
scene.world = world
world.light_settings.distance = 0.15
mat = bpy.data.materials.new("bake")
mat.use_nodes = True
node = mat.node_tree.nodes.new("ShaderNodeTexImage")
mat.node_tree.nodes.active = node
me.materials.append(mat)
for name, kind, samples in (("folds.normal", "NORMAL", 16), ("folds.ao", "AO", 64)):
    scene.cycles.samples = samples
    node.image = bpy.data.images.new(name, 2048, 2048)
    node.image.colorspace_settings.name = "Non-Color"
    for o in scene.objects:
        o.select_set(o in (low, sheet))
    bpy.context.view_layer.objects.active = low
    bpy.ops.object.bake(type=kind, use_selected_to_active=True, cage_extrusion=0.01, max_ray_distance=0.03, margin=16,
                        **({"normal_space": "TANGENT"} if kind == "NORMAL" else {}))
    node.image.filepath_raw = str(out / f"{name}.png")
    node.image.file_format = "PNG"
    node.image.save()

# --- look ---------------------------------------------------------------------------------------------------------------
scene.render.engine = "BLENDER_WORKBENCH"
scene.display.shading.light = "STUDIO"
scene.display.shading.show_cavity = True
scene.render.resolution_x, scene.render.resolution_y = 1024, 640
sheet.hide_render = True
cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
scene.camera = cam
centre = b(-0.07, 0.4, 0)
for k, (az, el, dist) in enumerate(((30, 30, 3.0), (95, 20, 2.4), (0, 70, 2.6))):
    az, el = math.radians(az), math.radians(el)
    cam.location = centre + b(math.cos(az) * math.cos(el) * dist, math.sin(el) * dist, -math.sin(az) * math.cos(el) * dist) - b(0, 0, 0)
    cam.rotation_euler = (centre - cam.location).to_track_quat("-Z", "Y").to_euler()
    scene.render.filepath = str(out / f"look_{k}.png")
    bpy.ops.render.render(write_still=True)
