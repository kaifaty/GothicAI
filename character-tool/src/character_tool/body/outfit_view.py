"""Views of the hero outfit in Blender: the whole figure from four sides and close-ups, for judging the pieces.

    BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender \
      Blender -b <run>/outfit.blend -P outfit_view.py -- <out dir> [zoom: whole,upper,lower]

Workbench with cavity, flat material colours of the pieces, a skin colour for the body.
"""
import bpy, os, sys
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:]
OUT = argv[0]; os.makedirs(OUT, exist_ok=True)
ZOOMS = argv[1].split(',') if len(argv) > 1 else ['whole']
FRAMES = {'whole': (0.88, 1.95), 'upper': (1.25, 0.85), 'lower': (0.45, 0.95)}   # centre height, ortho height (m)

sc = bpy.context.scene
sc.render.engine = 'BLENDER_WORKBENCH'
sc.display.shading.light = 'STUDIO'; sc.display.shading.color_type = 'MATERIAL'
sc.display.shading.show_cavity = True; sc.display.shading.show_specular_highlight = False
sc.render.resolution_x, sc.render.resolution_y = 640, 1000
body = bpy.data.objects['Human']
skin = bpy.data.materials.get('skin_view') or bpy.data.materials.new('skin_view')
skin.diffuse_color = (0.55, 0.42, 0.34, 1)
body.data.materials.clear(); body.data.materials.append(skin)
cam_d = bpy.data.cameras.new('view'); cam_d.type = 'ORTHO'
cam = bpy.data.objects.new('view', cam_d); sc.collection.objects.link(cam); sc.camera = cam
for zoom in ZOOMS:
    h, size = FRAMES[zoom]
    cam_d.ortho_scale = size
    centre = Vector((0, 0, h))
    for name, d in (('front', (0, -1, 0)), ('side', (1, 0, 0)), ('back', (0, 1, 0)), ('quarter', (0.7, -0.7, 0))):
        cam.location = centre + Vector(d) * 4
        cam.rotation_euler = (centre - cam.location).to_track_quat('-Z', 'Y').to_euler()
        sc.render.filepath = os.path.join(OUT, f'{zoom}_{name}.png')
        bpy.ops.render.render(write_still=True)
print('VIEWS', OUT, ZOOMS)
