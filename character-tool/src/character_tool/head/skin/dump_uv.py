# Dumps the registered head's skin for texture projection: per loop triangle its vertex ids and UVs, per vertex the
# position with 'hero_reg' applied and the normal.
#   blender -b <round>/reg.blend -P dump_uv.py -- <out.npz>
import bpy, sys
import numpy as np
OUT = sys.argv[sys.argv.index('--')+1]
ob = bpy.data.objects['Human']; me = ob.data
for m in ob.modifiers: m.show_viewport = False
dg = bpy.context.evaluated_depsgraph_get(); ev = ob.evaluated_get(dg); em = ev.to_mesh()
em.calc_loop_triangles()
co = np.array([v.co for v in em.vertices]); no = np.array([v.normal for v in em.vertices])
uvl = em.uv_layers.active.data
tri = np.array([t.vertices for t in em.loop_triangles]); tuv = np.array([[uvl[l].uv[:] for l in t.loops] for t in em.loop_triangles])
mat = np.array([t.material_index for t in em.loop_triangles])
np.savez(OUT, co=co, no=no, tri=tri, tuv=tuv, mat=mat)
print('DUMP verts', len(co), 'tris', len(tri), 'uv layers', [u.name for u in em.uv_layers], 'z', co[:, 2].min(), co[:, 2].max())
