import sys,time,numpy as np,pymeshlab
src,dst,n=sys.argv[1],sys.argv[2],int(sys.argv[3])
# Optional 4th, 'plain': ignore the UV, for a scan whose dense seams stop the collapse. Then scan_rock.py --diffuse re-unwraps and bakes.
plain=len(sys.argv)>4 and sys.argv[4]=='plain'
t=time.time(); ms=pymeshlab.MeshSet(); ms.load_new_mesh(src); m=ms.current_mesh(); print('loaded',m.face_number(),m.vertex_number(),m.has_wedge_tex_coord(),m.has_vertex_tex_coord(),round(time.time()-t))
if plain:
    ms.meshing_decimation_quadric_edge_collapse(targetfacenum=n,qualitythr=0.5,preserveboundary=True,boundaryweight=2.0,optimalplacement=True,preservenormal=True,planarquadric=False,preservetopology=False,autoclean=True)
else:
    if not m.has_wedge_tex_coord(): ms.compute_texcoord_transfer_vertex_to_wedge()
    ms.meshing_decimation_quadric_edge_collapse_with_texture(targetfacenum=n,qualitythr=0.5,extratcoordw=1.0,preserveboundary=True,boundaryweight=2.0,optimalplacement=True,preservenormal=True,planarquadric=False)
m=ms.current_mesh(); print('out',m.face_number(),m.vertex_number(),round(time.time()-t)); ms.save_current_mesh(dst,save_textures=False,save_wedge_texcoord=not plain,save_vertex_normal=False); print('saved')
