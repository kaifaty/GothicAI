"""Test edit of a region: splits the edges of the faces of the chunk 1_1 and of the TEST_A faces of 2_1 that do not
touch the outer border. The faces on the other side of an inner border or of the feature seam x = 500 are not
chosen: bmesh splits them along the shared edges because the joined mesh is one surface (allowed)."""
import bmesh

from world_bpy import gothic_name


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    border, chunk = ctx.border_layer(bm), ctx.chunk_layer(bm)
    a = next(i for i, m in enumerate(ctx.mesh.materials) if gothic_name(m) == 'TEST_A')
    faces = [f for f in bm.faces if not any(v[border] for v in f.verts) and
             (f[chunk] == ctx.chunk_number('1_1') or (f[chunk] == ctx.chunk_number('2_1') and f.material_index == a))]
    assert faces
    before = len(bm.verts)
    bm.edges.index_update()
    cut = {e.index for f in faces for e in f.edges}
    bmesh.ops.subdivide_edges(bm, edges=[e for e in bm.edges if e.index in cut], cuts=1, use_grid_fill=True)
    ctx.mark_new(bm, list(bm.verts)[before:])
    bmesh.ops.triangulate(bm, faces=[f for f in bm.faces if len(f.verts) > 3])
    bm.to_mesh(ctx.mesh)
    bm.free()
