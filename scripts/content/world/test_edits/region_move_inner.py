"""Test edit of a region: lifts the vertex at glTF (500, 0, 500) by 10 cm, on the border between the region's own
chunks (allowed: every chunk with it follows)."""
import bmesh


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    for v in bm.verts:
        if abs(v.co.x - 500) < .01 and abs(v.co.y + 500) < .01:
            v.co.z += 10
    bm.to_mesh(ctx.mesh)
    bm.free()
