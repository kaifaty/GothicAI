"""Test edit after harbour_stairs.py: deletes the largest NW_HARBOUR_STAIRS_01 face of the stairs (a tread);
world_edit.py must refuse the hole."""
import bmesh

from world_bpy import gothic_name, to_gothic

RAMP_BOX = ((8300.0, 8850.0), (-5150.0, -4550.0))


def edit(ctx):
    slot = next(i for i, m in enumerate(ctx.mesh.materials) if gothic_name(m) == 'NW_HARBOUR_STAIRS_01')
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)

    def in_box(f):
        c = to_gothic(f.calc_center_median())
        return RAMP_BOX[0][0] < c.x < RAMP_BOX[0][1] and RAMP_BOX[1][0] < c.z < RAMP_BOX[1][1]
    face = max((f for f in bm.faces if f.material_index == slot and in_box(f)), key=lambda f: f.calc_area())
    bmesh.ops.delete(bm, geom=[face], context='FACES_ONLY')
    bm.to_mesh(ctx.mesh)
    bm.free()
