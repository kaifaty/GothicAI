"""Render a Kimodo sample as Kimodo made it: its own SOMA body, before any retarget to Gothic's skeleton.

    blender -b --factory-startup -P scripts/content/anim/blender/soma_view.py -- config.json

config.json:
    {"npz": "/abs/gen/run_k6/out/out_04.npz",          # a sample of kimodo_gen
     "skin": "/abs/skin_standard.npz",                 # kimodo/assets/skeletons/somaskel77/skin_standard.npz
     "seq": "/abs/chain.npz", "color": [0.62, 0.47, 0.38],   # instead of npz + skin: a body already skinned, see below
     "render": {"dir": "/abs/out/frames", "views": ["side", "quarter", "back"], "size": [640, 720], "ortho": 2.8}}

Why: a flaw seen in the game is either the generator's or ours (retarget, loop cut, fix layers, the engine). This
view is the generator alone - the sample's joint transforms skin Kimodo's own body mesh (linear blend skinning, as
`kimodo/viz/soma_skin.py` does), nothing of our pipeline in between. Render the same frames of the retargeted
record with `gothic_rig.py` (same views, size and `ortho`) and put the two side by side.

SOMA is right-handed, Y up, metres, x = the character's left, z = forward. Blender gets (-x, z, y), a rotation:
the character faces +Y and its right hand is at +X, as in `gothic_rig.py`. The camera follows the root over a
floor with a line every metre. One PNG per frame of the sample (30 a second) in `<dir>/<view>/f0001.png`.

`"seq"`: the same scene for a body skinned elsewhere (`hero_body/rig_direct.py`: the new body on its own skeleton) -
an npz with `verts` [frames, vertices, 3] and `root` [frames, 3] in Gothic's frame (cm, left-handed, Y up, +Z
ahead) and `faces`. Blender gets (x, z, y) in metres, a reflection, so the triangles are turned over.
"""
import json
import sys
from pathlib import Path

import bpy
import numpy as np

TURN = np.array([[-1.0, 0, 0], [0, 0, 1], [0, 1, 0]])


def skin_frames(sample, skin) -> np.ndarray:
    """Vertices of the body in every frame, in SOMA's frame: [frames, vertices, 3]."""
    rot, pos = sample["global_rot_mats"].astype(np.float64), sample["posed_joints"].astype(np.float64)
    frames, joints = pos.shape[:2]
    posed = np.tile(np.eye(4), (frames, joints, 1, 1))
    posed[..., :3, :3], posed[..., :3, 3] = rot, pos
    affine = (posed @ np.linalg.inv(skin["bind_rig_transform"].astype(np.float64)))[..., :3, :]     # [F, J, 3, 4]
    bind = np.concatenate([skin["bind_vertices"], np.ones((len(skin["bind_vertices"]), 1))], axis=1)
    idx, w = skin["lbs_indices"], skin["lbs_weights"].astype(np.float64)
    out = np.zeros((frames, len(bind), 3))
    for k in range(idx.shape[1]):                                # one influence at a time keeps the memory small
        out += w[None, :, k, None] * np.einsum("fvij,vj->fvi", affine[:, idx[:, k]], bind)
    return out


def place_camera(cam, view: str):
    # The same places as gothic_rig.py's, about the pivot under the hips.
    if view == "side":
        cam.location, cam.rotation_euler = (6, 0, 0.9), (np.pi / 2, 0, np.pi / 2)
    elif view == "front":
        cam.location, cam.rotation_euler = (0, 6, 0.9), (np.pi / 2, 0, np.pi)
    elif view == "back":
        cam.location, cam.rotation_euler = (0, -6, 0.9), (np.pi / 2, 0, 0)
    else:
        cam.location, cam.rotation_euler = (4.0, 4.0, 2.2), (np.radians(75), 0, np.radians(135))


def main():
    cfg = json.loads(Path(sys.argv[sys.argv.index("--") + 1]).read_text())
    if cfg.get("seq"):
        seq = np.load(cfg["seq"])
        verts = seq["verts"].astype(np.float64)[..., [0, 2, 1]] * 0.01
        root = seq["root"].astype(np.float64)[..., [0, 2, 1]] * 0.01
        faces = seq["faces"][:, ::-1].tolist()
    else:
        sample, skin = np.load(cfg["npz"]), np.load(cfg["skin"])
        verts = skin_frames(sample, skin) @ TURN.T
        root = sample["root_positions"].astype(np.float64) @ TURN.T
        faces = skin["faces"].tolist()
    frames = len(verts)

    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.fps = 30
    me = bpy.data.meshes.new("SOMA")
    me.from_pydata(verts[0].tolist(), [], faces)
    me.update()
    body = bpy.data.objects.new("SOMA", me)
    scene.collection.objects.link(body)
    body.color = (*cfg.get("color", [0.55, 0.62, 0.72]), 1)
    for p in me.polygons:
        p.use_smooth = True

    rcfg = cfg["render"]
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "OBJECT"
    scene.render.resolution_x, scene.render.resolution_y = rcfg.get("size", [640, 640])
    scene.render.image_settings.file_format = "PNG"
    cx, cy = (root[:, 0].min() + root[:, 0].max()) / 2, (root[:, 1].min() + root[:, 1].max()) / 2
    extent = int(np.ceil(max(np.ptp(root[:, 0]), np.ptp(root[:, 1])))) + 4
    bpy.ops.mesh.primitive_plane_add(size=2 * extent, location=(cx, cy, 0))
    bpy.context.active_object.color = (0.42, 0.40, 0.37, 1)
    for k in range(-extent, extent + 1):
        w = 0.025 if k == 0 else 0.008
        for loc, scale in (((cx + k, cy, 0.001), (w, 2 * extent, 0.001)), ((cx, cy + k, 0.001), (2 * extent, w, 0.001))):
            bpy.ops.mesh.primitive_cube_add(size=1, location=loc)
            line = bpy.context.active_object
            line.scale, line.color = scale, (0.2, 0.19, 0.18, 1)
    pivot = bpy.data.objects.new("pivot", None)
    scene.collection.objects.link(pivot)
    cam_data = bpy.data.cameras.new("cam")
    cam_data.type, cam_data.ortho_scale = "ORTHO", rcfg.get("ortho", 2.8)
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    cam.parent = pivot
    scene.camera = cam

    out = Path(rcfg["dir"])
    for view in rcfg.get("views", ["side"]):
        place_camera(cam, view)
        (out / view).mkdir(parents=True, exist_ok=True)
        for f in range(frames):
            me.vertices.foreach_set("co", verts[f].ravel())
            me.update()
            pivot.location = (root[f, 0], root[f, 1], 0)
            scene.render.filepath = str(out / view / f"f{f + 1:04d}.png")
            bpy.ops.render.render(write_still=True)
    print("SOMA_VIEW_DONE frames", frames)


main()
