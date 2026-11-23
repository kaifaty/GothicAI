"""Test edit of a region: lifts the one vertex with _VERTEX 55, glTF (500, 0, 500), by 10 cm. It lies on the border
between the region's own chunks and on a feature seam (allowed: every chunk and every side of the seam follows)."""
import bmesh


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    vid = bm.verts.layers.float['_VERTEX']
    vertex = [v for v in bm.verts if v[vid] == 55]
    assert len(vertex) == 1, f'_VERTEX 55 is {len(vertex)} vertices: the weld left a seam'
    vertex[0].co.z += 10
    bm.to_mesh(ctx.mesh)
    bm.free()
