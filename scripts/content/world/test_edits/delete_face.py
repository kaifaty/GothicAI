"""Test edit: deletes a face of the grid and declares nothing (world_edit.py must refuse the open edges)."""
import bmesh


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    face = next(f for f in bm.faces if 0 < f.calc_center_median().x < 100 and -100 < f.calc_center_median().y < 0)
    bmesh.ops.delete(bm, geom=[face], context='FACES_ONLY')
    bm.to_mesh(ctx.mesh)
    bm.free()
