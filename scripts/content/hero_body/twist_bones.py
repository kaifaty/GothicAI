"""Twist bones on MPFB's game_engine armature in Blender (rig_parts.TWISTS): the bones are added under their parents
and each parent's skin weight spreads along the bone over itself and its twist bones (rig_parts.TWIST_NODES).
Used by mpfb_export.py (RIG=1) before it reads the bones and weights, and by rig_preview.py (TWIST=1).
"""
import bpy
import numpy as np
import rig_parts as rp


def add(arm, body, co):
    """arm - the armature at the origin, body - its skinned mesh, co - the body's rest vertices in armature space (m).
    Returns the number of skin weights spread; 0 when the armature has its twist bones already."""
    if rp.twists(rp.SIDES[0])[0][0] in arm.data.bones:
        return 0
    bpy.context.view_layer.objects.active = arm
    bpy.ops.object.mode_set(mode='EDIT')
    eb = arm.data.edit_bones
    for sd in rp.SIDES:
        for name, parent, _of, _ag, axis, t, _sh in rp.twists(sd):
            pe = eb[parent]
            m = pe.matrix.copy(); m.translation = pe.head + t * (eb[axis].head - pe.head)
            b = eb.new(name); b.head = (0, 0, 0); b.tail = (0, 0.04, 0); b.matrix = m; b.length = 0.04
            b.parent = pe; b.use_deform = True; b.use_connect = False
    bpy.ops.object.mode_set(mode='OBJECT')
    rest = {b.name: np.array(b.matrix_local) for b in arm.data.bones}
    vg = body.vertex_groups
    moved = 0
    for sd in rp.SIDES:
        for parent, nodes in rp.TWIST_NODES.items():
            axis = rp.TWIST_AXIS[parent].format(s=sd); parent = parent.format(s=sd)
            nodes = [(t, n.format(s=sd)) for t, n in nodes]
            h = rest[parent][:3, 3]; ax = rest[axis][:3, 3] - h; L = np.linalg.norm(ax); ax = ax / L
            g = vg[parent]; groups = {n: (vg.get(n) or vg.new(name=n)) for _, n in nodes}
            for v in body.data.vertices:
                w = next((x.weight for x in v.groups if x.group == g.index), 0.0)
                if w <= 0:
                    continue
                t = min(1.0, max(0.0, float((co[v.index] - h) @ ax) / L))
                share = {n: 0.0 for _, n in nodes}
                for (t0, n0), (t1, n1) in zip(nodes, nodes[1:]):
                    if t0 <= t <= t1:
                        k = (t - t0) / (t1 - t0); share[n0] += 1 - k; share[n1] += k
                        break
                for n, k in share.items():
                    if k > 0:
                        groups[n].add([v.index], w * k, 'REPLACE')
                    elif n == parent:
                        g.remove([v.index])
                moved += 1
    return moved
