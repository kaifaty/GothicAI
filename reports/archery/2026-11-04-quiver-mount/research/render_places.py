#!/usr/bin/env python3
"""Frames of the quiver's candidate places, held at rest, on the body in a walk (research.md).

    uv run --no-project --with numpy --with matplotlib python \
        reports/archery/2026-11-04-quiver-mount/research/render_places.py --out /tmp/quiver-places
    blender -b -P reports/archery/2026-11-04-quiver-mount/research/render_places.py -- --render /tmp/quiver-places

The first step poses the light leather armour (ARMOR_LEATHER_L, the body in it; no head mesh) at two frames of
S_WALKL: the one where the right hand comes nearest the hip (mid-step) and the one where it is farthest back,
places the quiver model as analysis.py does for each candidate, and writes OBJ files and scene.json. The second
step runs in Blender: a plain material each, from the right side and from behind on the right, a sheet per
frame. Geometry only: the game's lighting and textures are not reproduced.
"""
from __future__ import annotations

import json
import math
import sys
from pathlib import Path

PLACES = ["now", "now_upright", "behind30", "behind60", "behind60_tuned", "lumbar"]


def prepare(out: Path) -> None:
    import numpy as np
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import analysis as A

    base = Path(sorted(Path("~/.cache/gothic-ai/base").expanduser().glob("*/models"))[0]).parent
    body = A.Body(base)
    an = A.clip(base, "S_WALKL")
    mesh = "ARMOR_LEATHER_L"
    import gothic_anim as ga
    skin = ga.read_skin_mesh(base / f"models/{mesh}.MDM.json", body.skel)
    pos, _, dom = body.mesh(mesh)

    def around(deg):
        return body.right * math.cos(math.radians(deg)) - body.fwd * math.sin(math.radians(deg))
    places = {
        "now": {"min_push": A.MIN_OUT},
        "now_upright": {"back": 0.0, "push": around(0), "tilt_back": 5.0, "tilt_out": 3.0, "min_push": 22.0},
        "behind30": {"back": 0.0, "push": around(30), "tilt_back": 15.0, "tilt_out": 6.0, "min_push": 20.0},
        "behind60": {"back": 0.0, "push": around(60), "tilt_back": 12.0, "tilt_out": 8.0, "min_push": 17.0},
        "behind60_tuned": {"back": 0.0, "push": around(60), "tilt_back": 20.0, "tilt_out": 10.0, "min_push": 20.0},
        "lumbar": {"back": 0.0, "side": 6.0, "height": 108.0, "push": around(90), "tilt_back": 8.0, "tilt_out": 0.0,
                   "tilt_right": -55.0, "min_push": 16.0},
    }
    frames = {pn: A.quiver_frame(body, pos, dom, pl)[0] for pn, pl in places.items()}
    quiver = A.glb_points(Path("~/.local/share/gothic-ai/content-staging/quiver/content/meshes/quiver/model.glb").expanduser(), 100.0)
    qtris = glb_tris(Path("~/.local/share/gothic-ai/content-staging/quiver/content/meshes/quiver/model.glb").expanduser())

    rhand = body.group(mesh, ("R HAND", "R FINGER"))
    # the frames: the right hand nearest the quiver now (mid-step), and the hand farthest back
    best, back, ts = None, None, [i / 60 for i in range(int(an.duration * 60))]
    for t in ts:
        pose = A.frame_pose(body, an, t)
        hand = body.pose_points(mesh, pose, rhand)
        inv = np.linalg.inv(pose[body.pelvis] @ frames["now"])
        q = (inv @ np.c_[hand, np.ones(len(hand))].T).T[:, :3]
        c = float(A.tube_clearance(q)[0].min())
        hz = float(A.to_body_axes(body, pose[body.pelvis], hand).mean(0)[2])
        if best is None or c < best[0]:
            best = (c, t)
        if back is None or hz < back[0]:
            back = (hz, t)
    out.mkdir(parents=True, exist_ok=True)
    scene = {"frames": []}
    for label, t in (("midstep", best[1]), ("handback", back[1])):
        pose = A.frame_pose(body, an, t)
        verts = body.pose_points(mesh, pose)
        # into the pelvis's body axes, the feet on the ground: x right, y up, z forward
        bv = A.to_body_axes(body, pose[body.pelvis], verts)
        floor = bv[:, 1].min()
        bv[:, 1] -= floor
        write_obj(out / f"body_{label}.obj", bv, skin.triangles)
        entry = {"label": label, "t": t, "places": {}}
        for pn, f in frames.items():
            m = pose[body.pelvis] @ f
            qv = (m @ np.c_[quiver, np.ones(len(quiver))].T).T[:, :3]
            qb = A.to_body_axes(body, pose[body.pelvis], qv)
            qb[:, 1] -= floor
            write_obj(out / f"quiver_{label}_{pn}.obj", qb, qtris)
            hand = body.pose_points(mesh, pose, rhand)
            inv = np.linalg.inv(m)
            q = (inv @ np.c_[hand, np.ones(len(hand))].T).T[:, :3]
            entry["places"][pn] = {"hand_clearance_cm": round(float(A.tube_clearance(q)[0].min()), 1)}
        scene["frames"].append(entry)
    (out / "scene.json").write_text(json.dumps(scene, indent=1))
    print(json.dumps(scene, indent=1))


def glb_tris(path: Path):
    import struct
    import numpy as np
    b = path.read_bytes()
    jl = struct.unpack_from("<I", b, 12)[0]
    j = json.loads(b[20:20 + jl])
    bo = 20 + jl + 8
    tris, base = [], 0
    for m in j["meshes"]:
        for p in m["primitives"]:
            a = j["accessors"][p["attributes"]["POSITION"]]
            ia = j["accessors"][p["indices"]]
            v = j["bufferViews"][ia["bufferView"]]
            dt = {5121: "<u1", 5123: "<u2", 5125: "<u4"}[ia["componentType"]]
            idx = np.frombuffer(b, dtype=dt, count=ia["count"], offset=bo + v.get("byteOffset", 0) + ia.get("byteOffset", 0))
            tris.append(idx.reshape(-1, 3).astype(np.int64) + base)
            base += a["count"]
    t = np.concatenate(tris)
    return t[:, [0, 2, 1]]  # the mirrored X turns the winding


def write_obj(path: Path, v, t) -> None:
    with open(path, "w") as f:
        for p in v:
            # the body's axes (x right, y up, z forward) are left-handed: x mirrored for Blender
            f.write(f"v {-p[0]/100:.5f} {p[1]/100:.5f} {p[2]/100:.5f}\n")
        for a, b, c in t:
            f.write(f"f {a+1} {c+1} {b+1}\n")


def render(folder: Path) -> None:
    import bpy
    from mathutils import Vector
    scene = json.loads((folder / "scene.json").read_text())

    def material(name, rgb):
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        bsdf = m.node_tree.nodes["Principled BSDF"]
        bsdf.inputs["Base Color"].default_value = (*rgb, 1)
        bsdf.inputs["Roughness"].default_value = 0.8
        return m

    views = {"side": (Vector((3.0, 1.1, -0.15)), Vector((0.0, 0.95, -0.15))),
             "backside": (Vector((2.0, 1.25, -2.3)), Vector((0.05, 0.9, -0.1)))}
    for fr in scene["frames"]:
        for pn in fr["places"]:
            for vn, (eye, target) in views.items():
                bpy.ops.wm.read_factory_settings(use_empty=True)
                skin = material("body", (0.55, 0.47, 0.40))
                leather = material("quiver", (0.35, 0.16, 0.06))
                ground = material("ground", (0.3, 0.3, 0.3))
                for obj, mat in ((f"body_{fr['label']}.obj", skin), (f"quiver_{fr['label']}_{pn}.obj", leather)):
                    bpy.ops.wm.obj_import(filepath=str(folder / obj), forward_axis="NEGATIVE_Z", up_axis="Y")
                    o = bpy.context.selected_objects[0]
                    o.data.materials.append(mat)
                    bpy.ops.object.shade_smooth()
                bpy.ops.mesh.primitive_plane_add(size=6, location=(0, 0, 0))
                bpy.context.object.data.materials.append(ground)
                # Blender: z up; the OBJ came in with y up turned to z, z forward turned to -y
                cam_data = bpy.data.cameras.new("cam")
                cam_data.lens = 50
                cam = bpy.data.objects.new("cam", cam_data)
                bpy.context.scene.collection.objects.link(cam)
                cam.location = (-eye.x, -eye.z, eye.y)
                tgt = Vector((-target.x, -target.z, target.y))
                cam.rotation_euler = (tgt - cam.location).to_track_quat("-Z", "Y").to_euler()
                bpy.context.scene.camera = cam
                sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
                sun.data.energy = 3.0
                sun.rotation_euler = (math.radians(50), 0, math.radians(30))
                bpy.context.scene.collection.objects.link(sun)
                world = bpy.data.worlds.new("w")
                world.use_nodes = True
                world.node_tree.nodes["Background"].inputs[0].default_value = (0.75, 0.8, 0.85, 1)
                world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
                bpy.context.scene.world = world
                sc = bpy.context.scene
                sc.render.engine = "BLENDER_EEVEE_NEXT" if "BLENDER_EEVEE_NEXT" in [e.identifier for e in bpy.types.RenderSettings.bl_rna.properties["engine"].enum_items] else "BLENDER_EEVEE"
                sc.render.resolution_x, sc.render.resolution_y = 640, 720
                sc.render.filepath = str(folder / f"{fr['label']}_{pn}_{vn}.png")
                bpy.ops.render.render(write_still=True)


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    if argv and argv[0] == "--render":
        render(Path(argv[1]))
    else:
        prepare(Path(argv[argv.index("--out") + 1]) if "--out" in argv else Path("/tmp/quiver-places"))
