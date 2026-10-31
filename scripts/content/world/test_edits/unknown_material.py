"""Test edit: puts a material the base and the set do not know on one face (world_edit.py must refuse it)."""
import bpy


def edit(ctx):
    m = bpy.data.materials.new('TEST_UNKNOWN')
    m['gothic'] = {'name': 'TEST_UNKNOWN', 'texture': 'TEST_UNKNOWN.TGA'}
    ctx.mesh.materials.append(m)
    ctx.mesh.polygons[0].material_index = len(ctx.mesh.materials) - 1
