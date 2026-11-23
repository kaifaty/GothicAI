"""Find closed parts of a model that are inside out.

blender -b --factory-startup -P volume_check.py -- MODEL.glb [MODEL.glb ...]

The engine culls back faces, so a closed part wound inward is invisible from outside: the hood of the wide
fireplace showed only its front and read as a flat slab (city-hearth, 2026-10-31). Each connected part is
welded, and a closed (manifold) part with a negative signed volume is printed with its area and centre.
Open parts (discs, sheets) cannot be judged this way and are skipped. Exits 1 if any part is inside out.
"""
import sys

import bmesh
import bpy

bad_total = 0
for path in sys.argv[sys.argv.index("--") + 1:]:
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=path)
    parts, bad = 0, []
    for obj in bpy.context.scene.objects:
        if obj.type != "MESH":
            continue
        bm = bmesh.new()
        bm.from_mesh(obj.data)
        bm.transform(obj.matrix_world)
        bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
        seen = set()
        for start in bm.faces:
            if start.index in seen:
                continue
            stack, comp = [start], []
            seen.add(start.index)
            while stack:
                face = stack.pop()
                comp.append(face)
                for edge in face.edges:
                    for other in edge.link_faces:
                        if other.index not in seen:
                            seen.add(other.index)
                            stack.append(other)
            parts += 1
            if not all(e.is_manifold for f in comp for e in f.edges):
                continue
            volume = 0.0
            for face in comp:
                vs = [v.co for v in face.verts]
                for i in range(1, len(vs) - 1):
                    volume += vs[0].dot(vs[i].cross(vs[i + 1])) / 6
            if volume < -1e-7:
                verts = [v.co for f in comp for v in f.verts]
                centre = sum(verts, verts[0] * 0) / len(verts)
                bad.append((round(sum(f.calc_area() for f in comp), 3), [round(x, 2) for x in centre]))
        bm.free()
    bad_total += len(bad)
    print(f"{path}: {parts} parts, {len(bad)} inside out", *(f"\n  area {a} m2 at {c}" for a, c in sorted(bad, reverse=True)))
sys.exit(1 if bad_total else 0)
