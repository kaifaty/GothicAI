"""Own canopy of the master's bed for gen_prop.py ("own": {"module": "canopy"}): head posts, the frame on them, the
linen and its ties. The bed below them is the generator's.

reports/content/remaster/city-furniture/decisions.md, 2026-11-03: the generator builds thin cloth as torn shards, and
in the house its faces turned from the warm lamp take only the cold sky term of the engine's ambient
(shader/lighting/ambient_light.frag: the sky irradiance by the normal), so the canopy read as blue patches. The user
chose own cloth («Своя ткань в Blender»). The form follows the original's mesh (BEDHIGH_NW_MASTER_01, read as numbers,
no vertex copied): a box over the head end, a short front valance, a back panel behind the head board, side
curtains swept from the front corners back to a tie on each head post at 0.76 m, tails from the tie to the floor.
Folds stay shallow (a slope under about 20 degrees outside the gathers) so a curtain faces the lamp as one sheet,
as the original's flat sheets do; the gathers at the ties are steep but small.

glTF metres here (x along the bed, the head at -x; y up; z across), b() turns them into Blender axes.
"""
import math

import bpy
import numpy as np
from mathutils import Vector

TH = 0.003  # the two layers of the linen
LINEN = (0.389, 0.335, 0.230)  # NW_SHIP_SAIL_02, linear mean; pack sets the zone to it again
BAND = (0.050, 0.042, 0.024)  # the tie: a dark woven band with two ochre stripes, as the original's tie
STRIPE = (0.160, 0.115, 0.050)
TIE_Y = 0.76


def b(x, y, z):
    return Vector((x, -z, y))


def lerp(a, c, t):
    return a + (c - a) * t


def _object(name, kind, verts, faces, uvs=None, sharp=None):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in verts], [], faces)
    if uvs is not None:
        layer = me.uv_layers.new(name="UVMap")
        layer.data.foreach_set("uv", np.array([uv for face in uvs for uv in face], dtype=np.float32).ravel())
    me.validate()
    me.update()
    obj = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(obj)
    obj["kind"] = kind
    me.shade_smooth()
    if sharp:
        me.set_sharp_from_angle(angle=math.radians(sharp))
    if uvs is None:  # solid wood and bands: Blender's own unwrap, packed with the rest by gen_prop
        bpy.context.view_layer.objects.active = obj
        for o in bpy.context.scene.objects:
            o.select_set(o is obj)
        bpy.ops.object.mode_set(mode="EDIT")
        bpy.ops.mesh.select_all(action="SELECT")
        bpy.ops.uv.smart_project(angle_limit=math.radians(50), island_margin=0.01)
        bpy.ops.object.mode_set(mode="OBJECT")
    return obj


def ruffle(u, n, seed):
    """n folds across u in 0..1, each of its own width and depth, 0 at both ends like sin(2 pi n u): the phase runs
    unevenly (a few slow sines on its rate) and the depth wanders; a narrow fold is made shallower, so no fold is
    steeper than the even ones were (the lamp-facing rule in the docstring). The user, 2026-11-03: even folds read
    too perfect for cloth."""
    rng = np.random.default_rng(seed)
    a, p = rng.uniform(0.08, 0.17, 3) * n, rng.uniform(0, 2 * math.pi, 5)
    phase = n * u + sum(a[i] / (2 * math.pi * (i + 1)) * (math.sin(2 * math.pi * (i + 1) * u + p[i]) - math.sin(p[i]))
                        for i in range(3))
    rate = 1 + sum(a[i] / n * math.cos(2 * math.pi * (i + 1) * u + p[i]) for i in range(3))
    depth = min(1.6, (1 + 0.3 * math.sin(2 * math.pi * 1.7 * u + p[3]) + 0.2 * math.sin(2 * math.pi * 3.3 * u + p[4])) / rate ** 0.8)
    return depth * math.sin(2 * math.pi * phase)


def sheet(name, grid, rest=None):
    """Linen from a grid of glTF points [row][col], rows down the cloth: two layers TH apart, each smooth on its own,
    UV in metres along the cloth; the point attribute "edge" holds the distance to the nearest side edge and to the
    bottom edge (metres), where the material lays its hem. rest: the flat sheet's (across, down) metres per point, for
    cloth that a simulation gathered (the UV and the hem then stay where they were on the flat linen)."""
    P = np.array(grid, dtype=np.float64)
    R, C, _ = P.shape
    n = np.cross(np.gradient(P, axis=1), np.gradient(P, axis=0))
    n /= np.maximum(np.linalg.norm(n, axis=2, keepdims=True), 1e-9)
    if rest is not None:
        s, t = rest[..., 0], rest[..., 1]
    else:
        s = np.concatenate([np.zeros((R, 1)), np.cumsum(np.linalg.norm(np.diff(P, axis=1), axis=2), axis=1)], axis=1)
        t = np.concatenate([np.zeros((1, C)), np.cumsum(np.linalg.norm(np.diff(P, axis=0), axis=2), axis=0)], axis=0)
    verts, faces, uvs = [], [], []
    for side in (1, -1):
        base = len(verts)
        verts += [b(*q) for q in (P + n * (side * TH / 2)).reshape(-1, 3)]
        for r in range(R - 1):
            for c in range(C - 1):
                quad = [(r, c), (r, c + 1), (r + 1, c + 1), (r + 1, c)]
                if side < 0:
                    quad = quad[::-1]
                faces.append([base + i * C + j for i, j in quad])
                uvs.append([(s[i, j], t[i, j]) for i, j in quad])
    obj = _object(name, "cloth", verts, faces, uvs)
    edge = np.dstack([np.minimum(s, s[:, -1:] - s), t[-1:, :] - t, np.zeros((R, C))]).reshape(-1, 3)
    attr = obj.data.attributes.new("edge", "FLOAT_VECTOR", "POINT")
    attr.data.foreach_set("vector", np.concatenate([edge, edge]).astype(np.float32).ravel())
    return obj


def block(name, x, y, z, bevel=0.006):
    """A planed wooden bar (glTF ranges), its edges rounded."""
    import bmesh
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = b(lerp(x[0], x[1], v.co.x + 0.5), lerp(y[0], y[1], v.co.z + 0.5), lerp(z[0], z[1], 0.5 - v.co.y))
    bmesh.ops.bevel(bm, geom=bm.edges[:], offset=bevel, segments=2, affect="EDGES", profile=0.5)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    verts = [v.co.copy() for v in me.vertices]
    faces = [list(p.vertices) for p in me.polygons]
    bpy.data.meshes.remove(me)
    return _object(name, "wood", verts, faces, sharp=40)


def band(name, s, x=(-1.232, -1.118), z=(0.572, 0.640), y=(TIE_Y - 0.045, TIE_Y + 0.045), th=0.010, rc=0.022):
    """The tie: a woven band round the gathered linen, against the post's outer face."""
    ring = []
    corners = [(x[1] - rc, z[1] - rc, 0), (x[0] + rc, z[1] - rc, 90), (x[0] + rc, z[0] + rc, 180), (x[1] - rc, z[0] + rc, 270)]
    for cx, cz, a0 in corners:
        for k in range(6):
            a = math.radians(a0 + 90 * k / 5)
            ring.append((cx + rc * math.cos(a), cz + rc * math.sin(a), math.cos(a), math.sin(a)))
    ring.append(ring[0])  # a seam for the UV
    section = [(-th / 2, y[0]), (th / 2, y[0]), (th / 2, y[1]), (-th / 2, y[1])]
    verts, faces, uvs, length = [], [], [], [0.0]
    for (x0, z0, *_), (x1, z1, *_) in zip(ring, ring[1:]):
        length.append(length[-1] + math.hypot(x1 - x0, z1 - z0))
    for px, pz, ox, oz in ring:
        verts += [b(px + ox * o, yy, s * (pz + oz * o)) for o, yy in section]
    along = [0, th, th + y[1] - y[0], 2 * th + y[1] - y[0], 2 * th + 2 * (y[1] - y[0])]
    for i in range(len(ring) - 1):
        for k in range(4):
            quad = [(i, k), (i + 1, k), (i + 1, (k + 1) % 4), (i, (k + 1) % 4)]
            if s > 0:  # the ring runs counterclockwise seen from above: keep the faces outward on each side
                quad = quad[::-1]
            faces.append([a * 4 + c for a, c in quad])
            uvs.append([(length[a], along[k if c == k else k + 1]) for a, c in quad])
    return _object(name, "band", verts, faces, uvs, sharp=60)


def bezier(p, t):
    return (1 - t) ** 3 * p[0] + 3 * (1 - t) ** 2 * t * p[1] + 3 * (1 - t) * t ** 2 * p[2] + t ** 3 * p[3]


def curtain(s, rows=22, cols=37, folds=6):
    """A side curtain hung from the rail (top edge x -1.30..-0.29) and drawn back to the tie: each column falls
    first, then sweeps back; the front edge passes the original's (-0.40, 1.62) and (-0.70, 1.20)."""
    grid = []
    for i in range(rows):
        v = i / (rows - 1)
        row = []
        for j in range(cols):
            u = j / (cols - 1)
            p = [np.array(q) for q in ((lerp(-1.300, -0.290, u), 2.122), (lerp(-1.300, -0.290, u), lerp(1.70, 1.80, u)),
                                       (lerp(-1.255, -0.680, u), lerp(1.15, 1.00, u)), (lerp(-1.215, -1.135, u), TIE_Y))]
            x, y = bezier(p, v)
            fold = (0.003 + 0.012 * v ** 2.5) * ruffle(u, folds, 11 + s)
            row.append((x, y, s * (0.600 + fold)))
        grid.append(row)
    return sheet(f"curtain{s:+d}", grid)


def hung(s, path, source):
    """The side curtain as curtain_sim.py hung it, the tail below the tie included: the game takes every 4th row and
    3rd column of its grid (5.9 x 4.5 cm), the bake its full 1.5 cm grid, whose fine folds go to the normal map."""
    data = np.load(path)
    step = (slice(None), slice(None)) if source else (slice(None, None, 4), slice(None, None, 3))
    return sheet(f"curtain{s:+d}", data["grid"][step], data["rest"][step])


def tail(s, rows=14, cols=31, folds=5):
    """The curtain below the tie, spreading to the floor beside the head post; it also hides where the generator's
    bed meets the post (its head corners keep slivers of the cloth it built there)."""
    grid = []
    for i in range(rows):
        v = i / (rows - 1)
        row = []
        for j in range(cols):
            u = j / (cols - 1)
            x = lerp(lerp(-1.222, -1.128, u), lerp(-1.340, -0.960, u), v ** 0.55)
            z = 0.603 + 0.035 * v ** 2 + (0.005 + 0.014 * v) * ruffle(u, folds, 21 + s)
            row.append((x, lerp(TIE_Y + 0.005, 0.006, v), s * z))
        grid.append(row)
    return sheet(f"tail{s:+d}", grid)


def top():
    """The roof of the box, stretched over the rails with a slight sag, its edges rounded down over them."""
    a = sorted({0.0, 0.012, 0.03, 0.97, 0.988, 1.0, *np.linspace(0.06, 0.94, 10)})
    droop = lambda p: 0.022 * max(0.0, 1 - min(p, 1 - p) / 0.03) ** 2  # noqa: E731
    return sheet("top", [[(lerp(-1.315, -0.262, r), 2.138 - 0.010 * math.sin(math.pi * r) * math.sin(math.pi * c) - droop(r) - droop(c),
                           lerp(-0.612, 0.612, c)) for c in a] for r in a])


def valance():
    """The short front drop, pleated toward its hem, which sags in the middle (the original: 1.95 m at the
    corners, 1.89 in the middle)."""
    zs = np.linspace(-0.612, 0.612, 41)
    return sheet("valance", [[(-0.270 - 0.006 * t * ruffle(z / 1.224 + 0.5, 8, 31), lerp(2.128, 1.95 - 0.045 * (1 - (z / 0.612) ** 2), t), z)
                              for z in zs] for t in np.linspace(0, 1, 6)])


def back():
    """The panel behind the head board, down to 0.30 m (the original: 0.63; lower hides the generator's ragged back of
    the board), in soft folds that deepen toward its hem."""
    zs = np.linspace(-0.605, 0.605, 31)
    return sheet("back", [[(-1.306 - (0.003 + 0.005 * (1 - y / 2.13)) * max(0.0, 1 + ruffle(z / 1.21 + 0.5, 6, 41)) / 2, y, z)
                           for z in zs] for y in np.linspace(2.128, 0.30, 14)])


def mattress(cols=40, rows=56, x=(-1.135, 0.995), half=0.49, base=0.28, edge=0.50, crown=0.03, r=0.05, tufts=True,
             lumps=0.0, seed=0, kind="tick"):
    """The tick: a soft quilted mattress between the head and foot boards, in place of the generator's one (its
    colour lost the weave and the woven borders, reports/content/remaster/city-furniture/mattress-fabric.md). UV
    is the original's sheet: across, side - top - side (0..1); along, the length (0..1), the borders at its ends.
    tufts: the quilting of a rich tick; lumps: the height of the lumps straw settles into (and the hollow where one
    sleeps, half as deep again), for a plain straw sack."""
    profile = [(-half, base), (-half, edge - r)]
    profile += [(-half + r - r * math.cos(a), edge - r + r * math.sin(a)) for a in np.linspace(0, math.pi / 2, 7)[1:]]
    profile += [(half - r + r * math.sin(a), edge - r + r * math.cos(a)) for a in np.linspace(0, math.pi / 2, 7)]
    profile += [(half, base)]
    profile = np.array(profile)
    arc = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(profile, axis=0), axis=1))])
    us = np.linspace(0, 1, cols)
    zs, ys = np.interp(us * arc[-1], arc, profile[:, 0]), np.interp(us * arc[-1], arc, profile[:, 1])
    tufts = [(tx, tz) for tx in np.linspace(x[0] + 0.25, x[1] - 0.25, 5) for tz in (-0.21, 0.21)] if tufts else []
    rng = np.random.default_rng(seed)
    bumps = [(rng.uniform(*x), rng.uniform(-half, half), rng.uniform(0.06, 0.14), rng.uniform(-1, 1) * lumps)
             for _ in range(28 if lumps else 0)]
    if lumps:
        bumps.append((lerp(*x, 0.45), 0.02, 0.32, -1.5 * lumps))  # the hollow of the sleeper, toward the head
    verts, faces, uvs = [], [], []
    for i in range(rows):
        v = i / (rows - 1)
        px = lerp(x[0], x[1], v)
        end = min(px - x[0], x[1] - px) / 0.06  # the ends round down where they meet the boards
        end = math.sqrt(max(0.0, 1 - (1 - min(end, 1.0)) ** 2))
        for z, y in zip(zs, ys):
            top = min(max((y - (edge - r)) / r, 0.0), 1.0)
            y = base + (y - base) * (0.75 + 0.25 * end) + top * crown * (1 - (z / half) ** 2)
            y += top * 0.004 * math.sin(7.1 * px + 1.3) * math.sin(9.7 * z + 0.4)  # the filling settles unevenly
            y -= top * sum(0.012 * math.exp(-((px - tx) ** 2 + (z - tz) ** 2) / (2 * 0.035 ** 2)) for tx, tz in tufts)
            y += top * sum(h * math.exp(-((px - bx) ** 2 / 2.5 + (z - bz) ** 2) / (2 * w ** 2)) for bx, bz, w, h in bumps)
            verts.append(b(px, y, z * (0.97 + 0.03 * end)))
    for i in range(rows - 1):
        for j in range(cols - 1):
            quad = [(i, j), (i, j + 1), (i + 1, j + 1), (i + 1, j)]
            faces.append([a * cols + c for a, c in quad])
            uvs.append([(us[c], a / (rows - 1)) for a, c in quad])
    return _object("mattress", kind, verts, faces, uvs)


def coverlet(path, kind="tick"):
    """The cloth that coverlet_sim.py dropped on the mattress: its game mesh and the sheet's UV."""
    data = np.load(path)
    return _object("coverlet", kind, [b(*p) for p in data["vertices"]], data["faces"].tolist(),
                   [[tuple(c) for c in face] for face in data["uv"]])


def build(own=None, source=False):
    """Every part as its own object with a UVMap and its "kind" (cloth, wood, band, tick). With own["coverlet"]
    (coverlet_sim.py's npz) the coverlet lies on the bed instead of the bare mattress it hides; with own["curtains"]
    (curtain_sim.py's folder) the side curtains are the simulated ones, at full density when source (gen_prop's bake
    source) and thinned for the game otherwise."""
    own = own or {}
    bed = coverlet(own["coverlet"]) if own.get("coverlet") else mattress()
    parts = [top(), valance(), back(), bed]
    for s in (-1, 1):
        if own.get("curtains"):
            parts += [hung(s, f"{own['curtains']}/curtain{s:+d}.npz", source), band(f"tie{s:+d}", s)]
        else:
            parts += [curtain(s), tail(s), band(f"tie{s:+d}", s)]
        lo, hi = sorted((s * 0.42, s * 0.57))
        parts.append(block(f"post{s:+d}", (-1.29, -1.13), (0.0, 2.11), (lo, hi)))
        lo, hi = sorted((s * 0.49, s * 0.57))
        parts.append(block(f"rail{s:+d}", (-1.13, -0.29), (2.03, 2.11), (lo, hi)))
    parts.append(block("rail-back", (-1.29, -1.20), (2.03, 2.11), (-0.42, 0.42)))
    parts.append(block("rail-front", (-0.37, -0.29), (2.04, 2.11), (-0.49, 0.49)))
    return parts


# --- materials for the bake: the colour, the relief and the zone masks ---------------------------------------------

def _node(nt, kind, **inputs):
    node = nt.nodes.new(kind)
    for key, value in inputs.items():
        if hasattr(node, key):
            setattr(node, key, value)
        else:
            node.inputs[key].default_value = value
    return node


def _noise(nt, coord, scale, stretch=(1, 1, 1), detail=3.0, space="Object"):
    mapping = _node(nt, "ShaderNodeMapping")
    mapping.inputs["Scale"].default_value = [scale * k for k in stretch]
    nt.links.new(coord.outputs[space], mapping.inputs["Vector"])
    noise = _node(nt, "ShaderNodeTexNoise", Scale=1.0, Detail=detail)
    nt.links.new(mapping.outputs[0], noise.inputs["Vector"])
    return noise.outputs["Fac"]


def _math(nt, operation, *values):
    node = _node(nt, "ShaderNodeMath", operation=operation)
    for k, v in enumerate(values):
        if isinstance(v, (int, float)):
            node.inputs[k].default_value = v
        else:
            nt.links.new(v, node.inputs[k])
    return node.outputs[0]


def _lines(nt, value, period, offset, width):
    """1 on lines every period (value in metres), fading to 0 at width: a crease where the linen lay folded."""
    d = _math(nt, "ABSOLUTE", _math(nt, "WRAP", _math(nt, "ADD", value, offset), period / 2, -period / 2))
    return _range(nt, d, 1.0, 0.0, 0.0, width)


def _linen(nt, coord):
    """The linen's own marks, as factors 0..1, in UV metres along the cloth (u across, v down it) and in place:
    slubs (thick threads) of the warp and the weft, the grain of the weave, streaks of uneven bleaching along the warp,
    the creases of storage in a chest, the hem with its stitch, the hands at the tie, dust on what faces up."""
    uv = _node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(coord.outputs["UV"], uv.inputs[0])
    f = {"slub": _math(nt, "MAXIMUM", _range(nt, _noise(nt, coord, 1.0, (520, 16, 1), 1.0, "UV"), 0, 1, 0.6, 0.68),
                       _range(nt, _noise(nt, coord, 1.0, (16, 520, 1), 1.0, "UV"), 0, 1, 0.6, 0.68)),
         "grain": _range(nt, _noise(nt, coord, 1.0, (380, 380, 1), 2.0, "UV"), 0, 1, 0.42, 0.58),
         # thread by thread the yarn is thicker or thinner: fine lines along the warp and across the weft
         "warp": _range(nt, _noise(nt, coord, 1.0, (650, 4, 1), 1.0, "UV"), 0, 1, 0.4, 0.6),
         "weft": _range(nt, _noise(nt, coord, 1.0, (4, 650, 1), 1.0, "UV"), 0, 1, 0.4, 0.6),
         "streak": _range(nt, _noise(nt, coord, 1.0, (14, 0.7, 1), 2.0, "UV"), 0, 1, 0.3, 0.7),
         "crease": _math(nt, "MAXIMUM", _lines(nt, uv.outputs["X"], 0.31, 0.05, 0.005),
                         _lines(nt, uv.outputs["Y"], 0.37, 0.11, 0.005))}
    attr = _node(nt, "ShaderNodeAttribute", attribute_name="edge")
    edge = _node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(attr.outputs["Vector"], edge.inputs[0])
    e = _math(nt, "MINIMUM", edge.outputs["X"], edge.outputs["Y"])
    f["hem"] = _range(nt, e, 1.0, 0.0, 0.022, 0.027)
    f["stitch"] = _range(nt, _math(nt, "ABSOLUTE", _math(nt, "SUBTRACT", e, 0.017)), 1.0, 0.0, 0.0005, 0.0014)
    obj = _node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(coord.outputs["Object"], obj.inputs[0])
    tie = _node(nt, "ShaderNodeCombineXYZ")
    nt.links.new(_math(nt, "ADD", obj.outputs["X"], 1.17), tie.inputs["X"])  # Blender X = glTF x
    nt.links.new(_math(nt, "SUBTRACT", _math(nt, "ABSOLUTE", obj.outputs["Y"]), 0.60), tie.inputs["Y"])
    nt.links.new(_math(nt, "SUBTRACT", obj.outputs["Z"], TIE_Y), tie.inputs["Z"])
    length = _node(nt, "ShaderNodeVectorMath", operation="LENGTH")
    nt.links.new(tie.outputs[0], length.inputs[0])
    f["hands"] = _range(nt, length.outputs["Value"], 1.0, 0.0, 0.05, 0.25)
    geo = _node(nt, "ShaderNodeNewGeometry")
    up = _node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(geo.outputs["Normal"], up.inputs[0])
    f["dust"] = _range(nt, up.outputs["Z"], 0.0, 1.0, 0.6, 0.95)
    return f


def _rgba(sockets, name):
    """The colour socket of a Mix node (it has a float, a vector and a colour one of each name)."""
    return next(k for k in sockets if k.name == name and k.type == "RGBA")


def _range(nt, value, lo, hi, a=0.0, c=1.0):
    node = _node(nt, "ShaderNodeMapRange", clamp=True)
    node.inputs["From Min"].default_value, node.inputs["From Max"].default_value = a, c
    node.inputs["To Min"].default_value, node.inputs["To Max"].default_value = lo, hi
    nt.links.new(value, node.inputs["Value"])
    return node.outputs["Result"]


def _mul(nt, *values):
    out = values[0]
    for v in values[1:]:
        m = _node(nt, "ShaderNodeMath", operation="MULTIPLY")
        nt.links.new(out, m.inputs[0])
        nt.links.new(v, m.inputs[1])
        out = m.outputs[0]
    return out


def material(kind, mode, wood):
    """mode albedo: emission of the colour (shade without a direction only: dirt low down, the cloth's own folds);
    normal: a Principled surface with the relief as bump; a mask name: white where kind is that zone."""
    mat = bpy.data.materials.new(f"own-{kind}-{mode}")
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = _node(nt, "ShaderNodeOutputMaterial")
    coord = _node(nt, "ShaderNodeTexCoord")
    xyz = _node(nt, "ShaderNodeSeparateXYZ")
    nt.links.new(coord.outputs["Object"], xyz.inputs[0])
    if mode not in ("albedo", "normal"):
        emit = _node(nt, "ShaderNodeEmission", Color=(1, 1, 1, 1) if kind == mode else (0, 0, 0, 1))
        nt.links.new(emit.outputs[0], out.inputs[0])
        return mat
    if mode == "albedo":
        if kind == "cloth":
            ao = _node(nt, "ShaderNodeAmbientOcclusion", only_local=True, Distance=0.12)
            f = _linen(nt, coord)
            shade = [_range(nt, _noise(nt, coord, 3.0, detail=4.0), 0.91, 1.09),
                     _range(nt, f["streak"], 0.94, 1.06), _range(nt, f["grain"], 0.94, 1.06),
                     _range(nt, f["warp"], 0.92, 1.08), _range(nt, f["weft"], 0.93, 1.07),
                     _range(nt, f["slub"], 1.0, 1.12), _range(nt, f["crease"], 1.0, 1.05),
                     _range(nt, f["hem"], 1.0, 0.88), _range(nt, f["stitch"], 1.0, 0.72),
                     _range(nt, f["hands"], 1.0, 0.86), _range(nt, f["dust"], 1.0, 0.88),
                     _range(nt, xyz.outputs["Z"], 0.80, 1.0, 0.0, 0.35),  # dust and wear at the floor
                     _range(nt, ao.outputs["AO"], 0.6, 1.0)]
            # unbleached linen is not one colour: warmer where the flax was, greyer where it was bleached in the sun
            tint = _node(nt, "ShaderNodeMix", data_type="RGBA")
            _rgba(tint.inputs, "A").default_value = (LINEN[0] * 1.05, LINEN[1], LINEN[2] * 0.88, 1)
            _rgba(tint.inputs, "B").default_value = (LINEN[0] * 0.95, LINEN[1] * 0.99, LINEN[2] * 1.10, 1)
            nt.links.new(_range(nt, _noise(nt, coord, 1.6, detail=2.0), 0, 1, 0.38, 0.62), tint.inputs["Factor"])
            base = None
            mix = tint
        elif kind == "wood":
            shade = [_range(nt, _noise(nt, coord, 1.0, (40, 40, 2)), 0.85, 1.12)]
            base = wood
        else:
            z = xyz.outputs["Z"]
            stripes = []
            for centre in (TIE_Y - 0.03, TIE_Y + 0.03):
                d = _node(nt, "ShaderNodeMath", operation="SUBTRACT")
                nt.links.new(z, d.inputs[0])
                d.inputs[1].default_value = centre
                a = _node(nt, "ShaderNodeMath", operation="ABSOLUTE")
                nt.links.new(d.outputs[0], a.inputs[0])
                stripes.append(_range(nt, a.outputs[0], 1.0, 0.0, 0.003, 0.006))
            mx = _node(nt, "ShaderNodeMath", operation="MAXIMUM")
            nt.links.new(stripes[0], mx.inputs[0])
            nt.links.new(stripes[1], mx.inputs[1])
            mix = _node(nt, "ShaderNodeMix", data_type="RGBA")
            _rgba(mix.inputs, "A").default_value, _rgba(mix.inputs, "B").default_value = (*BAND, 1), (*STRIPE, 1)
            nt.links.new(mx.outputs[0], mix.inputs["Factor"])
            shade = [_range(nt, _noise(nt, coord, 60.0), 0.9, 1.1)]
            base = None
        factor = _mul(nt, *shade)
        colour = _node(nt, "ShaderNodeMix", data_type="RGBA", blend_type="MULTIPLY")
        colour.inputs["Factor"].default_value = 1.0
        if base is None:
            nt.links.new(_rgba(mix.outputs, "Result"), _rgba(colour.inputs, "A"))
        else:
            _rgba(colour.inputs, "A").default_value = (*base, 1)
        nt.links.new(factor, _rgba(colour.inputs, "B"))
        emit = _node(nt, "ShaderNodeEmission")
        nt.links.new(_rgba(colour.outputs, "Result"), emit.inputs["Color"])
        nt.links.new(emit.outputs[0], out.inputs[0])
        return mat
    # relief: linen slubs along the warp and a soft crumple; wood grain along the bar; the band's woven ribs
    if kind == "cloth":
        f = _linen(nt, coord)
        height = _math(nt, "ADD", _math(nt, "ADD", _math(nt, "MULTIPLY", f["slub"], 0.7), _math(nt, "ADD", _math(nt, "MULTIPLY", f["grain"], 0.35),
                                                                                      _math(nt, "MULTIPLY", _math(nt, "ADD", f["warp"], f["weft"]), 0.3))),
                       _math(nt, "ADD", _math(nt, "MULTIPLY", f["crease"], 0.8),
                             _math(nt, "SUBTRACT", _math(nt, "MULTIPLY", f["hem"], 0.6), _math(nt, "MULTIPLY", f["stitch"], 0.5))))
        strength, distance = 0.3, 0.0015
    elif kind == "wood":
        height, strength, distance = _noise(nt, coord, 1.0, (60, 60, 3)), 0.3, 0.002
    else:
        wave = _node(nt, "ShaderNodeTexWave", wave_type="BANDS", bands_direction="Z", Scale=120.0)
        nt.links.new(coord.outputs["Object"], wave.inputs["Vector"])
        height, strength, distance = wave.outputs["Fac"], 0.3, 0.001
    bump = _node(nt, "ShaderNodeBump", Strength=strength, Distance=distance)
    nt.links.new(height, bump.inputs["Height"])
    bsdf = _node(nt, "ShaderNodeBsdfPrincipled")
    nt.links.new(bump.outputs[0], bsdf.inputs["Normal"])
    nt.links.new(bsdf.outputs[0], out.inputs[0])
    return mat
