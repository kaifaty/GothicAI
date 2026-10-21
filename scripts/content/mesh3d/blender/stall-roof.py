"""Build the canvas roof of a market stall by parts from an image_gen concept and a numeric target.

blender -b --factory-startup -P stall-roof.py -- stall-roof.json OUT_DIR   (run from the repository root)
config: {"id": "stall-roof", "target": "data/content/mesh3d-market.json", "seed": 1, "posts": 3, "post": 0.1,
         "arm": [0.08, 0.1, 0.1], "brace": [0.6, -0.1, 0.07], "cloth": 0.006, "sag": [0.12, 0.24],
         "cols_per_span": 16, "rows": 6, "lowest": 0.7, "valance": [9, 4, 0.08, 0.05, 0.008, 0.25],
         "stripes": [[0.015, 0.03], [0.04, 0.05]], "lash": [0.01, 0.06], "tiles": "market-set/tiles",
         "stave_tile": 0.38, "planks_per_tile": 4}
The stall roof stands on the sales table (a vob of its own) with its posts at the back,
-Y in Blender (glTF +Z), as the original's: three square posts carry sloping arms that
reach forward to the front edge, each with a diagonal brace. A sheet of canvas is thrown
over the three arms and hangs between them like the original's cloth: over an arm it lies
on it, between two arms it sags, "sag" [at the front and back edge, in the middle of the
sheet]. From its free front edge hangs a valance cut into scallops, with two woven stripes
and small soft folds ("valance": [scallops, segments per scallop, depth at the points,
more in the middle, fold depth, fold length]). No cloth comes lower than "lowest" above the
origin, the height of the original's lowest cloth, so the goods on the table stay in view.
Ropes lash the canvas to each arm's ends. The stripes and the ropes are zones of their own
("zones" in the config): red band and rope tiles.
"""
import math
import shutil
import sys
from pathlib import Path

import numpy as np
from mathutils import Vector

sys.path.insert(0, str(Path(__file__).resolve().parent))
from parts import X, Y, Z, Parts, args, load_goal  # noqa: E402

cfg, out = args()
goal, lo, hi = load_goal(cfg)
rng = np.random.default_rng(cfg.get("seed", 1))
keys = [z["key"] for z in goal["zones"]]
FRAME, CLOTH, ROPE, BAND = (keys.index(k) for k in ("OWN_PROP_WOOD_FRAME", "OWN_PROP_CLOTH", "OWN_PROP_ROPE",
                                                    "OWN_PROP_CLOTH_BAND"))
tile = cfg.get("stave_tile", 0.38)
P = Parts(rng, tile / cfg.get("planks_per_tile", 4))
staging = Path.home() / ".local/share/gothic-ai/content-staging"


def tiles():
    """OUT_DIR/tiles for the atlas bake: shared tiles, the canvas tile under both cloth keys."""
    folder = out / "tiles"
    source = staging / cfg.get("tiles", "market-set/tiles")
    for key, src in (("own_prop_wood_frame", "own_prop_wood_frame"), ("own_prop_rope", "own_prop_rope"),
                     ("own_prop_cloth", "own_prop_canvas"), ("own_prop_cloth_band", "own_prop_canvas")):
        shutil.rmtree(folder / key, ignore_errors=True)
        shutil.copytree(source / src, folder / key)


def beam(a, b, width, height, material):
    """A square-section timber from point a to point b, its height vertical as far as it can be."""
    d = (b - a).normalized()
    side = d.cross(Z).normalized() if abs(d.z) < 0.95 else X
    up = side.cross(d).normalized()
    P.post(b, a, width, width, side, up, material, tile) if height == width else \
        P.wood((a + b) / 2, d, up, side, (b - a).length, height, width, material, tile)


def sheet(points, normals, uvs, thick, row_material):
    """A closed sheet of cloth `thick` thick over a grid of points (rows x cols) offset along normals."""
    part = P.new_part()
    rows, cols = len(points), len(points[0])
    v = {(r, c, s): P.vert(points[r][c] + normals[r][c] * (s * thick / 2), part)
         for r in range(rows) for c in range(cols) for s in (-1, 1)}

    def quad(keys_, material):
        P.poly([v[k] for k in keys_], material, [uvs[k[0]][k[1]] for k in keys_])

    for r in range(rows - 1):
        m = row_material(r)
        for c in range(cols - 1):
            quad(((r, c, 1), (r, c + 1, 1), (r + 1, c + 1, 1), (r + 1, c, 1)), m)
            quad(((r, c + 1, -1), (r, c, -1), (r + 1, c, -1), (r + 1, c + 1, -1)), m)
        for c in (0, cols - 1):
            quad(((r, c, -1), (r, c, 1), (r + 1, c, 1), (r + 1, c, -1)), m)
    for r in (0, rows - 1):
        for c in range(cols - 1):
            quad(((r, c, 1), (r, c, -1), (r, c + 1, -1), (r, c + 1, 1)), row_material(min(r, rows - 2)))


def grid_normals(points):
    """Unit normals of a grid surface from its neighbours (rows run along v, cols along u)."""
    rows, cols = len(points), len(points[0])
    normals = []
    for r in range(rows):
        line = []
        for c in range(cols):
            du = points[r][min(c + 1, cols - 1)] - points[r][max(c - 1, 0)]
            dv = points[min(r + 1, rows - 1)][c] - points[max(r - 1, 0)][c]
            line.append(du.cross(dv).normalized())
        normals.append(line)
    return normals


# --- layout: X along the stall, Y toward the customer, Z up; posts at the back ---
tiles()
pw = cfg.get("post", 0.1)
arm_w, arm_h, arm_drop = cfg.get("arm", [0.08, 0.1, 0.1])
cloth = cfg.get("cloth", 0.006)
lash_t, lash_w = cfg.get("lash", [0.01, 0.06])
count, per, d0, d1, fold, fold_len = cfg.get("valance", [9, 4, 0.08, 0.05, 0.008, 0.25])
x_post = hi.x - pw / 2
y_post = lo.y + pw / 2
y_valance = hi.y - fold - cloth / 2               # the valance's middle plane; its folds meet the bbox
y_front = y_valance - cloth / 2 - 0.002           # front end of the arms, under the canvas's edge
z_back = hi.z - lash_t - cloth                    # arm top at the back; the lashing meets the bbox
post_top = z_back - arm_h


def arm_top(y):
    return z_back - arm_drop * (y - lo.y) / (y_front - lo.y)


xs = [float(x) for x in np.linspace(-x_post, x_post, cfg.get("posts", 3))]
brace_z, brace_y, brace_w = cfg.get("brace", [0.6, -0.1, 0.07])
for x in xs:
    beam(Vector((x, y_post, lo.z)), Vector((x, y_post, post_top)), pw, pw, FRAME)
    # The arm rests on the post and runs forward to the canvas's front edge; its sloping
    # back end starts 5 mm in so that its corners stay inside the bbox.
    beam(Vector((x, lo.y + 0.005, arm_top(lo.y) - arm_h / 2)), Vector((x, y_front, arm_top(y_front) - arm_h / 2)),
         arm_w, arm_h, FRAME)
    beam(Vector((x, y_post + pw / 2 - 0.01, lo.z + brace_z)), Vector((x, brace_y, arm_top(brace_y) - arm_h + 0.01)),
         brace_w, brace_w, FRAME)

# The canvas over the arms: z(x, y) = the arm's line, less the sag between two arms,
# deeper in the middle of the sheet than at its edges.
sag_edge, sag_mid = cfg.get("sag", [0.12, 0.24])
cols_per_span, rows_n = cfg.get("cols_per_span", 16), cfg.get("rows", 6)
x_cols = [lo.x + 0.003]
for a, b in zip(xs[:-1], xs[1:]):
    x_cols += [float(x) for x in np.linspace(a, b, cols_per_span + 1)[:-1]]
x_cols += [xs[-1], hi.x - 0.003]


def between(x):
    """0 over an arm (and past the outer ones), 1 midway between two arms."""
    for a, b in zip(xs[:-1], xs[1:]):
        if a <= x <= b:
            return math.sin(math.pi * (x - a) / (b - a))
    return 0.0


def canvas_z(x, t):
    """Top of the canvas at x, a share t of the way from the back (0) to the front edge (1)."""
    y = lo.y + t * (y_front - lo.y)
    return arm_top(y) + cloth - between(x) * (sag_edge + (sag_mid - sag_edge) * math.sin(math.pi * t))


pts, uvs = [], []
for r in range(rows_n + 1):
    t = r / rows_n
    y = lo.y + 0.01 + t * (y_valance - lo.y - 0.01)
    pts.append([Vector((x, y, canvas_z(x, t) - cloth / 2)) for x in x_cols])
    uvs.append([(x, t * (y_valance - lo.y)) for x in x_cols])
sheet(pts, grid_normals(pts), uvs, cloth, lambda r: CLOTH)

# Rope lashings round each arm's ends, over the canvas. Round the arm the lashing is as wide as
# the post under it, and the back one wraps the post's top: its sides are 2 mm thinner, inside
# the post's faces (one plane z-fights, coplanar.py).
for x in xs:
    for y in (lo.y + 0.06, y_front - 0.05):
        zc = arm_top(y) - arm_h / 2 + cloth / 2
        P.board(Vector((x, y, zc)), Y, X, Z, lash_w, arm_w + 2 * lash_t - 0.004, arm_h + cloth + 2 * lash_t, ROPE)

# Valance from the canvas's free front edge: scallops, two stripes, small soft folds.
stripes = cfg.get("stripes", [[0.015, 0.03], [0.04, 0.05]])
lowest = cfg.get("lowest", 0.7)
cols = [lo.x + 0.003 + (hi.x - lo.x - 0.006) * k / (count * per) for k in range(count * per + 1)]
depths = sorted({0.0, *[d for s in stripes for d in s]})
pts, uvs = [], []
for d in depths + [None]:
    row, uv_row = [], []
    for k, x in enumerate(cols):
        top = canvas_z(x, 1.0)
        bottom = min(d0 + d1 * math.sin(math.pi * (k % per) / per), top - lowest)
        z = top - (bottom if d is None else min(d, bottom - 0.005))
        swing = 1.0 if d is None or d > 0.02 else 0.4   # the folds open toward the hem
        row.append(Vector((x, y_valance + fold * swing * math.sin(2 * math.pi * x / fold_len), z)))
        uv_row.append((x, top - z))
    pts.append(row)
    uvs.append(uv_row)
stripe_rows = {i for i, d in enumerate(depths) if any(abs(d - s[0]) < 1e-9 for s in stripes)}
sheet(pts, grid_normals(pts), uvs, cloth, lambda r: BAND if r in stripe_rows else CLOTH)

lowest_z = min(v.z for v in pts[-1])
P.finish(goal, out, lo, hi, {"arms_x": [round(x, 3) for x in xs], "scallops": count, "lowest_cloth_z": round(lowest_z, 3)})
