"""Test edit of a region: lifts the vertex at glTF (200, 0, 400), shared with a chunk outside the region, by 10 cm
(build-region must refuse it)."""
import bmesh


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    border = ctx.border_layer(bm)
    for v in bm.verts:
        if abs(v.co.x - 200) < .01 and abs(v.co.y + 400) < .01:
            assert v[border] == 1, 'the vertex should be marked as the outer border'
            v.co.z += 10
    bm.to_mesh(ctx.mesh)
    bm.free()
