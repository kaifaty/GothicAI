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
    anim = rs.retarget(npz, skel, "T_TEST", "", 30.0, 30.0, None, ref, foot_ik=False)
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
def test_foot_ik_puts_ankles_on_soma_ankles(tmp_path):
    """With foot IK each Gothic ankle sits on SOMA's ankle carried over by the root's scale."""
    models = base_models()
    skel = ga.read_skeleton(models / "HUMANS.MDH.json")
    ref = ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json")
    pos, rot = synthetic_soma(np.random.default_rng(11), 6)
    npz = tmp_path / "soma.npz"
    np.savez(npz, posed_joints=pos, global_rot_mats=rot)
    anim = rs.retarget(npz, skel, "T_TEST", "", 30.0, 30.0, 0.0, ref)
    gskel = rs.with_offsets(skel, ref)
    scale = rs.soma_rig(gskel, pos[0], rot[0], rs.SOMA30, ref)["scale"]
    for f in range(anim.frame_count):
        world = ga.world_pose(gskel, anim, f)
        for g, s in (("BIP01 L FOOT", "LeftFoot"), ("BIP01 R FOOT", "RightFoot")):
            target = rs.F @ pos[f, rs.SOMA30.index(s)] * scale
            assert np.linalg.norm(world[gskel.index(g)][:3, 3] - target) < 0.5, (f, g)  # cm


@pytest.mark.skipif(base_models() is None, reason="needs a converted base with the human model")
@pytest.mark.parametrize("foot_ik", [False, True])
def test_stance_survives_soma_and_back(tmp_path, foot_ik):
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
    # SOMA's ankle stands at the measured 0.081 m only in Kimodo output; give the matching floor here.
    lift = rig["bind_foot_y"] - rig["soma_foot_y"] * rig["scale"] if foot_ik else None
    back = rs.retarget(npz, skel0, "T", "", 30.0, 30.0, lift, ref, foot_ik=foot_ik)
    w0, w1 = ga.world_pose(skel, ref, 0), ga.world_pose(skel, back, 0)
    for n in ("BIP01 R HAND", "BIP01 L HAND", "BIP01 R FOOT", "BIP01 L FOOT", "BIP01 HEAD"):
        i = skel.index(n)
        assert np.linalg.norm(w0[i][:3, 3] - w1[i][:3, 3]) < 1.0, n  # cm


def test_retime_keeps_ends_and_squeezes_phases():
    import finish
    rng = np.random.default_rng(5)
    frames = 30
    pos = np.zeros((frames, 2, 3))
    pos[:, 0, 2] = -np.linspace(0, 200, frames)  # the root walks 2 m back at a constant pace
    anim = ga.Animation(name="T", next="", layer=1, fps=25.0, frame_count=frames, node_indices=[0, 1],
                        positions=pos, rotations=random_quats(rng, (frames, 2)))
    out = finish.retime(anim, [(0, 0), (0.24, 0.06), (0.88, 0.36), (1.16, 0.56)])
    assert out.frame_count == 15
    assert np.allclose(out.positions[0], anim.positions[0]) and np.allclose(out.positions[-1], anim.positions[-1])
    dots = np.abs((out.rotations[-1] * anim.rotations[-1]).sum(-1))
    assert np.allclose(dots, 1.0, atol=1e-6)
    z = out.positions[:, 0, 2]
    assert np.all(np.diff(z) <= 1e-9)  # time never runs backwards
    # Keys land exactly: the source's 0.88 s is now frame 9 (0.36 s), 76% of the way.
    assert abs(z[9] + 200 * 0.88 / 1.16) < 0.5


def test_content_pack_writes_index_overlay_and_records(tmp_path):
    import json
    import content_pack
    rng = np.random.default_rng(2)
    anim = ga.Animation(name="T_TESTDASHB", next="", layer=1, fps=25.0, frame_count=4, node_indices=[0],
                        positions=rng.normal(size=(4, 1, 3)), rotations=random_quats(rng, (4, 1)))
    src = tmp_path / "src" / "any-name.MAN.json"
    src.parent.mkdir()
    ga.write_animation(src, anim, source="test")
    pack = tmp_path / "pack.json"
    pack.write_text(json.dumps({"model": "HUMANS", "script": "HUMANS.MSB",
                                "animations": [{"file": str(src), "blend_out": 0.2}]}))
    content = tmp_path / "content"
    sys.argv = ["content_pack.py", str(content), str(pack)]
    content_pack.main()
    models = content / "models"
    index = json.loads((models / "models.json").read_text())
    assert index["files"] == {"HUMANS-T_TESTDASHB.MAN": "HUMANS-T_TESTDASHB.MAN.json", "HUMANS.MSB": "HUMANS.MSB.json"}
    overlay = json.loads((models / "HUMANS.MSB.json").read_text())
    assert overlay["kind"] == "script-overlay" and (models / "HUMANS.MSB.bin").read_bytes() == b""
    entry, = overlay["data"]["animations"]
    assert (entry["name"], entry["first_frame"], entry["last_frame"], entry["blend_out"]) == ("T_TESTDASHB", 0, 3, 0.2)
    back = ga.read_animation(models / "HUMANS-T_TESTDASHB.MAN.json")
    assert back.frame_count == 4 and np.allclose(back.positions, anim.positions, atol=1e-4)


def test_snap_ends_lands_both_ends_on_the_stance_and_keeps_the_middle_and_the_way():
    import finish
    rng = np.random.default_rng(9)
    frames, nodes = 20, 3
    target = ga.Animation("S_T", "", 1, 25.0, 1, [0, 1, 2], rng.normal(size=(1, nodes, 3)), random_quats(rng, (1, nodes)))
    rot = random_quats(rng, (frames, nodes))
    pos = rng.normal(size=(frames, nodes, 3))
    way = np.array([0, 0, -200.0])
    pos[-1, 0] = target.positions[0, 0] + way + np.array([0.5, 2.0, -0.4])  # the end misses the stance a little
    anim = ga.Animation("T", "", 1, 25.0, frames, [0, 1, 2], pos.copy(), rot.copy())
    finish.snap_ends(anim, target, 0.12, 0.2)
    same = lambda a, b: np.allclose(np.abs((a * b).sum(-1)), 1.0, atol=1e-6)  # noqa: E731
    assert same(anim.rotations[0], target.rotations[0]) and same(anim.rotations[-1], target.rotations[0])
    assert np.allclose(anim.positions[0, 1:], target.positions[0, 1:])
    assert np.allclose(anim.positions[-1, 1:], target.positions[0, 1:])
    # the root: its height lands on the stance at both ends, its way stays as generated
    assert np.isclose(anim.positions[0, 0, 1], target.positions[0, 0, 1])
    assert np.isclose(anim.positions[-1, 0, 1], target.positions[0, 0, 1])
    assert np.allclose(anim.positions[:, 0, [0, 2]], pos[:, 0, [0, 2]])
    assert same(anim.rotations[8], rot[8]) and np.allclose(anim.positions[8], pos[8])


def test_body_nodes_take_the_moves_node_set_and_hold_undriven_nodes():
    import finish
    rng = np.random.default_rng(4)
    names = ["BIP01", "BIP01 PELVIS", "BIP01 L TOE0", "ZS_RIGHTHAND", "ZS_HELMET"]
    skel = ga.Skeleton([ga.Node(n, -1 if i == 0 else 0, np.eye(4)) for i, n in enumerate(names)], np.zeros(3))
    anim = ga.Animation("T", "", 1, 25.0, 3, [0, 1, 2, 3, 4], rng.normal(size=(3, 5, 3)), random_quats(rng, (3, 5)))
    driven = anim.rotations[:, 1].copy()
    root = anim.positions[:, 0].copy()
    stance = ga.Animation("S", "", 1, 25.0, 1, [0, 1, 3], rng.normal(size=(1, 3, 3)), random_quats(rng, (1, 3)))
    jump = ga.Animation("J", "", 1, 25.0, 1, [0, 1, 2, 3], rng.normal(size=(1, 4, 3)), random_quats(rng, (1, 4)))
    finish.body_nodes(anim, skel, [stance, jump], jump)
    assert anim.node_indices == [0, 1, 2, 3]  # the jump-back's set: no ZS_HELMET
    assert np.allclose(anim.rotations[:, 1], driven)  # Kimodo's pelvis stays
    assert np.allclose(anim.positions[:, 0], root)  # and the root's way: BIP01 is derived, not in the map
    assert np.allclose(anim.rotations[:, 2], jump.rotations[0, 2])  # the toe: only the jump-back has it
    assert np.allclose(anim.rotations[:, 3], stance.rotations[0, 2])  # the slot: the stance comes first


def test_lock_feet_pins_a_standing_foot():
    import finish
    # pelvis → thigh → calf → foot, straight down, a leg 90 cm long under a root at 95 cm
    names = ["BIP01", "BIP01 L THIGH", "BIP01 L CALF", "BIP01 L FOOT"]
    rests = [np.eye(4) for _ in names]
    for i, drop in ((1, 5.0), (2, 45.0), (3, 45.0)):
        rests[i][:3, 3] = [0, -drop, 0]
    skel = ga.Skeleton([ga.Node(n, i - 1, r) for i, (n, r) in enumerate(zip(names, rests))], np.zeros(3))
    frames = 10
    pos = np.array([[r[:3, 3] for r in rests]] * frames, dtype=float)
    pos[:, 0] = [0, 95.0, 0]
    pos[:, 0, 2] = -2.0 * np.arange(frames)  # the body drifts 2 cm a frame, the foot slides with it
    bend = np.array([np.cos(0.15), np.sin(0.15), 0, 0])  # knees a little bent, so IK has room
    rot = np.tile(np.array([1.0, 0, 0, 0]), (frames, 4, 1))
    rot[:, 1], rot[:, 2] = bend, bend * np.array([1, -1, 1, 1])
    anim = ga.Animation("T", "", 1, 25.0, frames, [0, 1, 2, 3], pos, rot)
    j = skel.index("BIP01 L FOOT")
    before = np.array([ga.world_pose(skel, anim, f)[j][:3, 3] for f in range(frames)])
    finish.lock_feet(anim, skel)
    after = np.array([ga.world_pose(skel, anim, f)[j][:3, 3] for f in range(frames)])
    assert np.ptp(before[:, 2]) > 15
    assert np.ptp(after[:, 2]) < 0.5 and np.ptp(after[:, 0]) < 0.5  # pinned where it touched down
    assert np.allclose(after[:, 1], before[:, 1], atol=0.5)


def test_own_poses_put_feet_blade_and_joints_where_asked(tmp_path):
    models = base_models()
    if models is None:
        pytest.skip("no converted base")
    import json
    import own_poses as op
    spec = json.loads((Path(__file__).resolve().parent / "poses-strikes.json").read_text())
    b = op.Builder(models, spec["stance"])
    for name, ps in spec["poses"].items():
        pose = b.build(ps)
        r, world = pose["report"], pose["world"]
        assert r["miss"]["foot L"] == r["miss"]["foot R"] == 0, name
        assert all(r[f"{j} {s}"] <= 178 for j in ("knee", "elbow") for s in "LR"), name
        assert r["wrist R"] <= 65 and r["wrist L"] < 1, name  # Gothic's own clips bend up to ~68°
        blade = world[b.gi["ZS_RIGHTHAND"]][:, 0]
        assert blade @ op.unit(ps["arms"]["R"]["blade"]) > 0.999, name
        b.save(tmp_path / f"{name}.MAN.json", name, pose, "test")
        back = ga.world_pose(b.skel, ga.read_animation(tmp_path / f"{name}.MAN.json"), 0)
        assert np.allclose(np.array([m[:3, :3] for m in back]), world, atol=1e-3), name


def test_eval_rejects_a_sword_climbing_back_after_the_follow_through():
    import probe_eval as pe
    m = {"slide_max": 0.5, "tip_below_floor": 0, "hand_drop_cm": 110, "hand_vmax": 9, "rearm_cm": 12.0}
    assert pe.fails("charge_release", m) == ["rearm"]
    m["rearm_cm"] = -10.0
    assert pe.fails("charge_release", m) == []


def test_clear_sword_lifts_the_blade_off_the_floor_and_keeps_the_ends():
    import finish
    # A torso with a hanging right arm and the blade held forward; in the middle frames the body pitches forward
    # onto its front at 30 cm, so the arm runs along the body and the blade points 60 cm into the floor
    names = ["BIP01", "BIP01 PELVIS", "BIP01 NECK", "BIP01 HEAD", "BIP01 R UPPERARM", "BIP01 R FOREARM",
             "BIP01 R HAND", "ZS_RIGHTHAND"]
    parents = [-1, 0, 1, 2, 2, 4, 5, 6]
    offsets = [[0, 0, 0], [0, 0, 0], [0, 55, 0], [0, 10, 0], [20, -5, 0], [0, -28, 0], [0, -25, 0], [0, -5, 0]]
    rests = []
    for o in offsets:
        r = np.eye(4)
        r[:3, 3] = o
        rests.append(r)
    skel = ga.Skeleton([ga.Node(n, p, r) for n, p, r in zip(names, parents, rests)], np.zeros(3))
    frames = 7
    pos = np.array([offsets] * frames, dtype=float)
    rot = np.tile(np.array([1.0, 0, 0, 0]), (frames, len(names), 1))
    rot[:, 7] = [np.sqrt(0.5), 0, -np.sqrt(0.5), 0]  # the slot's X (the blade) points forward
    lying = [2, 3, 4]
    pos[:, 0] = [0, 95.0, 0]
    pos[lying, 0] = [0, 30.0, 0]
    rot[lying, 0] = [np.sqrt(0.5), np.sqrt(0.5), 0, 0]  # pitched forward 90°: up becomes forward, forward down
    anim = ga.Animation("T", "", 1, 25.0, frames, list(range(len(names))), pos, rot.copy())

    def lowest(a):
        out = []
        for f in range(frames):
            w = ga.world_pose(skel, a, f)
            tip = w[7][:3, 3] + w[7][:3, :3] @ np.array([finish.BLADE_CM, 0, 0])
            out.append(min(w[6][1, 3], tip[1]))
        return np.array(out)

    assert lowest(anim)[lying].max() < -50
    report = finish.clear_sword(anim, skel, floor=0.0)
    assert report["before_cm"] < -50 and report["after_cm"] >= 0
    assert (lowest(anim) >= 5 - 0.5).all()  # hand and blade 5 cm over the floor
    assert np.allclose(anim.rotations[[0, frames - 1]], rot[[0, frames - 1]])  # the ends stay the stance
    changed = [j for j in range(len(names)) if not np.allclose(anim.rotations[:, j], rot[:, j])]
    assert set(changed) <= {4, 5}  # only the upper arm and the forearm turn
    for f in lying:  # and the blade stays out of the torso (pelvis to neck, 14 cm)
        w = ga.world_pose(skel, anim, f)
        a, b = w[1][:3, 3], w[2][:3, 3]
        for t in np.linspace(0.1, 1, 10):
            x = w[7][:3, 3] + w[7][:3, :3] @ np.array([finish.BLADE_CM * t, 0, 0])
            u = np.clip(np.dot(x - a, b - a) / np.dot(b - a, b - a), 0, 1)
            assert np.linalg.norm(x - (a + u * (b - a))) > 14
