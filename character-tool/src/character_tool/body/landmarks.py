# blender -b -P landmarks.py -- <glb> <out.json>
import bpy, sys, json, numpy as np
glb, out = sys.argv[sys.argv.index('--') + 1:][:2]
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=glb)
o = [o for o in bpy.context.scene.objects if o.type == 'MESH'][0]
me = o.data
co = np.empty(len(me.vertices) * 3); me.vertices.foreach_get('co', co); co = co.reshape(-1, 3)
M = np.array(o.matrix_world); co = co @ M[:3, :3].T + M[:3, 3]
T = np.c_[co[:, 0], co[:, 2], -co[:, 1]] * 100  # glTF frame, cm of the normalised model
X, Y, Z = T.T
def cen(y0, y1, mask):
    m = (Y >= y0) & (Y <= y1) & mask
    return T[m].mean(0), int(m.sum())
L = {}
for side, sg in (('L', 1), ('R', -1)):   # character left = glTF +x
    sx = sg * X
    L[f'elbow_{side}'], _ = cen(63, 64, sx > 11)
    L[f'wrist_{side}'], _ = cen(50, 51, sx > 15)
    a72, _ = cen(71.5, 72.5, sx > 10)
    e = L[f'elbow_{side}']; t = (76.5 - a72[1]) / (a72[1] - e[1])
    L[f'shoulder_{side}'] = a72 + t * (a72 - e)
    L[f'fingertip_{side}'], _ = cen(41, 42.5, sx > 17)
    L[f'knee_{side}'], _ = cen(30, 31, sx > 0.5)
    shin, _ = cen(9, 10, sx > 0.5)
    L[f'ankle_{side}'] = np.array([shin[0], 5.0, shin[2]])
    m = (Y < 2) & (sx > 0.5)
    L[f'toe_{side}'] = T[m][np.argmax(T[m][:, 2])]
    L[f'heel_{side}'] = T[m][np.argmin(T[m][:, 2])]
    body, _ = cen(52, 53, np.abs(X) < 12)
    L[f'hip_{side}'] = np.array([sg * 4.74, 52.6, body[2]])
L['pelvis'] = np.array([0, 52.6, cen(52, 53, np.abs(X) < 12)[0][2]])
L['neck'] = cen(80.8, 81.8, np.abs(X) < 6)[0]
L['head'] = cen(84.3, 85.3, np.abs(X) < 8)[0]
L['top'] = T[np.argmax(Y)]
json.dump({k: [round(float(c), 3) for c in v] for k, v in L.items()}, open(out, 'w'), indent=1)
