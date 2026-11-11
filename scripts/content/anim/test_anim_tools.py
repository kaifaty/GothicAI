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


@pytest.mark.skipif(base_models() is None, reason="needs a converted base with the human model")
def test_stance_shoulders_go_to_somas_at_ease():
    """Gothic's stance → SOMA puts each clavicle where Kimodo holds it standing; copying keeps Gothic's 24° ahead."""
    models = base_models()
    stance = ga.read_animation(models / "HUMANS-S_RUN.MAN.json")
    skel = rs.with_offsets(ga.read_skeleton(models / "HUMANS.MDH.json"), stance)
    pos, rot = synthetic_soma(np.random.default_rng(3), 1)
    rig = rs.soma_rig(skel, pos[0], rot[0], rs.SOMA30, stance)
    off = {n: (rot[0, rs.SOMA30.index(p)].T @ (pos[0, j] - pos[0, rs.SOMA30.index(p)]) if p else np.zeros(3))
           for j, (n, p) in enumerate(rs.SOMA30_PARENT.items())}

    def forward(clavicles):
        local, _ = rs.gothic_to_soma(skel, stance, 0, rig, rs.SOMA30, clavicles=clavicles)
        g = {}
        for j, n in enumerate(rs.SOMA30):
            ang = np.linalg.norm(local[j])
            q = np.array([1.0, 0, 0, 0]) if ang < 1e-12 else np.concatenate([[np.cos(ang / 2)], local[j] / ang * np.sin(ang / 2)])
            par = rs.SOMA30_PARENT[n]
            g[n] = ga.quat_matrix(q) if par is None else g[par] @ ga.quat_matrix(q)
        out = []
        for s in ("Left", "Right"):
            d = g["Chest"].T @ g[f"{s}Shoulder"] @ off[f"{s}Arm"]
            out.append(np.degrees(np.arctan2(d[2], abs(d[0]))))
        return np.array(out)

    assert np.abs(forward(True) - rs.CLAVICLE_AT_EASE["soma"][0]).max() < 1.5
    assert np.abs(forward(False) - rs.CLAVICLE_AT_EASE["gothic"][0]).max() < 1.5


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
        blade = -world[b.gi["ZS_RIGHTHAND"]][:, 0]  # the blade lies along the slot's -X
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
    rot[:, 7] = [np.sqrt(0.5), 0, np.sqrt(0.5), 0]  # the slot's -X (the blade) points forward
    lying = [2, 3, 4]
    pos[:, 0] = [0, 95.0, 0]
    pos[lying, 0] = [0, 30.0, 0]
    rot[lying, 0] = [np.sqrt(0.5), np.sqrt(0.5), 0, 0]  # pitched forward 90°: up becomes forward, forward down
    anim = ga.Animation("T", "", 1, 25.0, frames, list(range(len(names))), pos, rot.copy())

    def lowest(a):
        out = []
        for f in range(frames):
            w = ga.world_pose(skel, a, f)
            tip = w[7][:3, 3] + w[7][:3, :3] @ np.array([-finish.BLADE_CM, 0, 0])
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
            x = w[7][:3, 3] + w[7][:3, :3] @ np.array([-finish.BLADE_CM * t, 0, 0])
            u = np.clip(np.dot(x - a, b - a) / np.dot(b - a, b - a), 0, 1)
            assert np.linalg.norm(x - (a + u * (b - a))) > 14


def test_snap_ends_can_land_the_end_on_another_record_or_leave_an_end_alone():
    import finish
    rng = np.random.default_rng(11)
    frames, nodes = 20, 3
    start = ga.Animation("S", "", 1, 25.0, 1, [0, 1, 2], rng.normal(size=(1, nodes, 3)), random_quats(rng, (1, nodes)))
    apex = ga.Animation("P", "", 1, 25.0, 1, [0, 1, 2], rng.normal(size=(1, nodes, 3)), random_quats(rng, (1, nodes)))
    rot, pos = random_quats(rng, (frames, nodes)), rng.normal(size=(frames, nodes, 3))
    same = lambda a, b: np.allclose(np.abs((a * b).sum(-1)), 1.0, atol=1e-6)  # noqa: E731
    anim = ga.Animation("T", "", 1, 25.0, frames, [0, 1, 2], pos.copy(), rot.copy())
    finish.snap_ends(anim, start, 0.12, 0.2, end=apex)
    assert same(anim.rotations[0], start.rotations[0]) and same(anim.rotations[-1], apex.rotations[0])
    assert np.allclose(anim.positions[-1, 1:], apex.positions[0, 1:])
    anim = ga.Animation("T", "", 1, 25.0, frames, [0, 1, 2], pos.copy(), rot.copy())
    finish.snap_ends(anim, start, 0.0, 0.2, end=apex)
    assert same(anim.rotations[0], rot[0]) and np.allclose(anim.positions[0], pos[0])


def test_hold_arm_window_keeps_the_swing_and_takes_the_stance_on_the_way_back():
    import finish
    models = base_models()
    if models is None:
        pytest.skip("no converted base")
    skel = ga.read_skeleton(models / "HUMANS.MDH.json")
    ref = ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json")
    rng = np.random.default_rng(5)
    frames = 25
    rot = random_quats(rng, (frames, len(ref.node_indices)))
    anim = ga.Animation("T", "", 1, 25.0, frames, list(ref.node_indices), np.repeat(ref.positions[:1], frames, 0), rot.copy())
    finish.hold_arm(anim, skel, ref, [1, 1, 1, 1], window=(0.4, 0.8))
    same = lambda a, b: np.allclose(np.abs((a * b).sum(-1)), 1.0, atol=1e-6)  # noqa: E731
    for name in finish.ARM:
        j = anim.node_indices.index(skel.index(name))
        assert same(anim.rotations[:10, j], rot[:10, j])  # t < 0.4 s: the clip's own swing
        assert same(anim.rotations[20:, j], np.broadcast_to(ref.rotations[0, j], (5, 4)))  # t >= 0.8 s: the stance


def test_arm_ref_holds_the_arm_towards_a_frame_of_another_record(tmp_path):
    import finish
    models = base_models()
    if models is None:
        pytest.skip("no converted base")
    skel = ga.read_skeleton(models / "HUMANS.MDH.json")
    walk = ga.read_animation(models / "HUMANS_1HST2-S_1HWALKL.MAN.json")
    arms = finish.pose_at(f"{models / 'HUMANS_1HST2-S_1HWALKL.MAN.json'}:9")
    assert arms.frame_count == 1 and np.allclose(arms.rotations[0], walk.rotations[9])
    assert np.allclose(finish.pose_at(str(models / "HUMANS_1HST2-S_1HWALKL.MAN.json")).rotations[0], walk.rotations[0])
    with pytest.raises(SystemExit):
        finish.pose_at(f"{models / 'HUMANS_1HST2-S_1HWALKL.MAN.json'}:{walk.frame_count}")
    rng = np.random.default_rng(7)
    rot = random_quats(rng, (10, len(walk.node_indices)))
    anim = ga.Animation("T", "", 1, 25.0, 10, list(walk.node_indices), np.repeat(walk.positions[:1], 10, 0), rot)
    finish.hold_arm(anim, skel, arms, [1, 1, 1, 1])
    same = lambda a, b: np.allclose(np.abs((a * b).sum(-1)), 1.0, atol=1e-6)  # noqa: E731
    for name in finish.ARM:
        j = anim.node_indices.index(skel.index(name))
        assert same(anim.rotations[:, j], np.broadcast_to(walk.rotations[9, j], (10, 4)))


def test_lift_skims_turns_a_foot_dragged_along_the_floor_into_a_step():
    import finish
    models = base_models()
    if models is None:
        pytest.skip("no converted base")
    skel = ga.read_skeleton(models / "HUMANS.MDH.json")
    ref = ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json")
    skel = rs.with_offsets(skel, ref)
    frames = 12
    pos = np.repeat(ref.positions[:1], frames, 0)
    pos[:, 0, 2] += np.clip(np.arange(frames) - 3, 0, 5) * 10.0  # the body slides 10 cm a frame, feet and all
    anim = ga.Animation("T", "", 1, 25.0, frames, list(ref.node_indices), pos, np.repeat(ref.rotations[:1], frames, 0))
    j = skel.index("BIP01 L FOOT")
    before = np.array([ga.world_pose(skel, anim, f)[j][1, 3] for f in range(frames)])
    arcs = finish.lift_skims(anim, skel)
    after = np.array([ga.world_pose(skel, anim, f)[j][1, 3] for f in range(frames)])
    assert any(side == "L" and a <= 3 and b >= 8 for side, a, b, _ in arcs)
    assert after[5] - before[5] > 2.5  # the middle of the drag is in the air
    assert np.allclose(after[[0, 1, 2, 10, 11]], before[[0, 1, 2, 10, 11]], atol=0.05)  # standing frames stay


def test_hold_arm_four_point_window_starts_and_ends_on_the_stance():
    import finish
    models = base_models()
    if models is None:
        pytest.skip("no converted base")
    skel = ga.read_skeleton(models / "HUMANS.MDH.json")
    ref = ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json")
    rng = np.random.default_rng(6)
    frames = 30
    rot = random_quats(rng, (frames, len(ref.node_indices)))
    anim = ga.Animation("T", "", 1, 25.0, frames, list(ref.node_indices), np.repeat(ref.positions[:1], frames, 0), rot.copy())
    finish.hold_arm(anim, skel, ref, [1, 1, 1, 1], window=(0.0, 0.2, 0.8, 1.0))
    same = lambda a, b: np.allclose(np.abs((a * b).sum(-1)), 1.0, atol=1e-6)  # noqa: E731
    j = anim.node_indices.index(skel.index("BIP01 R HAND"))
    assert same(anim.rotations[0, j], ref.rotations[0, j])  # the start is the stance's arm
    assert same(anim.rotations[5:20, j], rot[5:20, j])  # the middle is the clip's own
    assert same(anim.rotations[25:, j], np.broadcast_to(ref.rotations[0, j], (5, 4)))  # the end is the stance's again


def test_lock_feet_turn_keeps_a_standing_foot_from_pivoting():
    import finish
    models = base_models()
    if models is None:
        pytest.skip("no converted base")
    skel = rs.with_offsets(ga.read_skeleton(models / "HUMANS.MDH.json"), ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json"))
    ref = ga.read_animation(models / "HUMANS_1HST1-S_1H.MAN.json")
    frames = 12
    rot = np.repeat(ref.rotations[:1], frames, 0)
    j = ref.node_indices.index(skel.index("BIP01 L FOOT"))
    for f in range(frames):  # the foot turns 30° about its own axis while it stands
        a = np.radians(30.0) * f / (frames - 1) / 2
        rot[f, j] = finish.qmul(rot[f, j], np.array([np.cos(a), np.sin(a), 0, 0]))
    anim = ga.Animation("T", "", 1, 25.0, frames, list(ref.node_indices), np.repeat(ref.positions[:1], frames, 0), rot)
    toe, foot = skel.index("BIP01 L TOE0"), skel.index("BIP01 L FOOT")
    def yaws(a):
        out = []
        for f in range(frames):
            w = ga.world_pose(skel, a, f)
            d = w[toe][:3, 3] - w[foot][:3, 3]
            out.append(np.degrees(np.arctan2(d[0], d[2])))
        return np.array(out)
    before = yaws(anim)
    finish.lock_feet(anim, skel, turn=True)
    after = yaws(anim)
    assert np.ptp(before) > 10
    assert np.ptp(after) < 1.0


def test_hit_events_are_the_ones_gothics_own_strikes_carry():
    import finish_set
    assert finish_set.hit_events(None) == []
    limb, opt, end = finish_set.hit_events([7, 30])
    assert (limb["type"], limb["slot"]) == (16, "ZS_RIGHTHAND")
    assert (opt["type"], opt["frames"], end["type"], end["frames"]) == (20, [7], 21, [30])


def leg_clip(frames: int, root_y: float = 108.0):
    """Root → thigh → calf → foot → toe, a 90 cm leg with the knee a little bent, the toe node 15 cm ahead."""
    names = ["BIP01", "BIP01 L THIGH", "BIP01 L CALF", "BIP01 L FOOT", "BIP01 L TOE0"]
    rests = [np.eye(4) for _ in names]
    for i, off in ((1, [0, -5.0, 0]), (2, [0, -45.0, 0]), (3, [0, -45.0, 0]), (4, [0, -8.0, 15.0])):
        rests[i][:3, 3] = off
    skel = ga.Skeleton([ga.Node(n, i - 1, r) for i, (n, r) in enumerate(zip(names, rests))], np.zeros(3))
    pos = np.array([[r[:3, 3] for r in rests]] * frames, dtype=float)
    pos[:, 0] = [0, root_y, 0]
    bend = np.array([np.cos(0.15), np.sin(0.15), 0, 0])
    rot = np.tile(np.array([1.0, 0, 0, 0]), (frames, 5, 1))
    rot[:, 1], rot[:, 2] = bend, bend * np.array([1, -1, 1, 1])
    return skel, ga.Animation("T", "", 1, 25.0, frames, [0, 1, 2, 3, 4], pos, rot)


def test_fix_layer_step_lifts_a_sliding_foot_and_leaves_the_other_frames():
    import fix_layer
    skel, anim = leg_clip(10)
    anim.positions[:, 0, 2] = np.clip(np.arange(10) - 3, 0, 3) * 10.0  # frames 3-6: the body and the foot slide 30 cm
    orig = ga.Animation(anim.name, "", 1, 25.0, 10, list(anim.node_indices), anim.positions.copy(), anim.rotations.copy())
    j = skel.index("BIP01 L FOOT")
    before = np.array([ga.world_pose(skel, anim, f)[j][:3, 3] for f in range(10)])
    fix_layer.apply(skel, anim, [{"kind": "step", "limb": "L leg", "from": 3, "to": 6, "lift": 8}], floor=0.0)
    after = np.array([ga.world_pose(skel, anim, f)[j][:3, 3] for f in range(10)])
    keep = [0, 1, 2, 3, 6, 7, 8, 9]
    assert np.array_equal(anim.rotations[keep], orig.rotations[keep]) and np.array_equal(anim.positions, orig.positions)
    assert np.all(after[[4, 5], 1] - before[[4, 5], 1] > 5)  # in the air in the middle
    assert np.allclose(after[[4, 5], 0], 0, atol=0.5) and before[3, 2] < after[4, 2] < after[5, 2] < before[6, 2]


def test_fix_layer_soles_lift_a_toe_out_of_the_floor_and_keep_the_ends():
    import fix_layer
    skel, anim = leg_clip(10)
    down = np.array([np.cos(np.radians(20)), np.sin(np.radians(20)), 0, 0])  # 40° toe down
    anim.rotations[3:7, 3] = down
    orig_rot = anim.rotations.copy()
    t = skel.index("BIP01 L TOE0")
    toe = lambda f: ga.world_pose(skel, anim, f)[t][1, 3]  # noqa: E731
    assert min(toe(f) for f in range(3, 7)) < 0
    fix_layer.apply(skel, anim, [{"kind": "soles", "window": [0, 0, 9, 9], "limbs": ["L leg"], "clear": 3}], floor=0.0)
    assert all(toe(f) > 3 - 0.05 for f in range(10))
    assert np.array_equal(anim.rotations[[0, 9]], orig_rot[[0, 9]])  # the ends stay on the stance
    assert np.array_equal(anim.rotations[:, [0, 1, 2, 4]], orig_rot[:, [0, 1, 2, 4]])  # only the foot turns


def test_complete_takes_the_nodes_a_stance_does_not_write_from_the_layer_below():
    import finish
    rng = np.random.default_rng(5)
    ref = ga.Animation("S", "", 1, 25.0, 1, [0, 1], rng.normal(size=(1, 2, 3)), random_quats(rng, (1, 2)))
    below = ga.Animation("B", "", 1, 25.0, 1, [0, 1, 2], rng.normal(size=(1, 3, 3)), random_quats(rng, (1, 3)))
    full = finish.complete(ref, below)
    assert full.node_indices == [0, 1, 2]
    assert np.allclose(full.rotations[0, :2], ref.rotations[0]) and np.allclose(full.positions[0, :2], ref.positions[0])
    assert np.allclose(full.rotations[0, 2], below.rotations[0, 2]) and np.allclose(full.positions[0, 2], below.positions[0, 2])


def test_fix_layer_joint_clear_keeps_the_knee_over_the_floor():
    import fix_layer
    skel, anim = leg_clip(6, root_y=25.0)  # lying: the hip 20 cm up, the leg forward, the knee bent towards the floor
    t, k = skel.index("BIP01 L FOOT"), skel.index("BIP01 L CALF")
    anim.rotations[:, 1] = [np.cos(-0.7), np.sin(-0.7), 0, 0]
    anim.rotations[:, 2] = [np.cos(-0.15), np.sin(-0.15), 0, 0]
    plain = ga.Animation("T", "", 1, 25.0, 6, list(anim.node_indices), anim.positions.copy(), anim.rotations.copy())
    fix = {"kind": "tuck", "limb": "L leg", "window": [0, 1, 4, 5], "k": 0.4}
    fix_layer.apply(skel, plain, [fix], floor=0.0)
    fix_layer.apply(skel, anim, [dict(fix, joint_clear=5)], floor=0.0)
    knee = lambda a, f: ga.world_pose(skel, a, f)[k][1, 3]  # noqa: E731
    foot = lambda a, f: ga.world_pose(skel, a, f)[t][:3, 3]  # noqa: E731
    assert min(knee(plain, f) for f in range(6)) < 0  # without the guard the knee goes into the floor
    assert all(knee(anim, f) >= 5 - 0.05 for f in (1, 2, 3, 4))
    assert all(np.allclose(foot(anim, f), foot(plain, f), atol=0.1) for f in range(6))  # the foot lands where asked


def test_fix_layer_hold_keeps_a_bone_still_against_its_parent_while_the_body_turns():
    import fix_layer
    skel, anim = leg_clip(8)
    turn = lambda deg: [np.cos(np.radians(deg) / 2), 0, np.sin(np.radians(deg) / 2), 0]  # noqa: E731
    anim.rotations[:, 2] = [turn(20 * f) for f in range(8)]  # the calf spins against the thigh, 20° a frame
    orig = anim.rotations.copy()
    fix_layer.apply(skel, anim, [{"kind": "hold", "bones": ["BIP01 L CALF"], "frame": 2, "window": [1, 2, 5, 6]}], floor=0.0)
    assert all(abs(anim.rotations[f, 2] @ orig[2, 2]) > 1 - 1e-9 for f in (2, 3, 4, 5))  # still at frame 2's turn
    assert np.array_equal(anim.rotations[[0, 1, 6, 7]], orig[[0, 1, 6, 7]])  # outside the window, as it was
    assert np.array_equal(anim.rotations[:, [0, 1, 3, 4]], orig[:, [0, 1, 3, 4]])  # only the held bone changes


def test_fix_layer_raise_lifts_the_root_in_its_window_and_keeps_the_path():
    import fix_layer
    skel, anim = leg_clip(8)
    anim.positions[:, 0, 2] = np.arange(8) * 10.0
    orig = ga.Animation(anim.name, "", 1, 25.0, 8, list(anim.node_indices), anim.positions.copy(), anim.rotations.copy())
    fix_layer.apply(skel, anim, [{"kind": "raise", "window": [1, 3, 4, 6], "cm": 12}], floor=0.0)
    assert np.allclose(anim.positions[:, 0, 1] - orig.positions[:, 0, 1], [0, 0, 6, 12, 12, 6, 0, 0])
    assert np.array_equal(anim.positions[:, 0, [0, 2]], orig.positions[:, 0, [0, 2]])  # the path is the same
    assert np.array_equal(anim.positions[:, 1:], orig.positions[:, 1:]) and np.array_equal(anim.rotations, orig.rotations)


def test_fix_layer_keep_leaves_a_child_where_it_was_when_its_parent_is_fixed():
    import fix_layer
    skel, anim = leg_clip(8)
    t, c = skel.index("BIP01 L THIGH"), skel.index("BIP01 L CALF")
    before = [ga.world_pose(skel, anim, f) for f in range(8)]
    orig = anim.rotations.copy()
    fix_layer.apply(skel, anim, [{"kind": "rot", "bone": "BIP01 L THIGH", "window": [1, 2, 5, 6], "axis": [1, 0, 0], "deg": 30},
                                 {"kind": "keep", "bones": ["BIP01 L CALF"], "window": [1, 2, 5, 6]}], floor=0.0)
    after = [ga.world_pose(skel, anim, f) for f in range(8)]
    assert all(not np.allclose(after[f][t][:3, :3], before[f][t][:3, :3]) for f in (2, 3, 4, 5))  # the thigh turned
    assert all(np.allclose(after[f][c][:3, :3], before[f][c][:3, :3], atol=1e-6) for f in range(8))  # the calf did not
    assert np.array_equal(anim.rotations[[0, 7]], orig[[0, 7]])  # outside the window, as it was


def gothic_run():
    models = base_models()
    if models is None or not (models / "HUMANS-S_RUNL.MAN.json").exists():
        pytest.skip("no converted base")
    return ga.read_skeleton(models / "HUMANS.MDH.json"), ga.read_animation(models / "HUMANS-S_RUNL.MAN.json")


def test_fix_layer_wrist_brings_a_bent_hand_back_to_the_forearm_line():
    import fix_layer
    skel, anim = gothic_run()
    c = anim.node_indices.index(skel.index("BIP01 R HAND"))
    bend = fix_layer.rot([0, 0, 1], np.radians(50))  # 50° about the hand's own z
    anim.rotations[:, c] = [rs.mat_to_quat(ga.quat_matrix(q) @ bend) for q in anim.rotations[:, c]]
    i_fore, i_hand, i_tip = (skel.index(f"BIP01 R {b}") for b in ("FOREARM", "HAND", "FINGER0"))

    def bends():
        out = []
        for f in range(anim.frame_count):
            W = ga.world_pose(skel, anim, f)
            u, v = W[i_hand][:3, 3] - W[i_fore][:3, 3], W[i_tip][:3, 3] - W[i_hand][:3, 3]
            out.append(np.degrees(np.arccos(np.clip(u @ v / np.linalg.norm(u) / np.linalg.norm(v), -1, 1))))
        return np.array(out)

    assert bends().min() > 40
    fore = anim.rotations[:, anim.node_indices.index(i_fore)].copy()
    fix_layer.apply(skel, anim, [{"kind": "wrist", "limbs": ["R arm"], "window": [0, 0, 99, 99], "max": 10}], floor=0.0)
    assert bends().max() < 10.5
    assert np.array_equal(anim.rotations[:, anim.node_indices.index(i_fore)], fore)  # only the hand turns


def test_fix_layer_pelvis_halves_the_hip_roll_and_keeps_the_feet_and_the_trunk():
    import fix_layer
    skel, anim = gothic_run()
    n = anim.frame_count - 1  # Gothic's run is a moving loop: its last frame is the first, moved on
    feet, trunk = [skel.index(f"BIP01 {s} FOOT") for s in "LR"], skel.index("BIP01 SPINE")
    lt, rt = skel.index("BIP01 L THIGH"), skel.index("BIP01 R THIGH")
    before = [ga.world_pose(skel, anim, f) for f in range(n + 1)]
    ahead = before[n][0][:3, 3] - before[0][0][:3, 3]
    ahead[1] = 0
    side = np.cross([0, 1.0, 0], ahead / np.linalg.norm(ahead))
    roll = lambda W: fix_layer.hip_roll(W, lt, rt, side)
    fix_layer.apply(skel, anim, [{"kind": "pelvis", "window": [0, 0, n - 1, n - 1], "roll": 0.5}], floor=0.0, cycle=n)
    after = [ga.world_pose(skel, anim, f) for f in range(n + 1)]
    r0, r1 = np.array([roll(W) for W in before[:n]]), np.array([roll(W) for W in after[:n]])
    assert np.allclose(r1 - r1.mean(), 0.5 * (r0 - r0.mean()), atol=np.radians(0.3))
    for f in range(n):
        assert all(np.allclose(after[f][i][:3, 3], before[f][i][:3, 3], atol=0.05) for i in feet)
        assert np.allclose(after[f][trunk][:3, :3], before[f][trunk][:3, :3], atol=1e-6)
    assert np.array_equal(anim.rotations[n], anim.rotations[0])  # the seam frame is the first again


def test_fix_layer_loop_window_runs_over_the_seam():
    import fix_layer
    skel, anim = gothic_run()
    n = anim.frame_count - 1
    orig = anim.rotations.copy()
    travel = anim.positions[n, 0] - anim.positions[0, 0]
    fix = {"kind": "rot", "bone": "BIP01 R CALF", "window": [n - 3, n - 1, n + 1, n + 3], "axis": [0, 1, 0], "deg": 20}
    fix_layer.apply(skel, anim, [fix], floor=0.0, cycle=n)
    c = anim.node_indices.index(skel.index("BIP01 R CALF"))
    changed = [f for f in range(n) if not np.array_equal(anim.rotations[f, c], orig[f, c])]
    assert changed == [0, 1, 2, n - 2, n - 1]  # frames n-3 and n+3 are the window's zero ends
    assert np.array_equal(anim.rotations[n], anim.rotations[0])
    assert np.allclose(anim.positions[n, 0], anim.positions[0, 0] + travel)


def test_fix_layer_steady_holds_the_head_still_and_keep_leaves_the_arms():
    import fix_layer
    skel, anim = gothic_run()
    n = anim.frame_count - 1
    head, hands = skel.index("BIP01 HEAD"), [skel.index(f"BIP01 {s} HAND") for s in "LR"]

    def swing(a):
        qs = np.array([rs.mat_to_quat(ga.world_pose(skel, a, f)[head][:3, :3]) for f in range(n)])
        qs[(qs @ qs[0]) < 0] *= -1
        m = qs.mean(0) / np.linalg.norm(qs.mean(0))
        return np.degrees(2 * np.arccos(np.clip(np.abs(qs @ m), -1, 1)))

    before = [ga.world_pose(skel, anim, f) for f in range(n)]
    s0 = swing(anim)
    fix_layer.apply(skel, anim, [{"kind": "steady", "bones": ["BIP01 NECK", "BIP01 HEAD"], "window": [0, 0, n - 1, n - 1],
                                  "share": 0.4},
                                 {"kind": "keep", "bones": ["BIP01 L CLAVICLE", "BIP01 R CLAVICLE"],
                                  "window": [0, 0, n - 1, n - 1]}], floor=0.0, cycle=n)
    assert swing(anim).max() < 0.45 * s0.max()
    after = [ga.world_pose(skel, anim, f) for f in range(n)]
    assert all(np.allclose(after[f][i][:3, 3], before[f][i][:3, 3], atol=0.5) for f in range(n) for i in hands)


def gothic_stance():
    models = base_models()
    if models is None or not (models / "HUMANS-S_RUN.MAN.json").exists():
        pytest.skip("no converted base")
    return models, ga.read_skeleton(models / "HUMANS.MDH.json"), ga.read_animation(models / "HUMANS-S_RUN.MAN.json")


def test_fix_layer_breath_lifts_the_shoulders_and_leaves_the_head_the_feet_and_the_seam():
    import fix_layer
    _, skel, anim = gothic_stance()
    n = anim.frame_count - 1
    anim.positions[:], anim.rotations[:] = anim.positions[0], anim.rotations[0]      # a still stance: only the breath moves
    shoulders = [skel.index(f"BIP01 {s} UPPERARM") for s in "LR"]
    still = [skel.index(b) for b in ("BIP01 HEAD", "BIP01 L FOOT", "BIP01 R FOOT")]
    first = (anim.positions[0].copy(), anim.rotations[0].copy())
    before = ga.world_pose(skel, anim, 0)
    fix_layer.apply(skel, anim, [{"kind": "breath", "window": [0, 0, n - 1, n - 1], "breaths": 2, "chest": 1.5,
                                  "shoulders": 0.7}], floor=0.0, cycle=n)
    assert np.array_equal(anim.positions[0], first[0]) and np.array_equal(anim.rotations[0], first[1])
    W = np.array([ga.world_pose(skel, anim, f) for f in range(n)])
    rise = W[:, shoulders, 1, 3] - before[shoulders, 1, 3]
    assert 0.5 < rise.max() < 0.9 and rise.min() > -0.05
    assert np.abs(W[:, still][:, :, :3, 3] - before[still][:, :3, 3]).max() < 0.15
    up = rise[:, 0]
    peaks = [f for f in range(1, n - 1) if up[f] > up[f - 1] and up[f] >= up[f + 1] and up[f] > 0.5]
    assert len(peaks) == 2                                                           # two breaths over the loop


def test_loop_idle_onto_holds_the_reference_pose_with_a_still_root(tmp_path, monkeypatch):
    import loop_idle
    models, skel, anim = gothic_stance()
    n = anim.frame_count - 1
    sway = np.sin(2 * np.pi * np.arange(n + 1) / n)
    anim.positions[:, 0, 0] += 1.0 * sway + 20.0                    # the sample stands off the origin and sways 2 cm
    wobble = np.array([np.cos(0.02 * sway), 0 * sway, 0 * sway, np.sin(0.02 * sway)]).T
    col = anim.node_indices.index(skel.index("BIP01 SPINE1"))
    anim.rotations[:, col] = [rs.mat_to_quat(ga.quat_matrix(q) @ ga.quat_matrix(w)) for q, w in zip(anim.rotations[:, col], wobble)]
    src, out = tmp_path / "HUMANS-SRC.MAN.json", tmp_path / "HUMANS-S_RUN.MAN.json"
    ga.write_animation(src, anim, "test")
    monkeypatch.setattr(sys, "argv", ["loop_idle.py", str(src), str(models), str(out), "--name", "S_RUN", "--onto",
                                      "--frames", f"0,{n}", "--report", str(tmp_path / "rep.json")])
    loop_idle.main()
    loop, ref = ga.read_animation(out), ga.read_animation(models / "HUMANS-S_RUN.MAN.json")
    assert loop.frame_count == n + 1
    assert np.ptp(loop.positions[:, 0], axis=0).max() < 1e-4                          # the root stands still
    feet = [skel.index(f"BIP01 {s} FOOT") for s in "LR"]
    Wr = ga.world_pose(skel, ref, 0)
    W = np.array([ga.world_pose(skel, loop, f) for f in range(n + 1)])
    assert np.abs(W[:, feet][:, :, :3, 3] - Wr[feet][:, :3, 3]).max() < 0.05                   # the feet are pinned on the reference
    head = skel.index("BIP01 HEAD")
    assert np.abs(W[:, head, :3, 3].mean(0) - Wr[head, :3, 3]).max() < 0.5            # the pose is the reference's
    assert np.ptp(W[:, head, :3, 3], axis=0).max() > 0.3                              # and the sample's motion is kept
    assert np.allclose(W[n], W[0], atol=1e-4)


def test_trans_clip_mirror_swaps_the_sides_and_keeps_a_symmetric_stance():
    import trans_clip
    _, skel, stance = gothic_stance()
    run = gothic_run()[1]
    one = trans_clip.mirror(skel, stance)
    bones = [i for i, n in enumerate(skel.nodes) if n.name.startswith("BIP01")]
    A, B = ga.world_pose(skel, stance, 0), ga.world_pose(skel, one, 0)
    assert np.abs(A[bones] - B[bones]).max() < 0.05                      # Gothic's stance is its own mirror image
    m = trans_clip.mirror(skel, run)
    lf, rf, lh, rh = (skel.index(f"BIP01 {b}") for b in ("L FOOT", "R FOOT", "L HAND", "R HAND"))
    flip = np.array([-1.0, 1.0, 1.0])
    for f in (0, 5, 10):
        X, Y = ga.world_pose(skel, run, f), ga.world_pose(skel, m, f)
        for a_, b_ in ((lf, rf), (rf, lf), (lh, rh), (rh, lh)):
            assert np.allclose(Y[a_][:3, 3], flip * X[b_][:3, 3], atol=1e-3)
        assert all(abs(np.linalg.det(Y[i][:3, :3]) - 1) < 1e-4 for i in bones)  # turns stay turns
    twice = trans_clip.mirror(skel, m)
    assert np.abs(ga.world_pose(skel, twice, 7) - ga.world_pose(skel, run, 7)).max() < 1e-3
    slot = skel.index("ZS_SWORD")                                               # the sword stays on its own hip
    c = run.node_indices.index(slot) if slot in run.node_indices else None
    if c is not None:
        assert np.array_equal(m.positions[:, c], run.positions[:, c])


def test_trans_clip_lands_both_ends_and_keeps_the_way_straight(tmp_path, monkeypatch):
    import trans_clip
    models, skel, stance = gothic_stance()
    run = gothic_run()[1]
    src = tmp_path / "HUMANS-SRC.MAN.json"
    ga.write_animation(src, run, "test")
    out = tmp_path / "HUMANS-T_RUNL_2_RUN.MAN.json"
    monkeypatch.setattr(sys, "argv", ["trans_clip.py", str(src), str(models), str(out), "--name", "T_RUNL_2_RUN",
                                      "--next", "S_RUN", "--frames", "2,15", "--start",
                                      f"{models / 'HUMANS-S_RUNL.MAN.json'}:0", "--end",
                                      f"{models / 'HUMANS-S_RUN.MAN.json'}:0", "--fade", "0.1,0.2"])
    trans_clip.main()
    clip = ga.read_animation(out)
    assert clip.name == "T_RUNL_2_RUN" and clip.next == "S_RUN" and clip.frame_count == 14
    bones = [i for i, n in enumerate(skel.nodes) if n.name.startswith("BIP01") and "FOOT" not in n.name
             and "TOE" not in n.name and "THIGH" not in n.name and "CALF" not in n.name]

    def about_root(anim, f):
        W = ga.world_pose(skel, anim, f)
        return W[bones][:, :3, 3] - W[0][:3, 3] * np.array([1.0, 0.0, 1.0])

    assert np.abs(about_root(clip, 0) - about_root(run, 0)).max() < 0.1          # the first frame: the loop's
    assert np.abs(about_root(clip, 13) - about_root(stance, 0)).max() < 0.1      # the last frame: the stance
    assert abs(clip.positions[-1, 0, 0] - clip.positions[0, 0, 0]) < 1e-3        # no drift aside
    assert clip.positions[-1, 0, 2] - clip.positions[0, 0, 2] > 100              # and the way ahead is kept


def test_run_style_lowers_the_hips_leans_the_trunk_opens_the_elbows_and_keeps_the_feet_and_the_seam():
    import copy

    import run_style
    skel, run = gothic_run()
    orig = copy.deepcopy(run)
    run_style.style(skel, run, drop=6, lean=10, elbow=32, arm=13, loop=True)
    ix = skel.index

    def lean(W):
        up = W[ix("BIP01 NECK")][:3, 3] - W[ix("BIP01 PELVIS")][:3, 3]
        return np.degrees(np.arctan2(up[2], up[1]))

    def bend(W, side):
        s, e, h = (W[ix(f"BIP01 {side} {b}")][:3, 3] for b in ("UPPERARM", "FOREARM", "HAND"))
        u, v = e - s, h - e
        return np.degrees(np.arccos(u @ v / np.linalg.norm(u) / np.linalg.norm(v)))

    for f in range(run.frame_count - 1):
        A, B = ga.world_pose(skel, orig, f), ga.world_pose(skel, run, f)
        assert abs(A[0][1, 3] - B[0][1, 3] - 6) < 1e-4                       # the hips came down
        assert np.allclose(A[0][[0, 2], 3], B[0][[0, 2], 3], atol=1e-4)      # along the same way
        for side in "LR":
            foot = ix(f"BIP01 {side} FOOT")
            assert np.abs(A[foot][:3, 3] - B[foot][:3, 3]).max() < 0.05      # to the same feet
            assert np.abs(A[foot][:3, :3] - B[foot][:3, :3]).max() < 1e-3
            assert bend(A, side) - bend(B, side) > 31                        # the elbows opened
        assert 9 < lean(B) - lean(A) < 14                                    # the trunk leans from the hips
        head = ix("BIP01 HEAD")
        tip = np.degrees(np.arccos(np.clip((np.trace(A[head][:3, :3].T @ B[head][:3, :3]) - 1) / 2, -1, 1)))
        assert tip < 4                                                       # and the head still looks ahead
    assert np.allclose(run.rotations[-1], run.rotations[0]) and np.allclose(run.positions[-1, 1:], run.positions[0, 1:])
    assert np.allclose(run.positions[-1, 0] - run.positions[0, 0], orig.positions[-1, 0] - orig.positions[0, 0])


def test_run_style_share_and_bump_leave_the_stance_end_alone_and_lean_into_the_start():
    import copy

    import run_style
    skel, run = gothic_run()
    orig = copy.deepcopy(run)
    run_style.style(skel, run, drop=6, lean=10, elbow=32, arm=13, w=[(0, 0.0), (5, 1.0)],
                    wdrop=[(0, 0.0), (6, 0.0), (12, 1.0)], bumps=[(6.5, 6.5, 10.0)])
    ix = skel.index

    def lean(anim, f):
        W = ga.world_pose(skel, anim, f)
        up = W[ix("BIP01 NECK")][:3, 3] - W[ix("BIP01 PELVIS")][:3, 3]
        return np.degrees(np.arctan2(up[2], up[1]))

    first = np.abs(ga.world_pose(skel, run, 0) - ga.world_pose(skel, orig, 0))
    assert first[:, :3, 3].max() < 0.05 and first[:, :3, :3].max() < 1e-3    # frame 0: the share and the bell are 0
    assert np.allclose(run.positions[:7, 0, 1], orig.positions[:7, 0, 1])    # the drop waits for the push
    assert abs(orig.positions[13, 0, 1] - run.positions[13, 0, 1] - 6) < 1e-4
    gain = [lean(run, f) - lean(orig, f) for f in range(14)]
    assert 18 < max(gain[5:9]) < 24 and 9 < gain[13] < 14                    # the bell on top of the carriage


def test_retime_clip_squeezes_the_start_and_keeps_the_ends_and_the_way(tmp_path, monkeypatch):
    import retime_clip
    models, skel, _ = gothic_stance()
    src = models / "HUMANS-T_RUN_2_RUNL.MAN.json"
    if not src.exists():
        pytest.skip("no converted base")
    before = ga.read_animation(src)
    last = before.frame_count - 1
    out = tmp_path / "HUMANS-T_RUN_2_RUNL.MAN.json"
    monkeypatch.setattr(sys, "argv", ["retime_clip.py", str(src), str(models), str(out), "--keys",
                                      f"0:0,6:3,{last}:{last - 3}", "--no-lock"])
    retime_clip.main()
    after = ga.read_animation(out)
    assert after.frame_count == before.frame_count - 3 and after.name == before.name and after.next == before.next
    assert np.allclose(after.positions[0], before.positions[0]) and np.allclose(after.positions[-1], before.positions[-1])
    assert np.allclose(np.abs((after.rotations[-1] * before.rotations[-1]).sum(-1)), 1, atol=1e-5)
    assert np.allclose(after.positions[3], before.positions[6], atol=1e-3)       # frame 6 is played at frame 3
    assert after.positions[1, 0, 2] - after.positions[0, 0, 2] > 1.5 * (before.positions[1, 0, 2] - before.positions[0, 0, 2])


def test_two_bone_guard_bends_a_straight_leg_forward_whatever_way_its_knee_is_off_the_line():
    import copy

    import fix_layer as fl
    models, skel, stance = gothic_stance()
    ix = skel.index
    hip, knee, foot = (ix(f"BIP01 L {b}") for b in ("THIGH", "CALF", "FOOT"))
    aside = []
    for nudge in (-0.3, 0.3):                       # the knee a hair to one side of the hip-ankle line, then the other
        anim = copy.deepcopy(stance)
        clip = fl.Clip(skel, anim)
        W = clip.world(0)
        H, K, A = (W[i][:3, 3].copy() for i in (hip, knee, foot))
        reach = np.linalg.norm(K - H) + np.linalg.norm(A - K)
        down = (A - H) / np.linalg.norm(A - H)
        fl.two_bone(clip, 0, W, "L leg", H + down * (reach - 0.01) + np.array([nudge, 0.0, 0.0]), W[foot][:3, :3].copy())
        straight = W[knee][:3, 3] - H
        assert np.linalg.norm(straight - (straight @ down) * down) < 1.5      # the leg is straight now
        target = H + down * (reach - 3.0)                                      # the hips come down by 3 cm
        fl.two_bone(clip, 0, W, "L leg", target, W[foot][:3, :3].copy(), guard=3.0)
        off = (W[knee][:3, 3] - H) - ((W[knee][:3, 3] - H) @ down) * down
        assert off[2] > 8 and abs(off[0]) < 0.5 * off[2]                       # the knee goes forward, not aside
        aside.append(off)
    assert np.linalg.norm(aside[0] - aside[1]) < 3                             # and the same way for both nudges


def test_knee_floor_brings_a_chain_drawn_into_a_line_back_to_the_bind_knee_and_leaves_a_bent_one():
    import copy

    import fix_layer as fl
    import knee_floor as kf
    models, skel, stance = gothic_stance()
    ix = skel.index
    hip, knee, foot = (ix(f"BIP01 L {b}") for b in ("THIGH", "CALF", "FOOT"))
    floor = kf.bind_bend(skel)
    assert 12 < floor < 15                                   # BIP01's chain is bent where the body's leg is straight
    assert kf.raised(floor + 6.0, floor, 6.0) == pytest.approx(floor + 6.0)      # smooth where the two parts meet
    assert kf.raised(floor + 20.0, floor, 6.0) == floor + 20.0
    assert floor < kf.raised(0.0, floor, 6.0) < floor + 0.5
    assert kf.raised(floor, floor, 6.0) == pytest.approx(floor + 6.0 / np.e)
    for nudge in (-0.3, 0.3):                                # the knee a hair to one side of the line, then the other
        anim = copy.deepcopy(stance)
        clip = fl.Clip(skel, anim)
        W = clip.world(0)
        H, K, A = (W[i][:3, 3].copy() for i in (hip, knee, foot))
        reach = np.linalg.norm(K - H) + np.linalg.norm(A - K)
        down = (A - H) / np.linalg.norm(A - H)
        fl.two_bone(clip, 0, W, "L leg", H + down * (reach - 0.01) + np.array([nudge, 0.0, 0.0]), W[foot][:3, :3].copy())
        assert abs(kf.bend(clip.world(0), skel, "L leg")) < 3                    # a line: the knee bent backwards
        other = kf.bend(clip.world(0), skel, "R leg")                            # the stance's own knee, at the floor
        at, turn = clip.world(0)[foot][:3, 3].copy(), clip.world(0)[foot][:3, :3].copy()
        rep = kf.lay(skel, anim, floor)
        W = clip.world(0)
        assert floor <= kf.bend(W, skel, "L leg") < floor + 0.5                  # back at the bind knee, to the front
        assert rep["L leg"]["frames"] == anim.frame_count and rep["L leg"]["least_after"] >= floor
        assert np.linalg.norm(W[foot][:3, 3] - at) < 0.7                         # the foot gives way by millimetres
        assert np.allclose(W[foot][:3, :3], turn, atol=1e-5)                     # and keeps its turn
        # the other leg stood at the floor: it comes up by its soft share, smoothly with the knees above
        assert kf.bend(W, skel, "R leg") == pytest.approx(kf.raised(other, floor, 6.0), abs=0.05)
        assert 1.5 < kf.bend(W, skel, "R leg") - other < 2.5
    # a leg bent well over the floor is not touched
    anim = copy.deepcopy(stance)
    clip = fl.Clip(skel, anim)
    W = clip.world(0)
    H, A = W[hip][:3, 3].copy(), W[foot][:3, 3].copy()
    fl.two_bone(clip, 0, W, "L leg", H + (A - H) * 0.9, W[foot][:3, :3].copy(), guard=3.0)
    assert kf.bend(clip.world(0), skel, "L leg") > floor + 15
    was = anim.rotations.copy()
    rep = kf.lay(skel, anim, floor)
    cols = [clip.col[i] for i in (hip, knee, foot)]
    assert np.allclose(np.abs((anim.rotations[0, cols] * was[0, cols]).sum(-1)), 1, atol=1e-9)   # frame 0 is the bent one


def _bvh_text(root_rows) -> str:
    """A 100STYLE-like BVH in a T-pose, facing +Z: only the root moves (x, y, z, yaw)."""
    def joint(name, off, kids=(), end=None, root=False):
        ch = "CHANNELS 6 Xposition Yposition Zposition Yrotation Xrotation Zrotation" if root else "CHANNELS 3 Yrotation Xrotation Zrotation"
        body = "".join(joint(*k) for k in kids)
        if end is not None:
            body += "End Site\n{\nOFFSET %g %g %g\n}\n" % end
        return "%s %s\n{\nOFFSET %g %g %g\n%s\n%s}\n" % ("ROOT" if root else "JOINT", name, *off, ch, body)

    def arm(s, x):
        return (f"{s}Collar", (3 * x, 7, 0), [(f"{s}Shoulder", (16 * x, 0, 0), [(f"{s}Elbow", (31 * x, 0, 0), [
            (f"{s}Wrist", (25 * x, 0, 0), (), (19 * x, 0, 0))])])])

    def leg(s, x):
        return (f"{s}Hip", (11 * x, 0, 0), [(f"{s}Knee", (0, -44, 0), [(f"{s}Ankle", (0, -43, 0), [
            (f"{s}Toe", (0, -10, 19), (), (0, -1, 5))])])])

    spine = ("Chest", (0, 13, 0), [("Chest2", (0, 10, 0), [("Chest3", (0, 9, 0), [("Chest4", (0, 9, 0), [
        ("Neck", (0, 13, 0), [("Head", (0, 9, 0), (), (0, 17, 0))]), arm("Right", -1), arm("Left", 1)])])])])
    text = "HIERARCHY\n" + joint("Hips", (0, 0, 0), [spine, leg("Right", -1), leg("Left", 1)], root=True)
    rows = [" ".join(f"{v:.6f}" for v in (x, y, z, yaw, 0, 0) + (0,) * 66) for x, y, z, yaw in root_rows]
    return text + f"MOTION\nFrames: {len(rows)}\nFrame Time: {1 / 60:.6f}\n" + "\n".join(rows) + "\n"


def test_bvh_to_soma_reads_a_walk_along_any_way_as_a_walk_along_z_and_straightens_a_turn(tmp_path):
    import bvh_to_soma as bs

    # a straight walk along +X, the body turned that way: comes out along +Z with every joint at rest
    rows = [(1.2 * f, 97.0, 0.0, 90.0) for f in range(120)]
    (tmp_path / "a.bvh").write_text(_bvh_text(rows))
    bvh = bs.read_bvh(tmp_path / "a.bvh")
    assert len(bvh["names"]) == 23 and abs(1 / bvh["dt"] - 60) < 0.01
    d = bs.convert(bvh)
    hips = d["posed_joints"][:, rs.SOMA30.index("Hips")]
    assert np.abs(hips[:, 0]).max() < 1e-6 and abs(hips[-1, 2] - 1.2 * 119) < 1e-6
    assert np.abs(d["global_rot_mats"] - np.eye(3)).max() < 1e-9
    left = d["posed_joints"][0, rs.SOMA30.index("LeftLeg")] - d["posed_joints"][0, rs.SOMA30.index("RightLeg")]
    assert left[0] > 20 and abs(left[2]) < 1e-9                       # the left side at +X, as SOMA's
    tip = d["posed_joints"][0, rs.SOMA30.index("LeftHandMiddleEnd")] - d["posed_joints"][0, rs.SOMA30.index("LeftHand")]
    assert np.allclose(tip, [19, 0, 0])

    # a walk round a circle of 2 m with the steps' sway on it: straightened, it goes along +Z by the arc, the
    # body does not turn, and the sway stays
    f = np.arange(600)
    ang = np.radians(0.5 * f)                                         # 30 degrees a second
    sway = 2.0 * np.sin(2 * np.pi * f / 75)                           # a stride of 1.25 s, 2 cm either way
    rad = 200.0 + sway
    rows = [(rad[i] * np.cos(ang[i]) - 200.0, 97.0, -rad[i] * np.sin(ang[i]), np.degrees(ang[i]) - 180.0) for i in f]
    (tmp_path / "b.bvh").write_text(_bvh_text(rows))
    bvh = bs.read_bvh(tmp_path / "b.bvh")
    d = bs.convert(bvh, 150, 451, straighten=2.5)
    hips = d["posed_joints"][:, rs.SOMA30.index("Hips")]
    arc = 200.0 * (ang[450] - ang[150])
    assert 0.96 * arc < hips[-1, 2] - hips[0, 2] <= arc             # the low-passed way cuts the bend: 3 % shorter here
    assert 1.5 < np.abs(hips[:, 0]).max() < 2.6                       # the sway, not the circle
    yaw = np.degrees(np.arctan2(d["global_rot_mats"][:, 0, 0, 2], d["global_rot_mats"][:, 0, 2, 2]))
    assert np.abs(yaw - yaw.mean()).max() < 2.0                       # the heading's turn is gone
    whole = bs.convert(bvh, 150, 451)
    assert np.ptp(np.degrees(np.arctan2(whole["global_rot_mats"][:, 0, 0, 2], whole["global_rot_mats"][:, 0, 2, 2]))) > 100


def test_loop_keys_tiles_a_loop_in_time_and_spreads_the_keys_evenly_under_kimodos_limit():
    import loop_keys as lk

    rng = np.random.default_rng(7)
    n = 23                                                       # Gothic's walk: 24 frames at 25 fps
    rot = random_quats(rng, (n + 1, 3))
    rot[n] = rot[0]
    pos = rng.normal(size=(n + 1, 3, 3))
    pos[:, 0, 2] = np.linspace(0.0, 131.5, n + 1)
    pos[n, 0, :2] = pos[0, 0, :2]
    pos[n, 1:] = pos[0, 1:]
    loop = ga.Animation(name="S_WALKL", next="S_WALKL", layer=1, fps=25.0, frame_count=n + 1,
                        node_indices=[0, 3, 7], positions=pos, rotations=rot)
    tiled = lk.tile(loop, 30.0, 170, "P_WALK30")
    assert tiled.frame_count == 170 and tiled.fps == 30.0
    # frame 6 of the record is 0.2 s: the loop's frame 5 exactly; frame 144 is 4.8 s: 5 cycles and frame 5 again
    for t, cycles in ((6, 0), (144, 5)):
        assert np.allclose(tiled.positions[t, 1:], pos[5, 1:])
        assert np.allclose(tiled.positions[t, 0], pos[5, 0] + cycles * np.array([0.0, 0.0, 131.5]))
        assert np.allclose(np.abs((tiled.rotations[t] * rot[5]).sum(-1)), 1.0)
    assert np.all((tiled.rotations[1:] * tiled.rotations[:-1]).sum(-1) >= 0)      # no sign flips between frames
    assert np.all(np.diff(tiled.positions[:, 0, 2]) > 0)                           # the root goes on over the seams

    cycle = n / 25.0 * 30                                        # 27.6 Kimodo frames
    assert lk.key_frames(cycle, 2) == [0, 14, 28, 41, 55, 69, 83, 97, 110, 124, 138, 152, 166]
    assert lk.key_frames(cycle, 4)[:5] == [0, 7, 14, 21, 28] and lk.key_frames(cycle, 4)[-1] == 110
    for per in (2, 4, 8):
        assert len(lk.key_frames(cycle, per)) < 20


@pytest.mark.skipif(base_models() is None, reason="needs a converted base with the human model")
def test_loop_keys_symmetric_makes_the_steps_mirror_images_and_stills_gothics_run():
    """Gothic's run leans back and forward once a cycle and turns its chest; the mean with the mirror keeps neither."""
    import loop_keys as lk
    import trans_clip
    models = base_models()
    skel = ga.read_skeleton(models / "HUMANS.MDH.json")
    loop = ga.read_animation(models / "HUMANS-S_RUNL.MAN.json")
    fps = 2 * loop.fps                                            # a cycle of 38 frames: half of it is a whole frame count
    cycle = (loop.frame_count - 1) * 2
    tiled = lk.tile(loop, fps, 2 * cycle, "P")
    sym = lk.symmetric(skel, loop, tiled)
    gi = {n.name: i for i, n in enumerate(skel.nodes)}

    def trunk_pitch(anim):
        w = [ga.world_pose(skel, anim, f) for f in range(cycle)]
        up = np.array([x[gi["BIP01 NECK"]][:3, 3] - x[gi["BIP01 PELVIS"]][:3, 3] for x in w])
        return np.ptp(np.degrees(np.arctan2(up[:, 2], up[:, 1])))

    assert trunk_pitch(tiled) > 8 and trunk_pitch(sym) < 2
    # half a cycle on, the pose is the mirror image of this one: the right step is the left one
    mirrored = trans_clip.mirror(skel, sym)
    for t in (3, 9, 15):
        a, b = ga.world_pose(skel, sym, t + cycle // 2), ga.world_pose(skel, mirrored, t)
        for name in ("BIP01 L HAND", "BIP01 R FOOT", "BIP01 HEAD"):
            d = (a[gi[name]][:3, 3] - a[0][:3, 3]) - (b[gi[name]][:3, 3] - b[0][:3, 3])
            assert np.linalg.norm(d) < 0.1, (t, name)                 # cm


def test_fix_layer_move_shifts_a_foot_by_a_vector_in_its_window_and_keeps_its_turn():
    import fix_layer
    skel, anim = leg_clip(10)
    orig = ga.Animation(anim.name, "", 1, 25.0, 10, list(anim.node_indices), anim.positions.copy(), anim.rotations.copy())
    j = skel.index("BIP01 L FOOT")
    before = [ga.world_pose(skel, anim, f)[j] for f in range(10)]
    fix_layer.apply(skel, anim, [{"kind": "move", "limb": "L leg", "window": [2, 4, 5, 7], "by": [0, 4, -6]}], floor=0.0)
    after = [ga.world_pose(skel, anim, f)[j] for f in range(10)]
    keep = [0, 1, 2, 7, 8, 9]
    assert np.array_equal(anim.rotations[keep], orig.rotations[keep]) and np.array_equal(anim.positions, orig.positions)
    for f in (4, 5):                                                # the whole vector where the window is full
        assert np.allclose(after[f][:3, 3] - before[f][:3, 3], [0, 4, -6], atol=0.05)
        assert np.allclose(after[f][:3, :3], before[f][:3, :3], atol=1e-6)   # the foot keeps its turn
    for f in (3, 6):                                                # half of it on the way in and out
        assert np.allclose(after[f][:3, 3] - before[f][:3, 3], [0, 2, -3], atol=0.05)


def test_trans_clip_plant_root_brings_the_body_to_a_foot_that_stands_the_whole_clip():
    import trans_clip
    skel, anim = leg_clip(10)
    anim.positions[:, 0, 2] = np.arange(10) * 3.0        # the body goes 27 cm, the rigid leg's foot with it: 3 cm a frame
    orig = anim.positions.copy()
    j = skel.index("BIP01 L FOOT")
    shift = trans_clip.plant_root(anim, skel)
    foot = np.array([ga.world_pose(skel, anim, f)[j][:3, 3] for f in (0, 9)])
    assert np.allclose(shift, [0.0, -27.0])
    assert np.allclose(foot[0], foot[1], atol=1e-6)                             # the foot ends where it began
    assert np.allclose(anim.positions[0], orig[0]) and np.array_equal(anim.positions[:, 0, :2], orig[:, 0, :2])
    step = np.diff(anim.positions[:, 0, 2])
    assert abs(step[0] - 3.0) < 1.0 and abs(step[-1] - 3.0) < 1.0               # the ends keep most of their speed:
    assert step[4] < -1.0                                                       # the shift is made in the middle
    skel2, moving = leg_clip(10)
    moving.positions[:, 0, 2] = np.arange(10) * 12.0     # 12 cm a frame: the foot does not stand, nothing moves
    assert np.allclose(trans_clip.plant_root(moving, skel2), 0.0)


def test_fix_layer_tread_lays_the_toe_on_the_floor_from_above_and_from_below():
    import fix_layer
    skel, anim = leg_clip(8)
    foot, toe = skel.index("BIP01 L FOOT"), skel.index("BIP01 L TOE0")
    tilt = lambda deg: np.array([np.cos(np.radians(deg) / 2), np.sin(np.radians(deg) / 2), 0, 0])
    anim.rotations[2:4, 3] = tilt(12)                      # the foot pitched one way for two frames,
    anim.rotations[4:6, 3] = tilt(-12)                     # the other way for two more
    before = np.array([ga.world_pose(skel, anim, f)[toe][1, 3] for f in range(8)])
    floor = float(before[0])                               # where the toe lies when the foot is as the leg carries it
    assert before[2:4].max() - floor < -1 or before[2:4].min() - floor > 1
    assert (before[2] - floor) * (before[4] - floor) < 0   # one pair under the floor, the other over it
    orig = anim.rotations.copy()
    fix_layer.apply(skel, anim, [{"kind": "tread", "limb": "L leg", "window": [1, 2, 5, 6]}], floor=floor - 3.0)
    after = np.array([ga.world_pose(skel, anim, f)[toe][1, 3] for f in range(8)])
    assert np.allclose(after[2:6], floor, atol=0.05)       # the toe lies on the floor in the whole window
    assert np.array_equal(anim.rotations[[0, 1, 6, 7]], orig[[0, 1, 6, 7]])
    ankle = [ga.world_pose(skel, anim, f)[foot][:3, 3] for f in range(8)]
    assert np.allclose(ankle, ankle[0])                    # the ankle stays: only the foot turns
