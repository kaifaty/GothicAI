"""Debris of the barrel built by barrel.py: its staves, head boards and hoops as closed solids.

barrel.py calls build() after it has exported the whole barrel, when its config has a "debris" key:
    "debris": {"seed": 7, "stave_thickness": 0.025, "joint_gap": 0.0005, "head_thickness": 0.025,
               "broken": 10, "break_columns": 14, "board_gap": 0.001, "clearance": 0.0003, "end_scale": 2.0,
               "zones": {"INNER": [KEY, GROUP], "END": [KEY, GROUP], "RUST": [KEY, GROUP]}}
It writes model.debris.glb next to model.glb: one glTF node per piece (stave-07, stave-07-top, lid-2,
bottom-2, hoop-3), in the pose of the whole barrel, whose outer surface is the whole's within 1 mm
(reports/content/2026-11-wood-fracture/README.md). The whole has no staves: its body is a one-sided
shell. So the pieces are rebuilt here from the same angles, levels, profile and seed:
- a stave runs from the ground to its own crown top, its outer face the whole's facet, its inner face
  "stave_thickness" in; below the lid it stands "joint_gap" from its neighbours (the whole's facets meet
  there), its crown as far as the whole's crown blocks;
- "broken" staves, picked by the debris seed, break across the open band between two hoops with a
  ragged edge: teeth across the width, the inner fibres torn at other heights than the outer, one or two
  splinters on the outer face; the halves share the break surface;
- the lid and the bottom are cut into the whole's planks; each board is a prism of the staves' inner
  polygon at the whole's head face, so it touches the staves there and never overlaps them;
- a hoop is a closed ring: the whole's skin and lips plus an inner face lying on the staves, with the
  seam plate and rivets (barrel.py's seam_plate) as small closed parts of the same piece.
Faces the whole shows keep its zones and tile UVs (STAVE, HEAD, BAND), so the baked debris reads as the
whole barrel. Faces the whole does not have get their own zones: "INNER" (stave insides, joints, the
bands under the hoops, board backs and edges, stave feet), "END" (the fresh break, end grain from a log
end tile, "end_scale" times its metres) and "RUST" (the hoops' inner side). Every vertex carries "_part":
the stave index, the whole's sector count plus the plank for the boards, -1 for the hoops (the baker then
tones them by sector, as the whole).
"""
import math

import bmesh
import bpy
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

STAVE, HEAD, BAND, INNER, END, RUST = range(6)


def build(B, spec, out):
    """B: barrel.py's globals after the whole barrel is built; returns the debris metrics."""
    rng = np.random.default_rng(spec.get("seed", 7))
    t = spec.get("stave_thickness", B.rim)
    th = spec.get("head_thickness", 0.025)
    clear = spec.get("clearance", 0.0003)
    sides, angles, levels, r_at = B.sides, B.angles, B.levels, B.r_at
    cx, cy, base_z, top_z = B.cx, B.cy, B.base_z, B.top_z
    slope = B.cfg.get("hoop_slope", 0.02)
    end_scale = spec.get("end_scale", 2.0)
    joint = spec.get("joint_gap", 0.0005)

    def stave_frame(i):
        """Per level: the stave's two edge angles, its outer radius and its tile V. Below the lid the
        staves stand "joint_gap" apart, at the crown top as far as the whole's crown blocks."""
        a_lo = angles[i]
        a_hi = angles[i + 1] if i + 1 < sides else angles[0] + 2 * math.pi
        zs = list(levels) + [float(B.tops[i])]             # levels[-1] is the lid; the crown top above it
        radii = [r_at(z) for z in zs]
        edges = [(a_lo + joint / 2 / r, a_hi - joint / 2 / r) for r in radii[:-1]]
        edges.append((a_lo + B.gap / 2 / B.r_low, a_hi - B.gap / 2 / B.r_low))
        arcs = list(B.arc) + [B.arc[-1] + float(B.tops[i]) - top_z]
        return edges, zs, radii, arcs

    def corner(radius, ang, z):
        return Vector((cx + radius * math.cos(ang), cy + radius * math.sin(ang), base_z + z))

    # --- one stave or one half of a broken stave ---
    def stave_piece(bm, uvl, i, lo, hi):
        """lo, hi: (outer heights, inner heights) per column across the stave; flat ends repeat one z."""
        edges, zs, radii, arcs = stave_frame(i)
        nu = len(lo[0]) - 1
        us = np.linspace(0.0, 1.0, nu + 1)
        u0, u1 = B.plank[i] * B.plank_u, (B.plank[i] + 1) * B.plank_u
        if B.flip[i]:
            u0, u1 = u1, u0
        voff = float(B.v_off[i])
        width = (edges[0][1] - edges[0][0]) * B.r_low

        def point(u, z, w):
            j = min(max(int(np.searchsorted(zs, z, side="right")) - 1, 0), len(zs) - 2)
            s = (z - zs[j]) / (zs[j + 1] - zs[j])
            row = [corner(radii[jj] - w * t, edges[jj][0], zs[jj]).lerp(corner(radii[jj] - w * t, edges[jj][1], zs[jj]), u)
                   for jj in (j, j + 1)]
            return row[0].lerp(row[1], s)

        flat_lo = len(set(np.round(np.r_[lo[0], lo[1]], 9))) == 1
        flat_hi = len(set(np.round(np.r_[hi[0], hi[1]], 9))) == 1
        # A half of a broken stave needs its columns across the width only near the break: past the first
        # level beyond it the inner columns stop and the stave is one facet wide again, as the whole.
        cut = None
        if nu > 1 and flat_lo != flat_hi:
            if flat_lo:
                cut = max(z for z in zs if z < float(np.min(hi)) - 0.003)
            else:
                cut = min(z for z in zs if z > float(np.max(lo)) + 0.003)
            if not lo[0][0] + 0.003 < cut < hi[0][0] - 0.003:
                cut = None
        param = {}
        cols = [[None] * (nu + 1) for _ in range(2)]
        for w in (0, 1):
            for k in range(nu + 1):
                z_lo, z_hi = float(lo[w][k]), float(hi[w][k])
                if cut is not None and 0 < k < nu:
                    z_lo, z_hi = (cut, z_hi) if flat_lo else (z_lo, cut)
                column = [z_lo] + [z for z in zs if z_lo + 0.003 < z < z_hi - 0.003] + [z_hi]
                verts = []
                for z in column:
                    v = bm.verts.new(point(us[k], z, w))
                    param[v] = (us[k], z, w)
                    verts.append(v)
                cols[w][k] = verts

        z_break = float(np.mean(hi if not flat_hi else lo))

        def U(u):
            return u0 + (u1 - u0) * u

        # The whole barrel's staves share vertices, so it shades round; a lone stave would shade as a flat
        # facet. Its outer and inner faces below the lid take the whole's smooth normal instead: radial at
        # the whole's stave angle, leaning with the profile; the crown, joints and ends stay flat.
        a_lo = angles[i]
        a_hi = angles[i + 1] if i + 1 < sides else angles[0] + 2 * math.pi
        band_slope = np.diff(radii[:-1]) / np.diff(zs[:-1])
        level_slope = np.r_[band_slope[0], (band_slope[:-1] + band_slope[1:]) / 2, band_slope[-1]]

        def smooth_normal(u, z, w):
            phi = a_lo + (a_hi - a_lo) * u
            n = Vector((math.cos(phi), math.sin(phi), -float(np.interp(z, zs[:-1], level_slope)))).normalized()
            return -n if w else n

        def face(verts, kind):
            zc = sum(param[v][1] for v in verts) / len(verts)
            if kind == "outer":
                hidden = any(a - 1e-6 <= zc <= b + 1e-6 for a, b in B.hoops)
                zone = INNER if hidden else STAVE
            elif kind in ("inner", "side"):
                zone = STAVE if zc > top_z else INNER    # the crown above the lid is the whole's
            elif kind == "top":
                zone = STAVE
            elif kind == "foot":
                zone = INNER
            else:
                zone = END
            uvs = []
            for v in verts:
                u, z, w = param[v]
                if kind in ("outer", "inner"):
                    uvs.append((U(u), float(np.interp(z, zs, arcs)) + voff))
                elif kind == "side":
                    inward = (1 if u1 > u0 else -1) * (1 if u < 0.5 else -1)
                    uvs.append((U(u) + inward * w * t, float(np.interp(z, zs, arcs)) + voff))
                elif kind == "top":
                    uvs.append((U(u), arcs[-1] + voff + w * t))
                elif kind == "foot":
                    uvs.append((U(u), voff - w * t))
                else:  # end grain: rings run across the width, stacked through the thickness
                    uvs.append((0.5 / end_scale + (i % 5) * 0.03 + u * width,
                                0.86 / end_scale + w * t + (z - z_break)))
            f = bm.faces.new(verts)
            f.material_index = zone
            round_ = kind in ("outer", "inner") and zc < top_z
            f[flat_layer] = 0 if round_ else 1
            for loop, coord in zip(f.loops, uvs):
                loop[uvl].uv = coord
                if round_:
                    loop[normal_layer] = smooth_normal(*param[loop.vert])

        normal_layer = bm.loops.layers.float_vector.new("_cn")
        flat_layer = bm.faces.layers.int.new("_flat")

        def zipper(a, b, kind):
            """Triangles between two vertex columns running upwards from one end to the other."""
            p, q = 0, 0
            while p < len(a) - 1 or q < len(b) - 1:
                if q == len(b) - 1 or (p < len(a) - 1 and param[a[p + 1]][1] <= param[b[q + 1]][1]):
                    face((a[p], a[p + 1], b[q]), kind)
                    p += 1
                else:
                    face((a[p], b[q + 1], b[q]), kind)
                    q += 1

        def at(col, z):
            return next(n for n, v in enumerate(col) if abs(param[v][1] - z) < 1e-9)

        for w, kind in ((0, "outer"), (1, "inner")):
            col = cols[w]
            if cut is None:
                narrow = col
            else:  # one facet-wide strip away from the break, one flat face where the columns stop
                e0, en = at(col[0], cut), at(col[nu], cut)
                inner_cols = col[1:nu]
                if flat_lo:
                    zipper(col[nu][:en], col[0][:e0], kind)
                    face([col[0][e0 - 1], col[nu][en - 1], col[nu][en]] + [c[0] for c in inner_cols[::-1]]
                         + [col[0][e0]], kind)
                    narrow = [col[0][e0:]] + inner_cols + [col[nu][en:]]
                else:
                    zipper(col[nu][en + 1:], col[0][e0 + 1:], kind)
                    face([col[0][e0]] + [c[-1] for c in inner_cols] + [col[nu][en], col[nu][en + 1],
                         col[0][e0 + 1]], kind)
                    narrow = [col[0][:e0 + 1]] + inner_cols + [col[nu][:en + 1]]
            for k in range(nu):
                zipper(narrow[k + 1], narrow[k], kind)
        zipper(cols[1][0], cols[0][0], "side")
        zipper(cols[0][nu], cols[1][nu], "side")
        ends = [(k, k + 1) for k in range(nu)]
        for k0, k1 in (ends if cut is None or not flat_lo else [(0, nu)]):
            face((cols[0][k0][0], cols[0][k1][0], cols[1][k1][0], cols[1][k0][0]), "foot" if flat_lo else "break")
        for k0, k1 in (ends if cut is None or not flat_hi else [(0, nu)]):
            face((cols[0][k0][-1], cols[1][k0][-1], cols[1][k1][-1], cols[0][k1][-1]), "top" if flat_hi else "break")

    # --- broken staves and their ragged break ---
    open_bands = [(B.hoops[k][1], B.hoops[k + 1][0]) for k in range(len(B.hoops) - 1)]
    band = max(open_bands, key=lambda g: g[1] - g[0])
    broken = sorted(int(i) for i in rng.choice(sides, spec.get("broken", 3), replace=False))
    nu = spec.get("break_columns", 14)
    us = np.linspace(0.0, 1.0, nu + 1)
    breaks = {}
    for i in broken:
        z_b = rng.uniform(band[0] + 0.09, band[1] - 0.09)
        outer = z_b + rng.uniform(-0.05, 0.05) * (us - 0.5) + rng.normal(0.0, 0.008, nu + 1)
        for _ in range(int(rng.integers(1, 3))):  # splinters of the outer face, on one half or the other
            k = int(rng.integers(1, nu))
            outer[k] += rng.choice([-1.0, 1.0]) * rng.uniform(0.035, 0.06)
        inner = outer + rng.uniform(-0.02, 0.02) + rng.normal(0.0, 0.006, nu + 1)
        lo_z, hi_z = band[0] + 0.01, band[1] - 0.01
        breaks[i] = (np.clip(outer, lo_z, hi_z), np.clip(inner, lo_z, hi_z))

    # --- pieces ---
    pieces = []  # (name, kind, bmesh)

    def new_piece(name, kind, part):
        bm = bmesh.new()
        uvl = B.use_shell(bm)                # barrel.py's vert/ring/quad/box/rivet now draw into it
        pieces.append((name, kind, bm, part))
        return bm, uvl

    for i in range(sides):
        top = float(B.tops[i])
        if i in breaks:
            outer, inner = breaks[i]
            bm, uvl = new_piece(f"stave-{i + 1:02d}", "stave-half", i)
            stave_piece(bm, uvl, i, (np.zeros(nu + 1), np.zeros(nu + 1)), (outer, inner))
            bm, uvl = new_piece(f"stave-{i + 1:02d}-top", "stave-half", i)
            stave_piece(bm, uvl, i, (outer, inner), (np.full(nu + 1, top), np.full(nu + 1, top)))
        else:
            bm, uvl = new_piece(f"stave-{i + 1:02d}", "stave", i)
            stave_piece(bm, uvl, i, (np.zeros(2), np.zeros(2)), (np.full(2, top), np.full(2, top)))

    def inner_polygon(level):
        pts = []
        for i in range(sides):
            edges, _zs, radii, _ = stave_frame(i)
            radius = radii[level] - t - clear
            pts += [(cx + radius * math.cos(a), cy + radius * math.sin(a)) for a in edges[level]]
        return pts

    def clip(poly, x0, keep):  # keep: +1 keeps x >= x0, -1 keeps x <= x0
        res = []
        for p, q in zip(poly, poly[1:] + poly[:1]):
            pin, qin = keep * (p[0] - x0) >= 0, keep * (q[0] - x0) >= 0
            if pin:
                res.append(p)
            if pin != qin:
                s = (x0 - p[0]) / (q[0] - p[0])
                res.append((x0, p[1] + s * (q[1] - p[1])))
        return res

    g = spec.get("board_gap", 0.001)
    n_planks, pw = B.n_planks, B.plank_width
    for head, level, z0, z1 in (("lid", len(levels) - 1, top_z - th, top_z), ("bottom", 0, 0.0, th)):
        poly = inner_polygon(level)
        for p in range(n_planks):
            left = cx - B.r_top + p * pw
            shape = poly
            if p > 0:
                shape = clip(shape, left + g / 2, +1)
            if p < n_planks - 1:
                shape = clip(shape, left + pw - g / 2, -1)
            bm, uvl = new_piece(f"{head}-{p + 1}", "board", sides + p)
            low = [bm.verts.new((x, y, base_z + z0)) for x, y in shape]
            high = [bm.verts.new((x, y, base_z + z1)) for x, y in shape]
            u_left = B.lid_plank[p] * B.plank_u
            off = float(B.lid_off[p])

            def plank_uv(v, u_left=u_left, left=left, off=off):
                return (u_left + (v.co.x - left) / pw * B.plank_u, v.co.y - cy + off)

            def put(verts, zone, uvs, bm=bm, uvl=uvl):
                f = bm.faces.new(verts)
                f.material_index = zone
                for loop, coord in zip(f.loops, uvs):
                    loop[uvl].uv = coord

            outer_face = high if head == "lid" else low
            for ring_, up in ((high, True), (low, False)):
                verts = ring_ if up else ring_[::-1]
                if ring_ is outer_face and head == "bottom":
                    uvs = [(v.co.x - cx, v.co.y - cy) for v in verts]       # the whole's bottom mapping
                else:
                    uvs = [plank_uv(v) for v in verts]
                put(verts, HEAD if ring_ is outer_face else INNER, uvs)
            n = len(shape)
            for m in range(n):
                quad_ = (low[m], low[(m + 1) % n], high[(m + 1) % n], high[m])
                put(quad_, INNER, [(u_left + 0.3 * B.plank_u + 0.5 * (v.co.z - base_z - z0),
                                    v.co.y - cy + 0.3 * v.co.x + off) for v in quad_])

    t_hoop = B.t_hoop
    for h, (a, b) in enumerate(B.hoops):
        bm, uvl = new_piece(f"hoop-{h + 1}", "hoop", -1)
        idx = [j for j, z in enumerate(levels) if a - 1e-6 <= z <= b + 1e-6]
        skin = [levels[j] - (slope if j == idx[-1] else 0.0) for j in idx]
        inner = [B.ring(levels[j], r_at(levels[j])) for j in idx]
        outer = [B.ring(z, r_at(z) + t_hoop) for z in skin]
        r_mid = r_at((a + b) / 2) + t_hoop
        for i in range(sides):
            k = (i + 1) % sides
            u0, u1 = angles[i] * r_mid, (angles[i] + 2 * math.pi / sides) * r_mid
            for m in range(len(idx) - 1):    # the whole's skin
                z0, z1 = skin[m], skin[m + 1]
                B.quad((outer[m][i], outer[m][k], outer[m + 1][k], outer[m + 1][i]), BAND,
                       ((u0, z0), (u1, z0), (u1, z1), (u0, z1)))
                y0, y1 = levels[idx[m]], levels[idx[m + 1]]  # the inner face, on the staves
                B.quad((inner[m + 1][i], inner[m + 1][k], inner[m][k], inner[m][i]), RUST,
                       ((u0, y1), (u1, y1), (u1, y0), (u0, y0)))
            for m, j in ((0, idx[0]), (len(idx) - 1, idx[-1])):   # the whole's lips
                z, dz = skin[m], abs(levels[j] - skin[m]) + t_hoop
                B.quad((inner[m][i], inner[m][k], outer[m][k], outer[m][i]), BAND,
                       ((u0, z), (u1, z), (u1, z + dz), (u0, z + dz)))
        B.seam_plate(a, b)

    # --- meshes, checks, export ---
    keys = [next(z["key"] for z in B.goal["zones"] if z["key"].endswith(s)) for s in ("STAVE", "HEAD", "BAND")]
    groups = [next(z["group"] for z in B.goal["zones"] if z["key"].endswith(s)) for s in ("STAVE", "HEAD", "BAND")]
    for name in ("INNER", "END", "RUST"):
        keys.append(spec["zones"][name][0])
        groups.append(spec["zones"][name][1])
    materials = []
    for key, group in zip(keys, groups):
        mat = bpy.data.materials.get(key) or bpy.data.materials.new(key)
        mat["group"] = group
        materials.append(mat)

    objects, open_edges, inside_out, volumes = [], 0, 0, {}
    for name, kind, bm, part in pieces:
        layer = bm.verts.layers.float.new("_part")
        for v in bm.verts:
            v[layer] = part
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
        bmesh.ops.triangulate(bm, faces=bm.faces)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        open_edges += sum(1 for e in bm.edges if not e.is_manifold)
        normal_layer, flat_layer = bm.loops.layers.float_vector.get("_cn"), bm.faces.layers.int.get("_flat")
        if flat_layer is not None:  # a stave: the faces not shaded round keep their own normal
            for f in bm.faces:
                if f[flat_layer]:
                    for loop in f.loops:
                        loop[normal_layer] = f.normal
        # signed volume per closed shell: a plate or rivet sits on its hoop as its own shell
        seen, total = set(), 0.0
        for start in bm.faces:
            if start.index in seen:
                continue
            stack, comp = [start], []
            seen.add(start.index)
            while stack:
                f = stack.pop()
                comp.append(f)
                for e in f.edges:
                    for o in e.link_faces:
                        if o.index not in seen:
                            seen.add(o.index)
                            stack.append(o)
            vol = sum(f.verts[0].co.dot(f.verts[1].co.cross(f.verts[2].co)) / 6 for f in comp)
            inside_out += vol < 0
            total += vol
        volumes[name] = total
        mesh = bpy.data.meshes.new(name)
        bm.to_mesh(mesh)
        bm.free()
        mesh.validate()
        mesh.update()
        for mat in materials:
            mesh.materials.append(mat)
        mesh.shade_smooth()
        mesh.set_sharp_from_angle(angle=math.radians(30))
        if "_cn" in mesh.attributes:
            custom = np.zeros(len(mesh.loops) * 3, np.float32)
            mesh.attributes["_cn"].data.foreach_get("vector", custom)
            mesh.normals_split_custom_set(custom.reshape(-1, 3).tolist())
            mesh.attributes.remove(mesh.attributes["_cn"])
            mesh.attributes.remove(mesh.attributes["_flat"])
        o = bpy.data.objects.new(name, mesh)
        bpy.context.scene.collection.objects.link(o)
        objects.append(o)
    B.use_shell(bmesh.new())

    for o in list(bpy.context.scene.objects):
        o.select_set(o in objects)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.gltf(filepath=str(out / "model.debris.glb"), export_format="GLB", use_selection=True,
                              export_apply=True, export_yup=True, export_extras=True, export_normals=True,
                              export_texcoords=True, export_animations=False, export_attributes=True)

    checks = fit_checks(B.obj, objects)
    kinds = [kind for _, kind, _, _ in pieces]
    wood = {n: v for (n, k, _, _), v in zip(pieces, volumes.values()) if k != "hoop"}
    return {"pieces": len(pieces), "staves": kinds.count("stave") + kinds.count("stave-half"),
            "broken_staves": [f"stave-{i + 1:02d}" for i in broken],
            "break_heights_m": [round(float(np.mean(breaks[i][0])), 3) for i in broken],
            "boards": kinds.count("board"), "hoops": kinds.count("hoop"),
            "triangles": sum(len(o.data.polygons) for o in objects),
            "stave_thickness_m": t, "head_thickness_m": th,
            "open_edges": open_edges, "inside_out_shells": int(inside_out),
            "largest_wood_piece_l": round(max(wood.values()) * 1000, 3),
            "volume_total_l": round(sum(volumes.values()) * 1000, 2), **checks}


def fit_checks(whole, objects):
    """How the pieces sit: overlap between them and their outer surface against the whole barrel."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    trees = [BVHTree.FromObject(o, depsgraph) for o in objects]
    samples = []
    for o in objects:
        mesh = o.data
        pts = [v.co.copy() for v in mesh.vertices] + [p.center.copy() for p in mesh.polygons]
        pts += [(mesh.vertices[e.vertices[0]].co + mesh.vertices[e.vertices[1]].co) / 2 for e in mesh.edges]
        samples.append(pts)
    bounds = [(Vector(np.min([v.co for v in o.data.vertices], axis=0)), Vector(np.max([v.co for v in o.data.vertices], axis=0)))
              for o in objects]

    def inside(tree, p):
        """Ray parity along a skew direction: odd hits = inside the closed piece."""
        d = Vector((0.31, 0.17, 0.93)).normalized()
        hits, origin = 0, p.copy()
        for _ in range(64):
            loc, _n, _i, _dist = tree.ray_cast(origin, d)
            if loc is None:
                break
            hits += 1
            origin = loc + d * 1e-6
        return hits % 2 == 1

    worst, worst_pair = 0.0, None
    for a, (tree, (lo, hi)) in enumerate(zip(trees, bounds)):
        for b, pts in enumerate(samples):
            if a == b:
                continue
            for p in pts:
                if not all(lo[c] - 1e-3 <= p[c] <= hi[c] + 1e-3 for c in range(3)):
                    continue
                loc, normal, _i, dist = tree.find_nearest(p)
                if loc is None or dist <= worst or (p - loc).dot(normal) >= 0 or not inside(tree, p):
                    continue
                worst, worst_pair = dist, (objects[b].name, objects[a].name)

    whole_tree = BVHTree.FromObject(whole, depsgraph)
    shown = {0, 1, 2}  # STAVE, HEAD, BAND: the faces the whole barrel shows
    out_dev = 0.0
    for o in objects:
        for poly in o.data.polygons:
            if poly.material_index not in shown:
                continue
            for p in [o.data.vertices[i].co for i in poly.vertices] + [poly.center]:
                out_dev = max(out_dev, whole_tree.find_nearest(p)[3])
    whole_dev = 0.0
    union = BVHTree.FromPolygons(
        [v.co.copy() for o in objects for v in o.data.vertices],
        [[i + base for i in poly.vertices] for o, base in zip(objects, np.cumsum([0] + [len(o.data.vertices) for o in objects]))
         for poly in o.data.polygons])
    for poly in whole.data.polygons:
        for p in [whole.data.vertices[i].co for i in poly.vertices] + [poly.center]:
            probe = p + poly.normal * 0.002   # a face the whole shows has open air in front of it
            if any(inside(tree, probe) for tree, (lo, hi) in zip(trees, bounds)
                   if all(lo[c] <= probe[c] <= hi[c] for c in range(3))):
                continue
            whole_dev = max(whole_dev, union.find_nearest(p)[3])
    return {"max_overlap_mm": round(worst * 1000, 3), "max_overlap_pair": worst_pair,
            "debris_to_whole_mm": round(out_dev * 1000, 3), "whole_to_debris_mm": round(whole_dev * 1000, 3)}
