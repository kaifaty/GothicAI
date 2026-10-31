"""Bake one unique texture atlas for a prop from stage-1 tiling materials.

blender -b --factory-startup -P bake_atlas.py -- config.json OUT_DIR
config: {"glb": model with tile UVs in metres and zone-key materials (barrel.py),
         "maps_dir": dir with <key lower>/{albedo,normal,roughness,metallic}.png
         (relative paths: under content-staging),
         "scales": {KEY: tile scale}, "targets": {KEY: [measured linear r, g, b]},
         "slots": {KEY: [ATLAS_KEY, GROUP]}, "metal": [KEY, ...],
         "size": 1024, "colour_samples": 16, "seed": 1, "ao_samples": 64, "ao_distance": 0.15, "ao_strength": 0.85,
         "sectors": 20, "tone_spread": 0.28, "light_part": [index, gain],
         "island_scale": {KEY: share}, "despeckle": {"radius": 6, "below": 0.6, "lift": 0.7}, "edge_radius": 0.006, "wet_height": 0.18, "algae": 0.7, "bleach": 0.35,
         "planks_per_tile": {KEY: n}, "seam_width": 0.07, "seam_dark": 0.75, "contrast": {KEY: gamma},
         "nodes": [NODE NAME, ...], "tile_normal": {KEY: strength}, "edge_wear": {KEY: share},
         "measure_ao": {KEY: min baked AO},
         "crevice": {"distance": 0.02, "dirt": 0.7, "colour": [r, g, b]},
         "seam": {"radius": 0.012, "gain": 4.0, "dark": 0.5, "colour": [r, g, b]},
         "streaks": {"length": 0.25, "rust": 0.6, "dirt": 0.35, "rust_colour": [r, g, b], "column": 0.012},
         "wet_roughness": 0.6, "metal_scale": 1.0,
         "moss": {"zones": [KEY, ...], "cover": share or {KEY: share}, "scale": 0.12, "grain": 1.0, "detail": 0.0, "top": 1.0, "bottom": 0.8,
                  "low": 0.25, "shade": 0.6, "edges": 0.0, "soft": 0.25, "damp": 0.25, "dark": [r, g, b], "light": [r, g, b],
                  "opacity": 1.0, "keep_grain": 0.0}}
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
Weathering placed by the model, as for the roofs made of pieces (docs/remaster-pipeline.md,
«Материал из модели»); each is off unless its key is set, so older props bake as before:
- "crevice": dirt (or moss, by its colour) packed in the joints between parts, from a short AO;
- "seam": dirt along every edge of a part, from a wider Bevel than the worn edges: a slit of a few
  millimetres between two lid planks darkens no flat top in the AO, and the slit alone broke into a
  dotted line under a grazing look (crate lids, harbour-rigging fourth review); a band about "radius"
  wide on both sides of it stays a line in the mips;
- "streaks": water runs down from the parts above a point; a ray from every texel up along its
  surface finds them. Under iron it leaves rust, under another part dirt, fading over "length"
  metres, in vertical columns about "column" metres wide;
- "wet_roughness": the wet bottom is smoother, down to this roughness; rust and dirt stay matte.
The weathering darkens before each zone is scaled to its measured mean, like the AO.
- "moss": patches of moss after the zones met their targets, so the green is the moss's own: on tops,
  near the ground and in shaded corners, cut out of a smooth 3D noise at "cover" of each zone's open texels,
  with a damp darker halo around them; "grain" > 1 stretches the patches that many times along each part's
  long axis, so they run as strips along boards and beams; "detail" adds a finer unstretched layer that
  breaks their edges (the strips alone read as regular hatching on the crate lids, fourth review); "edges"
  draws it to the "seam" mask, to the gaps and edges of boards, not into camouflage spots on their faces;
  "opacity" below 1 and "keep_grain" (0-1, how much of the wood's own light and dark the moss keeps) let
  the grain show through, so a mossy beam is not a flat green fill (fifth review). The harbour crates had a thin dotted line of
  crevice moss and 0-0.2 % green on their lids against 1.2-6 % on the original's; round patches then read
  as stickers (asset harbour-rigging, reviews 2026-10-30).
"metal_scale" multiplies the metallic of the "metal" zones: rusty, grimy iron is mostly not metal.
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
if cfg.get("island_scale"):
    # Faces nobody sees (a crate's dark core) shrink before packing, so the open boards get the texels:
    # the harbour pile gave its cores as much atlas as its planks and its lids read as felt from afar.
    # Each such face shrinks about its own centre; pack_islands then treats it as its own island.
    bpy.ops.object.mode_set(mode="OBJECT")
    uv = mesh.uv_layers["Atlas"].data
    for poly in mesh.polygons:
        factor = cfg["island_scale"].get(keys[poly.material_index], 1.0)
        if factor == 1.0:
            continue
        loops = list(poly.loop_indices)
        cu = sum(uv[i].uv[0] for i in loops) / len(loops)
        cv = sum(uv[i].uv[1] for i in loops) / len(loops)
        for i in loops:
            uv[i].uv = (cu + (uv[i].uv[0] - cu) * factor, cv + (uv[i].uv[1] - cv) * factor)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
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
part_attr = mesh.attributes.new("part_id", "FLOAT", "FACE")    # which part a texel belongs to, for the streaks
part_attr.data.foreach_set("value", part.astype(np.float32))
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
    seam_bevel = node(nodes, "ShaderNodeBevel", samples=16)
    seam_bevel.inputs["Radius"].default_value = cfg.get("seam", {}).get("radius", 0.012)
    seam_dot = node(nodes, "ShaderNodeVectorMath", operation="DOT_PRODUCT")
    links.new(seam_bevel.outputs[0], seam_dot.inputs[0])
    links.new(geo.outputs["True Normal"], seam_dot.inputs[1])
    seam_mask = math_node(nodes, links, "MULTIPLY", math_node(nodes, links, "SUBTRACT", 1.0, seam_dot.outputs["Value"]),
                          cfg.get("seam", {}).get("gain", 4.0), clamp=True)
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
        if "metal_scale" in cfg:
            # Rusty, grimy iron is mostly not metal: a metal gets no diffuse light, so in shade
            # the original's mossy hoops, lighter than the staves, came out black.
            metal_out = math_node(nodes, links, "MULTIPLY", metal_out, cfg["metal_scale"])
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
    # where a texel lies on the model and which way it faces, for the weathering placed by geometry
    rel = node(nodes, "ShaderNodeVectorMath", operation="SUBTRACT")
    links.new(geo.outputs["Position"], rel.inputs[0])
    rel.inputs[1].default_value = tuple(float(v) for v in lo)
    rel_scaled = node(nodes, "ShaderNodeVectorMath", operation="DIVIDE")
    links.new(rel.outputs[0], rel_scaled.inputs[0])
    rel_scaled.inputs[1].default_value = tuple(float(max(v, 1e-6)) for v in hi - lo)
    facing = node(nodes, "ShaderNodeVectorMath", operation="MULTIPLY_ADD")
    links.new(geo.outputs["Normal"], facing.inputs[0])
    facing.inputs[1].default_value = (0.5, 0.5, 0.5)
    facing.inputs[2].default_value = (0.5, 0.5, 0.5)
    part_val = node(nodes, "ShaderNodeAttribute", attribute_type="GEOMETRY", attribute_name="part_id").outputs["Fac"]
    passes[mat.name] = dict(nodes=nodes, links=links, output=output, emission=emission, bsdf=bsdf,
                            colour=colour, rough=rough_out, metal=metal_out, zone=zone_val,
                            position=rel_scaled.outputs[0], facing=facing.outputs[0], part_id=part_val, seam=seam_mask)
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
if "despeckle" in cfg:
    # Knots in a plank tile are small, near-black and repeat with the tile: from above a crate lid they read as a
    # pattern of dark dots (crates-path, fifth review). Texels much darker than their neighbourhood are lifted
    # toward its mean, so a knot stays a soft trace.
    spec = cfg["despeckle"]
    r = spec.get("radius", 6)

    def box(a):
        c = np.cumsum(np.cumsum(np.pad(a, ((r + 1, r), (r + 1, r), (0, 0)), mode="edge"), 0), 1)
        return (c[2 * r + 1:, 2 * r + 1:] - c[:-2 * r - 1, 2 * r + 1:] - c[2 * r + 1:, :-2 * r - 1] + c[:-2 * r - 1, :-2 * r - 1]) / (2 * r + 1) ** 2
    local = box(colour)
    lum, lum_local = colour @ np.array([0.2126, 0.7152, 0.0722]), local @ np.array([0.2126, 0.7152, 0.0722])
    dark = np.clip((spec.get("below", 0.6) - lum / np.maximum(lum_local, 1e-6)) / 0.2, 0, 1) * spec.get("lift", 0.7)
    colour = colour * (1 - dark[..., None]) + local * dark[..., None]
    albedo = colour * (1 - strength * (1 - ao))[..., None]
weathering = {}
if any(k in cfg for k in ("crevice", "streaks", "wet_roughness", "moss", "seam")):
    position = lo + bake_pass("position", "EMIT", "position", samples=1, margin=16) * (hi - lo)
    texel_part = np.rint(bake_pass("part", "EMIT", "part_id", samples=1, margin=16)[:, :, 0]).astype(int)
    facing = bake_pass("facing", "EMIT", "facing", samples=1, margin=16) * 2 - 1
    is_metal = np.array([keys[i] in metal_keys for i in range(len(keys))])
    zone_index = np.floor(zone + 1e-3).astype(int) - 1
    zone_metal = (zone_index >= 0) & is_metal[np.clip(zone_index, 0, len(keys) - 1)]
    if "crevice" in cfg:
        spec = cfg["crevice"]
        world.light_settings.distance = spec.get("distance", 0.02)
        crevice = bake_pass("crevice", "AO", samples=cfg.get("ao_samples", 64))[:, :, 0]
        world.light_settings.distance = cfg.get("ao_distance", 0.15)
        gap = np.clip((1 - crevice) / 0.5, 0, 1) * ~zone_metal
        dirt = spec.get("dirt", 0.7) * gap
        albedo = albedo * (1 - dirt[..., None]) + np.array(spec.get("colour", [0.008, 0.007, 0.005])) * dirt[..., None]
        weathering["crevice"] = gap
    seam = np.zeros((size, size))
    if "seam" in cfg or cfg.get("moss", {}).get("edges"):
        seam = bake_pass("seam", "EMIT", "seam", samples=cfg.get("seam", {}).get("samples", 16))[:, :, 0] * ~zone_metal
    if "seam" in cfg:
        weathering["seam"] = seam
    if "streaks" in cfg:
        from mathutils import Vector
        from mathutils.bvhtree import BVHTree
        spec = cfg["streaks"]
        length = spec.get("length", 0.25)
        tree = BVHTree.FromObject(obj, bpy.context.evaluated_depsgraph_get())
        rows, cols = np.nonzero(inside)
        rust = np.zeros((size, size))
        drip = np.zeros((size, size))
        for r, c in zip(rows.tolist(), cols.tolist()):
            p, n = position[r, c], facing[r, c]
            # Water runs down along the surface, so its source lies up along the surface: on a barrel
            # that is up the stave to the hoop sitting on it. Tops and bottoms get no streaks.
            along = Vector((-n[2] * n[0], -n[2] * n[1], 1 - n[2] * n[2]))
            if along.length < 0.3:
                continue
            along.normalize()
            origin = Vector((p[0] + n[0] * 0.003, p[1] + n[1] * 0.003, p[2] + n[2] * 0.003))
            travelled, face = 0.0, None
            for _ in range(4):                      # a part does not drip on itself: look past it
                hit, _normal, found, distance = tree.ray_cast(origin, along, length - travelled)
                if hit is None:
                    break
                travelled += distance
                if part[found] != texel_part[r, c] or mat_index[found] != zone_index[r, c]:
                    face = found
                    break
                origin = hit + along * 0.002
                travelled += 0.002
            if face is None or travelled < 0.004:
                continue
            distance = travelled
            fade = (1 - distance / length) ** 1.5
            if keys[mat_index[face]] in metal_keys:
                rust[r, c] = fade
            else:
                drip[r, c] = fade
        # streaks run in vertical columns: a noise over the horizontal plane only
        column = spec.get("column", 0.012)
        grid = 256
        field = rng.standard_normal((grid, grid))
        fy = np.fft.fftfreq(grid)[:, None] * grid
        fx = np.fft.fftfreq(grid)[None, :] * grid
        extent = max(hi[0] - lo[0], hi[1] - lo[1])
        field = np.fft.ifft2(np.fft.fft2(field) * np.exp(-(fx ** 2 + fy ** 2) / (extent / column / 2) ** 2)).real
        field = (field - field.mean()) / (field.std() + 1e-9)
        gx = np.clip(((position[..., 0] - lo[0]) / extent * (grid - 1)).astype(int), 0, grid - 1)
        gy = np.clip(((position[..., 1] - lo[1]) / extent * (grid - 1)).astype(int), 0, grid - 1)
        columns = np.clip(0.4 + 0.6 * field[gy, gx], 0, 1)
        rust *= columns * ~zone_metal
        drip *= columns * ~zone_metal
        tint = spec.get("rust", 0.6) * rust
        albedo = albedo * (1 - tint[..., None]) + np.array(spec.get("rust_colour", [0.045, 0.022, 0.010])) * tint[..., None]
        albedo *= (1 - spec.get("dirt", 0.35) * drip)[..., None]
        weathering["rust"], weathering["drip"] = rust, drip
    if "wet_roughness" in cfg:
        wet = np.clip((cfg.get("wet_height", 0.18) - (position[..., 2] - z0)) / cfg.get("wet_height", 0.18), 0, 1)
        wet *= inside & ~zone_metal
        rough = rough - (rough - cfg["wet_roughness"]) * np.clip(wet, 0, 1) * (rough > cfg["wet_roughness"])
        matte = np.maximum(weathering.get("rust", 0) * 1.5, weathering.get("drip", 0))
        rough = np.maximum(rough, np.clip(matte, 0, 0.95))
        weathering["wet"] = wet
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
if "seam" in cfg:
    # After the zones met their targets, like the moss: laid before, the darker seams raised every zone's
    # gain and washed the damp and the moss off the crate sides (crates-path, fifth review).
    spec = cfg["seam"]
    dirt = spec.get("dark", 0.5) * seam
    albedo = albedo * (1 - dirt[..., None]) + np.array(spec.get("colour", [0.008, 0.007, 0.005])) * dirt[..., None]
if "moss" in cfg:
    # Moss in patches, laid after each zone met its colour target so its green is the moss's own:
    # on tops (facing up), near the ground and in shaded corners, cut out of a smooth 3D noise so it
    # forms patches, not a film. "cover" is the share of each listed zone's open texels it takes.
    spec = cfg["moss"]
    scale = spec.get("scale", 0.12)
    dirs = rng.standard_normal((14, 3))
    dirs /= np.linalg.norm(dirs, axis=1, keepdims=True)
    freqs = 2 * np.pi / (scale * rng.uniform(0.6, 1.8, 14))
    phases = rng.uniform(0, 2 * np.pi, 14)
    # Along the grain: each part's long axis (from its texels), the noise stretched "grain" times along it,
    # so moss runs in strips along boards and beams, in their gaps and along their edges, not in round dots.
    stretched = position.copy()
    grain = spec.get("grain", 1.0)
    if grain != 1.0:
        for pid in np.unique(texel_part[inside]):
            sel_p = inside & (texel_part == pid)
            pts = position[sel_p]
            if len(pts) < 16:
                continue
            centred = pts - pts.mean(axis=0)
            axis = np.linalg.svd(centred[:: max(1, len(pts) // 2000)], full_matrices=False)[2][0]
            along = centred @ axis
            stretched[sel_p] = pts + np.outer(along, axis) * (1 / grain - 1)
    noise = sum(np.sin(stretched @ d * f + ph) for d, f, ph in zip(dirs, freqs, phases)) / np.sqrt(7)
    if spec.get("detail", 0.0):
        # a finer, unstretched layer breaks the strips' edges and spacing: one scale alone read as hatching
        d2 = rng.standard_normal((10, 3))
        d2 /= np.linalg.norm(d2, axis=1, keepdims=True)
        f2 = 2 * np.pi / (scale * rng.uniform(0.2, 0.45, 10))
        noise = noise + spec["detail"] * sum(np.sin(position @ d * f + ph) for d, f, ph in
                                             zip(d2, f2, rng.uniform(0, 2 * np.pi, 10))) / np.sqrt(5)
    up = np.clip(facing[..., 2], 0, 1)
    low = np.clip(1 - (position[..., 2] - z0) / spec.get("low", 0.25), 0, 1)
    shade = np.clip((1 - ao) / 0.4, 0, 1)
    bias = (spec.get("top", 1.0) * up ** 2 + spec.get("bottom", 0.8) * low + spec.get("shade", 0.6) * shade
            + spec.get("edges", 0.0) * seam)
    field = noise + bias
    moss = np.zeros((size, size))
    cover = {}
    for key in spec.get("zones", keys):
        index = keys.index(key) + 1
        sel = (zone_id == index) & inside & ~down
        if not sel.any():
            continue
        share = spec.get("cover", {}).get(key, 0.06) if isinstance(spec.get("cover"), dict) else spec.get("cover", 0.06)
        cut = np.quantile(field[sel], 1 - share)
        soft = spec.get("soft", 0.25)
        m = np.clip((field - cut) / soft + 0.5, 0, 1) * (zone_id == index) * inside
        moss = np.maximum(moss, m)
        cover[key] = round(float((m[sel] > 0.5).mean()), 4)
    dark, light = np.array(spec.get("dark", [0.018, 0.028, 0.007])), np.array(spec.get("light", [0.045, 0.06, 0.013]))
    # the moss's own light and dark from a fine noise in many directions: one plane wave here laid regular
    # diagonal stripes 6-7 px apart over every beam of the net rack (fifth review)
    d3 = rng.standard_normal((9, 3))
    d3 /= np.linalg.norm(d3, axis=1, keepdims=True)
    f3 = 2 * np.pi / (scale * rng.uniform(0.15, 0.4, 9))
    # on the stretched coordinates, so the moss's light and dark runs along the boards like their grain
    tone = sum(np.sin(stretched @ d * f + ph) for d, f, ph in zip(d3, f3, rng.uniform(0, 2 * np.pi, 9))) / np.sqrt(4.5)
    tone = np.clip(0.5 + 0.35 * tone, 0, 1)[..., None]
    green = dark * (1 - tone) + light * tone
    green = green * (0.55 + 0.45 * ao)[..., None]
    if spec.get("keep_grain", 0.0):
        # the wood's own light and dark, relative to its zone's mean, carried into the moss
        lum = albedo @ np.array([0.2126, 0.7152, 0.0722])
        rel = np.ones((size, size))
        for key in spec.get("zones", keys):
            sel = (zone_id == keys.index(key) + 1) & inside
            if sel.any():
                rel[sel] = lum[sel] / max(float(lum[sel].mean()), 1e-6)
        green = green * np.clip(rel, 0.3, 2.5)[..., None] ** spec["keep_grain"]
    moss = moss * spec.get("opacity", 1.0)
    halo = np.clip((field - np.quantile(field[inside], 0.5)) / 1.5, 0, 1) * (1 - moss) * inside
    albedo *= (1 - spec.get("damp", 0.25) * halo)[..., None]
    albedo = albedo * (1 - moss[..., None]) + green * moss[..., None]
    rough = np.maximum(rough, moss * 0.95)
    weathering["moss"] = moss
    report["moss"] = cover
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
        for name, mask in weathering.items():
            save_png(out / f"atlas-{name}.png", np.clip(mask, 0, 1))
        first = folder
    elif not folder.exists():
        shutil.copytree(first, folder)

# --- export: atlas UV only, zone materials merged into atlas materials ---
mesh.uv_layers.remove(mesh.uv_layers["Tile"])
mesh.attributes.remove(mesh.attributes["tone"])
mesh.attributes.remove(mesh.attributes["part_id"])
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
