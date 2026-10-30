"""Test edit: lifts the middle vertex of the grid by 5 cm (inside the chunk: allowed)."""
import bmesh

PARAMETERS = {'lift_cm': 5}


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    for v in bm.verts:
        if abs(v.co.x - 100) < .01 and abs(v.co.y + 100) < .01:
            v.co.z += PARAMETERS['lift_cm']
    bm.to_mesh(ctx.mesh)
    bm.free()
