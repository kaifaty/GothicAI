"""Test edit after roof_holes.py: deletes the 20 largest OWN_ROOF_HOLE_WALL faces (the sides of the holes);
world_edit.py must refuse the gaps."""
import bmesh

from world_bpy import gothic_name


def edit(ctx):
    slot = next(i for i, m in enumerate(ctx.mesh.materials) if gothic_name(m) == 'OWN_ROOF_HOLE_WALL')
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    walls = sorted((f for f in bm.faces if f.material_index == slot), key=lambda f: f.calc_area())[-20:]
    bmesh.ops.delete(bm, geom=walls, context='FACES_ONLY')
    bm.to_mesh(ctx.mesh)
    bm.free()
