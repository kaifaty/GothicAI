"""Test edit of a region: adds a free-standing triangle above the chunk 1_1 and declares its open edges. With
PARAMETERS chunk None it leaves the face's chunk unset (build-region must refuse it); region_face_in_chunk.py puts it
into 2_2."""
import bmesh
from mathutils import Vector

PARAMETERS = {'chunk': None}


def add_face(ctx, chunk):
    bm = bmesh.new()
    bm.from_mesh(ctx.mesh)
    # glTF (290, 50, 290), (310, 50, 290), (290, 50, 310) in Blender's axes (x, -z, y)
    vs = [bm.verts.new(Vector(p)) for p in ((290, -290, 50), (310, -290, 50), (290, -310, 50))]
    ctx.mark_new(bm, vs)
    f = bm.faces.new(vs)
    if chunk is not None:
        f[ctx.chunk_layer(bm)] = ctx.chunk_number(chunk)
    ctx.allow_open([f], 'a triangle standing on its own')
    bm.to_mesh(ctx.mesh)
    bm.free()


def edit(ctx):
    add_face(ctx, PARAMETERS['chunk'])
