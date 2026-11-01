"""Build the Gothic human rig in Blender from the converted base and play MAN records on it.

    blender -b --factory-startup -P scripts/content/anim/blender/gothic_rig.py -- config.json

config.json:
    {"models": "<base>/models", "skeleton": "HUMANS.MDH", "body": "HUM_BODY_NAKED0.MDM",
     "anims": ["HUMANS-T_JUMPB.MAN", "/abs/path/OUR-T_DASHB.MAN.json"],
     "blend": "/abs/out/rig.blend",                         # optional
     "render": {"dir": "/abs/out/frames", "views": ["side", "front"], "size": [640, 640],
                "sword": true}}                                # optional

Gothic is left-handed, Y up, centimetres, facing +Z. Blender gets (x, z, y) in metres: the swap
is a reflection, which is what turns the left-handed frame into Blender's right-handed one.
The character then faces +Y and its right hand is at +X. The .blend holds original content of the
player's install: keep it out of git.
"""
import json
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Matrix

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gothic_anim as ga  # noqa: E402

SWAP = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=np.float64)
CM = 0.01


def to_blender(m: np.ndarray) -> Matrix:
    b = SWAP @ m @ SWAP
    b[:3, 3] *= CM
    return Matrix(b.tolist())


def record(models: Path, name: str) -> Path:
    p = Path(name)
    if p.is_absolute():
        return p
    return models / (name + ".json")


def clear_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def build_armature(skel: ga.Skeleton):
    bind = ga.world_pose(skel, None, 0)
    arm = bpy.data.armatures.new("GothicHuman")
    obj = bpy.data.objects.new("GothicHuman", arm)
    bpy.context.scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode="EDIT")
    for i, n in enumerate(skel.nodes):
        eb = arm.edit_bones.new(n.name)
        mb = to_blender(bind[i])
        head = mb.translation
        # Biped bones run along the node's local X; a short stub keeps leaf nodes visible.
        children = [j for j, c in enumerate(skel.nodes) if c.parent == i]
        length = 0.05
        if children:
            d = (to_blender(bind[children[0]]).translation - head).length
            length = max(d, 0.02)
        x_axis = mb.to_3x3().col[0].normalized()
        eb.head = head
        eb.tail = head + x_axis * length
        eb.align_roll(mb.to_3x3().col[2])
        if n.parent >= 0:
            eb.parent = arm.edit_bones[skel.nodes[n.parent].name]
    bpy.ops.object.mode_set(mode="OBJECT")
    # Constant offset between a node frame and Blender's bone frame, per bone.
    offset = {}
    for i, n in enumerate(skel.nodes):
        offset[n.name] = to_blender(bind[i]).inverted() @ arm.bones[n.name].matrix_local
    return obj, offset


def build_body(skel: ga.Skeleton, mdm: Path, arm_obj):
    mesh = ga.read_skin_mesh(mdm, skel)
    verts = [tuple((SWAP[:3, :3] @ p) * CM) for p in mesh.positions]
    faces = [tuple(t) for t in mesh.triangles.tolist()]
    me = bpy.data.meshes.new("Body")
    me.from_pydata(verts, [], faces)
    me.update()
    obj = bpy.data.objects.new("Body", me)
    bpy.context.scene.collection.objects.link(obj)
    for i, n in enumerate(skel.nodes):
        obj.vertex_groups.new(name=n.name)
    for v, vw in enumerate(mesh.weights):
        for node, w in vw:
            obj.vertex_groups[skel.nodes[node].name].add([v], w, "REPLACE")
    mod = obj.modifiers.new("Armature", "ARMATURE")
    mod.object = arm_obj
    obj.parent = arm_obj
    for p in me.polygons:
        p.use_smooth = True
    return obj


def add_head(arm_obj):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=0.11, segments=16, ring_count=8)
    head = bpy.context.active_object
    head.name = "Head"
    head.parent = arm_obj
    head.parent_type = "BONE"
    head.parent_bone = "BIP01 HEAD"
    head.matrix_parent_inverse = Matrix.Identity(4)
    head.matrix_world = arm_obj.matrix_world @ arm_obj.pose.bones["BIP01 HEAD"].matrix @ Matrix.Translation((0, 0.08, 0))
    return head


def add_sword(arm_obj):
    """A plain 1H blade on ZS_RIGHTHAND: 0.9 m along the slot's -X, where the engine's weapon meshes lie."""
    bpy.ops.mesh.primitive_cube_add(size=1)
    blade = bpy.context.active_object
    blade.name = "Sword"
    blade.scale = (0.02, 0.9, 0.05)
    bpy.ops.object.transform_apply(scale=True)
    for v in blade.data.vertices:
        v.co.y -= 0.45
    blade.parent = arm_obj
    blade.parent_type = "BONE"
    blade.parent_bone = "ZS_RIGHTHAND"
    blade.matrix_world = arm_obj.matrix_world @ arm_obj.pose.bones["ZS_RIGHTHAND"].matrix
    return blade


def apply_animation(arm_obj, offset, skel: ga.Skeleton, anim: ga.Animation, start: int) -> int:
    """Key the clip's frames from `start`; returns the next free frame."""
    names = [n.name for n in skel.nodes]
    for f in range(anim.frame_count):
        world = ga.world_pose(skel, anim, f)
        for i, name in enumerate(names):
            pb = arm_obj.pose.bones[name]
            pose = to_blender(world[i]) @ offset[name]
            if pb.parent is None:
                pb.matrix_basis = pb.bone.matrix_local.inverted() @ pose
            else:
                parent = to_blender(world[skel.nodes[i].parent]) @ offset[pb.parent.name]
                rest_rel = pb.parent.bone.matrix_local.inverted() @ pb.bone.matrix_local
                pb.matrix_basis = rest_rel.inverted() @ parent.inverted() @ pose
            pb.keyframe_insert("location", frame=start + f)
            pb.keyframe_insert("rotation_quaternion", frame=start + f)
    return start + anim.frame_count


def setup_render(cfg, frames: int, center, span, arm_obj):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.display.shading.light = "STUDIO"
    scene.display.shading.color_type = "OBJECT"
    scene.render.resolution_x, scene.render.resolution_y = cfg.get("size", [640, 640])
    scene.render.image_settings.file_format = "PNG"
    scene.frame_start, scene.frame_end = 1, frames
    extent = int(np.ceil(span)) + 4
    cx, cy = center
    bpy.ops.mesh.primitive_plane_add(size=2 * extent, location=(cx, cy, 0))
    bpy.context.active_object.color = (0.42, 0.40, 0.37, 1)
    # Lines every metre, a thicker one through the clip's start, to read distances off frames.
    for k in range(-extent, extent + 1):
        w = 0.025 if k == 0 else 0.008
        for loc, scale in (((cx + k, cy, 0.001), (w, 2 * extent, 0.001)),
                           ((cx, cy + k, 0.001), (2 * extent, w, 0.001))):
            bpy.ops.mesh.primitive_cube_add(size=1, location=loc)
            line = bpy.context.active_object
            line.scale = scale
            line.color = (0.2, 0.19, 0.18, 1)
    pivot = bpy.data.objects.new("pivot", None)
    scene.collection.objects.link(pivot)
    if cfg.get("follow", True):
        c = pivot.constraints.new("COPY_LOCATION")
        c.target, c.subtarget = arm_obj, "BIP01"
        c.use_z = False
    else:
        pivot.location = (cx, cy, 0)
    cam_data = bpy.data.cameras.new("cam")
    cam_data.type = "ORTHO"
    cam_data.ortho_scale = cfg.get("ortho", 2.8) if cfg.get("follow", True) else max(2.8, span + 1.4)
    cam = bpy.data.objects.new("cam", cam_data)
    scene.collection.objects.link(cam)
    cam.parent = pivot
    scene.camera = cam
    return cam


def place_camera(cam, view: str):
    # Relative to the pivot under the hips; the character faces +Y, its right hand is at +X.
    if view == "side":
        cam.location = (6, 0, 0.9)
        cam.rotation_euler = (np.pi / 2, 0, np.pi / 2)
    elif view == "front":
        cam.location = (0, 6, 0.9)
        cam.rotation_euler = (np.pi / 2, 0, np.pi)
    elif view == "back":
        cam.location = (0, -6, 0.9)
        cam.rotation_euler = (np.pi / 2, 0, 0)
    else:  # three-quarter from the front right, slightly above
        cam.location = (4.0, 4.0, 2.2)
        cam.rotation_euler = (np.radians(75), 0, np.radians(135))


def main():
    cfg = json.loads(Path(sys.argv[sys.argv.index("--") + 1]).read_text())
    models = Path(cfg["models"])
    clear_scene()
    bpy.context.scene.render.fps = 25
    skel = ga.read_skeleton(record(models, cfg.get("skeleton", "HUMANS.MDH")))
    arm_obj, offset = build_armature(skel)
    for pb in arm_obj.pose.bones:
        pb.rotation_mode = "QUATERNION"
    body = build_body(skel, record(models, cfg.get("body", "HUM_BODY_NAKED0.MDM")), arm_obj)
    body.color = (0.72, 0.6, 0.5, 1)
    head = add_head(arm_obj)
    head.color = body.color
    rcfg = cfg.get("render")
    if rcfg and rcfg.get("sword"):
        add_sword(arm_obj).color = (0.75, 0.75, 0.8, 1)
    frame = 1
    roots = []
    marks = []
    for name in cfg["anims"]:
        anim = ga.read_animation(record(models, name))
        marks.append({"anim": anim.name, "file": name, "first": frame, "frames": anim.frame_count,
                      "fps": anim.fps, "travel_cm": ga.root_travel(anim)[-1].round(1).tolist()})
        if anim.node_indices[0] == 0:
            roots.append(anim.positions[:, 0])
        frame = apply_animation(arm_obj, offset, skel, anim, frame)
    total = frame - 1
    if cfg.get("blend"):
        bpy.ops.wm.save_as_mainfile(filepath=cfg["blend"])
    if rcfg:
        pts = np.concatenate(roots) if roots else np.zeros((1, 3))
        xz = pts[:, [0, 2]] * CM
        center = ((xz[:, 0].min() + xz[:, 0].max()) / 2, (xz[:, 1].min() + xz[:, 1].max()) / 2)
        span = float(max(np.ptp(xz[:, 0]), np.ptp(xz[:, 1])))
        cam = setup_render(rcfg, total, center, span, arm_obj)
        out = Path(rcfg["dir"])
        for view in rcfg.get("views", ["side"]):
            place_camera(cam, view)
            (out / view).mkdir(parents=True, exist_ok=True)
            bpy.context.scene.render.filepath = str(out / view / "f")
            bpy.ops.render.render(animation=True)
        (out / "clips.json").write_text(json.dumps(marks, indent=1))
    print("GOTHIC_RIG_DONE frames", total)


main()
