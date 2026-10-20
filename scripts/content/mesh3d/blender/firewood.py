"""Build a stack of split firewood (OC_FIREWOOD_V3) from an image_gen concept and a numeric target.

blender -b --factory-startup -P firewood.py -- firewood.json OUT_DIR   (run from the repository root)
config: {"id": "firewood", "target": "data/content/mesh3d-market.json", "seed": 1, "layers": 9,
         "log": [0.17, 0.15], "length": [0.82, 0.93], "step": 1, "poles": 0.08, "sides": 7,
         "kinds": [0.35, 0.45, 0.2], "bark_tile": 0.5, "split_tile": 0.38}
Logs lie front to back with their cut ends to the viewer, layer on layer on two
lengthwise poles; each layer loses about a log, from alternating ends, so the
front face is a stepped trapezoid as on the concept. A log is a whole round, a half or a
quarter: bark on the round side, flat split faces with grain along the log, and
end-grain discs on both ends mapped so the rings centre on the pith (at the
corner of a quarter). Logs vary in length and stick out unevenly at the front;
the back is flush against the wall. A dark core fills the inside of the stack.
"""
import math
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
BARK, END, SPLIT = (keys.index(k) for k in ("OWN_PROP_BARK", "OWN_PROP_WOOD_END", "OWN_PROP_WOOD_SPLIT"))
bark_tile, split_tile = cfg.get("bark_tile", 0.5), cfg.get("split_tile", 0.38)
P = Parts(rng, split_tile / 4)
sides = cfg.get("sides", 7)


def outline(kind, radius, spin):
    """Cross-section (x, z) points around the pith at (0, 0), with a flag: True for bark edges."""
    if kind == 0:   # whole round
        pts = [(math.cos(spin + 2 * math.pi * i / sides) * radius, math.sin(spin + 2 * math.pi * i / sides) * radius)
               for i in range(sides)]
        return pts, [True] * sides
    span = math.pi if kind == 1 else math.pi / 2
    n = max(sides // (2 if kind == 1 else 3), 2)
    arc = [(math.cos(spin + span * i / n) * radius, math.sin(spin + span * i / n) * radius) for i in range(n + 1)]
    if kind == 1:   # half: arc plus the flat split face back to the start
        return arc, [True] * n + [False]
    return arc + [(0.0, 0.0)], [True] * n + [False, False]   # quarter: arc, then two split faces through the pith


def log(c_x, c_z, y_back, length, radius, kind, spin):
    part = P.new_part()
    pts, bark = outline(kind, radius, spin)
    # sit the log on its lowest point
    low = min(z for _, z in pts)
    ends = []
    for y in (y_back, y_back - length):
        ends.append([P.vert(Vector((c_x + x, y, c_z - low + z)), part) for x, z in pts])
    v_off = rng.uniform(0, 1)
    n = len(pts)
    around = 0.0
    for i in range(n):
        j = (i + 1) % n
        edge = math.dist(pts[i], pts[j])
        a, b = ends[0], ends[1]
        if bark[i]:
            u0, u1 = around / bark_tile, (around + edge) / bark_tile
            uv = ((u0, v_off), (u1, v_off), (u1, v_off + length / bark_tile), (u0, v_off + length / bark_tile))
            mat = BARK
        else:
            u0, u1 = 0.1, 0.1 + edge / split_tile
            uv = ((u0, v_off), (u1, v_off), (u1, v_off + length / split_tile), (u0, v_off + length / split_tile))
            mat = SPLIT
        P.poly((a[i], a[j], b[j], b[i]), mat, uv)
        around += edge
    # end-grain discs: the log-end image spans 0..1 around the pith, scaled to this radius
    rot = rng.uniform(0, 2 * math.pi)
    def disc(x, z):
        xr, zr = x * math.cos(rot) - z * math.sin(rot), x * math.sin(rot) + z * math.cos(rot)
        return (0.5 + 0.47 * xr / radius, 0.5 + 0.47 * zr / radius)
    P.poly(ends[0][::-1], END, [disc(x, z) for x, z in pts][::-1])   # back end faces +Y
    P.poly(ends[1], END, [disc(x, z) for x, z in pts])               # front end faces -Y


layers = cfg.get("layers", 9)
log_w, log_h = cfg.get("log", [0.17, 0.15])
len_lo, len_hi = cfg.get("length", [0.82, 0.93])
poles = cfg.get("poles", 0.08)
kinds = np.array(cfg.get("kinds", [0.35, 0.45, 0.2]))
y_back = hi.y                      # flush against the wall
width = hi.x - lo.x
count0 = int(width // log_w)
# two lengthwise poles under the bottom layer
for frac in (0.25, 0.75):
    P.wood(Vector((lo.x + width / 2, lo.y + (hi.y - lo.y) * frac, lo.z + poles / 2)), X, Y, Z, width - 0.1, poles, poles, BARK, bark_tile)
layer_h = min((hi.z - lo.z - poles) / layers, log_h)
shrink = cfg.get("step", 1.2)       # logs lost per layer, taken from alternating ends
size = {0: 0.5, 1: 0.68, 2: 0.86}   # radius per log width: halves and quarters are cut from bigger rounds
for k in range(layers):
    lost = int(round(shrink * k))
    count = count0 - lost
    if count < 1:
        break
    left = lost // 2 + (lost % 2 if k % 2 else 0)
    x0 = lo.x + log_w / 2 + left * log_w + (width - count0 * log_w) / 2
    z = lo.z + poles + k * layer_h
    for i in range(count):
        kind = int(rng.choice(3, p=kinds / kinds.sum()))
        radius = log_w * size[kind] * rng.uniform(0.9, 1.05)
        spin = rng.uniform(0, 2 * math.pi) if kind == 0 else rng.choice([math.pi, 0.0, math.pi / 2]) + rng.uniform(-0.3, 0.3)
        length = rng.uniform(len_lo, len_hi)
        log(x0 + i * log_w + rng.uniform(-0.01, 0.01), z, y_back - rng.uniform(0, 0.02), length, radius, kind, spin)

# A dark core inside the stack, behind the shortest log ends, so gaps read as shadow.
top_count = count0 - int(round(shrink * (layers - 1)))
inset = 0.07
y_front, y_rear = y_back - len_lo + 0.06, y_back - 0.04
z0, z1 = lo.z + poles, lo.z + poles + layers * layer_h - inset
half_lo, half_hi = count0 * log_w / 2 - inset, max(top_count, 1) * log_w / 2 - inset
xc = lo.x + width / 2
core = P.new_part()
ring = [(-half_lo, z0), (half_lo, z0), (half_hi, z1), (-half_hi, z1)]
front = [P.vert(Vector((xc + x, y_front, z)), core) for x, z in ring]
rear = [P.vert(Vector((xc + x, y_rear, z)), core) for x, z in ring]
P.poly(front, BARK, [(0, 0), (1, 0), (1, 1), (0, 1)])
P.poly(rear[::-1], BARK, [(0, 0), (1, 0), (1, 1), (0, 1)])
for i in range(4):
    j = (i + 1) % 4
    P.poly((front[j], front[i], rear[i], rear[j]), BARK, [(0, 0), (1, 0), (1, 1), (0, 1)])

P.finish(goal, out, lo, hi, {"layers": layers, "bottom_logs": count0})
