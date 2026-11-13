"""Test edit of a region: reads the joined mesh and writes it back unchanged (build-region must publish nothing)."""
import bmesh


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    bm.to_mesh(ctx.mesh)
    bm.free()
