"""CPU Blender pipeline. Run with blender -b -P postprocess.py -- --help."""
import argparse
import json
import math
from pathlib import Path
import sys
import time

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from geometry import (box_uv, budget_ratio, classify_zones, cylinder_uv, face_adjacency, fit_bbox,
                      select_uv_projection, simplify_loop, smooth_labels, triangle_areas)


def load_target(path, ident=None):
    data = json.loads(Path(path).read_text())
    if isinstance(data, dict) and "targets" in data:
        data = data["targets"]
    if isinstance(data, list):
        matches = [row for row in data if ident is None or row["id"] == ident]
        if len(matches) != 1:
            raise ValueError("select exactly one target with --id")
        data = matches[0]
    if ident is not None and data["id"] != ident:
        raise ValueError(f"target {ident!r} not found")
    for key in ("id", "visual", "kind", "bbox_min", "bbox_max", "budget_tris", "zones"):
        if key not in data:
            raise ValueError(f"target missing {key}")
    if not isinstance(data["budget_tris"], int) or data["budget_tris"] < 12:
        raise ValueError("budget_tris must be an integer >= 12")
    keys = [zone["key"] for zone in data["zones"]]
    if not keys or len(keys) != len(set(keys)):
        raise ValueError("zone keys must be nonempty and unique")
    for zone in data["zones"]:
        if not all(isinstance(zone[k], str) and zone[k] for k in ("key", "group", "role")):
            raise ValueError("zones require key, group, role strings")
    return data


def read_raw(path):
    with np.load(path, allow_pickle=False) as raw:
        v, f, c = (raw[key] for key in ("vertices", "faces", "color"))
        m = raw["metallic"] if "metallic" in raw else None
        r = raw["roughness"] if "roughness" in raw else None
    if v.ndim != 2 or v.shape[1] != 3 or c.shape != v.shape:
        raise ValueError("vertices and color must have shape (N,3)")
    if f.ndim != 2 or f.shape[1] != 3 or not np.issubdtype(f.dtype, np.integer):
        raise ValueError("faces must be integer (M,3)")
    if len(v) == 0 or len(f) == 0 or f.min() < 0 or f.max() >= len(v):
        raise ValueError("empty mesh or face index out of range")
    if not np.isfinite(v).all() or not np.isfinite(c).all() or np.any((c < 0) | (c > 1)):
        raise ValueError("positions must be finite; linear color must be in [0,1]")
    for key, values in (("metallic", m), ("roughness", r)):
        if values is not None and (values.shape != (len(v),) or not np.isfinite(values).all()
                                   or np.any((values < 0) | (values > 1))):
            raise ValueError(f"{key} must be finite (N,) in [0,1]")
    return v, f, c, m, r


def make_object(name, vertices, faces, color=None, metallic=None):
    import bpy
    coords = np.asarray(vertices)[:, [0, 2, 1]].copy()
    coords[:, 1] *= -1  # glTF +Y up -> Blender +Z up; exporter reverses this.
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(coords.tolist(), [], np.asarray(faces).tolist())
    mesh.update()
    obj = bpy.data.objects.new(name, mesh)
    bpy.context.collection.objects.link(obj)
    if color is not None:
        attr = mesh.color_attributes.new(name="Color", type="FLOAT_COLOR", domain="POINT")
        attr.data.foreach_set("color", np.column_stack((color, np.ones(len(color)))).ravel())
    return obj


def mesh_arrays(obj):
    mesh = obj.data
    mesh.calc_loop_triangles()
    v = np.empty((len(mesh.vertices), 3), dtype=np.float64)
    mesh.vertices.foreach_get("co", v.ravel())
    v = v[:, [0, 2, 1]]
    v[:, 2] *= -1
    f = np.array([t.vertices[:] for t in mesh.loop_triangles], dtype=np.int32)
    return v, f


def components(bm):
    """Connected vertex sets, including separately enclosed shells."""
    remaining = set(bm.verts)
    result = []
    while remaining:
        first = remaining.pop()
        group, pending = [first], [first]
        while pending:
            for edge in pending.pop().link_edges:
                for vertex in edge.verts:
                    if vertex in remaining:
                        remaining.remove(vertex)
                        pending.append(vertex)
                        group.append(vertex)
        result.append(group)
    return result


def clean(obj):
    import bmesh
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    bm.normal_update()
    points = np.array([v.co[:] for v in bm.verts])
    extent = float(np.linalg.norm(np.ptp(points, axis=0)))
    epsilon = max(extent * 1e-5, 1e-7)
    bmesh.ops.remove_doubles(bm, verts=list(bm.verts), dist=epsilon)
    bmesh.ops.dissolve_degenerate(bm, edges=list(bm.edges), dist=epsilon)
    # Remove fins inside nonmanifold junctions; keep ordinary boundary surfaces.
    interior = [f for f in bm.faces if all(len(e.link_faces) > 2 for e in f.edges)]
    if interior:
        bmesh.ops.delete(bm, geom=interior, context="FACES")
    groups = components(bm)
    total_area = sum(f.calc_area() for f in bm.faces)
    discard = []
    for group in groups:
        faces = {f for v in group for f in v.link_faces}
        if sum(f.calc_area() for f in faces) < total_area * 0.0005:
            discard.extend(group)
    if discard:
        bmesh.ops.delete(bm, geom=discard, context="VERTS")
    # A fully enclosed disconnected shell contributes no visible surface.
    groups = components(bm)
    if len(groups) > 1:
        bm.verts.index_update()
        bm.faces.index_update()
        tree = BVHTree.FromBMesh(bm, epsilon=epsilon * 0.1)
        direction = Vector((0.8731, 0.3717, 0.3139)).normalized()
        hidden = []
        for group in groups:
            faces = {f for v in group for f in v.link_faces}
            own = {f.index for f in faces}
            # Open components cannot define an interior.
            if not faces or any(not e.is_manifold for v in group for e in v.link_edges):
                continue
            samples = sorted(group, key=lambda v: v.index)[::max(1, len(group) // 5)][:5]
            enclosed = True
            for vertex in samples:
                point, count = vertex.co + direction * epsilon * 2, 0
                for _ in range(len(groups) * 4 + 32):
                    hit, _, face_id, _ = tree.ray_cast(point, direction)
                    if hit is None:
                        break
                    count += face_id not in own
                    point = hit + direction * epsilon * 2
                else:
                    enclosed = False
                    break
                if count % 2 == 0:
                    enclosed = False
                    break
            if enclosed:
                hidden.extend(group)
        if hidden:
            bmesh.ops.delete(bm, geom=list(set(hidden)), context="VERTS")
    # Only short boundary cycles: avoid sealing deliberate openings such as a mouth.
    boundary = {e for e in bm.edges if e.is_boundary}
    while boundary:
        seed = boundary.pop()
        ring, pending = {seed}, [seed]
        while pending:
            for v in pending.pop().verts:
                for e in v.link_edges:
                    if e in boundary:
                        boundary.remove(e)
                        ring.add(e)
                        pending.append(e)
        if sum(e.calc_length() for e in ring) <= extent * 0.04 and len(ring) <= 32:
            bmesh.ops.holes_fill(bm, edges=list(ring), sides=32)
    if bm.faces:
        bmesh.ops.recalc_face_normals(bm, faces=list(bm.faces))
    bm.to_mesh(obj.data)
    bm.free()
    obj.data.update()


def activate(obj):
    import bpy
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj


def decimate(obj, budget, projection):
    import bpy
    import bmesh
    activate(obj)
    # Collapse does not respect material delimiters. Simplify the contour itself,
    # then lock it with zero vertex weights while collapsing the surface inside.
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    borders = {e for e in bm.edges if len(e.link_faces) == 2
               and e.link_faces[0].material_index != e.link_faces[1].material_index}
    neighbors = {}
    for edge in borders:
        a, b = edge.verts
        neighbors.setdefault(a, []).append(b)
        neighbors.setdefault(b, []).append(a)
    remaining = set(neighbors)
    loops = []
    while remaining:
        first = remaining.pop()
        loop, previous, current = [first], None, first
        while len(neighbors[current]) == 2:
            following = next((v for v in neighbors[current] if v != previous), None)
            if following == first:
                loops.append(loop)
                break
            if following not in remaining:
                break
            remaining.remove(following)
            loop.append(following)
            previous, current = current, following
    extent = float(np.linalg.norm(np.ptp(np.array([v.co[:] for v in bm.verts]), axis=0)))
    tolerance = extent * 0.0005
    for _ in range(5):
        kept = [simplify_loop(np.array([v.co[:] for v in loop]), tolerance) for loop in loops]
        if sum(map(len, kept)) <= budget // 4:
            break
        tolerance *= 1.6
    pending = {}
    for loop, indices in zip(loops, kept):
        points = np.array([v.co[:] for v in loop])
        # Stay on the contour arc between consecutive retained vertices. Global
        # nearest points can jump across a narrow corner and join separate fans.
        for left, right in zip(indices, np.roll(indices, -1)):
            arc = (np.arange((right - left) % len(loop) + 1) + left) % len(loop)
            distance = np.r_[0., np.cumsum(np.linalg.norm(np.diff(points[arc], axis=0), axis=1))]
            for position, index in enumerate(arc[1:-1], 1):
                anchor = left if distance[position] <= distance[-1] * .5 else right
                pending[loop[index]] = loop[anchor]
    while pending:
        mapping, reserved = {}, set()
        for vertex, anchor in pending.items():
            if not vertex.is_valid or not anchor.is_valid:
                continue
            edge = next((e for e in vertex.link_edges if e.other_vert(vertex) == anchor), None)
            if edge is None or not edge.is_manifold or any(len(f.verts) != 3 for f in edge.link_faces):
                continue
            vertex_neighbors = {e.other_vert(vertex) for e in vertex.link_edges}
            anchor_neighbors = {e.other_vert(anchor) for e in anchor.link_edges}
            opposite = {v for f in edge.link_faces for v in f.verts} - {vertex, anchor}
            # The edge link condition preserves the two manifold surface fans.
            if len(opposite) != 2 or vertex_neighbors & anchor_neighbors != opposite:
                continue
            # A tetrahedron also shares both opposite vertices, but collapsing
            # it would identify two surviving triangles with each other.
            if (any(set(f.verts) == opposite | {vertex} for f in vertex.link_faces)
                    and any(set(f.verts) == opposite | {anchor} for f in anchor.link_faces)):
                continue
            neighborhood = vertex_neighbors | anchor_neighbors | {vertex, anchor}
            if not reserved.isdisjoint(neighborhood):
                continue
            mapping[vertex] = anchor
            reserved.update(neighborhood)
        if not mapping:
            break  # Freeze any contour vertices whose collapse is not safe.
        bmesh.ops.weld_verts(bm, targetmap=mapping)
        pending = {vertex: anchor for vertex, anchor in pending.items()
                   if vertex.is_valid and anchor.is_valid}
    bm.normal_update()
    bm.verts.index_update()
    locked = {v.index for e in bm.edges if len(e.link_faces) == 2
              and e.link_faces[0].material_index != e.link_faces[1].material_index
              for v in e.verts}
    bm.to_mesh(obj.data)
    bm.free()
    weights = obj.vertex_groups.new(name="Collapse inside material contours")
    weights_name = weights.name
    movable = [v.index for v in obj.data.vertices if v.index not in locked]
    if movable:
        weights.add(movable, 1.0, "REPLACE")
    for _ in range(4):
        count = len(mesh_arrays(obj)[1])
        if count <= budget:
            break
        modifier = obj.modifiers.new("Triangle budget", "DECIMATE")
        modifier.decimate_type = "COLLAPSE"
        modifier.ratio = budget_ratio(count, budget)
        modifier.use_collapse_triangulate = True
        if locked:
            modifier.vertex_group = weights_name
            modifier.vertex_group_factor = 1.0
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    if projection == "cylinder":
        # A curved strip can become one nonplanar ngon under repeated planar
        # dissolves. Its retriangulation creates narrow chords across the bow.
        # Keep the collapsed side triangles; dissolve only cap/box patches.
        v, f = mesh_arrays(obj)
        _, _, types = cylinder_uv(v, f, return_patches=True)
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bm.faces.index_update()
        edges = [e for e in bm.edges if e.is_manifold
                 and all(types[face.index] != 1 for face in e.link_faces)]
        vertices = [v for v in bm.verts
                    if all(types[face.index] != 1 for face in v.link_faces)]
        bmesh.ops.dissolve_limit(bm, angle_limit=math.radians(5),
                                 verts=vertices, edges=edges, delimit={"MATERIAL", "SHARP"})
        bmesh.ops.triangulate(bm, faces=list(bm.faces))
        bm.to_mesh(obj.data)
        bm.free()
    else:
        modifier = obj.modifiers.new("Planar 5 degrees", "DECIMATE")
        modifier.decimate_type = "DISSOLVE"
        modifier.angle_limit = math.radians(5)
        modifier.delimit = {"MATERIAL", "SHARP"}
        bpy.ops.object.modifier_apply(modifier=modifier.name)
    obj.vertex_groups.remove(obj.vertex_groups[weights_name])
    # Export and UV assignment operate on the exact same triangles.
    modifier = obj.modifiers.new("Export triangles", "TRIANGULATE")
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    # Welding a dense contour can leave a tiny folded flap. Such a triangle has
    # both an exposed edge and an edge shared by >2 faces; deleting it restores
    # the closed surface without sealing intentional openings.
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    total_area = sum(f.calc_area() for f in bm.faces)
    fins = [f for f in bm.faces if f.calc_area() < total_area * 0.001
            and any(e.is_boundary for e in f.edges)
            and any(len(e.link_faces) > 2 for e in f.edges)]
    if fins:
        bmesh.ops.delete(bm, geom=fins, context="FACES")
    # Dissolve can retriangulate a curved strip into long, narrow chords. Rotate
    # internal diagonals for better triangles without moving material contours
    # or adding triangles; keep the sharp corners of boards intact.
    bm.normal_update()
    diagonals = [e for e in bm.edges if e.is_manifold
                 and e.link_faces[0].material_index == e.link_faces[1].material_index
                 and e.calc_face_angle() < math.radians(60)]
    bmesh.ops.beautify_fill(bm, faces=list(bm.faces), edges=diagonals, method="AREA")
    bm.to_mesh(obj.data)
    bm.free()
    count = len(obj.data.polygons)
    if count > budget or count == 0:
        raise RuntimeError(f"decimation produced {count} triangles for budget {budget}")


def materials(obj, target):
    import bpy
    palette = [(0.34, 0.16, 0.045, 1), (0.14, 0.23, 0.32, 1),
               (0.59, 0.31, 0.10, 1), (0.24, 0.39, 0.12, 1)]
    for index, zone in enumerate(target["zones"]):
        mat = bpy.data.materials.new(zone["key"])
        # bpy auto-suffixes duplicate names across objects; glTF must use the key.
        mat.name = zone["key"]
        mat["group"] = zone["group"]
        mat.diffuse_color = palette[index % len(palette)]
        mat.use_nodes = True
        shader = mat.node_tree.nodes.get("Principled BSDF")
        shader.inputs["Base Color"].default_value = mat.diffuse_color
        shader.inputs["Metallic"].default_value = float(zone["group"] == "METAL")
        shader.inputs["Roughness"].default_value = 0.7
        obj.data.materials.append(mat)


def uv_continuity(vertices, faces, uv, patch_ids):
    """Measure shared-corner jumps inside charts, before normal vertex splits."""
    pairs = face_adjacency(faces)
    pairs = pairs[patch_ids[pairs[:, 0]] == patch_ids[pairs[:, 1]]]
    a, b = pairs.T
    match = faces[a, :, None] == faces[b, None, :]
    corners_a = np.argsort(~match.any(axis=2), axis=1)[:, :2]
    corners_b = np.take_along_axis(match.argmax(axis=2), corners_a, axis=1)
    difference = uv[a[:, None], corners_a] - uv[b[:, None], corners_b]
    jump = np.linalg.norm(difference, axis=2).max(axis=1)
    triangles = vertices[faces]
    normals = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    normals /= np.linalg.norm(normals, axis=1)[:, None]
    coplanar = np.linalg.norm(normals[a] - normals[b], axis=1) < 1e-6
    return {"patches": int(len(np.unique(patch_ids))), "interior_edges": int(len(pairs)),
            "coplanar_interior_edges": int(coplanar.sum()),
            "seam_edges": int((jump > 1e-5).sum()),
            "coplanar_seam_edges": int(((jump > 1e-5) & coplanar).sum()),
            "max_jump": float(jump.max(initial=0.0))}


def unwrap(obj, projection):
    v, f = mesh_arrays(obj)
    project = cylinder_uv if projection == "cylinder" else box_uv
    uv, patch_ids, patch_types = project(v, f, return_patches=True)
    # Keep chart identities through normal splits for the acceptance checks.
    for name, values in (("UVPatch", patch_ids), ("UVProjection", patch_types)):
        attribute = obj.data.attributes.new(name=name, type="INT", domain="FACE")
        attribute.data.foreach_set("value", values)
    layer = obj.data.uv_layers.new(name="Meters")
    layer.data.foreach_set("uv", uv.ravel())
    return uv, uv_continuity(v, f, uv, patch_ids)


def export(obj, path):
    import bpy
    activate(obj)
    bpy.ops.export_scene.gltf(filepath=str(path), export_format="GLB", use_selection=True,
                              export_extras=True, export_yup=True, export_normals=True,
                              export_texcoords=True,
                              export_animations=False, export_cameras=False,
                              export_lights=False, export_apply=True)


def collision(obj, output, ident):
    import bpy
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(obj.data)
    hulls = []
    for index, group in enumerate(components(bm)):
        if len(group) < 4:
            continue
        hull = bmesh.new()
        points = [hull.verts.new(v.co) for v in group]
        result = bmesh.ops.convex_hull(hull, input=points, use_existing_faces=False)
        remove = list({g for g in result["geom_interior"] + result["geom_unused"]
                       if isinstance(g, bmesh.types.BMVert) and g.is_valid})
        if remove:
            bmesh.ops.delete(hull, geom=remove, context="VERTS")
        mesh = bpy.data.meshes.new(f"{ident}_collision_{index}")
        hull.to_mesh(mesh)
        hull.free()
        item = bpy.data.objects.new(mesh.name, mesh)
        item["collision"] = True
        item["convex"] = True
        bpy.context.collection.objects.link(item)
        hulls.append(item)
    bm.free()
    if not hulls:
        raise RuntimeError("no nondegenerate collision hull")
    bpy.ops.object.select_all(action="DESELECT")
    for item in hulls:
        item.select_set(True)
    bpy.context.view_layer.objects.active = hulls[0]
    bpy.ops.export_scene.gltf(filepath=str(output / "collision.glb"), export_format="GLB",
                              use_selection=True, export_extras=True, export_yup=True,
                              export_animations=False)
    for item in hulls:
        bpy.data.objects.remove(item, do_unlink=True)
    return len(hulls)


def process_arrays(vertices, faces, color, metallic, target, output_dir, meta, cleanup=True):
    import bpy
    import bmesh
    started = time.monotonic()
    output = Path(output_dir).resolve()
    output.mkdir(parents=True, exist_ok=True)
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    # Previous variants may have left material datablocks with these exact keys.
    for mat in list(bpy.data.materials):
        bpy.data.materials.remove(mat)
    aligned = fit_bbox(vertices, meta, target)
    projection = select_uv_projection(aligned, faces, target)
    labels = classify_zones(aligned, faces, color, metallic, target["zones"])
    labels = smooth_labels(faces, labels, triangle_areas(aligned, faces),
                           locked_labels=[i for i, z in enumerate(target["zones"])
                                          if z["group"] == "METAL"])
    obj = make_object(target["id"], aligned, faces)
    materials(obj, target)
    obj.data.polygons.foreach_set("material_index", labels.astype(np.int32))
    if cleanup:
        clean(obj)
    decimate(obj, target["budget_tris"], projection)
    # Cleanup / simplification can move extremes: preserve the supplied anchor exactly.
    final_v, final_f = mesh_arrays(obj)
    lo, hi = np.asarray(target["bbox_min"]), np.asarray(target["bbox_max"])
    span = np.ptp(final_v, axis=0)
    if np.any(span <= 1e-9):
        raise RuntimeError("degenerate mesh after cleanup")
    final_v = (final_v - final_v.min(axis=0)) * ((hi - lo) / span) + lo
    coords = final_v[:, [0, 2, 1]].copy()
    coords[:, 1] *= -1
    obj.data.vertices.foreach_set("co", coords.ravel())
    obj.data.update()
    uv, continuity = unwrap(obj, projection)
    # Hulls use physical connectivity, before normal seams duplicate vertices.
    hull_count = collision(obj, output, target["id"])
    # Blender smooth-by-angle, plus actual split vertices for Gothic importers.
    for p in obj.data.polygons:
        p.use_smooth = True
    obj.data.set_sharp_from_angle(angle=math.radians(30))
    activate(obj)
    modifier = obj.modifiers.new("Hard edges 30 degrees", "EDGE_SPLIT")
    modifier.split_angle = math.radians(30)
    modifier.use_edge_angle = True
    modifier.use_edge_sharp = True
    bpy.ops.object.modifier_apply(modifier=modifier.name)
    # Topology metrics precede normal splits: hard edges are not actual holes.
    bm = bmesh.new()
    pre_split = bpy.data.meshes.new("Topology")
    pre_split.from_pydata(final_v.tolist(), [], final_f.tolist())
    bm.from_mesh(pre_split)
    open_edges = sum(e.is_boundary for e in bm.edges)
    nonmanifold = sum(not e.is_manifold for e in bm.edges)
    bm.free()
    bpy.data.meshes.remove(pre_split)
    areas = triangle_areas(final_v, final_f)
    labels = np.array([p.material_index for p in obj.data.polygons])
    shares = {z["key"]: float(areas[labels == i].sum() / areas.sum())
              for i, z in enumerate(target["zones"])}
    uv_edges = uv[:, 1:] - uv[:, :1]
    uv_area = abs(np.linalg.det(uv_edges)) * 0.5
    export(obj, output / "model.glb")
    activate(obj)
    bpy.ops.wm.save_as_mainfile(filepath=str(output / "scene.blend"))
    metrics = {"target": target, "triangles": len(final_f), "raw_triangles": len(faces),
               "open_edges": open_edges, "non_manifold_edges": nonmanifold,
               "zone_area_fractions": shares, "collision_hulls": hull_count,
               "uv_area_ratio": float(uv_area.sum() / areas.sum()),
               "uv_projection": projection,
               "uv_continuity": continuity,
               "seconds": round(time.monotonic() - started, 3),
               "blender_version": bpy.app.version_string}
    (output / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    return obj, metrics


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--raw", required=True)
    p.add_argument("--meta", help="default: meta.json beside raw.npz")
    p.add_argument("--target", required=True)
    p.add_argument("--id")
    p.add_argument("--out", required=True)
    args = p.parse_args(sys.argv[sys.argv.index("--") + 1:])
    target = load_target(args.target, args.id)
    meta = json.loads(Path(args.meta or Path(args.raw).with_name("meta.json")).read_text())
    v, f, c, m, _roughness = read_raw(args.raw)
    process_arrays(v, f, c, m, target, args.out, meta)


if __name__ == "__main__":
    main()
