"""Build a barrel by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P barrel.py -- barrel.json OUT_DIR
config: {"target": mesh3d-pilot.json (default: the repo's), "id": "barrel",
         "front": front-view RGBA PNG (relative: under content-sources), "sides": 20, "seed": 1,
         "hoops": [[centre_fraction, height_m], ...], "hoop_thickness": 0.008, "hoop_slope": 0.02,
         "rim": 0.025, "chime_depth": 0.03, "gap": 0.004, "rim_drop": 0.006,
         "lid_planks": 4, "jitter": 0.12,
         "stave_tile": 0.38, "planks_per_tile": 4, "ring_step": 0.09}
The concept's front view gives the silhouette; part counts are read from the
concept by eye and passed in. The target gives size, origin and budget. One
facet per stave, so the texture's plank seams fall on facet edges; each stave
and lid plank picks its own plank of the tile and its own V offset.
"""
import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np

args = sys.argv[sys.argv.index("--") + 1:]
cfg = json.loads(Path(args[0]).read_text())
out = Path(args[1])
out.mkdir(parents=True, exist_ok=True)
repo = Path(__file__).resolve().parents[4]
sources = Path.home() / ".local/share/gothic-ai/content-sources"
targets = json.loads(Path(cfg.get("target", repo / "data/content/mesh3d-pilot.json")).read_text())
targets = targets["targets"] if isinstance(targets, dict) else targets
goal = next(t for t in targets if t["id"] == cfg["id"])
rng = np.random.default_rng(cfg.get("seed", 1))


# --- silhouette from the concept's front view ---
def longest_run(cols):
    breaks = np.where(np.diff(cols) > 1)[0]
    starts = np.r_[0, breaks + 1]
    stops = np.r_[breaks, len(cols) - 1]
    best = np.argmax(stops - starts)
    return cols[starts[best]], cols[stops[best]]


image = bpy.data.images.load(str(sources / cfg["front"]))  # an absolute path stays as it is
w, h = image.size
pixels = np.empty(w * h * 4, np.float32)
image.pixels.foreach_get(pixels)
alpha = pixels.reshape(h, w, 4)[:, :, 3] >= 0.5  # row 0 is the bottom
rows = np.where(alpha.sum(axis=1) > 3)[0]
bottom_row, top_row = rows.min(), rows.max()
t_rows, half = [], []
for r in range(bottom_row, top_row + 1):
    cols = np.where(alpha[r])[0]
    if len(cols) > 3:
        a, b = longest_run(cols)
        t_rows.append((r - bottom_row) / (top_row - bottom_row))
        half.append((b - a + 1) / 2)
t_rows, half = np.array(t_rows), np.array(half)
concept_aspect = (top_row - bottom_row + 1) / (2 * half.max())
keep = (t_rows > 0.03) & (t_rows < 0.97)
for _ in range(3):  # hoops stand out of the curve; drop them and refit
    coef = np.polyfit(t_rows[keep], half[keep], 4)
    resid = half - np.polyval(coef, t_rows)
    keep &= np.abs(resid) < 2.5 * np.std(resid[keep]) + 0.5
grid = np.linspace(0, 1, 201)
shape = np.polyval(coef, grid)
shape /= shape.max()
print("CONCEPT aspect h/d", round(concept_aspect, 3), "ends/belly",
      round(float(shape[0]), 3), round(float(shape[-1]), 3), "belly at", round(float(grid[np.argmax(shape)]), 3))

# --- size from the target (glTF metres; Blender Z = glTF Y) ---
gmin, gmax = np.array(goal["bbox_min"]), np.array(goal["bbox_max"])
height = gmax[1] - gmin[1]
t_hoop = cfg.get("hoop_thickness", 0.008)
target_r = min(gmax[0] - gmin[0], gmax[2] - gmin[2]) / 2
hoop_shape = max(float(np.interp(c, grid, shape)) for c, _ in cfg["hoops"])
belly = min(target_r, (target_r - t_hoop) / hoop_shape)  # the widest of staves and raised hoops meets the bbox
base_z = gmin[1]
cx, cy = (gmin[0] + gmax[0]) / 2, -(gmin[2] + gmax[2]) / 2


def r_at(z):
    return float(np.interp(z / height, grid, shape)) * belly


hoops = []
for centre, size in cfg["hoops"]:
    a = max(centre * height - size / 2, 0.0)
    hoops.append((a, min(a + size, height)))
chime = cfg.get("chime_depth", 0.03)
top_z = height - chime  # the lid level; stave ends stand above it as the crown
levels = {0.0, top_z}
for a, b in hoops:
    levels.update((a, b))
step = cfg.get("ring_step", 0.09)
sorted_levels = sorted(levels)
for lo, hi in zip(sorted_levels[:-1], sorted_levels[1:]):
    n = int(math.ceil((hi - lo) / step - 1e-6))
    for k in range(1, n):
        levels.add(lo + (hi - lo) * k / n)
levels = sorted(levels)

sides = cfg.get("sides", 20)
jitter = cfg.get("jitter", 0.12)
angles = [2 * math.pi * (i + rng.uniform(-jitter / 2, jitter / 2)) / sides for i in range(sides)]
tile = cfg.get("stave_tile", 0.38)
ppt = cfg.get("planks_per_tile", 4)
plank_u = tile / ppt

bm = bmesh.new()
uv = bm.loops.layers.uv.new("UVMap")
STAVE, HEAD, BAND = 0, 1, 2


def vert(r, a, z):
    return bm.verts.new((cx + r * math.cos(a), cy + r * math.sin(a), base_z + z))


def ring(z, r):
    return [vert(r, a, z) for a in angles]


def quad(verts, material, uvs):
    face = bm.faces.new(verts)
    face.material_index = material
    for loop, coord in zip(face.loops, uvs):
        loop[uv].uv = coord
    return face


def inside_hoop(z0, z1):
    return any(a - 1e-6 <= z0 and z1 <= b + 1e-6 for a, b in hoops)


# Staves: one facet each, own plank of the tile, own V offset; the top rim is uneven by a few mm.
body = [ring(z, r_at(z)) for z in levels]
arc = [0.0]
for j in range(1, len(levels)):
    arc.append(arc[-1] + math.hypot(levels[j] - levels[j - 1], r_at(levels[j]) - r_at(levels[j - 1])))
plank = rng.integers(0, ppt, sides)
flip = rng.random(sides) < 0.5
v_off = rng.uniform(0, tile, sides)
for i in range(sides):
    k = (i + 1) % sides
    u0, u1 = plank[i] * plank_u, (plank[i] + 1) * plank_u
    if flip[i]:
        u0, u1 = u1, u0
    for j in range(len(levels) - 1):
        if inside_hoop(levels[j], levels[j + 1]):
            continue
        v0, v1 = arc[j] + v_off[i], arc[j + 1] + v_off[i]
        quad((body[j][i], body[j][k], body[j + 1][k], body[j + 1][i]), STAVE,
             ((u0, v0), (u1, v0), (u1, v1), (u0, v1)))

# Hoops: raised skin plus top and bottom lips, welded to the stave rings.
for a, b in hoops:
    idx = [j for j, z in enumerate(levels) if a - 1e-6 <= z <= b + 1e-6]
    # The hoop's top edge slopes down to the staves instead of a flat ledge: a narrow ledge
    # facing the sun breaks into a bright dashed line on the game's shadow map.
    slope = cfg.get("hoop_slope", 0.02)
    skin = [levels[j] - (slope if j == idx[-1] else 0.0) for j in idx]
    outer = [ring(z, r_at(z) + t_hoop) for z in skin]
    r_mid = r_at((a + b) / 2) + t_hoop
    for i in range(sides):
        k = (i + 1) % sides
        u0, u1 = angles[i] * r_mid, (angles[i] + 2 * math.pi / sides) * r_mid
        for m in range(len(idx) - 1):
            z0, z1 = skin[m], skin[m + 1]
            quad((outer[m][i], outer[m][k], outer[m + 1][k], outer[m + 1][i]), BAND,
                 ((u0, z0), (u1, z0), (u1, z1), (u0, z1)))
        for m, j in ((0, idx[0]), (len(idx) - 1, idx[-1])):
            z, dz = skin[m], abs(levels[j] - skin[m]) + t_hoop
            quad((body[j][i], body[j][k], outer[m][k], outer[m][i]), BAND,
                 ((u0, z), (u1, z), (u1, z + dz), (u0, z + dz)))

# Hoop seams: an overlapping plate with two rivet heads, all on one side as in the concept.
def box(centre, tu, tv, tn, hu, hv, depth, material):
    corners = [[bm.verts.new(centre + tu * su * hu + tv * sv * hv + tn * d)
                for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1))] for d in (0.0, depth)]
    lo, hi = corners
    sides_ = [(lo[0], lo[1], lo[2], lo[3]), (hi[3], hi[2], hi[1], hi[0])]
    sides_ += [(lo[m], hi[m], hi[(m + 1) % 4], lo[(m + 1) % 4]) for m in range(4)]
    for f in sides_:
        quad(f, material, [((v.co - centre).dot(tu), (v.co - centre).dot(tv) + (v.co - centre).dot(tn)) for v in f])


def rivet(centre, tu, tv, tn, radius, height_, material):
    rim_ = [bm.verts.new(centre + (tu * math.cos(2 * math.pi * m / 6) + tv * math.sin(2 * math.pi * m / 6)) * radius)
            for m in range(6)]
    tip = bm.verts.new(centre + tn * height_)
    base = bm.faces.new(rim_[::-1])
    base.material_index = material
    for m in range(6):
        quad((rim_[m], rim_[(m + 1) % 6], tip), material,
             [((v.co - centre).dot(tu), (v.co - centre).dot(tv)) for v in (rim_[m], rim_[(m + 1) % 6], tip)])


from mathutils import Vector  # noqa: E402

seam = cfg.get("seam_stave", 0)
for a, b in hoops:
    za, zb = a, b - cfg.get("hoop_slope", 0.02)  # the plate sits on the hoop skin, below its sloped top
    s0, s1 = angles[seam], angles[seam + 1]
    def p(r, ang, z):
        return Vector((cx + r * math.cos(ang), cy + r * math.sin(ang), base_z + z))
    mid_lo = (p(r_at(za) + t_hoop, s0, za) + p(r_at(za) + t_hoop, s1, za)) / 2
    mid_hi = (p(r_at(zb) + t_hoop, s0, zb) + p(r_at(zb) + t_hoop, s1, zb)) / 2
    centre = (mid_lo + mid_hi) / 2
    tv = (mid_hi - mid_lo).normalized()
    tu = (p(1.0, s1, 0) - p(1.0, s0, 0)).normalized()
    tn = tu.cross(tv).normalized()
    if tn.dot(Vector((centre.x - cx, centre.y - cy, 0))) < 0:
        tn = -tn
    plate = cfg.get("seam_plate", 0.03)
    depth = cfg.get("seam_depth", 0.004)
    box(centre, tu, tv, tn, plate, (zb - za) / 2 - 0.004, depth, BAND)
    for side in (-1, 1):
        rivet(centre + tn * depth + tu * side * plate * 0.45, tu, tv, tn, 0.009, 0.006, BAND)

# Crown: every stave end is its own block above the lid, with a gap to the next and an uneven top.
rim = cfg.get("rim", 0.025)
gap = cfg.get("gap", 0.004)
tops = height - rng.uniform(0.0, cfg.get("rim_drop", 0.006), sides)
tops[np.argmax(tops)] = height
r_low = r_at(top_z)
for i in range(sides):
    a0 = angles[i] + gap / 2 / r_low
    a1 = (angles[i + 1] if i + 1 < sides else angles[0] + 2 * math.pi) - gap / 2 / r_low
    zt = tops[i]
    r_hi = r_at(zt)
    ob0, ob1, ot0, ot1 = vert(r_low, a0, top_z), vert(r_low, a1, top_z), vert(r_hi, a0, zt), vert(r_hi, a1, zt)
    ib0, ib1 = vert(r_low - rim, a0, top_z), vert(r_low - rim, a1, top_z)
    it0, it1 = vert(r_hi - rim, a0, zt), vert(r_hi - rim, a1, zt)
    u0, u1 = plank[i] * plank_u, (plank[i] + 1) * plank_u
    v0, v1 = arc[-1] + v_off[i], arc[-1] + v_off[i] + (zt - top_z)
    quad((ob0, ob1, ot1, ot0), STAVE, ((u0, v0), (u1, v0), (u1, v1), (u0, v1)))
    quad((ib1, ib0, it0, it1), STAVE, ((u1, v0), (u0, v0), (u0, v1), (u1, v1)))
    quad((ot0, ot1, it1, it0), STAVE, ((u0, v1), (u1, v1), (u1, v1 + rim), (u0, v1 + rim)))
    quad((ob1, ob0, ib0, ib1), STAVE, ((u1, v0), (u0, v0), (u0, v0 - rim), (u1, v0 - rim)))
    for o_b, o_t, i_t, i_b in ((ob0, ot0, it0, ib0), (ob1, ib1, it1, ot1)):
        quad((o_b, o_t, i_t, i_b), STAVE, ((u0, v0), (u0, v1), (u0 + rim, v1), (u0 + rim, v0)))
# Lid at the crown's foot, cut into planks.
r_top = r_low
inner_low = body[-1]
lid = bm.faces.new(inner_low)
lid.material_index = HEAD
n_planks = cfg.get("lid_planks", 4)
for p in range(1, n_planks):  # cut the lid into planks; the cuts also split the chime wall's lower edges
    x = cx - r_top + 2 * r_top * p / n_planks
    faces = [f for f in bm.faces if f.material_index == HEAD]
    geom = faces + list({e for f in faces for e in f.edges}) + list({v for f in faces for v in f.verts})
    bmesh.ops.bisect_plane(bm, geom=geom, plane_co=(x, cy, 0.0), plane_no=(1.0, 0.0, 0.0))
plank_width = 2 * r_top / n_planks
lid_plank = rng.integers(0, ppt, n_planks)
lid_off = rng.uniform(0, tile, n_planks)
for face in [f for f in bm.faces if f.material_index == HEAD]:
    centre = face.calc_center_median()
    p = min(int((centre.x - (cx - r_top)) / plank_width), n_planks - 1)
    left = cx - r_top + p * plank_width
    for loop in face.loops:
        co = loop.vert.co
        loop[uv].uv = (lid_plank[p] * plank_u + (co.x - left) / plank_width * plank_u, co.y - cy + lid_off[p])
# Bottom: flat, hidden on the ground.
bottom = bm.faces.new(body[0][::-1])
bottom.material_index = HEAD
for loop in bottom.loops:
    loop[uv].uv = (loop.vert.co.x - cx, loop.vert.co.y - cy)

bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
bmesh.ops.triangulate(bm, faces=bm.faces)
bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
open_edges = sum(1 for e in bm.edges if not e.is_manifold)
mesh = bpy.data.meshes.new(goal["id"])
bm.to_mesh(mesh)
bm.free()
mesh.validate()
mesh.update()
for suffix in ("STAVE", "HEAD", "BAND"):
    zone = next(z for z in goal["zones"] if z["key"].endswith(suffix))
    mat = bpy.data.materials.new(zone["key"])
    mat["group"] = zone["group"]
    mesh.materials.append(mat)
mesh.shade_smooth()
mesh.set_sharp_from_angle(angle=math.radians(30))
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
target_lo = np.array([gmin[0], -gmax[2], gmin[1]])
target_hi = np.array([gmax[0], -gmin[2], gmax[1]])
metrics = {"concept_front": cfg["front"], "triangles": len(mesh.polygons), "budget": goal["budget_tris"], "open_edges": open_edges,
           "hoops": len(hoops), "staves": sides, "lid_planks": n_planks,
           "concept_aspect_h_over_d": round(float(concept_aspect), 4),
           "target_aspect_h_over_d": round(float(height / (2 * target_r)), 4),
           "bbox_delta_min_m": (lo - target_lo).round(4).tolist(), "bbox_delta_max_m": (hi - target_hi).round(4).tolist(),
           "blender_version": bpy.app.version_string}
(out / "metrics.json").write_text(json.dumps(metrics, indent=2))
print("BUILT", json.dumps(metrics))
