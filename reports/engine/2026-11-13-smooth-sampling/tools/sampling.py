"""Ways to sample a clip between its keys, tried outside the game (reports/engine/2026-11-13-smooth-sampling).

Keys are uniform in time. `sample` gives the pose at any time in key units by one of:
  linear  - what the engine did: lerp of positions, slerp of rotations between the two keys around the time
  cr      - cubic Hermite with Catmull-Rom tangents (half the chord over the two neighbours); a rotation as four
            numbers of its quaternion, the result normalized (glTF CUBICSPLINE, Unity curves)
  crlim   - cr whose tangent is not longer than 3 x the shorter of the two chords at the key: a key next to a hold
            gets a zero tangent, so a hold stays a hold and a sharp stop does not swing past its key
  mono    - Fritsch-Carlson per number: zero tangent where a number turns, else cr limited to 3 x the smaller step
  squad   - Shoemake's spherical cubic for rotations (positions as cr)
Pure numpy: runs anywhere the base is.
"""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "scripts/content/anim"))
import gothic_anim as ga  # noqa: E402

METHODS = ("linear", "cr", "crlim", "mono", "squad")


def _norm(v):
    return np.sqrt((v * v).sum(-1, keepdims=True))


def _align(ref, q):
    s = np.where((ref * q).sum(-1, keepdims=True) < 0, -1.0, 1.0)
    return q * s


def qmul(a, b):
    aw, ax, ay, az = np.moveaxis(a, -1, 0)
    bw, bx, by, bz = np.moveaxis(b, -1, 0)
    return np.stack([aw * bw - ax * bx - ay * by - az * bz,
                     aw * bx + ax * bw + ay * bz - az * by,
                     aw * by - ax * bz + ay * bw + az * bx,
                     aw * bz + ax * by - ay * bx + az * bw], -1)


def qconj(q):
    return q * np.array([1.0, -1.0, -1.0, -1.0])


def qlog(q):
    """Rotation vector / 2 of a unit quaternion (short way)."""
    q = np.where(q[..., :1] < 0, -q, q)
    s = _norm(q[..., 1:])
    ang = np.arctan2(s, q[..., :1])
    k = np.where(s > 1e-9, ang / np.maximum(s, 1e-9), 1.0)
    return q[..., 1:] * k


def qexp(v):
    ang = _norm(v)
    k = np.where(ang > 1e-9, np.sin(ang) / np.maximum(ang, 1e-9), 1.0)
    return np.concatenate([np.cos(ang), v * k], -1)


def slerp(a, b, t):
    b = _align(a, b)
    c = np.clip((a * b).sum(-1, keepdims=True), -1.0, 1.0)
    ang = np.arccos(c)
    s = np.sin(ang)
    lin = s < 1e-6
    s = np.where(lin, 1.0, s)
    ka = np.where(lin, 1.0 - t, np.sin((1.0 - t) * ang) / s)
    kb = np.where(lin, t, np.sin(t * ang) / s)
    out = a * ka + b * kb
    return out / _norm(out)


def _hermite(p1, p2, m1, m2, t):
    t2, t3 = t * t, t * t * t
    return (2 * t3 - 3 * t2 + 1) * p1 + (t3 - 2 * t2 + t) * m1 + (-2 * t3 + 3 * t2) * p2 + (t3 - t2) * m2


def _tangents(p0, p1, p2, p3, method):
    d0, d1, d2 = p1 - p0, p2 - p1, p3 - p2
    m1, m2 = 0.5 * (d0 + d1), 0.5 * (d1 + d2)
    if method == "crlim":
        for m, a, b in ((m1, d0, d1), (m2, d1, d2)):
            lim = 3.0 * np.minimum(_norm(a), _norm(b))
            n = _norm(m)
            m *= np.where(n > lim, lim / np.maximum(n, 1e-12), 1.0)
    elif method == "mono":
        def mono(m, a, b):
            lim = 3.0 * np.minimum(np.abs(a), np.abs(b))
            return np.where(a * b <= 0, 0.0, np.sign(m) * np.minimum(np.abs(m), lim))
        m1, m2 = mono(m1, d0, d1), mono(m2, d1, d2)
    return m1, m2


def sample(pos, rot, u, method="linear", loop=False, ends="engine"):
    """pos [K, N, 3], rot [K, N, 4] keys; u [T] times in key units. Returns pos [T, N, 3], rot [T, N, 4].

    ends - what stands for the key a clip that is no loop does not have outside its first and last one:
      engine  mirror at the start, rest at the end (what the engine has)
      rest    the outer key again: the clip starts from rest and comes to rest (zero tangent there)
      mirror  the next key mirrored: the outer chord goes on at its speed, as on the straight line
      half    the outer key again, without the limit there: half the chord's speed (plain Catmull-Rom's end)
    """
    u = np.asarray(u, dtype=np.float64)
    count = pos.shape[0]
    i = np.floor(u).astype(np.int64)
    t = (u - i)[:, None, None]
    if loop:
        idx = [(i + k) % count for k in (-1, 0, 1, 2)]
        ext = [False] * 4
    else:
        last = count - 1
        i = np.clip(i, 0, last)
        idx = [np.clip(i + k, 0, last) for k in (-1, 0, 1, 2)]
        ext = [i - 1 < 0, None, None, i + 2 > last]
    p = [pos[k] for k in idx]
    q = [rot[k] for k in idx]
    q[0] = _align(q[1], q[0])
    q[2] = _align(q[1], q[2])
    q[3] = _align(q[2], q[3])
    if method == "linear":
        return p[1] + (p[2] - p[1]) * t, slerp(q[1], q[2], t)
    lo = hi = None
    if not loop:
        lo, hi = ext[0][:, None, None], ext[3][:, None, None]
        if ends in ("mirror", "engine"):
            p[0] = np.where(lo, 2 * p[1] - p[2], p[0])
            q[0] = np.where(lo, 2 * q[1] - q[2], q[0])
        if ends == "mirror":
            p[3] = np.where(hi, 2 * p[2] - p[1], p[3])
            q[3] = np.where(hi, 2 * q[2] - q[1], q[3])

    def tangents(x, m):
        a, b = _tangents(*x, m)
        if lo is not None and ends == "half":
            a = np.where(lo, 0.5 * (x[2] - x[1]), a)
            b = np.where(hi, 0.5 * (x[2] - x[1]), b)
        return a, b

    pm = tangents(p, "cr" if method == "squad" else method)
    out_p = _hermite(p[1], p[2], pm[0], pm[1], t)
    if method == "squad":
        def inner(a, b, c):  # the control point at b between a and c
            ib = qconj(b)
            return qmul(b, qexp(-0.25 * (qlog(qmul(ib, c)) + qlog(qmul(ib, a)))))
        qn = [x / _norm(x) for x in q]
        s1, s2 = inner(qn[0], qn[1], qn[2]), inner(qn[1], qn[2], qn[3])
        out_q = slerp(slerp(qn[1], qn[2], t), slerp(s1, s2, t), 2 * t * (1 - t))
    else:
        qm = tangents(q, method)
        out_q = _hermite(q[1], q[2], qm[0], qm[1], t)
        out_q = out_q / _norm(out_q)
    return out_p, out_q


def quat_matrices(q):
    w, x, y, z = np.moveaxis(q, -1, 0)
    m = np.empty(q.shape[:-1] + (3, 3))
    m[..., 0, 0] = 1 - 2 * (y * y + z * z); m[..., 0, 1] = 2 * (x * y - w * z); m[..., 0, 2] = 2 * (x * z + w * y)
    m[..., 1, 0] = 2 * (x * y + w * z); m[..., 1, 1] = 1 - 2 * (x * x + z * z); m[..., 1, 2] = 2 * (y * z - w * x)
    m[..., 2, 0] = 2 * (x * z - w * y); m[..., 2, 1] = 2 * (y * z + w * x); m[..., 2, 2] = 1 - 2 * (x * x + y * y)
    return m


def world(skel, node_indices, pos, rot):
    """Model-space 4x4 of every node for sampled poses: pos [T, C, 3], rot [T, C, 4] of the clip's nodes."""
    frames = pos.shape[0]
    local = np.repeat(np.stack([n.rest for n in skel.nodes])[None], frames, 0)
    r = quat_matrices(rot)
    for col, node in enumerate(node_indices):
        if node < len(skel.nodes):
            local[:, node, :3, :3] = r[:, col]
            local[:, node, :3, 3] = pos[:, col]
            local[:, node, 3] = (0, 0, 0, 1)
    out = np.empty_like(local)
    for i, n in enumerate(skel.nodes):
        out[:, i] = local[:, i] if n.parent < 0 else out[:, n.parent] @ local[:, i]
    return out


def second_difference(p):
    """|p[k] - 2 p[k-1] + p[k-2]| of a track [T, 3]: the change of the step between two frames, cm."""
    return np.linalg.norm(p[2:] - 2 * p[1:-1] + p[:-2], axis=-1)


def load(base, model, name):
    models = Path(base) / "models"
    return ga.read_animation(models / f"{model}-{name}.MAN.json")
