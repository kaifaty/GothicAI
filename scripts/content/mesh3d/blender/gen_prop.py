"""Turn a TRELLIS raw mesh (raw.npz from generate_trellis.py) into a game prop: the generator pilot of city-furniture.

blender -b --factory-startup -P gen_prop.py -- CONFIG.json OUT_DIR   (run from the repository root)
config: {"id": target id, "target": targets json, "raw": raw.npz, "yaw_deg": 0, "symmetrize": false,
         "material": "OWN_FURN_<ITEM>_GEN", "group": "WOOD", "roughness": 0.8, "metallic": 0.0, "atlas": 2048}

The user opened the pilot on 2026-11-02 after the code-built set lost to the original (reports/content/remaster/
city-furniture/decisions.md). The generator gives the continuous form and a texture that follows it; this script
only fits it to the game: the original's bounding box (where the npc sits and the collision hull come from), a
triangle budget, one atlas. The dense generator mesh is kept as the bake source, so the colour and the fine relief
come from it onto the lighter model.

"own": {"module": M, "cut": [box...], "wood_from": box, "masks": [kind...]} replaces part of the generator's form
with parts built by blender/M.py (the canopy of the master's bed, 2026-11-03): faces whose centre lies in a cut box
(glTF metres, keys x y z and az = |z|, each a [min, max]) go from both meshes, M.build() adds its objects to the game
mesh with their own UVs, and they are baked from M.build(own, source=True) (the same parts, or denser where M simulated
them) with M.material(kind, mode, wood); wood is the generator's median colour inside wood_from. Each mask kind is
baked white on its parts as maps/<kind>.png, for a zone of furniture_gen.py pack. "materials": {kind: material key} keeps such parts out of the atlas with a material of
their own; only their AO is baked, as <kind>/ao.png at "sizes"[kind], and pack takes the rest from the item's
<kind>/ folder.

TRELLIS: +Z up, a nominal cube -0.5..0.5, linear vertex colour. yaw_deg turns the result about Z so that its front
looks at Blender +Y (glTF -Z, where the npc stands). Pixal3D gives the same cube +Y up: "up": "y" turns it first.
"""
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
raw = np.load(cfg["raw"])
V, F, C = raw["vertices"].astype(np.float64), raw["faces"], raw["color"]

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene


def glb_centres(points):
    """glTF axes of Blender points (x, -z, y), and |z|."""
    return {"x": points[:, 0], "y": points[:, 2], "z": -points[:, 1], "az": np.abs(points[:, 1])}


def in_boxes(g, boxes):
    hit = np.zeros(g["x"].shape, bool)
    for box in boxes:
        inside = np.ones(g["x"].shape, bool)
        for key, span in box.items():
            if key != "any":
                inside &= (g[key] >= span[0]) & (g[key] <= span[1])
        hit |= inside
    return hit


def cut(mesh, boxes):
    """Delete the faces whose centre lies in any of the boxes; with "any": true a box takes a face with any vertex
    inside (long slivers of the generator's cloth reach out of a box their centre misses)."""
    centres = np.empty(len(mesh.polygons) * 3)
    mesh.polygons.foreach_get("center", centres)
    drop = in_boxes(glb_centres(centres.reshape(-1, 3)), [b for b in boxes if not b.get("any")])
    loose = [b for b in boxes if b.get("any")]
    if loose:  # the generator's meshes are triangles
        co = np.empty(len(mesh.vertices) * 3)
        mesh.vertices.foreach_get("co", co)
        tris = np.empty(len(mesh.polygons) * 3, np.int64)
        mesh.polygons.foreach_get("vertices", tris)
        drop |= in_boxes(glb_centres(co.reshape(-1, 3)), loose)[tris.reshape(-1, 3)].any(axis=1)
    drop = np.flatnonzero(drop)
    bm = bmesh.new()
    bm.from_mesh(mesh)
    bm.faces.ensure_lookup_table()
    bmesh.ops.delete(bm, geom=[bm.faces[i] for i in drop], context="FACES")
    bm.to_mesh(mesh)
    bm.free()
    return len(drop)


def link(mesh, name):
    obj = bpy.data.objects.new(name, mesh)
    scene.collection.objects.link(obj)
    return obj


# --- the dense source mesh with its vertex colour -------------------------------------------------------------------
yaw = math.radians(cfg.get("yaw_deg", 0))
rot = np.array([[math.cos(yaw), -math.sin(yaw), 0], [math.sin(yaw), math.cos(yaw), 0], [0, 0, 1]])
# Pixal3D's single view builds the object in the frame of its picture's camera: a picture from above tilts it about X
# (the armchair from image b by -18 degrees, found as the tightest box). tilt_deg undoes that before the yaw.
tilt = math.radians(cfg.get("tilt_deg", 0))
rot = rot @ np.array([[1, 0, 0], [0, math.cos(tilt), -math.sin(tilt)], [0, math.sin(tilt), math.cos(tilt)]])
if cfg.get("up") == "y":  # (x, y, z) -> (x, -z, y)
    rot = rot @ np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]])
V = V @ rot.T
mesh = bpy.data.meshes.new("high")
mesh.vertices.add(len(V))
mesh.vertices.foreach_set("co", V.astype(np.float32).ravel())
mesh.loops.add(F.size)
mesh.loops.foreach_set("vertex_index", F.astype(np.int32).ravel())
mesh.polygons.add(len(F))
mesh.polygons.foreach_set("loop_start", np.arange(0, F.size, 3, dtype=np.int32))
mesh.update()
mesh.validate()
col = mesh.color_attributes.new("Col", "FLOAT_COLOR", "POINT")
col.data.foreach_set("color", np.c_[C, np.ones(len(C))].astype(np.float32).ravel())

bm = bmesh.new()
bm.from_mesh(mesh)
bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
# Drop floaters: connected pieces far smaller than the object (generator noise), keep real small parts.
bm.faces.ensure_lookup_table()
seen, pieces = set(), []
for f in bm.faces:
    if f.index in seen:
        continue
    stack, piece = [f], []
    seen.add(f.index)
    while stack:
        g = stack.pop()
        piece.append(g)
        for e in g.edges:
            for h in e.link_faces:
                if h.index not in seen:
                    seen.add(h.index)
                    stack.append(h)
    pieces.append(piece)
largest = max(len(p) for p in pieces)
small = [f for p in pieces if len(p) < max(60, largest * 0.002) for f in p]
bmesh.ops.delete(bm, geom=small, context="FACES")
if cfg.get("symmetrize"):  # furniture is symmetric left to right: keep the better-lit +X half's form on both sides
    bmesh.ops.symmetrize(bm, input=bm.verts[:] + bm.edges[:] + bm.faces[:], direction="X", dist=1e-4)
# The generator's winding is not reliably outward; the normal bake reads the dense mesh's normals.
bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
# Fit into the original's bounding box, each axis on its own: the npc's seat, the collision and the neighbours
# come from it.
co = np.array([v.co[:] for v in bm.verts])
cmin, cmax = co.min(0), co.max(0)
glo, ghi = np.array(lo[:]), np.array(hi[:])
scale = (ghi - glo) / np.maximum(cmax - cmin, 1e-6)
for v in bm.verts:
    v.co = [(v.co[i] - cmin[i]) * scale[i] + glo[i] for i in range(3)]
bm.to_mesh(mesh)
bm.free()
own = cfg.get("own")
wood = None
if own:
    co = np.empty(len(mesh.vertices) * 3)
    mesh.vertices.foreach_get("co", co)
    colours = np.empty(len(mesh.vertices) * 4, np.float32)
    mesh.color_attributes["Col"].data.foreach_get("color", colours)
    if "wood_from" in own:  # own wooden parts take the generator's wood; bedding alone needs none
        picked = in_boxes(glb_centres(co.reshape(-1, 3)), [own["wood_from"]])
        wood = tuple(float(c) for c in np.median(colours.reshape(-1, 4)[picked, :3], axis=0))
    print("CUT high", cut(mesh, own["cut"]), "wood", wood and np.round(wood, 4).tolist())
high = link(mesh, "high")
print("HIGH", len(mesh.polygons), "pieces", len(pieces), "dropped", len(small), "scale", scale.round(3).tolist())

# --- the game mesh ---------------------------------------------------------------------------------------------------
# The generator's skin is torn: in the armchair 104k edges shared by more than two faces and 32k open ones, so
# Blender's collapse stalled at 263k triangles for a 16k budget and a voxel remesh kept only outlines. Both meshes
# come from cumesh_clean.py on dev-ubuntu (a dual-contouring remesh, then simplification to the budget); the game
# mesh is placed here by the same turn and fit as the dense one.
budget = goal["budget_tris"]
small_mesh = np.load(cfg["low"])
LV = (small_mesh["vertices"].astype(np.float64) @ rot.T - cmin) * scale + glo
LF = small_mesh["faces"]
lmesh = bpy.data.meshes.new(goal["id"])
lmesh.vertices.add(len(LV))
lmesh.vertices.foreach_set("co", LV.astype(np.float32).ravel())
lmesh.loops.add(LF.size)
lmesh.loops.foreach_set("vertex_index", LF.astype(np.int32).ravel())
lmesh.polygons.add(len(LF))
lmesh.polygons.foreach_set("loop_start", np.arange(0, LF.size, 3, dtype=np.int32))
lmesh.update()
lmesh.validate()
if own:
    print("CUT low", cut(lmesh, own["cut"]))
lbm = bmesh.new()
lbm.from_mesh(lmesh)
bmesh.ops.recalc_face_normals(lbm, faces=lbm.faces)
lbm.to_mesh(lmesh)
lbm.free()
low = link(lmesh, goal["id"])
bpy.context.view_layer.objects.active = low
for o in scene.objects:
    o.select_set(o is low)
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.uv.smart_project(angle_limit=math.radians(cfg.get("angle_limit", 60)), island_margin=0.004)
bpy.ops.object.mode_set(mode="OBJECT")
low.data.shade_smooth()
low.data.set_sharp_from_angle(angle=math.radians(cfg.get("sharp_deg", 50)))
sources = []
if own:
    import importlib
    module = importlib.import_module(own["module"])
    parts = module.build(own)
    sources = module.build(own, source=True)  # the bake source: the same form (denser where M has it), shader colour
    # Parts with a texture of their own (the mattress's tick: a weave the atlas cannot hold) stay out of the atlas:
    # they get their own material and only their AO is baked here, onto their own UVs.
    separate = own.get("materials", {})
    kept = [o for o in parts if o["kind"] in separate]
    parts = [o for o in parts if o not in kept]
    for o in [o for o in sources if o["kind"] in separate]:
        sources.remove(o)
        bpy.data.objects.remove(o)
    if parts:  # with every own part in a material of its own, the atlas stays the generator's
        for o in scene.objects:
            o.select_set(o is low or o in parts)
        bpy.context.view_layer.objects.active = low
        bpy.ops.object.join()
        # One atlas for both: the islands of smart_project and the parts' metre UVs brought to one density, packed.
        scene.tool_settings.use_uv_select_sync = True
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.average_islands_scale()
        bpy.ops.uv.pack_islands(rotate=True, margin=0.004)
        bpy.ops.object.mode_set(mode="OBJECT")
    materials = {}


    def dress(mode):
        """Give the own sources their material for this bake (the generator's mesh keeps its vertex colour, black
        for a mask)."""
        for o in sources:
            key = (o["kind"], mode)
            if key not in materials:
                materials[key] = module.material(o["kind"], mode, wood)
            o.data.materials.clear()
            o.data.materials.append(materials[key])
print("LOW", len(low.data.polygons))

# --- bake the generator's colour and relief onto the game mesh ------------------------------------------------------
size = cfg.get("atlas", 2048)
scene.render.engine = "CYCLES"
scene.cycles.samples = 16
scene.cycles.device = "CPU"
src = bpy.data.materials.new("src")
src.use_nodes = True
nt = src.node_tree
nt.nodes.clear()
attr = nt.nodes.new("ShaderNodeVertexColor")
attr.layer_name = "Col"
emit = nt.nodes.new("ShaderNodeEmission")
outn = nt.nodes.new("ShaderNodeOutputMaterial")
nt.links.new(attr.outputs[0], emit.inputs[0])
nt.links.new(emit.outputs[0], outn.inputs[0])
high.data.materials.append(src)

mat = bpy.data.materials.new(cfg["material"])
mat["group"] = cfg["group"]
mat.use_nodes = True
low.data.materials.append(mat)
target = mat.node_tree.nodes.new("ShaderNodeTexImage")
mat.node_tree.nodes.active = target
maps = out / "maps"
maps.mkdir(parents=True, exist_ok=True)


def bake(kind, name, colorspace, selected=True, **kw):
    image = bpy.data.images.new(name, size, size, float_buffer=False)
    image.colorspace_settings.name = colorspace
    target.image = image
    for o in scene.objects:
        o.select_set(o is low or (selected and (o is high or o in sources)))
    bpy.context.view_layer.objects.active = low
    bpy.ops.object.bake(type=kind, use_selected_to_active=selected, cage_extrusion=0.015, max_ray_distance=0.04,
                        margin=16, **kw)
    image.filepath_raw = str(maps / f"{name}.png")
    image.file_format = "PNG"
    image.save()


if own:
    dress("albedo")
bake("EMIT", "albedo", "sRGB")
if own:
    dress("normal")
bake("NORMAL", "normal", "Non-Color", normal_space="TANGENT")
if own:
    black = bpy.data.materials.new("black")
    black.use_nodes = True
    black.node_tree.nodes.clear()
    emit_black = black.node_tree.nodes.new("ShaderNodeEmission")
    emit_black.inputs["Color"].default_value = (0, 0, 0, 1)
    black.node_tree.links.new(emit_black.outputs[0], black.node_tree.nodes.new("ShaderNodeOutputMaterial").inputs[0])
    high.data.materials[0] = black
    for kind in own.get("masks", []):
        dress(kind)
        bake("EMIT", kind, "Non-Color")
    high.data.materials[0] = src
scene.cycles.samples = 64
bake("AO", "ao", "Non-Color", selected=False)
for name, value in (("roughness", cfg.get("roughness", 0.8)), ("metallic", cfg.get("metallic", 0.0))):
    image = bpy.data.images.new(name, size, size)  # the packer stacks it with the AO into one ORM map
    image.colorspace_settings.name = "Non-Color"
    image.pixels.foreach_set(np.tile([value, value, value, 1.0], size * size).astype(np.float32))
    image.filepath_raw = str(maps / f"{name}.png")
    image.file_format = "PNG"
    image.save()

if own:
    for o in kept:  # their own material named by the config; the AO at the size of their texture
        kind = o["kind"]
        own_mat = bpy.data.materials.new(separate[kind])
        own_mat["group"] = own.get("groups", {}).get(kind, "UNDEF")
        own_mat.use_nodes = True
        node = own_mat.node_tree.nodes.new("ShaderNodeTexImage")
        own_mat.node_tree.nodes.active = node
        o.data.materials.append(own_mat)
        w, h = own.get("sizes", {}).get(kind, [1024, 1024])
        node.image = bpy.data.images.new(f"{kind}-ao", w, h, float_buffer=False)
        node.image.colorspace_settings.name = "Non-Color"
        for other in scene.objects:
            other.select_set(other is o)
        bpy.context.view_layer.objects.active = o
        bpy.ops.object.bake(type="AO", margin=16)
        (out / kind).mkdir(exist_ok=True)
        node.image.filepath_raw = str(out / kind / "ao.png")
        node.image.file_format = "PNG"
        node.image.save()
        node.image = None
    for o in scene.objects:
        o.select_set(o is low or o in kept)
    bpy.context.view_layer.objects.active = low
    bpy.ops.object.join()

# --- export ----------------------------------------------------------------------------------------------------------
target.image = None
for o in sources:
    bpy.data.objects.remove(o)
for o in scene.objects:
    o.select_set(o is low)
bpy.context.view_layer.objects.active = low
bpy.ops.export_scene.gltf(filepath=str(out / "model.glb"), export_format="GLB", use_selection=True, export_apply=True,
                          export_yup=True, export_extras=True, export_normals=True, export_texcoords=True,
                          export_animations=False)
co = np.array([low.matrix_world @ v.co for v in low.data.vertices])
metrics = {"triangles": sum(len(p.vertices) - 2 for p in low.data.polygons), "budget": budget,
           "high_triangles": len(mesh.polygons), "bbox_delta_min_m": (co.min(0) - glo).round(4).tolist(),
           "bbox_delta_max_m": (co.max(0) - ghi).round(4).tolist(), "blender_version": bpy.app.version_string}
(out / "metrics.json").write_text(__import__("json").dumps(metrics, indent=2))
print("BUILT", metrics)
