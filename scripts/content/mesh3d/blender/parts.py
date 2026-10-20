"""Shared part builders for props made by parts (crate.py, table.py, bench.py).

Every part is a closed shell (nail heads excepted: their open base is sunk into
the wood) with its own "_part" number, so bake_atlas.py can tone each one. Tile
UVs are in metres with the grain along the part's length; a board's face takes
one plank of the wood tile.
"""
import json
import math
import sys
from pathlib import Path

import bmesh
import bpy
import numpy as np
from mathutils import Vector

X, Y, Z = Vector((1, 0, 0)), Vector((0, 1, 0)), Vector((0, 0, 1))


def load_goal(cfg):
    """The config's target, its zone keys and its bbox in Blender axes (glTF X, -Z, Y)."""
    if "goal" in cfg:  # a target measured outside the pilot set, written into the config
        goal = cfg["goal"]
    else:
        repo = Path(__file__).resolve().parents[4]
        targets = json.loads(Path(cfg.get("target", repo / "data/content/mesh3d-pilot.json")).read_text())
        targets = targets["targets"] if isinstance(targets, dict) else targets
        goal = next(t for t in targets if t["id"] == cfg["id"])
    if "zones" in cfg:  # the original's intent can differ from the pilot's guessed zones (stone, not wood)
        goal = {**goal, "zones": cfg["zones"]}
    gmin, gmax = np.array(goal["bbox_min"]), np.array(goal["bbox_max"])
    lo = Vector((gmin[0], -gmax[2], gmin[1]))
    hi = Vector((gmax[0], -gmin[2], gmax[1]))
    return goal, lo, hi


def args():
    rest = sys.argv[sys.argv.index("--") + 1:]
    cfg = json.loads(Path(rest[0]).read_text())
    out = Path(rest[1])
    out.mkdir(parents=True, exist_ok=True)
    return cfg, out


class Parts:
    def __init__(self, rng, plank_u):
        self.rng = rng
        self.plank_u = plank_u
        self.bm = bmesh.new()
        self.uv = self.bm.loops.layers.uv.new("UVMap")
        self.part_layer = self.bm.verts.layers.float.new("_part")
        self.parts = 0
        self.nails = 0

    def new_part(self):
        self.parts += 1
        return self.parts - 1

    def vert(self, co, part):
        v = self.bm.verts.new(co)
        v[self.part_layer] = part
        return v

    def poly(self, verts, material, uvs):
        face = self.bm.faces.new(verts)
        face.material_index = material
        for loop, coord in zip(face.loops, uvs):
            loop[self.uv].uv = coord
        return face

    def board(self, c, ax_len, ax_w, ax_n, length, width, thick, material, plank=None, v_off=0.0):
        """A closed box; grain along ax_len. The face along ax_n takes one plank of the tile."""
        part = self.new_part()
        corner = {}
        for sl in (-1, 1):
            for sw in (-1, 1):
                for sn in (-1, 1):
                    corner[sl, sw, sn] = self.vert(c + ax_len * (sl * length / 2) + ax_w * (sw * width / 2)
                                                   + ax_n * (sn * thick / 2), part)
        local = {v: (sl * length / 2, sw * width / 2, sn * thick / 2) for (sl, sw, sn), v in corner.items()}
        plank_u = self.plank_u

        def coords(v, axis):
            l, w, n = local[v]
            if axis == "n":  # the board's face: across = one plank of the tile, along = grain
                k = plank if plank is not None else 0
                return (k * plank_u + (w / width + 0.5) * plank_u, l + v_off)
            if axis == "w":
                return (n, l + v_off)
            return (w, n)

        for axis, index in (("l", 0), ("w", 1), ("n", 2)):
            for sign in (-1, 1):
                quad = [v for key, v in corner.items() if key[index] == sign]
                others = [i for i in range(3) if i != index]  # order the four corners around the face
                ring = sorted(quad, key=lambda v: math.atan2(local[v][others[1]], local[v][others[0]]))
                self.poly(ring, material, [coords(v, axis) for v in ring])

    def post(self, top, bottom, top_w, bottom_w, ax_a, ax_b, material, tile):
        """A tapered square post from top to bottom centre (a leg); grain along its length."""
        part = self.new_part()
        k = int(self.rng.integers(0, 4))
        v_off = self.rng.uniform(0, tile)
        length = (top - bottom).length
        ends = []
        for centre, w, v in ((top, top_w, length), (bottom, bottom_w, 0.0)):
            ends.append([self.vert(centre + ax_a * (sa * w / 2) + ax_b * (sb * w / 2), part)
                         for sa, sb in ((-1, -1), (1, -1), (1, 1), (-1, 1))])
        hi, lo = ends
        for i in range(4):
            j = (i + 1) % 4
            u0, u1 = k * self.plank_u, (k + 1) * self.plank_u
            self.poly((lo[i], lo[j], hi[j], hi[i]), material, ((u0, v_off), (u1, v_off), (u1, v_off + length), (u0, v_off + length)))
        self.poly(hi[::-1], material, [(0, 0), (top_w, 0), (top_w, top_w), (0, top_w)])
        self.poly(lo, material, [(0, 0), (bottom_w, 0), (bottom_w, bottom_w), (0, bottom_w)])

    def block(self, c, size, material, tile):
        """A closed axis-aligned box (a stone) with planar UVs in metres at a random spot of the tile."""
        part = self.new_part()
        ou, ov = self.rng.uniform(0, tile), self.rng.uniform(0, tile)
        h = Vector(size) / 2
        corner = {(sx, sy, sz): self.vert(c + Vector((sx * h.x, sy * h.y, sz * h.z)), part)
                  for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)}
        for index in range(3):
            others = [i for i in range(3) if i != index]
            for sign in (-1, 1):
                quad = [(key, v) for key, v in corner.items() if key[index] == sign]
                ring = sorted(quad, key=lambda kv: math.atan2(kv[0][others[1]], kv[0][others[0]]))
                uvs = [(ou + (v.co - c)[others[0]], ov + (v.co - c)[others[1]]) for _, v in ring]
                self.poly([v for _, v in ring], material, uvs)

    def wood(self, c, ax_len, ax_w, ax_n, length, width, thick, material, tile):
        """A board with a random plank of the tile and a random grain offset."""
        self.board(c, ax_len, ax_w, ax_n, length, width, thick, material,
                   int(self.rng.integers(0, 4)), self.rng.uniform(0, tile))

    def plate_l(self, c, ax_a, ax_b, ax_n, leg, width, thick, material):
        """Flat L-shaped plate in the corner at c, legs along +ax_a and +ax_b, raised along ax_n."""
        part = self.new_part()
        # Seven points: (0, width) splits the concave L into two convex faces for triangulation.
        outline = [(0, 0), (leg, 0), (leg, width), (width, width), (width, leg), (0, leg), (0, width)]
        low, top = ([self.vert(c + ax_a * a + ax_b * b + ax_n * d, part) for a, b in outline] for d in (0.0, thick))
        for idx in ((0, 1, 2, 3, 6), (6, 3, 4, 5)):
            self.poly([top[i] for i in idx], material, [outline[i] for i in idx])
            self.poly([low[i] for i in idx[::-1]], material, [outline[i] for i in idx[::-1]])
        n = len(outline)
        for i in range(n):
            j = (i + 1) % n
            self.poly((low[i], low[j], top[j], top[i]), material,
                      ((i * 0.02, 0), ((i + 1) * 0.02, 0), ((i + 1) * 0.02, thick), (i * 0.02, thick)))

    def strap(self, c, ax_len, ax_w, ax_n, length, width, thick, material):
        """A flat iron strap: a thin closed box lying on a surface at c, raised along ax_n."""
        self.board(c + ax_n * (thick / 2), ax_len, ax_w, ax_n, length, width, thick, material)

    def nail(self, c, ax_a, ax_b, ax_n, radius, height, material, sink=0.001):
        """A three-sided nail head sunk into the wood; its open base is hidden, so it costs 3 triangles."""
        part = self.new_part()
        self.nails += 1
        offset = self.rng.uniform(0, 2 * math.pi)
        base = [self.vert(c + (ax_a * math.cos(t) + ax_b * math.sin(t)) * radius - ax_n * sink, part)
                for t in (2 * math.pi * i / 3 + offset for i in range(3))]
        tip = self.vert(c + ax_n * height, part)
        for i in range(3):
            self.poly((base[i], base[(i + 1) % 3], tip), material, [(0, 0), (radius, 0), (radius / 2, height)])

    def to_object(self, goal, name):
        """Triangulate into a new object with zone materials and "_part"; returns (object, open edges)."""
        bm = self.bm
        bmesh.ops.triangulate(bm, faces=bm.faces)
        bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
        open_edges = sum(1 for e in bm.edges if not e.is_manifold)
        mesh = bpy.data.meshes.new(name)
        bm.to_mesh(mesh)
        bm.free()
        mesh.validate()
        mesh.update()
        for zone in goal["zones"]:
            mat = bpy.data.materials.get(zone["key"])
            if mat is None:
                mat = bpy.data.materials.new(zone["key"])
                mat["group"] = zone["group"]
            mesh.materials.append(mat)
        mesh.shade_smooth()
        mesh.set_sharp_from_angle(angle=math.radians(30))
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        return obj, open_edges

    def finish(self, goal, out, lo, hi, extra=None):
        """Triangulate, export model.glb with zone materials and "_part", write metrics.json."""
        return finish_nodes(goal, out, lo, hi, [(goal["id"], self)], extra)


def finish_nodes(goal, out, lo, hi, nodes, extra=None):
    """Export one GLB node per (name, Parts): a model with several attachments (a chest body and
    its lid) keeps each attachment's node name. Part numbers must not repeat across the nodes."""
    objects, open_edges, nails, parts = [], 0, 0, 0
    for name, P in nodes:
        obj, edges = P.to_object(goal, name)
        objects.append(obj)
        open_edges += edges
        nails += P.nails
        parts = max(parts, P.parts)
    for o in list(bpy.context.scene.objects):
        o.select_set(o in objects)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.export_scene.gltf(filepath=str(out / "model.glb"), export_format="GLB", use_selection=True,
                              export_apply=True, export_yup=True, export_extras=True, export_normals=True,
                              export_texcoords=True, export_animations=False, export_attributes=True)
    corners = np.array([o.matrix_world @ Vector(c) for o in objects for c in o.bound_box])
    blo, bhi = corners.min(axis=0), corners.max(axis=0)
    metrics = {"triangles": sum(len(o.data.polygons) for o in objects), "budget": goal["budget_tris"],
               "open_edges": open_edges, "open_edges_expected": 3 * nails,
               "parts": parts, "has_part_attribute": all(o.data.attributes.get("_part") is not None for o in objects),
               "bbox_delta_min_m": (blo - np.array(lo)).round(4).tolist(),
               "bbox_delta_max_m": (bhi - np.array(hi)).round(4).tolist(),
               "blender_version": bpy.app.version_string, **(extra or {})}
    if len(nodes) > 1:
        metrics["nodes"] = {o.name: len(o.data.polygons) for o in objects}
    (out / "metrics.json").write_text(json.dumps(metrics, indent=2))
    print("BUILT", json.dumps(metrics))
    return metrics
