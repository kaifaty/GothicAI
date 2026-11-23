"""Test edit of a region: splits the outer border edge glTF (200, 0, 400) - (200, 0, 500) in the middle (its ends
stay; build-region must refuse the new open edges)."""
import bmesh


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    border = ctx.border_layer(bm)
    edge = next(e for e in bm.edges if all(abs(v.co.x - 200) < .01 and v[border] for v in e.verts) and
                sorted(round(-v.co.y) for v in e.verts) == [400, 500])
    before = len(bm.verts)          # a fresh bmesh puts the vertices it makes after its own
    bmesh.ops.subdivide_edges(bm, edges=[edge], cuts=1)
    ctx.mark_new(bm, list(bm.verts)[before:])
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.to_mesh(ctx.mesh)
    bm.free()
