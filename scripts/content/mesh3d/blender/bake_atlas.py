"""Bake one unique texture atlas for a prop from stage-1 tiling materials.

blender -b --factory-startup -P bake_atlas.py -- config.json OUT_DIR
config: {"glb": model with tile UVs in metres and zone-key materials (barrel.py),
         "maps_dir": dir with <key lower>/{albedo,normal,roughness,metallic}.png
         (relative paths: under content-staging),
         "scales": {KEY: tile scale}, "targets": {KEY: [measured linear r, g, b]},
         "slots": {KEY: [ATLAS_KEY, GROUP]}, "metal": [KEY, ...],
         "size": 1024, "colour_samples": 16, "seed": 1, "ao_samples": 64, "ao_distance": 0.15, "ao_strength": 0.85,
         "sectors": 20, "tone_spread": 0.28, "light_part": [index, gain],
         "edge_radius": 0.006, "wet_height": 0.18, "algae": 0.7, "bleach": 0.35,
         "planks_per_tile": {KEY: n}, "seam_width": 0.07, "seam_dark": 0.75, "contrast": {KEY: gamma},
         "nodes": [NODE NAME, ...], "tile_normal": {KEY: strength}, "edge_wear": {KEY: share},
         "measure_ao": {KEY: min baked AO}}
The tiling materials are sampled through the tile UVs and baked into a new atlas
UV: per-part tone (the builder's "_part" attribute when present, otherwise
angular sectors around the axis and lid planks along X), dark seams where a part's tile plank ends, worn light edges from the Bevel
node, sun-bleached tops, a wet algae-dark bottom and baked AO. Each zone then
gets more local contrast and is scaled back to its measured mean colour: the
original reads by painted contrast, not by a brighter mean.
Writes atlas PNGs, model.glb with the atlas as its only UV map, metrics.json.
With "nodes" (a MOB with several attachments, chest.py) the GLB's objects are
baked into one atlas together and exported again as those named nodes.
The stage-1 filter (bakeoff.filtered_maps) stretches every tile's relief to the full
range: a wood tile's normal leans 38 degrees on average, canvas 64, where real grain is a
few. Baked at full strength the wood read in game as an embossed imitation and cloth as
a hard, sparkling surface (user, 2026-10-20). So each tile's normal map is baked at a
strength by its material: cloth x0.12, our own gentle procedural tiles (glass, wax, bone)
x1, everything else x0.25; "tile_normal" overrides it per zone. Cloth, rope and fur get
no worn light edges ("edge_wear", default 0.7 of the Bevel edge mask).
A zone's mean is measured over all its texels, so hidden faces count: half of a crate's
plank area is plank backs against the dark core, ends under posts and edges in the gaps,
and the visible planks baked 1.9 times lighter than the target (reports/engine/2026-10-21-
rough-specular). "measure_ao" measures such a zone only over texels whose baked AO is at
least the given value, the faces open to the outside; the gain still reaches every texel.
"""
import json
import math
import shutil
import sys
from pathlib import Path

import bpy
import numpy as np

args = sys.argv[sys.argv.index("--") + 1:]
cfg = json.loads(Path(args[0]).read_text())
out = Path(args[1])
out.mkdir(parents=True, exist_ok=True)
staging = Path.home() / ".local/share/gothic-ai/content-staging"
size = cfg.get("size", 1024)
rng = np.random.default_rng(cfg.get("seed", 1))
metal_keys = set(cfg.get("metal", []))
CLOTH_WORDS = ("CLOTH", "CANVAS", "SACKING", "CARPET")
GENTLE_WORDS = ("GLASS", "WAX", "BONE")      # own procedural tiles with real, weak relief
NO_WEAR_WORDS = CLOTH_WORDS + ("ROPE", "FUR")


def tile_normal(key):
    """Strength of a zone's tile normal map in the bake."""
    if key in cfg.get("tile_normal", {}):
        return cfg["tile_normal"][key]
    if any(w in key for w in CLOTH_WORDS):
        return 0.12
    if any(w in key for w in GENTLE_WORDS):
        return 1.0
    return 0.25


def edge_wear(key):
    """Share of the Bevel edge mask that lightens a non-metal zone's edges (worn wood)."""
    if key in cfg.get("edge_wear", {}):
        return cfg["edge_wear"][key]
    return 0.0 if any(w in key for w in NO_WEAR_WORDS) else 0.7

bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.render.engine = "CYCLES"
scene.cycles.device = "CPU"
world = bpy.data.worlds.new("world")
world.use_nodes = True
world.light_settings.distance = cfg.get("ao_distance", 0.15)
scene.world = world

bpy.ops.import_scene.gltf(filepath=str(staging / cfg["glb"]))  # an absolute path stays as it is
meshes = [o for o in scene.objects if o.type == "MESH"]
node_names = cfg.get("nodes", [])
if node_names:  # remember each face's node through the join
    if sorted(o.name for o in meshes) != sorted(node_names):
        raise SystemExit(f"GLB objects {[o.name for o in meshes]} do not match nodes {node_names}")
    for o in meshes:
        attr = o.data.attributes.new("_node", "INT", "FACE")
        attr.data.foreach_set("value", np.full(len(o.data.polygons), node_names.index(o.name), np.int32))
for o in scene.objects:
    o.select_set(o in meshes)
bpy.context.view_layer.objects.active = meshes[0]
if len(meshes) > 1:
    bpy.ops.object.join()
obj = bpy.context.view_layer.objects.active
mesh = obj.data
mesh.uv_layers[0].name = "Tile"
keys = [slot.material.name.split(".")[0] for slot in obj.material_slots]

# --- atlas UV: angle-based islands, packed with rotation ---
atlas = mesh.uv_layers.new(name="Atlas")
mesh.uv_layers.active = atlas
bpy.ops.object.mode_set(mode="EDIT")
bpy.ops.mesh.select_all(action="SELECT")
bpy.ops.uv.smart_project(angle_limit=math.radians(cfg.get("angle_limit", 35)), island_margin=0.0,
                         area_weight=0.0, correct_aspect=True, scale_to_bounds=False)
bpy.ops.uv.pack_islands(rotate=True, margin=cfg.get("margin", 0.006))
bpy.ops.object.mode_set(mode="OBJECT")

# --- per-part tone: sectors around the axis; lid planks along X ---
n_faces = len(mesh.polygons)
centres = np.empty(n_faces * 3)
mesh.polygons.foreach_get("center", centres)
centres = centres.reshape(-1, 3)
normals = np.empty(n_faces * 3)
mesh.polygons.foreach_get("normal", normals)
normals = normals.reshape(-1, 3)
mat_index = np.empty(n_faces, np.int32)
mesh.polygons.foreach_get("material_index", mat_index)
co = np.empty(len(mesh.vertices) * 3)
mesh.vertices.foreach_get("co", co)
co = co.reshape(-1, 3)
lo, hi = co.min(axis=0), co.max(axis=0)
axis = (lo[:2] + hi[:2]) / 2
sectors = cfg.get("sectors", 20)
angle = np.arctan2(centres[:, 1] - axis[1], centres[:, 0] - axis[0]) % (2 * math.pi)
part = np.floor(angle / (2 * math.pi / sectors)).astype(int)
head = np.array([keys[i].endswith("HEAD") for i in mat_index])
lid_planks = cfg.get("lid_planks", 4)
plank = np.clip(((centres[:, 0] - lo[0]) / (hi[0] - lo[0]) * lid_planks).astype(int), 0, lid_planks - 1)
part = np.where(head, sectors + plank, part)
if "_PART" in mesh.attributes:  # the builder numbered its parts (a POINT attribute "_part" in the GLB)
    values = np.empty(len(mesh.vertices), np.float32)
    mesh.attributes["_PART"].data.foreach_get("value", values)
    first_vertex = np.array([mesh.loops[p.loop_start].vertex_index for p in mesh.polygons])
    part = np.rint(values[first_vertex]).astype(int)
spread = cfg.get("tone_spread", 0.28)
tones = 1 + spread * rng.uniform(-1, 1, max(int(part.max()) + 1, sectors + lid_planks))
light = cfg.get("light_part")
if light:
    tones[light[0]] = light[1]
tone_attr = mesh.attributes.new("tone", "FLOAT", "FACE")
tone_attr.data.foreach_set("value", tones[part].astype(np.float32))
zone_attr = mesh.attributes.new("zone", "FLOAT", "FACE")
# Faces pointing down (a barrel's bottom on the ground) are left out of the measured mean, but still
# get their zone's gain: a canopy's underside is seen. They carry half a step on top of their zone.
hidden = normals[:, 2] < -0.9
zone_attr.data.foreach_set("value", (mat_index + 1 + np.where(hidden, 0.5, 0.0)).astype(np.float32))


# --- bake materials ---
def node(nodes, kind, **props):
    n = nodes.new(kind)
    for k, v in props.items():
        setattr(n, k, v)
    return n


def math_node(nodes, links, op, a, b=None, clamp=False):
    n = node(nodes, "ShaderNodeMath", operation=op, use_clamp=clamp)
    for socket, value in ((n.inputs[0], a), (n.inputs[1], b)):
        if value is None:
            continue
        if isinstance(value, (int, float)):
            socket.default_value = value
        else:
            links.new(value, socket)
    return n.outputs[0]


def mix(nodes, links, factor, a, b, blend="MIX"):
    n = node(nodes, "ShaderNodeMix", data_type="RGBA", blend_type=blend)
    for socket, value in ((n.inputs["Factor"], factor), (n.inputs["A"], a), (n.inputs["B"], b)):
        if isinstance(value, (int, float)):
            socket.default_value = value
        elif isinstance(value, tuple):
            socket.default_value = value
        else:
            links.new(value, socket)
    return n.outputs["Result"]


passes = {}
targets = {}
z0, height = lo[2], hi[2] - lo[2]
for slot in obj.material_slots:
    mat = slot.material
    key = mat.name.split(".")[0]
    folder = staging / cfg["maps_dir"] / key.lower()
    metal = key in metal_keys
    mat.use_nodes = True
    nodes, links = mat.node_tree.nodes, mat.node_tree.links
    nodes.clear()
    output = node(nodes, "ShaderNodeOutputMaterial")
    uv = node(nodes, "ShaderNodeUVMap", uv_map="Tile")
    mapping = node(nodes, "ShaderNodeMapping")
    scale = cfg["scales"].get(key, 1.0)
    mapping.inputs["Scale"].default_value = (scale, scale, 1)
    links.new(uv.outputs[0], mapping.inputs[0])

    def tex(name, colour):
        n = node(nodes, "ShaderNodeTexImage")
        n.image = bpy.data.images.load(str(folder / f"{name}.png"))
        n.image.colorspace_settings.name = "sRGB" if colour else "Non-Color"
        links.new(mapping.outputs[0], n.inputs[0])
        return n.outputs[0]

    albedo, normal_tex, rough, metallic = tex("albedo", True), tex("normal", False), tex("roughness", False), tex("metallic", False)
    tone = node(nodes, "ShaderNodeAttribute", attribute_type="GEOMETRY", attribute_name="tone").outputs["Fac"]
    geo = node(nodes, "ShaderNodeNewGeometry")
    bevel = node(nodes, "ShaderNodeBevel", samples=8)
    bevel.inputs["Radius"].default_value = cfg.get("edge_radius", 0.006)
    dot = node(nodes, "ShaderNodeVectorMath", operation="DOT_PRODUCT")
    links.new(bevel.outputs[0], dot.inputs[0])
    links.new(geo.outputs["True Normal"], dot.inputs[1])
    edge = math_node(nodes, links, "MULTIPLY", math_node(nodes, links, "SUBTRACT", 1.0, dot.outputs["Value"]), 9.0, clamp=True)
    sep = node(nodes, "ShaderNodeSeparateXYZ")
    links.new(geo.outputs["Position"], sep.inputs[0])
    z = math_node(nodes, links, "SUBTRACT", sep.outputs["Z"], float(z0))
    wet = node(nodes, "ShaderNodeMapRange", clamp=True)
    links.new(z, wet.inputs["Value"])
    wet.inputs["From Min"].default_value = cfg.get("wet_height", 0.18)
    wet.inputs["From Max"].default_value = 0.0
    wet = wet.outputs["Result"]
    nsep = node(nodes, "ShaderNodeSeparateXYZ")
    links.new(geo.outputs["Normal"], nsep.inputs[0])
    up = math_node(nodes, links, "POWER", math_node(nodes, links, "MAXIMUM", nsep.outputs["Z"], 0.0), 2.0)
    noise = node(nodes, "ShaderNodeTexNoise")
    noise.inputs["Scale"].default_value = 18.0
    noise.inputs["Detail"].default_value = 6.0
    links.new(geo.outputs["Position"], noise.inputs["Vector"])
    algae = math_node(nodes, links, "MULTIPLY", wet, math_node(nodes, links, "MULTIPLY",
                      math_node(nodes, links, "SUBTRACT", noise.outputs["Fac"], 0.42), 4.0, clamp=True))

    colour = mix(nodes, links, 1.0, albedo, tone, "MULTIPLY")
    planks = cfg.get("planks_per_tile", {}).get(key)
    if planks:  # one part = one plank of the tile, so its edges sit at whole plank fractions of U
        tile_uv = node(nodes, "ShaderNodeSeparateXYZ")
        links.new(mapping.outputs[0], tile_uv.inputs[0])
        f = math_node(nodes, links, "FRACT", math_node(nodes, links, "MULTIPLY", tile_uv.outputs["X"], float(planks)))
        near = math_node(nodes, links, "MINIMUM", f, math_node(nodes, links, "SUBTRACT", 1.0, f))
        seam = node(nodes, "ShaderNodeMapRange", clamp=True)
        links.new(near, seam.inputs["Value"])
        seam.inputs["From Min"].default_value = cfg.get("seam_width", 0.07)
        seam.inputs["From Max"].default_value = 0.0
        dark = math_node(nodes, links, "MULTIPLY", seam.outputs["Result"], cfg.get("seam_dark", 0.75))
        colour = mix(nodes, links, dark, colour, (0.0, 0.0, 0.0, 1.0))
    grey = node(nodes, "ShaderNodeRGBToBW")
    links.new(colour, grey.inputs[0])
    if metal:
        # Worn but not mirror-like: narrow hoop lips are all edge and face the sky, so a
        # shiny edge turns into a bright dashed line in game.
        bright = mix(nodes, links, 1.0, colour, (1.5, 1.5, 1.5, 1.0), "MULTIPLY")
        colour = mix(nodes, links, math_node(nodes, links, "MULTIPLY", edge, 0.5), colour, bright)
        # Hoop ledges face straight up or down and catch the sky as thin bright lines; real ones hold dirt.
        ledge = math_node(nodes, links, "POWER", math_node(nodes, links, "ABSOLUTE", nsep.outputs["Z"]), 4.0)
        colour = mix(nodes, links, math_node(nodes, links, "MULTIPLY", ledge, cfg.get("ledge_dark", 0.7)), colour,
                     (0.0, 0.0, 0.0, 1.0))
        rough_out = math_node(nodes, links, "ADD", rough, math_node(nodes, links, "MULTIPLY", edge, 0.15), clamp=True)
        metal_out = math_node(nodes, links, "MULTIPLY", metallic, math_node(nodes, links, "SUBTRACT", 1.0,
                              math_node(nodes, links, "MULTIPLY", edge, 0.5)), clamp=True)
    else:
        worn = mix(nodes, links, 1.0, colour, (2.2, 2.05, 1.85, 1.0), "MULTIPLY")
        colour = mix(nodes, links, math_node(nodes, links, "MULTIPLY", edge, edge_wear(key)), colour, worn)
        bleached = mix(nodes, links, 1.0, grey.outputs[0], (1.45, 1.4, 1.32, 1.0), "MULTIPLY")
        colour = mix(nodes, links, math_node(nodes, links, "MULTIPLY", up, cfg.get("bleach", 0.35)), colour, bleached)
        rough_out = rough
        metal_out = metallic
    colour = mix(nodes, links, math_node(nodes, links, "MULTIPLY", wet, 0.45), colour, (0.0, 0.0, 0.0, 1.0))
    colour = mix(nodes, links, math_node(nodes, links, "MULTIPLY", algae, cfg.get("algae", 0.7)), colour, (0.010, 0.016, 0.006, 1.0))

    nmap = node(nodes, "ShaderNodeNormalMap", uv_map="Tile")
    nmap.inputs["Strength"].default_value = tile_normal(key)
    links.new(normal_tex, nmap.inputs["Color"])
    bevel_n = node(nodes, "ShaderNodeBevel", samples=8)
    bevel_n.inputs["Radius"].default_value = cfg.get("edge_radius", 0.006)
    links.new(nmap.outputs[0], bevel_n.inputs["Normal"])
    bsdf = node(nodes, "ShaderNodeBsdfPrincipled")
    links.new(bevel_n.outputs[0], bsdf.inputs["Normal"])
    emission = node(nodes, "ShaderNodeEmission")
    zone_val = node(nodes, "ShaderNodeAttribute", attribute_type="GEOMETRY", attribute_name="zone").outputs["Fac"]
    passes[mat.name] = dict(nodes=nodes, links=links, output=output, emission=emission, bsdf=bsdf,
                            colour=colour, rough=rough_out, metal=metal_out, zone=zone_val)
    targets[key] = cfg["targets"][key]


def bake_pass(name, kind, source=None, samples=4, margin=16, colour_space="Non-Color"):
    image = bpy.data.images.new(name, size, size, alpha=False, float_buffer=True)
    image.colorspace_settings.name = colour_space
    for p in passes.values():
        nodes, links = p["nodes"], p["links"]
        target = nodes.get("bake_target") or node(nodes, "ShaderNodeTexImage", name="bake_target")
        target.image = image
        nodes.active = target
        for link in list(p["output"].inputs["Surface"].links):
            links.remove(link)
        if kind == "EMIT":
            for link in list(p["emission"].inputs["Color"].links):
                links.remove(link)
            value = p[source]
            if value.type == "VALUE":
                rgb = node(nodes, "ShaderNodeCombineColor")
                for socket in rgb.inputs[:3]:
                    links.new(value, socket)
                value = rgb.outputs[0]
            links.new(value, p["emission"].inputs["Color"])
            links.new(p["emission"].outputs[0], p["output"].inputs["Surface"])
        else:
            links.new(p["bsdf"].outputs[0], p["output"].inputs["Surface"])
    scene.cycles.samples = samples
    bake = scene.render.bake
    bake.margin = margin
    bake.margin_type = "EXTEND"
    for o in scene.objects:
        o.select_set(o == obj)
    bpy.context.view_layer.objects.active = obj
    extra = dict(normal_space="TANGENT") if kind == "NORMAL" else {}
    bpy.ops.object.bake(type=kind, use_clear=True, margin=margin, **extra)
    pixels = np.empty(size * size * 4, np.float32)
    image.pixels.foreach_get(pixels)
    print("BAKED", name)
    return pixels.reshape(size, size, 4)[:, :, :3]


colour = bake_pass("colour", "EMIT", "colour", samples=cfg.get("colour_samples", 16))  # samples filter the fine tile grain
rough = bake_pass("rough", "EMIT", "rough", samples=cfg.get("colour_samples", 16))[:, :, 0]
metal = bake_pass("metal", "EMIT", "metal")[:, :, 0]
zone = bake_pass("zone", "EMIT", "zone", samples=1, margin=16)[:, :, 0]
inside = bake_pass("inside", "EMIT", "zone", samples=1, margin=0)[:, :, 0] > 0.5
normal = bake_pass("normal", "NORMAL", samples=cfg.get("colour_samples", 16))
ao = bake_pass("ao", "AO", samples=cfg.get("ao_samples", 64))[:, :, 0]

strength = cfg.get("ao_strength", 0.85)
albedo = colour * (1 - strength * (1 - ao))[..., None]
zone_id = np.floor(zone + 1e-3).astype(int)
down = zone - zone_id > 0.25
report = {}
for index, key in enumerate(keys):
    sel = zone_id == index + 1  # the gain reaches every pixel of the zone, faces pointing down included
    measured = sel & inside & ~down
    floor = cfg.get("measure_ao", {}).get(key)
    if floor is not None:  # only the texels open to the outside
        measured &= ao >= floor
    if not measured.any():
        continue
    gamma = cfg.get("contrast", {}).get(key, 1.0)
    if gamma != 1.0:  # stretch each pixel's luminance ratio to the zone mean, keep its hue
        lum = albedo @ np.array([0.2126, 0.7152, 0.0722])
        ref = lum[measured].mean()
        ratio = np.where(sel, (np.maximum(lum, 1e-6) / ref) ** (gamma - 1.0), 1.0)
        albedo *= ratio[..., None]
    mean = albedo[measured].mean(axis=0)
    gain = np.array(targets[key]) / np.maximum(mean, 1e-6)
    albedo[sel] *= gain
    whole = sel & inside & ~down
    report[key] = {"pixels": int(measured.sum()), "measure_ao": floor,
                   "measured_share": round(float(measured.sum() / whole.sum()), 4),
                   "mean_all_after": albedo[whole].mean(axis=0).round(5).tolist(),
                   "contrast": gamma, "mean_before": mean.round(5).tolist(),
                   "gain": gain.round(3).tolist(), "target": targets[key],
                   "tile_normal": tile_normal(key), "edge_wear": edge_wear(key) if key not in metal_keys else None}
albedo = np.clip(albedo, 0, 1)


def srgb(lin):
    return np.where(lin <= 0.0031308, 12.92 * lin, 1.055 * np.power(lin, 1 / 2.4) - 0.055)


def save_png(path, array):
    """Write through a Blender image so the row order matches the bake."""
    data = np.clip(array, 0, 1)
    if data.ndim == 2:
        data = np.repeat(data[..., None], 3, axis=2)
    rgba = np.concatenate([data, np.ones(data.shape[:2] + (1,), np.float32)], axis=2)
    image = bpy.data.images.new(path.stem, size, size, alpha=True)
    image.colorspace_settings.name = "Non-Color"
    image.pixels.foreach_set(rgba.astype(np.float32).ravel())
    image.filepath_raw = str(path)
    image.file_format = "PNG"
    image.save()


maps = out / "maps"
first = None
for key, (atlas_key, _group) in cfg["slots"].items():
    folder = maps / atlas_key.lower()
    if first is None:
        folder.mkdir(parents=True, exist_ok=True)
        save_png(folder / "albedo.png", srgb(albedo))
        save_png(folder / "normal.png", normal)
        save_png(folder / "roughness.png", rough)
        save_png(folder / "metallic.png", metal)
        save_png(folder / "ao.png", np.ones_like(ao))  # AO is already in the albedo
        save_png(out / "atlas-ao.png", ao)
        first = folder
    elif not folder.exists():
        shutil.copytree(first, folder)

# --- export: atlas UV only, zone materials merged into atlas materials ---
mesh.uv_layers.remove(mesh.uv_layers["Tile"])
mesh.attributes.remove(mesh.attributes["tone"])
if "_PART" in mesh.attributes:
    mesh.attributes.remove(mesh.attributes["_PART"])
mesh.attributes.remove(mesh.attributes["zone"])
new_slots = []
for key, (atlas_key, group) in cfg["slots"].items():
    if atlas_key not in new_slots:
        new_slots.append(atlas_key)
remap = np.array([new_slots.index(cfg["slots"][k][0]) for k in keys])
mesh.materials.clear()  # resets every face's material index, so the indices are set after the new materials
groups = {atlas_key: group for atlas_key, group in cfg["slots"].values()}
for atlas_key in new_slots:
    mat = bpy.data.materials.new(atlas_key)
    mat["group"] = groups[atlas_key]
    mesh.materials.append(mat)
mesh.polygons.foreach_set("material_index", remap[mat_index].astype(np.int32))
mesh.update()
written = np.empty(len(mesh.polygons), np.int32)
mesh.polygons.foreach_get("material_index", written)
faces_per_material = np.bincount(written, minlength=len(new_slots)).tolist()
exported = [obj]
if node_names:  # split the baked mesh back into its named nodes
    for index in range(1, len(node_names)):
        node = np.empty(len(mesh.polygons), np.int32)
        mesh.attributes["_node"].data.foreach_get("value", node)
        faces = node == index
        verts = np.zeros(len(mesh.vertices), bool)
        loops = np.empty(len(mesh.loops), np.int32)
        mesh.loops.foreach_get("vertex_index", loops)
        starts = np.empty(len(mesh.polygons), np.int32)
        mesh.polygons.foreach_get("loop_start", starts)
        totals = np.empty(len(mesh.polygons), np.int32)
        mesh.polygons.foreach_get("loop_total", totals)
        for start, total in zip(starts[faces], totals[faces]):
            verts[loops[start:start + total]] = True
        mesh.vertices.foreach_set("select", verts)
        edge_verts = np.empty(len(mesh.edges) * 2, np.int32)
        mesh.edges.foreach_get("vertices", edge_verts)
        mesh.edges.foreach_set("select", verts[edge_verts.reshape(-1, 2)].all(axis=1))
        mesh.polygons.foreach_set("select", faces)
        for o in scene.objects:
            o.select_set(o == obj)
        bpy.context.view_layer.objects.active = obj
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.separate(type="SELECTED")
        bpy.ops.object.mode_set(mode="OBJECT")
    exported = [o for o in scene.objects if o.type == "MESH"]
    for o in exported:
        o.name = "tmp-" + o.name
    for o in exported:
        values = np.empty(len(o.data.polygons), np.int32)
        o.data.attributes["_node"].data.foreach_get("value", values)
        if len(set(values.tolist())) != 1:
            raise SystemExit(f"node split left mixed faces in {o.name}")
        o.name = o.data.name = node_names[int(values[0])]
        o.data.attributes.remove(o.data.attributes["_node"])
for o in scene.objects:
    o.select_set(o in exported)
bpy.ops.export_scene.gltf(filepath=str(out / "model.glb"), export_format="GLB", use_selection=True,
                          export_apply=True, export_yup=True, export_extras=True, export_normals=True,
                          export_texcoords=True, export_animations=False, export_attributes=False)
coverage = float(inside.mean())
metrics = {"atlas": size, "coverage": round(coverage, 4),
           "triangles": sum(len(p.vertices) - 2 for o in exported for p in o.data.polygons),
           "materials": new_slots, "faces_per_material": dict(zip(new_slots, faces_per_material)), "zones": report,
           "blender_version": bpy.app.version_string}
if node_names:
    metrics["nodes"] = {o.name: len(o.data.polygons) for o in exported}
(out / "metrics.json").write_text(json.dumps(metrics, indent=2))
print("ATLAS", json.dumps(metrics))
