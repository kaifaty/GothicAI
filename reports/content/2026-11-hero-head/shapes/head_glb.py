# A head GLB of export_test_head.py without Blender: its parts as dump_glb.py reads them (Blender's frame: the glTF
# (x, y, z) as (x, -z, y)), and a copy of it with other face shapes. Used by transfer.py and head_shapes.py.
import os
import sys

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), *(['..']*4), 'scripts', 'content', 'npc'))
from eye_cornea import Glb  # noqa: E402

to_blender = lambda p: np.asarray(p, float)[:, [0, 2, 1]]*[1, -1, 1]      # noqa: E731
to_gltf = lambda p: np.asarray(p, float)[:, [0, 2, 1]]*[1, 1, -1]         # noqa: E731


def read(path):
    """{'<part>|co', '<part>|tri', '<part>|sk|<shape>' (absolute), 'parts'} like dump_glb.py's npz."""
    g = Glb(path)
    out, names = {}, []
    for n in g.j['nodes']:
        if 'mesh' not in n:
            continue
        assert not any(k in n for k in ('matrix', 'rotation', 'scale', 'translation')), n['name']
        m = g.j['meshes'][n['mesh']]
        p = m['primitives'][0]
        names.append(n['name'])
        co = to_blender(g.acc(p['attributes']['POSITION']))
        out[n['name']+'|co'] = co
        out[n['name']+'|tri'] = g.acc(p['indices'])[:, 0].astype(np.int64).reshape(-1, 3)
        for nm, t in zip(m.get('extras', {}).get('targetNames', []), p.get('targets', [])):
            out[n['name']+'|sk|'+nm] = co+to_blender(g.acc(t['POSITION']))
    out['parts'] = np.array(names)
    return out


def normals(V, T):
    """Smooth vertex normals close to Blender's (its rest normals differ from these by 3 degrees on average): weighted
    by the corner angles, the same at every copy of a vertex that the glTF splits at a seam (copies share a position).
    Only the difference of two of them goes into a morph target."""
    _, key = np.unique(np.round(V, 7), axis=0, return_inverse=True)
    key = key.ravel()
    n = np.cross(V[T[:, 1]]-V[T[:, 0]], V[T[:, 2]]-V[T[:, 0]])
    n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-20)
    acc = np.zeros((key.max()+1, 3))
    for k in range(3):
        a, b = V[T[:, (k+1) % 3]]-V[T[:, k]], V[T[:, (k+2) % 3]]-V[T[:, k]]
        c = (a*b).sum(1)/np.maximum(np.linalg.norm(a, axis=1)*np.linalg.norm(b, axis=1), 1e-20)
        np.add.at(acc, key[T[:, k]], n*np.arccos(np.clip(c, -1, 1))[:, None])
    out = acc[key]
    return out/np.maximum(np.linalg.norm(out, axis=1, keepdims=True), 1e-12)


def write(src, shapes, dst):
    """A copy of src whose morph targets named in shapes ({'<part>|<shape>': absolute coordinates, Blender's frame})
    get those positions and the normals that go with them; every other byte of meaning is kept. The binary is
    rebuilt from the accessors in use, so the replaced targets leave nothing behind."""
    g = Glb(src)
    done = 0
    for n in g.j['nodes']:
        if 'mesh' not in n:
            continue
        m = g.j['meshes'][n['mesh']]
        p = m['primitives'][0]
        names = m.get('extras', {}).get('targetNames', [])
        if not any(n['name']+'|'+nm in shapes for nm in names):
            continue
        base = g.acc(p['attributes']['POSITION']).astype(float)
        T = g.acc(p['indices'])[:, 0].astype(np.int64).reshape(-1, 3)
        n0 = normals(base, T)
        for nm, t in zip(names, p['targets']):
            k = n['name']+'|'+nm
            if k not in shapes:
                continue
            P = to_gltf(shapes[k])
            assert P.shape == base.shape, k
            d = P-base
            dn = normals(P, T)-n0
            moved = np.nonzero((np.abs(d) > 1e-7).any(1) | (np.abs(dn) > 1e-5).any(1))[0]
            t['POSITION'] = sparse(g, d, moved)
            if 'NORMAL' in t:
                t['NORMAL'] = sparse(g, dn, moved)
            done += 1
    compact(g)
    g.save(dst)
    return done


def sparse(g, d, idx):
    """A sparse VEC3 accessor of d at idx (zeros elsewhere), as Blender writes morph targets."""
    d = np.ascontiguousarray(d, np.float32)
    idx = np.ascontiguousarray(idx, np.uint32)
    vals = np.ascontiguousarray(d[idx], np.float32)
    views = []
    for arr in (idx, vals):
        g.bin += b'\0'*((4-len(g.bin) % 4) % 4)
        g.j['bufferViews'].append({'buffer': 0, 'byteOffset': len(g.bin), 'byteLength': arr.nbytes})
        g.bin += arr.tobytes()
        views.append(len(g.j['bufferViews'])-1)
    acc = {'componentType': 5126, 'count': int(len(d)), 'type': 'VEC3',
           'min': (d.min(0) if len(d) else np.zeros(3)).tolist(), 'max': (d.max(0) if len(d) else np.zeros(3)).tolist()}
    if len(idx):
        acc['sparse'] = {'count': int(len(idx)), 'indices': {'bufferView': views[0], 'componentType': 5125},
                         'values': {'bufferView': views[1]}}
    g.j['accessors'].append(acc)
    return len(g.j['accessors'])-1


def compact(g):
    """Drops accessors and buffer views nothing refers to and rewrites the binary with only the views in use."""
    used = set()
    for m in g.j['meshes']:
        for p in m['primitives']:
            used |= set(p['attributes'].values())
            if 'indices' in p:
                used.add(p['indices'])
            for t in p.get('targets', []):
                used |= set(t.values())
    for s in g.j.get('skins', []):
        if 'inverseBindMatrices' in s:
            used.add(s['inverseBindMatrices'])
    for a in g.j.get('animations', []):
        for smp in a['samplers']:
            used |= {smp['input'], smp['output']}
    amap = {old: new for new, old in enumerate(sorted(used))}
    accs = [g.j['accessors'][i] for i in sorted(used)]
    vused = set()
    for a in accs:
        if 'bufferView' in a:
            vused.add(a['bufferView'])
        if 'sparse' in a:
            vused |= {a['sparse']['indices']['bufferView'], a['sparse']['values']['bufferView']}
    for im in g.j.get('images', []):
        if 'bufferView' in im:
            vused.add(im['bufferView'])
    vmap, views, data = {}, [], bytearray()
    for i in sorted(vused):
        v = dict(g.j['bufferViews'][i])
        o = v.get('byteOffset', 0)
        data += b'\0'*((4-len(data) % 4) % 4)
        chunk = g.bin[o:o+v['byteLength']]
        v['byteOffset'] = len(data)
        data += chunk
        vmap[i] = len(views)
        views.append(v)
    for a in accs:
        if 'bufferView' in a:
            a['bufferView'] = vmap[a['bufferView']]
        if 'sparse' in a:
            a['sparse']['indices']['bufferView'] = vmap[a['sparse']['indices']['bufferView']]
            a['sparse']['values']['bufferView'] = vmap[a['sparse']['values']['bufferView']]
    for im in g.j.get('images', []):
        if 'bufferView' in im:
            im['bufferView'] = vmap[im['bufferView']]
    for m in g.j['meshes']:
        for p in m['primitives']:
            p['attributes'] = {k: amap[v] for k, v in p['attributes'].items()}
            if 'indices' in p:
                p['indices'] = amap[p['indices']]
            p['targets'] = [{k: amap[v] for k, v in t.items()} for t in p.get('targets', [])] or p.pop('targets', None)
            if p.get('targets') is None:
                p.pop('targets', None)
    for s in g.j.get('skins', []):
        if 'inverseBindMatrices' in s:
            s['inverseBindMatrices'] = amap[s['inverseBindMatrices']]
    for a in g.j.get('animations', []):
        for smp in a['samplers']:
            smp['input'], smp['output'] = amap[smp['input']], amap[smp['output']]
    g.j['accessors'], g.j['bufferViews'], g.bin = accs, views, data
