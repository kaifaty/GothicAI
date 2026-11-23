"""Merge several GLBs of one prop into one: a baked atlas model and its tiled parts (net panels).

blender -b --factory-startup -P merge_glb.py -- OUT.glb IN.glb [IN.glb ...]
Every object of every input is kept with its node name, materials and the "group" extras; the output
has the objects in input order. Used by rigging_build.sh after bake_atlas.py (asset harbour-rigging).
"""
import sys

import bpy

rest = sys.argv[sys.argv.index("--") + 1:]
out, inputs = rest[0], rest[1:]
bpy.ops.wm.read_factory_settings(use_empty=True)
for path in inputs:
    bpy.ops.import_scene.gltf(filepath=path)
objects = [o for o in bpy.context.scene.objects if o.type == "MESH"]
for o in bpy.context.scene.objects:
    o.select_set(o in objects)
bpy.context.view_layer.objects.active = objects[0]
bpy.ops.export_scene.gltf(filepath=out, export_format="GLB", use_selection=True, export_apply=True, export_yup=True,
                          export_extras=True, export_normals=True, export_texcoords=True, export_animations=False,
                          export_attributes=True)
print("MERGED", out, [o.name for o in objects], sum(len(o.data.polygons) for o in objects))
