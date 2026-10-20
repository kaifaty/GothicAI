"""Build the inner side of a house window with curtains by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P window-in.py -- window-in.json OUT_DIR   (run from the repository root)
config: {"id": "window-in", "target": "data/content/mesh3d-market.json", "seed": 1,
         "frame": [0.8, -0.735, 0.715, 0.05], "jamb": 0.13, "head": 0.13, "bottom": 0.1, "mullion": [0.07, 0.03],
         "leaf": [0.045, 0.035, -0.07], "leaf_gap": 0.006, "glass": [0.01, -0.062], "diamonds": [6, 9],
         "sill": [0.95, 0.06, -0.12, 0.14], "rod": [0.817, 0.19, 0.012, 1.055], "finial": 0.02,
         "brackets": 0.93, "curtain": [0.985, 0.4, 0.47, 0.797, 5, 4, 0.025, 0.008, 0.19],
         "bands": [[0.70, 0.73], [-0.81, -0.70]], "rings": 5, "ring": [0.024, 0.006],
         "tiles": "market-set/tiles", "glass_tile": {"size": 1024, "per_tile": 4, "came": 0.045, "seed": 3},
         "stave_tile": 0.38, "planks_per_tile": 4}
The room side faces +Y in Blender (glTF -Z): the original's curtains hang on that side
and its glass sits back. The window keeps the original's layout in numbers: a frame
1.6 m wide around two leaded casements (the pane UV maps a procedural lattice tile,
as window-out.py), a deep sill board under it, an iron rod on two forged brackets over
the full width, and two curtains of pleated wool gathered at the sides, hanging in front
of the sill down to the bottom of the bbox. The curtains carry a woven band near the
hem and a narrow one near the top: a zone of their own ("zones" in the config) with the
red the concept asks for; the cloth itself takes the original curtain's measured colour.
"""
import json
import math
import shutil
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
FRAME, GLASS, IRON, CLOTH, BAND = (keys.index(k) for k in ("OWN_PROP_WOOD_FRAME", "OWN_PROP_GLASS", "OWN_PROP_IRON_BAND",
                                                           "OWN_PROP_CLOTH", "OWN_PROP_CLOTH_BAND"))
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
staging = Path.home() / ".local/share/gothic-ai/content-staging"


def save_png(path, rgb):
    """rgb: (n, n, 3) in 0..1, row 0 at the bottom (Blender's pixel order), stored as it is."""
    n = rgb.shape[0]
    rgba = np.concatenate([np.clip(rgb, 0, 1), np.ones((n, n, 1))], axis=2).astype(np.float32)
    image = bpy.data.images.new(path.stem, n, n, alpha=True)
    image.colorspace_settings.name = "Non-Color"
    image.pixels.foreach_set(rgba.ravel())
    image.filepath_raw = str(path)
    image.file_format = "PNG"
    image.save()
    bpy.data.images.remove(image)


def glass_tile(folder, size=1024, per_tile=4, came=0.045, seed=2):
    """Leaded glass (copied from window-out.py): per_tile x per_tile diamonds of slightly different
    panes, raised lead cames, grime in the corners. Seamless; the bake sets the mean colour."""
    r = np.random.default_rng(seed)
    v, u = (np.mgrid[0:size, 0:size] + 0.5) / size
    p, q = u * per_tile, v * per_tile
    a, b = p + q, p - q
    da = np.abs(a - np.round(a))
    db = np.abs(b - np.round(b))
    d = np.minimum(da, db) / math.sqrt(2)
    lead = np.clip(1 - (d - came * 0.5) / (came * 0.5), 0, 1)
    corner = np.exp(-(np.hypot(da, db) / 0.16) ** 2)
    cell = (np.floor(a).astype(int) % per_tile) * 7 + (np.floor(b).astype(int) % per_tile) * 13
    tint = r.uniform(-1, 1, (7 * per_tile + 13 * per_tile, 3))[cell]
    wave = np.sin(2 * math.pi * (p * 0.5 + r.uniform())) * np.sin(2 * math.pi * (q * 0.5 + r.uniform()))
    pane = np.array([0.30, 0.36, 0.31]) * (1 + 0.12 * tint) * (1 + 0.05 * wave)[..., None]
    pane *= (1 - 0.55 * corner)[..., None]
    metal = np.array([0.16, 0.16, 0.15])
    albedo = pane * (1 - lead[..., None]) + metal * lead[..., None]
    height = 0.6 * lead + 0.04 * wave
    gu = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * size / 2
    gv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * size / 2
    normal = np.stack([-gu * 0.004, -gv * 0.004, np.ones_like(gu)], axis=2)
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    folder.mkdir(parents=True, exist_ok=True)
    save_png(folder / "albedo.png", albedo)
    save_png(folder / "normal.png", normal * 0.5 + 0.5)
    rough = 0.22 + 0.35 * corner + 0.4 * lead
    for name, value in (("roughness", rough), ("metallic", np.zeros_like(rough)), ("ao", np.ones_like(rough)),
                        ("height", height)):
        save_png(folder / f"{name}.png", np.repeat(np.clip(value, 0, 1)[..., None], 3, axis=2))


def tiles():
    """OUT_DIR/tiles for the atlas bake: shared wood and iron, the canvas tile under both cloth keys, our glass."""
    folder = out / "tiles"
    source = staging / cfg.get("tiles", "market-set/tiles")
    for key, src in (("own_prop_wood_frame", "own_prop_wood_frame"), ("own_prop_iron_band", "own_prop_iron_band"),
                     ("own_prop_cloth", "own_prop_canvas"), ("own_prop_cloth_band", "own_prop_canvas")):
        shutil.rmtree(folder / key, ignore_errors=True)
        shutil.copytree(source / src, folder / key)
    glass_tile(folder / "own_prop_glass", **cfg.get("glass_tile", {}))


def wood(c, ax_len, ax_w, ax_n, length, width, thick, tilt_deg=0.0):
    if tilt_deg:
        R = Matrix.Rotation(math.radians(rng.uniform(-tilt_deg, tilt_deg)), 3, ax_n)
        ax_len, ax_w = R @ ax_len, R @ ax_w
    P.wood(c, ax_len, ax_w, ax_n, length, width, thick, FRAME, tile)


def flat(c, ax_a, ax_b, ax_n, outline, thick, material):
    """A flat plate: a convex outline in (a, b) extruded along ax_n from c."""
    part = P.new_part()
    low, top = ([P.vert(c + ax_a * a + ax_b * b + ax_n * d, part) for a, b in outline] for d in (0.0, thick))
    P.poly(top, material, list(outline))
    P.poly(low[::-1], material, list(outline[::-1]))
    n = len(outline)
    for i in range(n):
        j = (i + 1) % n
        P.poly((low[i], low[j], top[j], top[i]), material,
               ((i * 0.02, 0), ((i + 1) * 0.02, 0), ((i + 1) * 0.02, thick), (i * 0.02, thick)))


def prism(c, axis, radius, length, sides, material):
    """A closed capped n-sided prism along axis through c (copied from cart.py)."""
    part = P.new_part()
    a = axis.normalized()
    b = (X if abs(a.dot(X)) < 0.9 else Y).cross(a).normalized()
    d = a.cross(b)
    ends = [[P.vert(c + a * (s * length / 2) + (b * math.cos(2 * math.pi * i / sides)
                                                + d * math.sin(2 * math.pi * i / sides)) * radius, part)
             for i in range(sides)] for s in (-1, 1)]
    edge = 2 * math.pi * radius / sides
    for i in range(sides):
        j = (i + 1) % sides
        P.poly((ends[0][i], ends[0][j], ends[1][j], ends[1][i]), material,
               ((i * edge, 0), ((i + 1) * edge, 0), ((i + 1) * edge, length), (i * edge, length)))
    for ring_, sign in ((ends[0], -1), (ends[1], 1)):
        verts = ring_ if sign > 0 else ring_[::-1]
        P.poly(verts, material, [((v.co - c).dot(b), (v.co - c).dot(d)) for v in verts])


def ring(c, axis, radius, thick, segments, material):
    """A small closed ring of triangular section around axis through c (a curtain ring)."""
    part = P.new_part()
    a = axis.normalized()
    b = (X if abs(a.dot(X)) < 0.9 else Y).cross(a).normalized()
    d = a.cross(b)
    rows = []
    for i in range(segments):
        t = 2 * math.pi * i / segments
        radial = b * math.cos(t) + d * math.sin(t)
        rows.append([P.vert(c + radial * (radius + thick * math.cos(s)) + a * (thick * math.sin(s)), part)
                     for s in (0.0, 2 * math.pi / 3, 4 * math.pi / 3)])
    for i in range(segments):
        j = (i + 1) % segments
        for k in range(3):
            m = (k + 1) % 3
            P.poly((rows[i][k], rows[j][k], rows[j][m], rows[i][m]), material,
                   ((i * 0.01, k * 0.01), ((i + 1) * 0.01, k * 0.01), ((i + 1) * 0.01, (k + 1) * 0.01), (i * 0.01, (k + 1) * 0.01)))


def pane(x0, x1, z0, z1, y, thick, across, tall, per_tile):
    """A thin closed glass box whose UV maps the lattice tile: `across` x `tall` diamonds on the front."""
    part = P.new_part()
    du, dv = (x1 - x0) / across * per_tile, (z1 - z0) / tall * per_tile
    corner = {(sx, sy, sz): P.vert(Vector((x0 if sx < 0 else x1, y + sy * thick / 2, z0 if sz < 0 else z1)), part)
              for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)}
    for index in range(3):
        others = [i for i in range(3) if i != index]
        for sign in (-1, 1):
            quad = [(k, v) for k, v in corner.items() if k[index] == sign]
            ring_ = sorted(quad, key=lambda kv: math.atan2(kv[0][others[1]], kv[0][others[0]]))
            P.poly([v for _, v in ring_], GLASS, [((v.co.x - x0) / du, (v.co.z - z0) / dv) for _, v in ring_])


def curtain(x_wall, side, w_top, w_bottom, z_top, z_bottom, folds, per_fold, depth, thick, y_mid, bands):
    """A pleated curtain: a closed sheet thick `thick` hanging from z_top, its outer edge at x_wall,
    gathered to w_top at the rings and falling open to w_bottom; rows at the band edges get the band zone."""
    part = P.new_part()
    zs = sorted({z_top, z_bottom, *[z for band in bands for z in band]}, reverse=True)
    n = folds * per_fold
    phase = rng.uniform(0, 0.25)
    grid = {}
    for r, z in enumerate(zs):
        t = (z_top - z) / (z_top - z_bottom)
        width = w_top + (w_bottom - w_top) * t
        for i in range(n + 1):
            f = i / n
            x = x_wall + side * f * width
            fold = math.sin(2 * math.pi * (f * folds + phase)) * depth * (0.8 + 0.4 * t)
            for s in (-1, 1):
                grid[r, i, s] = (P.vert(Vector((x, y_mid + fold + s * thick / 2, z)), part), f * w_bottom * 1.6, z)
    flip = side < 0

    def quad(a, b, c, d, material):
        verts = [grid[k][0] for k in (a, b, c, d)]
        uvs = [(grid[k][1], grid[k][2]) for k in (a, b, c, d)]
        if flip:
            verts, uvs = verts[::-1], uvs[::-1]
        P.poly(verts, material, uvs)

    for r in range(len(zs) - 1):
        zc = (zs[r] + zs[r + 1]) / 2
        material = BAND if any(lo_ <= zc <= hi_ for lo_, hi_ in bands) else CLOTH
        for i in range(n):
            quad((r, i, 1), (r, i + 1, 1), (r + 1, i + 1, 1), (r + 1, i, 1), material)         # room side
            quad((r, i + 1, -1), (r, i, -1), (r + 1, i, -1), (r + 1, i + 1, -1), material)     # window side
        for i, s_out in ((0, 1), (n, -1)):                                                     # side hems
            quad((r, i, -s_out), (r, i, s_out), (r + 1, i, s_out), (r + 1, i, -s_out), material)
    for r, s_out in ((0, 1), (len(zs) - 1, -1)):                                               # top and bottom hems
        material = CLOTH
        for i in range(n):
            quad((r, i, s_out), (r, i, -s_out), (r, i + 1, -s_out), (r, i + 1, s_out), material)


# --- layout: X across, Y into the room, Z up ---
tiles()
fx, fz0, fz1, fy = cfg.get("frame", [0.8, -0.735, 0.715, 0.05])
jamb, head, bottom = cfg.get("jamb", 0.13), cfg.get("head", 0.13), cfg.get("bottom", 0.1)
y_wall = lo.y
# Frame: head, bottom rail and two jambs, from inside the wall to their face in the room.
wood(Vector((0, (y_wall + fy) / 2, fz1 - head / 2)), X, Z, Y, 2 * fx, head, fy - y_wall, 0.3)
wood(Vector((0, (y_wall + fy) / 2, fz0 + bottom / 2)), X, Z, Y, 2 * fx, bottom, fy - y_wall)
for sx in (-1, 1):
    wood(Vector((sx * (fx - jamb / 2), (y_wall + fy) / 2, (fz0 + bottom + fz1 - head) / 2)), Z, X, Y,
         fz1 - head - fz0 - bottom, jamb, fy - y_wall)
oz0, oz1, ox = fz0 + bottom, fz1 - head, fx - jamb      # the opening
mul_w, mul_y = cfg.get("mullion", [0.07, 0.03])
leaf_w, leaf_d, leaf_y = cfg.get("leaf", [0.045, 0.035, -0.07])
wood(Vector((0, (leaf_y + mul_y) / 2, (oz0 + oz1) / 2)), Z, X, Y, oz1 - oz0, mul_w, mul_y - leaf_y)
# Casements with leaded glass over a stop that closes the gaps.
lg = cfg.get("leaf_gap", 0.006)
glass_t, glass_y = cfg.get("glass", [0.01, -0.062])
across, tall = cfg.get("diamonds", [6, 9])
per_tile = cfg.get("glass_tile", {}).get("per_tile", 4)
z0, z1 = oz0 + lg, oz1 - lg
y_leaf = leaf_y + leaf_d / 2
for sx in (-1, 1):
    xa, xb = sorted((sx * (mul_w / 2 + lg), sx * (ox - lg)))
    xc, w, h, zc = (xa + xb) / 2, xb - xa, z1 - z0, (z0 + z1) / 2
    for s in (-1, 1):
        wood(Vector((xc + s * (w / 2 - leaf_w / 2), y_leaf, zc)), Z, X, Y, h, leaf_w, leaf_d)
        wood(Vector((xc, y_leaf, zc + s * (h / 2 - leaf_w / 2))), X, Z, Y, w - 2 * leaf_w, leaf_w, leaf_d)
    P.board(Vector((xc, leaf_y - 0.006, zc)), X, Z, Y, w + 2 * lg + 0.01, h + 2 * lg + 0.01, 0.008, FRAME)
    pane(xa + leaf_w - 0.004, xb - leaf_w + 0.004, z0 + leaf_w - 0.004, z1 - leaf_w + 0.004, glass_y, glass_t,
         across, tall, per_tile)
    # A turn button holds each leaf shut.
    flat(Vector((sx * (mul_w / 2 + 0.01), y_leaf + leaf_d / 2, zc + 0.02)), X * sx, Z, Y,
         [(0, -0.008), (0.05, -0.008), (0.05, 0.008), (0, 0.008)], 0.005, IRON)
# Deep sill board under the frame, standing out into the room.
sx_, st, sy0, sy1 = cfg.get("sill", [0.95, 0.06, -0.12, 0.14])
wood(Vector((0, (sy0 + sy1) / 2, fz0 - st / 2)), X, Y, Z, 2 * sx_, sy1 - sy0, st)
# Curtain rod on two forged brackets, finials at both ends.
rz, ry, rr, rx = cfg.get("rod", [0.8, 0.19, 0.012, 1.055])
prism(Vector((0, ry, rz)), X, rr, 2 * rx, 8, IRON)
fin = cfg.get("finial", 0.02)
for sx in (-1, 1):
    prism(Vector((sx * (hi.x - fin), ry, rz)), X, fin, 2 * fin, 6, IRON)
    bx = sx * cfg.get("brackets", 0.93)
    P.board(Vector((bx, (y_wall + 0.13 + ry) / 2, rz - rr - 0.006)), Y, X, Z, ry - y_wall - 0.13 + 0.02, 0.016, 0.012, IRON)
    P.board(Vector((bx, ry, rz - rr / 2)), Z, X, Y, rr * 3 + 0.012, 0.016, 0.008, IRON)        # the hook round the rod
    P.board(Vector((bx, y_wall + 0.13, rz - 0.035)), Z, X, Y, 0.12, 0.04, 0.006, IRON)         # wall plate
# Curtains gathered at both sides, hung on rings.
c_x, w_top, w_bottom, c_top, folds, per_fold, depth, thick, c_y = cfg.get("curtain", [0.985, 0.4, 0.47, 0.78, 5, 4, 0.025, 0.008, 0.19])
bands = cfg.get("bands", [[0.70, 0.72], [-0.80, -0.70]])
ring_r, ring_t = cfg.get("ring", [0.024, 0.006])
for sx in (-1, 1):
    curtain(sx * c_x, -sx, w_top, w_bottom, c_top, lo.z, folds, per_fold, depth, thick, c_y, bands)
    for i in range(cfg.get("rings", 5)):
        x = sx * (c_x - (i + 0.5) / cfg.get("rings", 5) * w_top)
        ring(Vector((x, ry, rz)), X, ring_r, ring_t, 6, IRON)

def finish(goal, extra):
    """parts.Parts.finish (copied) with soft pleats: an edge between two cloth faces stays smooth up to
    `soft_deg`, so the curtain's folds shade round instead of in facets; its hems stay sharp."""
    bm = P.bm
    bmesh.ops.triangulate(bm, faces=bm.faces)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    open_edges = sum(1 for e in bm.edges if not e.is_manifold)
    soft = math.radians(cfg.get("soft_deg", 115))
    cloth = {CLOTH, BAND}
    bm.edges.index_update()
    keep_smooth = [e.index for e in bm.edges if len(e.link_faces) == 2 and e.calc_face_angle(0) < soft
                   and all(f.material_index in cloth and abs(f.normal.y) > 0.3 for f in e.link_faces)]  # not the hems
    mesh = bpy.data.meshes.new(goal["id"])
    bm.to_mesh(mesh)
    bm.free()
    mesh.validate()
    mesh.update()
    for zone in goal["zones"]:
        mat = bpy.data.materials.new(zone["key"])
        mat["group"] = zone["group"]
        mesh.materials.append(mat)
    mesh.shade_smooth()
    mesh.set_sharp_from_angle(angle=math.radians(30))
    sharp = mesh.attributes["sharp_edge"].data
    for i in keep_smooth:
        sharp[i].value = False
    obj = bpy.data.objects.new(goal["id"], mesh)
    bpy.context.scene.collection.objects.link(obj)
    for o in list(bpy.context.scene.objects):
        o.select_set(o == obj)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.export_scene.gltf(filepath=str(out / "model.glb"), export_format="GLB", use_selection=True,
                              export_apply=True, export_yup=True, export_extras=True, export_normals=True,
                              export_texcoords=True, export_animations=False, export_attributes=True)
    blo = np.min([c[:] for c in obj.bound_box], axis=0)
    bhi = np.max([c[:] for c in obj.bound_box], axis=0)
    metrics = {"triangles": len(mesh.polygons), "budget": goal["budget_tris"],
               "open_edges": open_edges, "open_edges_expected": 3 * P.nails, "parts": P.parts,
               "has_part_attribute": mesh.attributes.get("_part") is not None, "soft_cloth_edges": len(keep_smooth),
               "bbox_delta_min_m": (blo - np.array(lo)).round(4).tolist(),
               "bbox_delta_max_m": (bhi - np.array(hi)).round(4).tolist(),
               "blender_version": bpy.app.version_string, **extra}
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2))
    print("BUILT", json.dumps(metrics))


finish(goal, {"opening": [round(2 * ox, 3), round(oz1 - oz0, 3)], "diamonds": [across, tall]})
