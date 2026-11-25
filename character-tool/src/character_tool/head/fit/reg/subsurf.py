import bpy
bm=bpy.data.objects['Human']; m=bm.modifiers.new('sub','SUBSURF'); m.levels=1; m.render_levels=1
sc=bpy.context.scene; print('ENGINE',sc.render.engine)
bpy.ops.wm.save_as_mainfile(filepath=bpy.data.filepath.replace('.blend','_sub.blend'))
