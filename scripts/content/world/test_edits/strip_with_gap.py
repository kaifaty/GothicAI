"""Test edit: builds a strip of three quads, declares it, then deletes the middle one (world_edit.py must refuse:
the declared outline does not cover the edges the strip's quads shared)."""
import bmesh
from mathutils import Vector


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    row = [[bm.verts.new(Vector((20 + 10 * k, -20 - 10 * side, 40))) for k in range(4)] for side in (0, 1)]
    quads = [bm.faces.new([row[0][k], row[0][k + 1], row[1][k + 1], row[1][k]]) for k in range(3)]
    ctx.allow_open(quads, 'a strip standing on its own')
    bmesh.ops.delete(bm, geom=[quads[1]], context='FACES_ONLY')
    bm.to_mesh(ctx.mesh)
    bm.free()
