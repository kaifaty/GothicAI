"""Build the outer wooden window of a timber-framed house by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P window-out.py -- window-out.json OUT_DIR   (run from the repository root)
config: {"id": "window-out", "target": "data/content/mesh3d-market.json", "seed": 1,
         "lintel": [0.2, 0.09], "sill": [0.16, 0.1125], "post": [0.15, 0.06], "mullion": [0.07, 0.04],
         "leaf": [0.05, 0.04, -0.035], "leaf_gap": 0.006, "glass": [0.012, -0.024], "diamonds": [6, 9],
         "hinge": [0.09, 0.026, 0.004], "lintel_nails": 4, "nail": 0.008, "nail_radius": 0.018,
         "tiles": "market-set/tiles", "glass_tile": {"size": 1024, "per_tile": 4, "came": 0.045, "seed": 2},
         "stave_tile": 0.38, "planks_per_tile": 4}
The street side faces +Y in Blender (glTF -Z): there the original's lintel and sill
stand out and its glass sits back. Lintel, sill and side posts run back into the wall
to the far side of the bbox; the mullion and two casement leaves sit between them,
each leaf a thin frame around a pane of leaded glass over a thin stop that closes the gaps. The lattice of lead cames is not
modelled: the pane's UV maps a procedural tile of diamonds (written to OUT_DIR/tiles
with copies of the wood and iron tiles, the maps_dir of the atlas bake) so that a
leaf shows the concept's diamonds across and tall. Iron: two hinges with pintles per
leaf, a latch, nail heads on the lintel.
"""
import math
import shutil
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
FRAME, GLASS, IRON = (keys.index(k) for k in ("OWN_PROP_WOOD_FRAME", "OWN_PROP_GLASS", "OWN_PROP_IRON_BAND"))
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
nail_h, nail_r = cfg.get("nail", 0.005), cfg.get("nail_radius", 0.012)
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
    """Leaded glass: per_tile x per_tile diamonds of slightly different panes, raised lead cames,
    grime gathered in the corners. Seamless; colour is only relative (the bake sets the mean)."""
    r = np.random.default_rng(seed)
    v, u = (np.mgrid[0:size, 0:size] + 0.5) / size        # row 0 = bottom = v near 0
    p, q = u * per_tile, v * per_tile
    a, b = p + q, p - q
    da = np.abs(a - np.round(a))                           # distance to the two families of cames
    db = np.abs(b - np.round(b))
    d = np.minimum(da, db) / math.sqrt(2)
    lead = np.clip(1 - (d - came * 0.5) / (came * 0.5), 0, 1)
    corner = np.exp(-(np.hypot(da, db) / 0.16) ** 2)       # where cames cross
    cell = (np.floor(a).astype(int) % per_tile) * 7 + (np.floor(b).astype(int) % per_tile) * 13
    tint = r.uniform(-1, 1, (7 * per_tile + 13 * per_tile, 3))[cell]
    wave = np.sin(2 * math.pi * (p * 0.5 + r.uniform())) * np.sin(2 * math.pi * (q * 0.5 + r.uniform()))
    pane = np.array([0.30, 0.36, 0.31]) * (1 + 0.12 * tint) * (1 + 0.05 * wave)[..., None]
    pane *= (1 - 0.55 * corner)[..., None]                 # grime in the corners of each diamond
    metal = np.array([0.16, 0.16, 0.15])
    albedo = pane * (1 - lead[..., None]) + metal * lead[..., None]
    height = 0.6 * lead + 0.04 * wave
    gu = (np.roll(height, -1, axis=1) - np.roll(height, 1, axis=1)) * size / 2   # per unit of UV
    gv = (np.roll(height, -1, axis=0) - np.roll(height, 1, axis=0)) * size / 2
    strength = 0.004                                       # relief height per tile unit
    normal = np.stack([-gu * strength, -gv * strength, np.ones_like(gu)], axis=2)
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    folder.mkdir(parents=True, exist_ok=True)
    save_png(folder / "albedo.png", albedo)
    save_png(folder / "normal.png", normal * 0.5 + 0.5)
    rough = 0.22 + 0.35 * corner + 0.4 * lead
    for name, value in (("roughness", rough), ("metallic", np.zeros_like(rough)), ("ao", np.ones_like(rough)),
                        ("height", height)):
        save_png(folder / f"{name}.png", np.repeat(np.clip(value, 0, 1)[..., None], 3, axis=2))


def tiles():
    """OUT_DIR/tiles: the shared wood and iron tiles plus our own glass tile, for the atlas bake."""
    folder = out / "tiles"
    source = staging / cfg.get("tiles", "market-set/tiles")
    for key in ("own_prop_wood_frame", "own_prop_iron_band"):
        shutil.rmtree(folder / key, ignore_errors=True)
        shutil.copytree(source / key, folder / key)
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


def pane(x0, x1, z0, z1, y, thick, across, tall, per_tile):
    """A thin closed glass box; every face maps the lattice tile so that the front shows
    `across` x `tall` diamonds, a vertex of the lattice in each corner."""
    part = P.new_part()
    du, dv = (x1 - x0) / across * per_tile, (z1 - z0) / tall * per_tile   # metres per tile unit
    corner = {(sx, sy, sz): P.vert(Vector((x0 if sx < 0 else x1, y + sy * thick / 2, z0 if sz < 0 else z1)), part)
              for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)}
    for index in range(3):
        others = [i for i in range(3) if i != index]
        for sign in (-1, 1):
            quad = [(k, v) for k, v in corner.items() if k[index] == sign]
            ring_ = sorted(quad, key=lambda kv: math.atan2(kv[0][others[1]], kv[0][others[0]]))
            P.poly([v for _, v in ring_], GLASS, [((v.co.x - x0) / du, (v.co.z - z0) / dv) for _, v in ring_])


# --- layout: X across, Y toward the street, Z up ---
tiles()
lint_h, lint_y = cfg.get("lintel", [0.2, 0.09])
sill_h, sill_y = cfg.get("sill", [0.16, 0.1125])
post_w, post_y = cfg.get("post", [0.15, 0.06])
mul_w, mul_y = cfg.get("mullion", [0.07, 0.04])
y_wall = lo.y                                      # the far side of the bbox, inside the wall
z_sill, z_lint = lo.z + sill_h, hi.z - lint_h      # the opening between sill and lintel

# Lintel and sill: whole beams across, deep into the wall; the sill stands out furthest.
wood(Vector((0, (y_wall + lint_y) / 2, hi.z - lint_h / 2)), X, Z, Y, hi.x - lo.x, lint_h, lint_y - y_wall, 0.3)
wood(Vector((0, (y_wall + sill_y) / 2, lo.z + sill_h / 2)), X, Z, Y, hi.x - lo.x, sill_h, sill_y - y_wall)
# Side posts between them, and the mullion.
for sx in (-1, 1):
    wood(Vector((sx * (hi.x - post_w / 2), (y_wall + post_y) / 2, (z_sill + z_lint) / 2)), Z, X, Y,
         z_lint - z_sill, post_w, post_y - y_wall)
leaf_w, leaf_d, leaf_y = cfg.get("leaf", [0.05, 0.04, -0.035])
y_leaf = leaf_y + leaf_d / 2
wood(Vector((0, (leaf_y + mul_y) / 2, (z_sill + z_lint) / 2)), Z, X, Y, z_lint - z_sill, mul_w, mul_y - leaf_y)

# Two casement leaves: a thin frame each, leaded glass inside.
lg = cfg.get("leaf_gap", 0.006)
glass_t, glass_y = cfg.get("glass", [0.012, -0.024])
across, tall = cfg.get("diamonds", [6, 9])
per_tile = cfg.get("glass_tile", {}).get("per_tile", 4)
hinge_len, hinge_w, hinge_t = cfg.get("hinge", [0.09, 0.026, 0.004])
z0, z1 = z_sill + lg, z_lint - lg
for sx in (-1, 1):
    xa, xb = sorted((sx * (mul_w / 2 + lg), sx * (hi.x - post_w - lg)))
    xc, w, h = (xa + xb) / 2, xb - xa, z1 - z0
    zc = (z0 + z1) / 2
    for s in (-1, 1):
        wood(Vector((xc + s * (w / 2 - leaf_w / 2), y_leaf, zc)), Z, X, Y, h, leaf_w, leaf_d)
        wood(Vector((xc, y_leaf, zc + s * (h / 2 - leaf_w / 2))), X, Z, Y, w - 2 * leaf_w, leaf_w, leaf_d)
    # A stop behind the leaf closes the gaps round it (the wall's hole is not ours to show).
    P.board(Vector((xc, leaf_y - 0.006, zc)), X, Z, Y, w + 2 * lg + 0.01, h + 2 * lg + 0.01, 0.008, FRAME)
    pane(xa + leaf_w - 0.004, xb - leaf_w + 0.004, z0 + leaf_w - 0.004, z1 - leaf_w + 0.004, glass_y, glass_t,
         across, tall, per_tile)
    # Hinges on the post side: a short strap on the leaf and a pintle on the post.
    face = y_leaf + leaf_d / 2
    x_edge = sx * (hi.x - post_w - lg / 2)
    for zh in (z1 - 0.16, z0 + 0.16):
        flat(Vector((x_edge + sx * 0.01, face, zh)), X * -sx, Z, Y,
             [(0, -hinge_w / 2), (hinge_len - 0.02, -hinge_w / 2), (hinge_len, 0), (hinge_len - 0.02, hinge_w / 2),
              (0, hinge_w / 2)], hinge_t, IRON)
        prism(Vector((x_edge + sx * 0.014, face + 0.004, zh)), Z, 0.008, hinge_w + 0.02, 6, IRON)
        P.nail(Vector((x_edge - sx * (hinge_len - 0.035), face + hinge_t, zh)), X, Z, Y, nail_r * 0.5, nail_h * 0.6, IRON)
# Latch: a flat bar across the meeting stiles and a turning knob on the mullion.
face = y_leaf + leaf_d / 2
zl = (z0 + z1) / 2 + 0.03
flat(Vector((-0.07, face, zl)), X, Z, Y, [(0, -0.009), (0.14, -0.009), (0.14, 0.009), (0, 0.009)], 0.005, IRON)
prism(Vector((0.0, face + 0.012, zl)), Y, 0.013, 0.016, 6, IRON)
# Nail heads on the lintel's face.
count = cfg.get("lintel_nails", 4)
for i in range(count):
    x = (i + 0.5) / count * (hi.x - lo.x) * 0.92 + lo.x * 0.92
    P.nail(Vector((x, lint_y, hi.z - lint_h / 2 + rng.uniform(-0.015, 0.015))), X, Z, Y, nail_r, nail_h, IRON)

P.finish(goal, out, lo, hi, {"opening": [round(2 * (hi.x - post_w), 3), round(z_lint - z_sill, 3)],
                             "diamonds": [across, tall]})
