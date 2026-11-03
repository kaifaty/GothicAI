"""Build the street lamp (FIREPLACE_NW_LAMP_01, a fire mob with one attachment) and its filigree faces.

blender -b --factory-startup -P lamp.py -- lamp.json OUT_DIR   (run from the repository root)
Writes OUT_DIR/model.glb (post, tray, frame, roof, candle: for bake_atlas.py) and OUT_DIR/lace.glb (the four
filigree faces, one quad each, the OWN_HEARTH_LACE zone: an alpha-tested tile of lamp_tiles.py, merged after
the bake by merge_glb.py).
config: {"id": "hearth-lamp", "target": "data/content/mesh3d-hearth.json", "seed": 31,
         "base": [bottom, top, radius]             the hexagonal footing under the ground (glTF y, m),
         "post": [[h, r], ...]                      the post's hexagonal profile from the footing up to the tray,
         "collars": [[h, r, rivets], ...]           rivet heads round the post at these heights,
         "tray": [[y0, y1, half], ...]              the stepped square tray under the lantern,
         "body": [y0, y1, half_bottom, half_top],   the lantern: a frustum widening upward,
         "frame": [width, thick],                   its corner and edge bars,
         "cornice": [y0, y1, half], "roof": [y0, apex, half, skirt], "finial": top,
         "candle": [y0, y1, radius], "dish": [radius, height], "wick": length}
Asset city-hearth wave 3 (reports/content/remaster/city-hearth/lamp-design.md; concept city-hearth/concept-attempt-5.png,
the user's choice of 2026-11-02: a candle in the open filigree and the lace's shadow). The form is the original's
mesh: a footing 1.25 m under the ground for slopes, a plain hexagonal post 0.15 m to 1.27 m, a stepped tray, the
lantern 0.45 m tall widening from 0.23 to 0.35 m, a cornice and a low pyramid roof to 1.97 m. The fire slot
BIP01 FIRE is at 1.61 m: the candle's flame stands there. The front is glTF -z (ZS_POS0_DIST at z +1.0 is
behind it as seen by the loader, which mirrors X; the lamp is square, so its faces are alike).
"""
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import Parts, args, finish_nodes, load_goal  # noqa: E402
from hearth_parts import bar, lump, turned  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 31))
keys = [z["key"] for z in goal["zones"]]
IRON, RIVET, TALLOW, LACE = (keys.index(k) for k in ("OWN_HEARTH_IRON", "OWN_HEARTH_RIVET", "OWN_HEARTH_TALLOW", "OWN_HEARTH_LACE"))
P = Parts(rng, 0.1)   # baked
L = Parts(rng, 0.1)   # the filigree faces, tiled


def b(x, y, z):
    """glTF metres to Blender axes."""
    return Vector((x, -z, y))


def frustum(Pp, y0, y1, h0, h1, material):
    """A closed square frustum round the vertical axis, half widths h0 at y0 and h1 at y1 (h1 0: a pyramid).
    UV in metres: u along a face's width, v up its slant."""
    part = Pp.new_part()
    corners = ((-1, -1), (1, -1), (1, 1), (-1, 1))
    low = [Pp.vert(b(sx * h0, y0, sz * h0), part) for sx, sz in corners]
    high = [Pp.vert(b(0, y1, 0), part)] * 4 if h1 == 0 else [Pp.vert(b(sx * h1, y1, sz * h1), part) for sx, sz in corners]
    slant = math.hypot(y1 - y0, h1 - h0)
    for k in range(4):
        j = (k + 1) % 4
        if h1 == 0:
            Pp.poly([low[k], low[j], high[0]], material, [(0, 0), (2 * h0, 0), (h0, slant)])
        else:
            Pp.poly([low[k], low[j], high[j], high[k]], material, [(0, 0), (2 * h0, 0), (h0 + h1, slant), (h0 - h1, slant)])
    Pp.poly(low[::-1], material, [(v.co.x, v.co.y) for v in low[::-1]])
    if h1 > 0:
        Pp.poly(high, material, [(v.co.x, v.co.y) for v in high])
    return part


# --- footing, post and collars -------------------------------------------------------------------------------
b0, b1, br = cfg["base"]
turned(P, b(0, 0, 0), [(b0, br), (b1, br)], [], 6, IRON)
turned(P, b(0, 0, 0), [tuple(row) for row in cfg["post"]], [], 6, IRON)
for h, r, n in cfg["collars"]:  # rivet heads on the collar's faces (a hexagon's face centres at 30 + 60 k degrees)
    for k in range(n):
        t = math.radians(30 + 60 * k)
        normal = Vector((math.cos(t), math.sin(t), 0))
        side = Vector((-math.sin(t), math.cos(t), 0))
        P.nail(Vector((0, 0, h)) + normal * (r * math.cos(math.radians(30))), side, Vector((0, 0, 1)), normal, 0.006, 0.005, RIVET)
# --- tray, lantern frame, cornice, roof ----------------------------------------------------------------------
for y0, y1, half in cfg["tray"]:
    frustum(P, y0, y1, half, half, IRON)
y0, y1, h0, h1 = cfg["body"]
fw, ft = cfg["frame"]
frustum(P, y0 - 0.006, y0, h0 + 0.004, h0 + 0.004, IRON)  # the lantern's floor plate
corner = lambda y, h, sx, sz: b(sx * h, y, sz * h)
signs = ((-1, -1), (1, -1), (1, 1), (-1, 1))
for k, (sx, sz) in enumerate(signs):
    nx, nz = signs[(k + 1) % 4]
    bar(P, corner(y0, h0, sx, sz), corner(y1, h1, sx, sz), fw, fw, IRON)  # corner bars
    for y, h in ((y0 + ft / 2, h0), (y1 - ft / 2, h1)):  # bottom and top edge bars
        bar(P, corner(y, h, sx, sz), corner(y, h, nx, nz), fw * 1.2, ft, IRON)
cy0, cy1, ch = cfg["cornice"]
frustum(P, cy0, cy1, ch, ch, IRON)
ry0, apex, rh, skirt = cfg["roof"]
frustum(P, ry0 - skirt, ry0, rh, rh, IRON)  # the roof's turned-down edge
frustum(P, ry0, apex, rh, 0, IRON)
top = cfg["finial"]
turned(P, b(0, apex - 0.004, 0), [(0, 0.008), (0.006, 0.012), (top - apex + 0.004, 0.003)], [], 10, RIVET)
# --- the candle on its dish and spike, the flame at the fire slot --------------------------------------------
c0, c1, cr = cfg["candle"]
dr, dh = cfg["dish"]
turned(P, b(0, y0, 0), [(0, dr * 0.6), (dh * 0.4, dr), (dh, dr * 1.04)], [(dh, dr * 0.92), (dh * 0.5, dr * 0.85)], 16, IRON)
dent = lambda h, t, r: r * (1 + 0.04 * math.sin(3 * t + 1.3) + 0.03 * math.sin(7 * t))  # hand-dipped, a little uneven
turned(P, b(0, c0, 0), [(0, cr), (c1 - c0 - 0.006, cr * 1.02), (c1 - c0, cr * 0.8)], [(c1 - c0 - 0.003, cr * 0.45)], 14, TALLOW,
       radius_at=dent)
for k in range(4):  # runs of tallow down the side from the rim, and a puddle on the dish
    t = rng.uniform(0, 2 * math.pi)
    h = c1 - rng.uniform(0.004, 0.02)
    lump(P, rng, b(cr * 1.02 * math.cos(t), h, -cr * 1.02 * math.sin(t)), rng.uniform(0.004, 0.007), TALLOW)
lump(P, rng, b(cr * 1.3, y0 + dh * 0.8, 0.004), 0.008, TALLOW)
wick = cfg["wick"]
bar(P, b(0, c1 - 0.003, 0), b(0.0015, c1 + wick, 0.0008), 0.0025, 0.0025, RIVET)
# --- the filigree faces: one quad per face in the frame's plane, the tile's trapezoid on it ---------------------
part = L.new_part(wound=True)
u_bottom = 0.5 - h0 / (2 * h1)  # the tile's bottom corners (lamp_tiles.py: 0.23 of 0.35 m)
for k, (sx, sz) in enumerate(signs):
    nx, nz = signs[(k + 1) % 4]
    inset = 0.004  # just inside the corner bars' outer faces
    quad = [L.vert(b(sx * (h0 - inset), y0 + ft, sz * (h0 - inset)), part), L.vert(b(nx * (h0 - inset), y0 + ft, nz * (h0 - inset)), part),
            L.vert(b(nx * (h1 - inset), y1 - ft, nz * (h1 - inset)), part), L.vert(b(sx * (h1 - inset), y1 - ft, sz * (h1 - inset)), part)]
    centre = sum((v.co for v in quad), Vector()) / 4
    if (quad[1].co - quad[0].co).cross(quad[3].co - quad[0].co).dot(Vector((centre.x, centre.y, 0))) < 0:
        quad = [quad[1], quad[0], quad[3], quad[2]]  # wind outward
        uv = [(1 - u_bottom, 0), (u_bottom, 0), (0, 1), (1, 1)]
    else:
        uv = [(u_bottom, 0), (1 - u_bottom, 0), (1, 1), (0, 1)]
    L.poly(quad, LACE, uv)

metrics = finish_nodes(goal, out, lo, hi, [(goal["id"], P)], {"lace_faces": 4})
for o in list(bpy.context.scene.objects):
    bpy.data.objects.remove(o)
obj, _ = L.to_object(goal, goal["id"] + "_lace")
obj.select_set(True)
bpy.context.view_layer.objects.active = obj
bpy.ops.export_scene.gltf(filepath=str(out / "lace.glb"), export_format="GLB", use_selection=True, export_apply=True,
                          export_yup=True, export_extras=True, export_normals=True, export_texcoords=True,
                          export_animations=False, export_attributes=True)
metrics["lace_triangles"] = len(obj.data.polygons)
(out / "metrics.json").write_text(__import__("json").dumps(metrics, indent=2))
print("LACE", metrics["lace_triangles"])
