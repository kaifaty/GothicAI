"""Checks of the animation tools: MAN record round trip and the SOMA retarget's bone directions."""
import os
import sys
from pathlib import Path

import numpy as np
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402


def base_models() -> Path | None:
    root = Path(os.environ.get("OPENGOTHIC_BASE_CACHE", Path.home() / ".cache/gothic-ai/base"))
    for d in sorted(root.glob("*/models"), key=lambda p: p.stat().st_mtime, reverse=True):
        if (d / "HUMANS.MDH.json").exists() and (d / "HUMANS_1HST1-S_1H.MAN.json").exists():
            return d
    return None


def random_quats(rng, shape):
    q = rng.normal(size=shape + (4,))
    return q / np.linalg.norm(q, axis=-1, keepdims=True)


def test_record_round_trip(tmp_path):
    rng = np.random.default_rng(1)
    anim = ga.Animation(name="T_TEST", next="S_RUN", layer=1, fps=25.0, frame_count=5,
                        node_indices=[0, 3, 7], positions=rng.normal(size=(5, 3, 3)) * 50,
                        rotations=random_quats(rng, (5, 3)),
                        events=[{"type": "combo_window", "frames": [1, 4]}])
    path = tmp_path / "HUMANS-T_TEST.MAN.json"
    ga.write_animation(path, anim, source="test")
    back = ga.read_animation(path)
    assert (back.name, back.next, back.frame_count, back.node_indices) == ("T_TEST", "S_RUN", 5, [0, 3, 7])
    assert np.allclose(back.positions, anim.positions, atol=1e-3)
    # Equal rotations up to the quaternion sign.
    dots = np.abs((back.rotations * anim.rotations).sum(-1))
    assert np.allclose(dots, 1.0, atol=1e-6)
    assert back.events == anim.events


def synthetic_soma(rng, frames: int) -> tuple[np.ndarray, np.ndarray]:
    """A SOMA-like 30-joint body: T-pose offsets (metres, world-aligned rest) under random rotations."""
    up, left, fwd = np.array([0, 1.0, 0]), np.array([1.0, 0, 0]), np.array([0, 0, 1.0])
    off = {"Hips": np.zeros(3), "Spine1": up * .1, "Spine2": up * .1, "Chest": up * .15, "Neck1": up * .2,
           "Neck2": up * .05, "Head": up * .08, "Jaw": fwd * .05, "LeftEye": up * .05 + left * .03,
           "RightEye": up * .05 - left * .03}
    for side, s in (("Left", 1), ("Right", -1)):
        off.update({f"{side}Shoulder": left * s * .04 + up * .02, f"{side}Arm": left * s * .15,
                    f"{side}ForeArm": left * s * .28, f"{side}Hand": left * s * .26,
                    f"{side}HandThumbEnd": left * s * .05 + fwd * .04, f"{side}HandMiddleEnd": left * s * .18,
                    f"{side}Leg": left * s * .09 - up * .05, f"{side}Shin": -up * .42,
                    f"{side}Foot": -up * .42, f"{side}ToeBase": fwd * .14 - up * .06})
    pos = np.zeros((frames, 30, 3))
    rot = np.zeros((frames, 30, 3, 3))
    for t in range(frames):
        g = {}
        for j, n in enumerate(rs.SOMA30):
            q = random_quats(rng, ())
            q[1:] *= 0.35  # moderate joint rotations
            local = ga.quat_matrix(q / np.linalg.norm(q))
            p = rs.SOMA30_PARENT[n]
            g[n] = local if p is None else g[p] @ local
            rot[t, j] = g[n]
            pos[t, j] = np.array([0, 0.95, 0]) if p is None else pos[t, rs.SOMA30.index(p)] + g[p] @ off[n]
    return pos, rot


@pytest.mark.skipif(base_models() is None, reason="needs a converted base with the human model")
def test_retarget_keeps_bone_directions(tmp_path):
    models = base_models()
    skel = ga.read_skeleton(models / "HUMANS.MDH.json")
    ref = ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json")
    pos, rot = synthetic_soma(np.random.default_rng(7), 6)
    npz = tmp_path / "soma.npz"
    np.savez(npz, posed_joints=pos, global_rot_mats=rot)
    anim = rs.retarget(npz, skel, "T_TEST", "", 30.0, 30.0, None, ref)
    gskel = rs.with_offsets(skel, ref)
    child = {"BIP01 L UPPERARM": "BIP01 L FOREARM", "BIP01 R FOREARM": "BIP01 R HAND",
             "BIP01 L THIGH": "BIP01 L CALF", "BIP01 R CALF": "BIP01 R FOOT", "BIP01 SPINE1": "BIP01 SPINE2"}
    for f in range(anim.frame_count):
        world = ga.world_pose(gskel, anim, f)
        for bone, c in child.items():
            s, sc = rs.MAP[bone]
            dg = world[gskel.index(c)][:3, 3] - world[gskel.index(bone)][:3, 3]
            ds = rs.F @ (pos[f, rs.SOMA30.index(sc)] - pos[f, rs.SOMA30.index(s)])
            cos = dg @ ds / np.linalg.norm(dg) / np.linalg.norm(ds)
            assert cos > np.cos(np.radians(0.5)), (f, bone)


@pytest.mark.skipif(base_models() is None, reason="needs a converted base with the human model")
def test_stance_survives_soma_and_back(tmp_path):
    """The 1H stance → SOMA keyframe → retarget lands within millimetres of the original."""
    models = base_models()
    skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
    ref = ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json")
    skel = rs.with_offsets(skel0, ref)
    pos, rot = synthetic_soma(np.random.default_rng(3), 1)
    rig = rs.soma_rig(skel, pos[0], rot[0], rs.SOMA30, ref)
    local, hips = rs.gothic_to_soma(skel, ref, 0, rig, rs.SOMA30)
    offsets = {n: (rot[0, rs.SOMA30.index(p)].T @ (pos[0, j] - pos[0, rs.SOMA30.index(p)]) if p else np.zeros(3))
               for j, (n, p) in enumerate(rs.SOMA30_PARENT.items())}
    g, p = {}, {}
    for j, n in enumerate(rs.SOMA30):
        par = rs.SOMA30_PARENT[n]
        ang = np.linalg.norm(local[j])
        q = np.array([1.0, 0, 0, 0]) if ang < 1e-12 else np.concatenate([[np.cos(ang / 2)], local[j] / ang * np.sin(ang / 2)])
        m = ga.quat_matrix(q)
        g[n] = m if par is None else g[par] @ m
        p[n] = hips if par is None else p[par] + g[par] @ offsets[n]
    npz = tmp_path / "stance.npz"
    np.savez(npz, posed_joints=np.array([[p[n] for n in rs.SOMA30]] * 2),
             global_rot_mats=np.array([[g[n] for n in rs.SOMA30]] * 2))
    back = rs.retarget(npz, skel0, "T", "", 30.0, 30.0, None, ref)
    w0, w1 = ga.world_pose(skel, ref, 0), ga.world_pose(skel, back, 0)
    for n in ("BIP01 R HAND", "BIP01 L HAND", "BIP01 R FOOT", "BIP01 L FOOT", "BIP01 HEAD"):
        i = skel.index(n)
        assert np.linalg.norm(w0[i][:3, 3] - w1[i][:3, 3]) < 1.0, n  # cm
