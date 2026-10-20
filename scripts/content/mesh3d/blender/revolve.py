"""Rebuild a body of revolution (barrel) from a dense generator mesh.

blender -b --factory-startup -P revolve.py -- config.json
config: {"raw": raw.npz, "target": mesh3d-pilot.json, "id": "barrel", "out": dir,
         "sides": 24, "band_thickness": 0.006, "chime_depth": 0.025}
The generator gives the radius profile and band layout; geometry, UVs and
zones are built here, so the mesh is clean and inside the budget.
"""
import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np

cfg = json.loads(Path(sys.argv[sys.argv.index("--") + 1]).read_text())
out = Path(cfg["out"])
out.mkdir(parents=True, exist_ok=True)
targets = json.loads(Path(cfg["target"]).read_text())
targets = targets["targets"] if isinstance(targets, dict) else targets
goal = next(t for t in targets if t["id"] == cfg["id"])
zones = {z["role"] if "role" in z else z["key"]: z for z in goal["zones"]}
keys = {z["group"] + ":" + z["key"]: z for z in goal["zones"]}


def oklab(rgb):
    m1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                   [0.2119034982, 0.6806995451, 0.1073969566],
                   [0.0883024619, 0.2817188376, 0.6299787005]])
    m2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                   [1.9779984951, -2.4285922050, 0.4505937099],
                   [0.0259040371, 0.7827717662, -0.8086757660]])
    return np.cbrt(np.clip(rgb, 0, 1) @ m1.T) @ m2.T


# --- analyse the generator mesh (+Z up, as TRELLIS writes it) ---
raw = np.load(cfg["raw"])
v = raw["vertices"].astype(np.float64)
colour = raw["color"].astype(np.float64)
v[:, :2] -= np.median(v[:, :2], axis=0)
v[:, 2] -= v[:, 2].min()
height = v[:, 2].max()
radius = np.linalg.norm(v[:, :2], axis=1)
lab = oklab(colour)
chroma = np.hypot(lab[:, 1], lab[:, 2])

bins = cfg.get("bins", 80)
edges = np.linspace(0, height, bins + 1)
centres = (edges[:-1] + edges[1:]) / 2
index = np.clip(np.digitize(v[:, 2], edges) - 1, 0, bins - 1)
outer = radius > np.percentile(radius, 35)
profile = np.full(bins, np.nan)
metal = np.zeros(bins)
for b in range(bins):
    sel = outer & (index == b)
    if sel.sum() >= 30:
        profile[b] = np.percentile(radius[sel], 75)
        metal[b] = np.mean(chroma[sel] < cfg.get("metal_chroma", 0.013))
valid = ~np.isnan(profile)
profile = np.interp(centres, centres[valid], profile[valid])
kernel = np.ones(5) / 5
smooth = np.convolve(np.pad(profile, 2, mode="edge"), kernel, mode="valid")

runs, start = [], None
for b in range(bins):
    on = metal[b] > cfg.get("metal_fraction", 0.6)
    if on and start is None:
        start = b
    if (not on or b == bins - 1) and start is not None:
        stop = b if not on else b + 1
        runs.append((edges[start], edges[stop]))
        start = None
min_band = cfg.get("min_band", 0.02) * height
runs = [r for r in runs if r[1] - r[0] >= min_band]

# Head (lid) level: inner vertices near the top and bottom.
inner = radius < 0.6 * smooth.max()
top_head = np.median(v[inner & (v[:, 2] > 0.8 * height), 2]) if np.any(inner & (v[:, 2] > 0.8 * height)) else height
print("ANALYSIS height", round(height, 4), "rmax", round(smooth.max(), 4),
      "bands", [(round(a / height, 3), round(b / height, 3)) for a, b in runs],
      "top head depth", round((height - top_head) / height, 4))

# --- scale into the target bbox (glTF metres; Blender Z = glTF Y) ---
gmin, gmax = np.array(goal["bbox_min"]), np.array(goal["bbox_max"])
target_h = gmax[1] - gmin[1]
target_r = min(gmax[0] - gmin[0], gmax[2] - gmin[2]) / 2
t_band = cfg.get("band_thickness", 0.006)
sz = target_h / height
sr = (target_r - t_band) / smooth.max()
base_z = gmin[1]
centre_xy = ((gmin[0] + gmax[0]) / 2, -(gmin[2] + gmax[2]) / 2)


def r_at(z_local):
    return float(np.interp(z_local / sz, centres, smooth)) * sr


max_band = cfg.get("max_band", 0.06)
bands = []
for a, b in runs:
    a, b = a * sz, b * sz
    if b - a > max_band:  # dark runs include the band's shadow; keep a band-wide strip at the run centre
        mid = (a + b) / 2
        a, b = mid - max_band / 2, mid + max_band / 2
    bands.append((max(a, 0.0), min(b, target_h)))
chime = cfg.get("chime_depth", 0.025)

# Profile rings: rim, band edges, and every ~6 cm in between.
levels = {0.0, target_h}
for a, b in bands:
    levels.update((max(a, 0.0), min(b, target_h)))
step = cfg.get("ring_step", 0.06)
for z in np.arange(step, target_h, step):
    if all(abs(z - l) > step * 0.35 for l in levels):
        levels.add(float(z))
levels = sorted(levels)

sides = cfg.get("sides", 24)
angles = [2 * math.pi * i / sides for i in range(sides)]
mean_r = float(np.mean([r_at(z) for z in levels]))

bm = bmesh.new()
uv_layer = bm.loops.layers.uv.new("UVMap")
slot = {"stave": 0, "head": 1, "band": 2}


def meridian(zs, offset=0.0):
    rs = [r_at(z) + offset for z in zs]
    arc = [0.0]
    for i in range(1, len(zs)):
        arc.append(arc[-1] + math.hypot(zs[i] - zs[i - 1], rs[i] - rs[i - 1]))
    return rs, arc


def ring_strip(zs, offset, material, u_radius, skip=()):
    rs, arc = meridian(zs, offset)
    rows = [[bm.verts.new((centre_xy[0] + r * math.cos(a), centre_xy[1] + r * math.sin(a), base_z + z))
             for a in angles] for z, r in zip(zs, rs)]
    for j in range(len(zs) - 1):
        if any(a - 1e-6 <= zs[j] and zs[j + 1] <= b + 1e-6 for a, b in skip):
            continue  # hidden under a band
        for i in range(sides):
            k = (i + 1) % sides
            face = bm.faces.new((rows[j][i], rows[j][k], rows[j + 1][k], rows[j + 1][i]))
            face.material_index = material
            u0, u1 = angles[i] * u_radius, (angles[i] + 2 * math.pi / sides) * u_radius
            for loop, (u, vv) in zip(face.loops, ((u0, arc[j]), (u1, arc[j]), (u1, arc[j + 1]), (u0, arc[j + 1]))):
                loop[uv_layer].uv = (u, vv)
    return rows


def disc(z, r, material, flip):
    verts = [bm.verts.new((centre_xy[0] + r * math.cos(a), centre_xy[1] + r * math.sin(a), base_z + z)) for a in angles]
    face = bm.faces.new(verts[::-1] if flip else verts)
    face.material_index = material
    for loop in face.loops:
        x, y = loop.vert.co.x - centre_xy[0], loop.vert.co.y - centre_xy[1]
        loop[uv_layer].uv = (x, y)
    return verts


# Staves.
body = ring_strip(levels, 0.0, slot["stave"], mean_r, skip=bands)
# Bands: outer skin raised by t_band plus top and bottom lips.
for a, b in bands:
    zs = [z for z in levels if a - 1e-6 <= z <= b + 1e-6]
    ring_strip(zs, t_band, slot["band"], mean_r + t_band)
    for z in (zs[0], zs[-1]):
        r = r_at(z)
        inner_ring = [bm.verts.new((centre_xy[0] + r * math.cos(t), centre_xy[1] + r * math.sin(t), base_z + z)) for t in angles]
        outer_ring = [bm.verts.new((centre_xy[0] + (r + t_band) * math.cos(t), centre_xy[1] + (r + t_band) * math.sin(t), base_z + z)) for t in angles]
        for i in range(sides):
            k = (i + 1) % sides
            quad = (inner_ring[i], inner_ring[k], outer_ring[k], outer_ring[i])
            face = bm.faces.new(quad if z == zs[-1] else quad[::-1])
            face.material_index = slot["band"]
            for loop in face.loops:
                p = loop.vert.co
                loop[uv_layer].uv = (math.atan2(p.y - centre_xy[1], p.x - centre_xy[0]) * (mean_r + t_band), math.hypot(p.x - centre_xy[0], p.y - centre_xy[1]))
# Chime: inner wall down to a recessed head at the top; flat bottom.
wall = 0.018
r_top = r_at(target_h) - wall
top_rim = body[-1]
inner_top = [bm.verts.new((centre_xy[0] + r_top * math.cos(a), centre_xy[1] + r_top * math.sin(a), base_z + target_h)) for a in angles]
inner_low = [bm.verts.new((centre_xy[0] + r_top * math.cos(a), centre_xy[1] + r_top * math.sin(a), base_z + target_h - chime)) for a in angles]
for i in range(sides):
    k = (i + 1) % sides
    lip = bm.faces.new((top_rim[i], top_rim[k], inner_top[k], inner_top[i]))
    lip.material_index = slot["stave"]
    wallf = bm.faces.new((inner_top[i], inner_top[k], inner_low[k], inner_low[i]))
    wallf.material_index = slot["stave"]
    for face in (lip, wallf):
        for loop in face.loops:
            p = loop.vert.co
            loop[uv_layer].uv = (angles[i] * mean_r if loop.vert in (top_rim[i], inner_top[i], inner_low[i]) else (angles[i] + 2 * math.pi / sides) * mean_r,
                                 p.z - base_z + math.hypot(p.x - centre_xy[0], p.y - centre_xy[1]))
head = bm.faces.new(inner_low)
head.material_index = slot["head"]
for loop in head.loops:
    loop[uv_layer].uv = (loop.vert.co.x - centre_xy[0], loop.vert.co.y - centre_xy[1])
bottom = bm.faces.new(body[0][::-1])
bottom.material_index = slot["head"]
for loop in bottom.loops:
    loop[uv_layer].uv = (loop.vert.co.x - centre_xy[0], loop.vert.co.y - centre_xy[1])

bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
bmesh.ops.triangulate(bm, faces=bm.faces)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
mesh = bpy.data.meshes.new(goal["id"])
bm.to_mesh(mesh)
bm.free()
mesh.validate()
mesh.update()
for role, index in (("stave", 0), ("head", 1), ("band", 2)):
    zone = next(z for z in goal["zones"] if z["key"].endswith({"stave": "STAVE", "head": "HEAD", "band": "BAND"}[role]))
    mat = bpy.data.materials.new(zone["key"])
    mat["group"] = zone["group"]
    mesh.materials.append(mat)
mesh.shade_smooth()
mesh.set_sharp_from_angle(angle=math.radians(35))
obj = bpy.data.objects.new(goal["id"], mesh)
bpy.context.scene.collection.objects.link(obj)
for o in list(bpy.context.scene.objects):
    o.select_set(o == obj)
bpy.context.view_layer.objects.active = obj
bpy.ops.export_scene.gltf(filepath=str(out / "model.glb"), export_format="GLB", use_selection=True,
                          export_apply=True, export_yup=True, export_extras=True, export_normals=True,
                          export_texcoords=True, export_animations=False)
lo = np.min([c[:] for c in obj.bound_box], axis=0)
hi = np.max([c[:] for c in obj.bound_box], axis=0)
metrics = {"triangles": len(mesh.polygons), "budget": goal["budget_tris"], "bands": len(bands),
           "bbox_blender_min": lo.round(4).tolist(), "bbox_blender_max": hi.round(4).tolist(),
           "blender_version": bpy.app.version_string}
(out / "metrics.json").write_text(json.dumps(metrics, indent=2))
print("BUILT", json.dumps(metrics))
