"""Test edit of a region: lifts every vertex that is not on the region's outer border by PARAMETERS lift_cm (with
region_subdivide.py after it, the relief edit of the pilot's dry run)."""
import bmesh

PARAMETERS = {'lift_cm': 1}


def edit(ctx):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    border = ctx.border_layer(bm)
    for v in bm.verts:
        if not v[border]:
            v.co.z += PARAMETERS['lift_cm']
    bm.to_mesh(ctx.mesh)
    bm.free()
