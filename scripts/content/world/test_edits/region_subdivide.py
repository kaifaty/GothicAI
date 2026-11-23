"""Test edit of a region: splits every edge of the faces that do not touch the region's outer border in the middle
and triangulates (allowed: the inner borders are split on both sides alike, the children keep their chunk)."""
import bmesh


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    border = ctx.border_layer(bm)
    faces = [f for f in bm.faces if not any(v[border] for v in f.verts)]
    before = len(bm.verts)          # a fresh bmesh puts the vertices it makes after its own
    bm.edges.index_update()
    cut = {e.index for f in faces for e in f.edges}
    bmesh.ops.subdivide_edges(bm, edges=[e for e in bm.edges if e.index in cut], cuts=1, use_grid_fill=True)
    ctx.mark_new(bm, list(bm.verts)[before:])
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.to_mesh(ctx.mesh)
    bm.free()
