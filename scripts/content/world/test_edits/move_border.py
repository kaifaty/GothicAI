"""Test edit: lifts a vertex on the border with the next chunk by 10 cm (world_edit.py must refuse it)."""
import bmesh


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    for v in bm.verts:
        if abs(v.co.x - 200) < .01 and abs(v.co.y + 100) < .01:
            v.co.z += 10
    bm.to_mesh(ctx.mesh)
    bm.free()
