#!/usr/bin/env python3
"""Why the swinging quiver looks wrong: offline measurements on the converted base (no game).

    uv run --no-project --with numpy --with matplotlib python \
        reports/archery/2026-11-04-quiver-mount/research/analysis.py [--base <base dir>] [--out <dir>]

Reads HUMANS.MDH, the walk/run/sneak clips, the naked body and armour meshes, a sword and the arrow,
and the quiver model (OPENGOTHIC_CONTENT of reports/content/2026-11-quiver). Places the quiver as
OpenGothic/common/graphics/mesh/quiver.cpp does (Quiver::fit), and the candidates of research.md, and prints:
1. the right hand and forearm path in the pelvis axes, the clearance to the quiver and to the arrows over its
   mouth; the left hand against the sword on the left hip (ZS_SWORD) for comparison;
2. the pelvis's acceleration from the 25 fps keys as the engine interpolates them (linear) and smooth (cubic),
   the spectrum, and the engine's pendulum (Quiver::step, without obstacles and with the hand) on both;
3. the quiver's axis against the trunk (pelvis against BIP01 SPINE2) in the walk;
4. the arrow's profile against the tube.
Writes numbers.json and the plots into --out.
"""
from __future__ import annotations

import argparse
import glob
import json
import math
import os
import struct
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / "scripts/content/anim"))
import gothic_anim as ga  # noqa: E402

# quiver.h
LENGTH, MOUTH_R, BOTTOM_R = 67.0, 5.75, 4.75
MOUTH_H, MOUTH_BACK, TILT_BACK, TILT_OUT, CLEAR, MIN_OUT = 105.0, 4.0, 22.5, 7.5, 2.0, 22.0
SWING_F, SWING_D, SWING_MAX, MASS_AT = 1.8, 0.3, 35.0, 30.0
FPS = 60.0
CLIPS = ["S_WALKL", "S_RUNL", "S_SNEAKL", "S_BOWWALKL", "S_BOWRUNL", "S_BOWSNEAKL"]
OVERLAY_WALKS = ["HUMANS_RELAXED-S_WALKL", "HUMANS_MILITIA-S_WALKL", "HUMANS_ARROGANCE-S_WALKL", "HUMANS_TIRED-S_WALKL"]
ARMOURS = ["HUM_BODY_NAKED0", "ARMOR_LEATHER_L", "ARMOR_BDT_H", "ARMOR_MIL_M", "ARMOR_KDF_H", "ARMOR_PAL_H"]


def glb_points(path: Path, scale: float = 1.0) -> np.ndarray:
    b = path.read_bytes()
    jl = struct.unpack_from("<I", b, 12)[0]
    j = json.loads(b[20:20 + jl])
    bo = 20 + jl + 8
    out = []
    for m in j["meshes"]:
        for p in m["primitives"]:
            a = j["accessors"][p["attributes"]["POSITION"]]
            v = j["bufferViews"][a["bufferView"]]
            off = bo + v.get("byteOffset", 0) + a.get("byteOffset", 0)
            out.append(np.frombuffer(b, dtype="<f4", count=a["count"] * 3, offset=off).reshape(-1, 3))
    pts = np.concatenate(out).astype(np.float64) * scale
    pts[:, 0] *= -1  # the engine mirrors X
    return pts


def tr(m: np.ndarray) -> np.ndarray:
    return m[:3, 3].copy()


def basis(x, y, z, at) -> np.ndarray:
    m = np.eye(4)
    m[:3, 0], m[:3, 1], m[:3, 2], m[:3, 3] = x, y, z, at
    return m


def norm(v):
    return v / np.linalg.norm(v)


class Body:
    """Skeleton, body axes and a skinned mesh posed by linear blend skinning from the bind pose."""

    def __init__(self, base: Path):
        self.base = base
        self.skel = ga.read_skeleton(base / "models/HUMANS.MDH.json")
        self.bind = ga.world_pose(self.skel, None, 0)
        self.inv_bind = np.linalg.inv(self.bind)
        s = self.skel
        self.idx = {n.name: i for i, n in enumerate(s.nodes)}
        self.right = np.array([1.0, 0, 0]) if tr(self.bind[self.idx["BIP01 R THIGH"]])[0] > tr(self.bind[self.idx["BIP01 L THIGH"]])[0] else np.array([-1.0, 0, 0])
        self.fwd = np.array([0, 0, 1.0]) if tr(self.bind[self.idx["BIP01 R TOE0"]])[2] > tr(self.bind[self.idx["BIP01 R FOOT"]])[2] else np.array([0, 0, -1.0])
        self.up = np.array([0, 1.0, 0])
        self.pelvis = self.idx["BIP01 PELVIS"]
        self.meshes = {}

    def mesh(self, name: str):
        if name not in self.meshes:
            m = ga.read_skin_mesh(self.base / f"models/{name}.MDM.json", self.skel)
            W = np.zeros((len(m.positions), len(self.skel.nodes)))
            dom = np.zeros(len(m.positions), dtype=int)
            for v, ws in enumerate(m.weights):
                for node, w in ws:
                    W[v, node] += w
                dom[v] = max(ws, key=lambda t: t[1])[0]
            self.meshes[name] = (m.positions, W, dom)
        return self.meshes[name]

    def pose_points(self, name: str, pose: np.ndarray, sel=None) -> np.ndarray:
        pos, W, _ = self.mesh(name)
        if sel is not None:
            pos, W = pos[sel], W[sel]
        skin = pose @ self.inv_bind  # [nodes,4,4]
        used = np.nonzero(W.sum(0))[0]
        ph = np.c_[pos, np.ones(len(pos))]
        out = np.zeros((len(pos), 3))
        for n in used:
            w = W[:, n]
            nz = w > 0
            out[nz] += w[nz, None] * (ph[nz] @ skin[n].T)[:, :3]
        return out

    def group(self, name: str, parts) -> np.ndarray:
        _, _, dom = self.mesh(name)
        names = [self.skel.nodes[d].name for d in dom]
        return np.array([any(p in nm for p in parts) for nm in names])


def clip(base: Path, name: str):
    full = name if name.startswith("HUMANS") else f"HUMANS-{name}"
    return ga.read_animation(base / f"models/{full}.MAN.json")


def frame_pose(body: Body, anim, t: float, smooth: bool = False, unwrap: bool = False) -> np.ndarray:
    """The pose at time t (s) of a loop, keys interpolated as the engine does (linear), or cubic. unwrap: the
    root keeps going forward cycle after cycle by the clip's travel (the world path the engine moves the npc
    along key by key); else it jumps back each cycle (model space of one cycle)."""
    f = t * anim.fps
    n = anim.frame_count
    g0 = int(math.floor(f))
    a = f - g0
    travel = (anim.positions[-1, 0] - anim.positions[0, 0]) * n / (n - 1)
    travel[1] = 0

    def key(g):
        p = anim.positions[g % n].copy()
        if unwrap:
            p[0] = p[0] + travel * (g // n)
        return p

    if not smooth:
        p = (1 - a) * key(g0) + a * key(g0 + 1)
    else:
        Pm, P0, P1, P2 = key(g0 - 1), key(g0), key(g0 + 1), key(g0 + 2)
        p = 0.5 * ((2 * P0) + (-Pm + P1) * a + (2 * Pm - 5 * P0 + 4 * P1 - P2) * a * a
                   + (-Pm + 3 * P0 - 3 * P1 + P2) * a ** 3)
    q0, q1 = anim.rotations[g0 % n], anim.rotations[(g0 + 1) % n]
    if smooth:
        a = a * a * (3 - 2 * a)  # smoothstep for the rotations: no speed step at the keys
    dot = (q0 * q1).sum(-1, keepdims=True)
    q1 = np.where(dot < 0, -q1, q1)
    q = (1 - a) * q0 + a * q1
    q /= np.linalg.norm(q, axis=-1, keepdims=True)
    local = [nd.rest.copy() for nd in body.skel.nodes]
    for col, node in enumerate(anim.node_indices):
        if node < len(local):
            local[node] = ga.sample_matrix(p[col], q[col])
    out = np.zeros((len(local), 4, 4))
    for i, nd in enumerate(body.skel.nodes):
        out[i] = local[i] if nd.parent < 0 else out[nd.parent] @ local[i]
    return out


def quiver_frame(body: Body, bind_pts: np.ndarray, dom: np.ndarray, place: dict) -> tuple[np.ndarray, float]:
    """Quiver::fit for a place: the mouth at floor+height, `back` behind the axis and `side` to the right,
    pushed along `push` till the armour standing is `CLEAR` away; the bottom back and out by the tilts.
    Returns the quiver's frame in the pelvis bone's frame of the bind pose and the push."""
    up, right, fwd = body.up, body.right, body.fwd
    arm = np.array([any(p in body.skel.nodes[d].name for p in ("CLAVICLE", "UPPERARM", "FOREARM", "HAND", "FINGER", "HEAD", "NECK"))
                    for d in dom])
    floor = bind_pts[:, 1].min()
    pel = tr(body.bind[body.pelvis])
    mouth0 = np.array([pel[0], floor + place.get("height", MOUTH_H), pel[2]]) - fwd * place.get("back", MOUTH_BACK) \
        + right * place.get("side", 0.0)
    push = norm(place.get("push", right))
    down = norm(-up - fwd * math.tan(math.radians(place.get("tilt_back", TILT_BACK)))
                + push * math.tan(math.radians(place.get("tilt_out", TILT_OUT)))
                + right * math.tan(math.radians(place.get("tilt_right", 0.0))))
    rp = push - down * (push @ down)
    rl = np.linalg.norm(rp)
    rh = rp / rl
    rd = push @ down
    pts = bind_pts[~arm]
    s = 0.0
    for _ in range(6):
        w0 = pts - mouth0
        t0 = w0 @ down
        t = t0 - s * rd
        m = (t >= -CLEAR) & (t <= LENGTH + CLEAR)
        wp = w0[m] - np.outer(t0[m], down)
        a = wp @ rh
        b = np.linalg.norm(wp - np.outer(a, rh), axis=1)
        r = MOUTH_R + (BOTTOM_R - MOUTH_R) * np.clip(t[m] / LENGTH, 0, 1) + CLEAR
        ok = b < r
        need = ((a[ok] + np.sqrt(r[ok] ** 2 - b[ok] ** 2)) / rl).max() if ok.any() else 0.0
        if abs(need - s) < 0.01:
            s = need
            break
        s = need
    s = max(s, place.get("min_push", 0.0))
    mouth = mouth0 + push * s
    y = -down
    z = norm(fwd - y * (fwd @ y))
    x = np.cross(y, z)
    return np.linalg.inv(body.bind[body.pelvis]) @ basis(x, y, z, mouth), s


def tube_clearance(q: np.ndarray) -> np.ndarray:
    """cm from points in the quiver's frame to the tube (negative: inside), and to the arrows over the mouth
    (a cylinder of the fletching's 5.3 cm, 30 cm up)."""
    y = q[:, 1]
    rho = np.hypot(q[:, 0], q[:, 2])
    R = MOUTH_R + (BOTTOM_R - MOUTH_R) * np.clip(-y / LENGTH, 0, 1)
    side = rho - R
    below = np.where(y < -LENGTH, np.hypot(np.maximum(side, 0), -LENGTH - y), np.inf)
    above = np.where(y > 0, np.hypot(np.maximum(side, 0), y), np.inf)
    tube = np.where((y <= 0) & (y >= -LENGTH), side, np.minimum(below, above))
    fr = rho - 5.3
    fl = np.where((y >= 0) & (y <= 30), fr, np.hypot(np.maximum(fr, 0), np.where(y > 30, y - 30, -y)))
    return tube, fl


def to_body_axes(body: Body, pelvis_now: np.ndarray, p: np.ndarray) -> np.ndarray:
    """Points into the pelvis bone's frame, its axes turned to the body's (x right, y up, z forward) as in
    the bind pose; origin at the pelvis."""
    rel = (np.linalg.inv(pelvis_now) @ np.c_[p, np.ones(len(p))].T).T[:, :3]
    rb = body.bind[body.pelvis][:3, :3]
    axes = np.stack([body.right, body.up, body.fwd])
    return (axes @ (rb @ rel.T)).T


def pendulum(frames_pivot, frames_rest_dir, dt, obstacles=None):
    """Quiver::step on the trajectory of the ring and the rest direction (world), 60 fps, substeps 1/120 s.
    obstacles: per frame list of (point, radius) pushing the axis (the hand). Returns the swing in deg."""
    w = 2 * math.pi * SWING_F
    cosmax = math.cos(math.radians(SWING_MAX))
    pivot0 = frames_pivot[0]
    rest0 = pivot0 + frames_rest_dir[0] * MASS_AT
    mass, vel = rest0.copy(), np.zeros(3)
    last_p, last_r = pivot0, rest0
    out = [0.0]
    for k in range(1, len(frames_pivot)):
        pv, rd = frames_pivot[k], frames_rest_dir[k]
        ra = pv + rd * MASS_AT
        restv = (ra - last_r) / dt
        pivv = (pv - last_p) / dt
        n = max(1, math.ceil(dt * 120))
        h = dt / n
        for i in range(1, n + 1):
            kk = i / n
            p = last_p + (pv - last_p) * kk
            r = last_r + (ra - last_r) * kk
            acc = (r - mass) * w * w - (vel - restv) * (2 * SWING_D * w)
            vel = vel + acc * h
            mass = mass + vel * h
            d = norm(mass - p)
            rdn = norm(r - p)
            c = d @ rdn
            if c < cosmax:
                side = norm(d - rdn * c)
                d = rdn * cosmax + side * math.sqrt(1 - cosmax ** 2)
            mass = p + d * MASS_AT
            rel = vel - pivv
            vel = vel - d * (rel @ d)
        a = norm(mass - pv)
        if obstacles is not None:
            before = a.copy()
            for q, rad in obstacles[k]:
                wv = q - pv
                t = float(np.clip(wv @ a, 0, LENGTH))
                if t < 2:
                    continue
                perp = (pv + a * t) - q
                perp -= a * (perp @ a)
                dist = np.linalg.norm(perp)
                need = MOUTH_R + (BOTTOM_R - MOUTH_R) * t / LENGTH + rad + 0.5
                if dist < need and dist > 1e-3:
                    a = norm(a + perp / dist * ((need - dist) / t))
            if np.linalg.norm(a - before) > 1e-6:
                moved = pv + a * MASS_AT
                nn = norm(moved - mass)
                vn = (vel - pivv) @ nn
                if vn < 0:
                    vel = vel - nn * vn
                mass = moved
        out.append(math.degrees(math.acos(float(np.clip(a @ rd, -1, 1)))))
        last_p, last_r = pv, ra
    return np.array(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--base", type=Path, default=Path(sorted(glob.glob(os.path.expanduser("~/.cache/gothic-ai/base/*/models")))[0]).parent)
    ap.add_argument("--quiver", type=Path, default=Path("~/.local/share/gothic-ai/content-staging/quiver/content/meshes/quiver/model.glb").expanduser())
    ap.add_argument("--out", type=Path, default=Path(__file__).resolve().parent)
    args = ap.parse_args()
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    body = Body(args.base)
    N = {}

    # --- 4. arrows against the tube
    arrow = glb_points(args.base / "meshes/ITRW_ARROW.glb")
    tipx = arrow[:, 0].min()
    from_tip = arrow[:, 0] - tipx
    r_shaft = np.hypot(arrow[:, 1], arrow[:, 2])
    bbox_c = (arrow[:, 1:].min(0) + arrow[:, 1:].max(0)) / 2
    head = r_shaft[from_tip < 17].max()
    quiv = glb_points(args.quiver, 100.0)
    rq = np.hypot(quiv[:, 0], quiv[:, 2])
    inner = {}
    for y0 in range(0, -66, -6):
        m = (quiv[:, 1] <= y0) & (quiv[:, 1] > y0 - 6) & (rq > 2) & (rq < 7)
        inner[y0] = float(rq[m].min()) if m.any() else None
    rin = MOUTH_R - 0.4 - 1.0
    ring12 = rin * math.sqrt(11.5 / 12)
    N["arrow"] = {"length": float(from_tip.max()), "head_half_width": float(head), "head_length": 16.0,
                  "fletch_radius": float(r_shaft[from_tip > 74].max()), "bbox_centre_yz": bbox_c.round(2).tolist(),
                  "layout_ring": rin, "layout_ring_12": ring12, "tube_inner_by_depth": inner,
                  "tip_depth_below_mouth": float(LENGTH - 1), "head_out_of_wall_worst":
                      float(ring12 + np.linalg.norm(bbox_c) + head - (inner[-60] or BOTTOM_R - 0.4))}

    # --- places: now and the candidates
    naked_pos, _, naked_dom = body.mesh("HUM_BODY_NAKED0")
    def around(deg):  # from the body's axis: deg behind the right side
        return body.right * math.cos(math.radians(deg)) - body.fwd * math.sin(math.radians(deg))
    places = {
        "now": {"min_push": MIN_OUT},
        "behind30": {"back": 0.0, "push": around(30), "tilt_back": 15.0, "tilt_out": 6.0, "min_push": 20.0},
        "behind60": {"back": 0.0, "push": around(60), "tilt_back": 12.0, "tilt_out": 8.0, "min_push": 17.0},
        "lumbar": {"back": 0.0, "side": 6.0, "height": 108.0, "push": around(90), "tilt_back": 8.0, "tilt_out": 0.0,
                   "tilt_right": -55.0, "min_push": 16.0},
    }
    frames = {}
    for name in ARMOURS:
        pos, _, dom = body.mesh(name)
        for pn, pl in places.items():
            f, s = quiver_frame(body, pos, dom, pl)
            frames[(name, pn)] = (f, s)
    N["places"] = {pn: {name: round(frames[(name, pn)][1], 1) for name in ARMOURS} for pn in places}

    # --- 1. hands, legs against each place, through the clips; the sword for the left hand
    rhand = body.group("HUM_BODY_NAKED0", ("R HAND", "R FINGER"))
    rfore = body.group("HUM_BODY_NAKED0", ("R FOREARM",))
    lhand = body.group("HUM_BODY_NAKED0", ("L HAND", "L FINGER"))
    rleg = body.group("HUM_BODY_NAKED0", ("R THIGH", "R CALF"))
    sword = glb_points(args.base / "meshes/ITMW_025_1H_MIL_SWORD_BROAD_01.glb")
    zs = body.idx["ZS_SWORD"]
    clipres = {}
    paths = {}
    for cn in CLIPS + OVERLAY_WALKS:
        try:
            an = clip(args.base, cn)
        except FileNotFoundError:
            continue
        T = an.duration
        ts = np.arange(0, T, 1 / FPS)
        res = {pn: {"hand": 1e9, "fore": 1e9, "hand_fletch": 1e9, "leg": 1e9} for pn in places}
        sword_gap = 1e9
        hp, sp = [], []
        for t in ts:
            pose = frame_pose(body, an, t)
            pel = pose[body.pelvis]
            hand = body.pose_points("HUM_BODY_NAKED0", pose, rhand)
            fore = body.pose_points("HUM_BODY_NAKED0", pose, rfore)
            leg = body.pose_points("HUM_BODY_NAKED0", pose, rleg)
            for pn in places:
                f, _ = frames[("ARMOR_LEATHER_L", pn)]
                inv = np.linalg.inv(pel @ f)
                for key, pts in (("hand", hand), ("fore", fore), ("leg", leg)):
                    q = (inv @ np.c_[pts, np.ones(len(pts))].T).T[:, :3]
                    tube, fl = tube_clearance(q)
                    res[pn][key] = min(res[pn][key], float(tube.min()))
                    if key == "hand":
                        res[pn]["hand_fletch"] = min(res[pn]["hand_fletch"], float(fl.min()))
            lh = body.pose_points("HUM_BODY_NAKED0", pose, lhand)
            sw = (pose[zs] @ np.c_[sword, np.ones(len(sword))].T).T[:, :3]
            d = np.linalg.norm(lh[:, None, :] - sw[None, ::3, :], axis=2).min()
            sword_gap = min(sword_gap, float(d))
            hp.append(to_body_axes(body, pel, tr(pose[body.idx["BIP01 R HAND"]])[None])[0])
            sp.append(to_body_axes(body, pel, sw[::3]).mean(0))
        paths[cn] = np.array(hp)
        hpa = np.array(hp)
        mouth = to_body_axes(body, body.bind[body.pelvis], tr(body.bind[body.pelvis] @ frames[("ARMOR_LEATHER_L", "now")][0])[None])[0]
        clipres[cn] = {"places": {pn: {k: round(v, 1) for k, v in r.items()} for pn, r in res.items()},
                       "left_hand_to_sword_min": round(sword_gap, 1),
                       "r_hand_x_range": [round(float(hpa[:, 0].min()), 1), round(float(hpa[:, 0].max()), 1)],
                       "r_hand_z_range": [round(float(hpa[:, 2].min()), 1), round(float(hpa[:, 2].max()), 1)],
                       "r_hand_y_range": [round(float(hpa[:, 1].min()), 1), round(float(hpa[:, 1].max()), 1)],
                       "sword_centre_body_axes": np.array(sp).mean(0).round(1).tolist(),
                       "quiver_mouth_body_axes": mouth.round(1).tolist()}
    N["clips"] = clipres
    pose0 = frame_pose(body, clip(args.base, "S_WALKL"), 0.0)
    swb = to_body_axes(body, pose0[body.pelvis], (pose0[zs] @ np.c_[sword, np.ones(len(sword))].T).T[:, :3])
    N["sword_body_axes_bbox"] = [swb.min(0).round(1).tolist(), swb.max(0).round(1).tolist()]

    # the right hand path against the quiver (top view and side view, body axes, pelvis origin)
    fig, ax = plt.subplots(1, 2, figsize=(11, 5))
    for cn in ("S_WALKL", "S_RUNL", "S_SNEAKL", "HUMANS_RELAXED-S_WALKL"):
        if cn in paths:
            p = paths[cn]
            ax[0].plot(p[:, 0], p[:, 2], label=cn)
            ax[1].plot(p[:, 2], p[:, 1], label=cn)
    for pn, col in (("now", "k"), ("behind30", "tab:red"), ("behind60", "tab:purple"), ("lumbar", "tab:green")):
        f, _ = frames[("ARMOR_LEATHER_L", pn)]
        seg = np.array([[0, 0, 0, 1], [0, -LENGTH, 0, 1]]) @ f.T
        sb = to_body_axes(body, body.bind[body.pelvis], (body.bind[body.pelvis] @ seg.T).T[:, :3])
        ax[0].plot(sb[:, 0], sb[:, 2], col, lw=8, alpha=0.35, label=f"quiver {pn}")
        ax[1].plot(sb[:, 2], sb[:, 1], col, lw=8, alpha=0.35)
    ax[0].set_xlabel("x right, cm"); ax[0].set_ylabel("z forward, cm"); ax[0].set_title("right hand from above (pelvis axes)")
    ax[1].set_xlabel("z forward, cm"); ax[1].set_ylabel("y up from the pelvis, cm"); ax[1].set_title("from the right side")
    for a_ in ax:
        a_.set_aspect("equal"); a_.grid(alpha=0.3)
    ax[0].legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(out / "hand-path.png", dpi=110)
    plt.close(fig)

    # --- 2. the pelvis's acceleration and the pendulum in the walk; 3. the axis against the trunk
    an = clip(args.base, "S_WALKL")
    cycles = 4
    ts = np.arange(0, an.duration * cycles, 1 / FPS)
    f_now, _ = frames[("ARMOR_LEATHER_L", "now")]
    spine2 = body.idx["BIP01 SPINE2"]
    variants = {}
    for smooth in (False, True):
        piv, rdir, twist, mass_acc, azim = [], [], [], [], []
        hand_obs = []
        for t in ts:
            pose = frame_pose(body, an, t, smooth, unwrap=True)
            q = pose[body.pelvis] @ f_now
            piv.append(tr(q))
            rdir.append(norm(q[:3, :3] @ np.array([0, -1.0, 0])))
            # trunk against the pelvis: yaw of the chest's forward against the pelvis's, about up
            fp = (pose[body.pelvis][:3, :3] @ body.bind[body.pelvis][:3, :3].T) @ body.fwd
            fc = (pose[spine2][:3, :3] @ body.bind[spine2][:3, :3].T) @ body.fwd
            twist.append(math.degrees(math.atan2(np.cross(fp, fc) @ body.up, fp @ fc)))
            # where the quiver's bottom points, about up, against the chest's backward (0: straight back)
            bh = rdir[-1] - body.up * (rdir[-1] @ body.up)
            back_c = -fc
            azim.append(math.degrees(math.atan2(np.cross(back_c, bh) @ body.up, back_c @ bh)))
            hand = body.pose_points("HUM_BODY_NAKED0", pose, rhand | rfore)
            hand_obs.append([(p_, 0.0) for p_ in hand[::4]])
        piv, rdir = np.array(piv), np.array(rdir)
        restpt = piv + rdir * MASS_AT
        acc = np.diff(restpt, 2, axis=0) * FPS * FPS / 100.0  # m/s^2
        free = pendulum(piv, rdir, 1 / FPS)
        pushed = pendulum(piv, rdir, 1 / FPS, hand_obs)
        jerk_free = np.abs(np.diff(free, 2)) * FPS * FPS
        jerk_push = np.abs(np.diff(pushed, 2)) * FPS * FPS
        spec = np.abs(np.fft.rfft(np.linalg.norm(acc, axis=1) - np.linalg.norm(acc, axis=1).mean()))
        freqs = np.fft.rfftfreq(len(acc), 1 / FPS)
        sspec = np.abs(np.fft.rfft(pushed - pushed.mean()))
        sfreq = np.fft.rfftfreq(len(pushed), 1 / FPS)
        key = "cubic" if smooth else "linear (engine)"
        variants[key] = dict(ts=ts, acc=acc, free=free, pushed=pushed, spec=(freqs, spec), sspec=(sfreq, sspec), twist=np.array(twist),
                             axis_vs_trunk=None)
        top = np.argsort(spec[1:])[::-1][:5] + 1
        stop = np.argsort(sspec[1:])[::-1][:5] + 1
        N.setdefault("walk", {})[key] = {
            "rest_point_acc_ms2": {"rms": round(float(np.sqrt((np.linalg.norm(acc, axis=1) ** 2).mean())), 2),
                                   "max": round(float(np.linalg.norm(acc, axis=1).max()), 1)},
            "acc_peaks_hz": [round(float(freqs[i]), 2) for i in top],
            "swing_free_deg": {"max": round(float(free[60:].max()), 2), "p2p": round(float(np.ptp(free[60:])), 2)},
            "swing_hand_deg": {"max": round(float(pushed[60:].max()), 2), "p2p": round(float(np.ptp(pushed[60:])), 2)},
            "swing_peaks_hz": [round(float(sfreq[i]), 2) for i in stop],
            "angular_jerk_free_deg_s2": {"p95": round(float(np.percentile(jerk_free[60:], 95)), 0), "max": round(float(jerk_free[60:].max()), 0)},
            "angular_jerk_hand_deg_s2": {"p95": round(float(np.percentile(jerk_push[60:], 95)), 0), "max": round(float(jerk_push[60:].max()), 0)},
            "pelvis_vs_chest_twist_deg": [round(float(np.min(twist)), 1), round(float(np.max(twist)), 1)],
            "bottom_azimuth_vs_chest_back_deg": [round(float(np.min(azim)), 1), round(float(np.max(azim)), 1)],
            "max_step_deg_per_frame": {"free": round(float(np.abs(np.diff(free[60:])).max()), 2),
                                       "hand": round(float(np.abs(np.diff(pushed[60:])).max()), 2)},
        }

    fig, ax = plt.subplots(3, 1, figsize=(11, 10))
    for key, v in variants.items():
        ax[0].plot(v["ts"][2:], np.linalg.norm(v["acc"], axis=1), lw=0.8, label=key)
        ax[1].plot(v["ts"], v["free"], lw=1, label=f"{key}: no obstacles")
        ax[1].plot(v["ts"], v["pushed"], lw=1, ls="--", label=f"{key}: the right hand pushes")
        ax[2].semilogy(*v["sspec"], lw=0.8, label=key)
    ax[0].set_ylabel("|acc| of the rest point, m/s²"); ax[0].set_title("S_WALKL, 60 fps: the quiver's rest point (30 cm down the axis)")
    ax[1].set_ylabel("swing from the rest, deg"); ax[1].set_xlabel("s")
    ax[2].set_ylabel("swing spectrum"); ax[2].set_xlabel("Hz"); ax[2].set_xlim(0, 30)
    for a_ in ax:
        a_.grid(alpha=0.3); a_.legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(out / "walk-swing.png", dpi=110)
    plt.close(fig)

    fig, ax = plt.subplots(figsize=(10, 3.5))
    ax.plot(variants["linear (engine)"]["ts"], variants["linear (engine)"]["twist"])
    ax.set_ylabel("chest yaw against the pelvis, deg"); ax.set_xlabel("s"); ax.grid(alpha=0.3)
    ax.set_title("S_WALKL: the pelvis turns against the shoulders (the quiver follows the pelvis)")
    fig.tight_layout()
    fig.savefig(out / "walk-twist.png", dpi=110)
    plt.close(fig)

    # --- reach for the reload: the right shoulder to each mouth in the aim pose
    aim = clip(args.base, "S_BOWSHOOT")
    pose = frame_pose(body, aim, 0.0)
    sh = tr(pose[body.idx["BIP01 R UPPERARM"]])
    hand = tr(pose[body.idx["BIP01 R HAND"]])
    arm_len = np.linalg.norm(tr(body.bind[body.idx["BIP01 R FOREARM"]]) - tr(body.bind[body.idx["BIP01 R UPPERARM"]])) + \
        np.linalg.norm(tr(body.bind[body.idx["BIP01 R HAND"]]) - tr(body.bind[body.idx["BIP01 R FOREARM"]])) + 8.0
    N["reach"] = {"arm_cm": round(float(arm_len), 1)}
    for pn in places:
        f, _ = frames[("ARMOR_LEATHER_L", pn)]
        mouth = tr(pose[body.pelvis] @ f)
        bm = to_body_axes(body, pose[body.pelvis], mouth[None])[0]
        N["reach"][pn] = {"shoulder_to_mouth": round(float(np.linalg.norm(mouth - sh)), 1),
                          "hand_to_mouth": round(float(np.linalg.norm(mouth - hand)), 1),
                          "mouth_body_axes": bm.round(1).tolist()}

    (out / "numbers.json").write_text(json.dumps(N, indent=1, ensure_ascii=False, default=float) + "\n")
    print(json.dumps(N, indent=1, ensure_ascii=False, default=float))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
