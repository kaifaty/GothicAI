"""Render the views of ../game_view.py: each prop alone, as the game frames it, into the layers its engine frame is
made of.

blender -b --factory-startup -P game_view.py -- WORK/scene.json

scene.json (written by ../game_view.py): fov_y, frame, out, views [{name, eye, target, orig, ours}], each object an
npz (positions, normals, uv, triangles, slots; Blender metres, placed) and its materials [{name, albedo, normal,
orm}]. Writes <view>-orig.exr and <view>-ours.exr: Diffuse Color (the albedo, times 1 - metal), Diffuse Direct
(the sky's light on the shading normal, normal map included, EEVEE's horizon occlusion standing in for the engine's
SSAO), the AOV "ao" (ORM red for ours, 1 for the original: the engine's ambient_light.frag occludes only the sky with
it) and the alpha. ../game_view.py turns them into the engine's frame. EEVEE samples textures as the engine does:
trilinear with 16x anisotropy. The light is the sky alone, brighter from above (the indoors of the furniture: the
sun is shadowed, and the engine's sky is not), strength 1 at the zenith.
"""
import json
import math
import sys
from pathlib import Path

import bpy
import numpy as np
from mathutils import Vector

scene_path = Path(sys.argv[sys.argv.index("--") + 1])
spec = json.loads(scene_path.read_text())
out = Path(spec["out"])

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.context.preferences.system.anisotropic_filter = "FILTER_16"
scene = bpy.context.scene
scene.render.engine = "BLENDER_EEVEE"
scene.eevee.taa_render_samples = 16
scene.render.resolution_x, scene.render.resolution_y = spec["frame"]
scene.render.film_transparent = True
scene.render.image_settings.media_type = "MULTI_LAYER_IMAGE"
scene.render.image_settings.file_format = "OPEN_EXR_MULTILAYER"
layer = scene.view_layers[0]
layer.use_pass_diffuse_color = layer.use_pass_diffuse_direct = True
aov_ao = layer.aovs.add()
aov_ao.name, aov_ao.type = "ao", "VALUE"

world = bpy.data.worlds.new("sky")
world.use_nodes = True
wt = world.node_tree
coord = wt.nodes.new("ShaderNodeTexCoord")
axes = wt.nodes.new("ShaderNodeSeparateXYZ")
wt.links.new(coord.outputs["Generated"], axes.inputs["Vector"])
ramp = wt.nodes.new("ShaderNodeMapRange")  # ground 0.3 (sky.cpp groundAlbedo), horizon 0.6, zenith 1
ramp.inputs["From Min"].default_value, ramp.inputs["To Min"].default_value = -1.0, 0.0
wt.links.new(axes.outputs["Z"], ramp.inputs["Value"])
curve = wt.nodes.new("ShaderNodeFloatCurve")
curve.mapping.curves[0].points[0].location = (0.0, 0.3)
curve.mapping.curves[0].points[1].location = (1.0, 1.0)
curve.mapping.curves[0].points.new(0.5, 0.6)
wt.links.new(ramp.outputs["Result"], curve.inputs["Value"])
wt.links.new(curve.outputs["Value"], wt.nodes["Background"].inputs["Strength"])
wt.nodes["Background"].inputs["Color"].default_value = (1, 1, 1, 1)
scene.world = world

cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
scene.collection.objects.link(cam)
scene.camera = cam
cam.data.sensor_fit = "VERTICAL"
cam.data.angle_y = math.radians(spec["fov_y"])
cam.data.clip_start, cam.data.clip_end = 0.05, 500

images = {}


def image(path, colour):
    if path not in images:
        images[path] = bpy.data.images.load(path)
        images[path].colorspace_settings.name = "sRGB" if colour else "Non-Color"
    return images[path]


def material(spec_m, own):
    m = bpy.data.materials.new(spec_m["name"])
    m.use_nodes = True
    nt = m.node_tree
    bsdf = nt.nodes["Principled BSDF"]
    bsdf.inputs["Roughness"].default_value = 1.0
    bsdf.inputs["Specular IOR Level"].default_value = 0.0
    aov = nt.nodes.new("ShaderNodeOutputAOV")
    aov.aov_name = "ao"
    aov.inputs["Value"].default_value = 1.0
    if not spec_m.get("albedo"):
        bsdf.inputs["Base Color"].default_value = (1, 0, 1, 1)
        return m
    albedo = nt.nodes.new("ShaderNodeTexImage")
    albedo.image = image(spec_m["albedo"], True)
    nt.links.new(albedo.outputs["Alpha"], bsdf.inputs["Alpha"])
    m.surface_render_method = "DITHERED"
    colour = albedo.outputs["Color"]
    if own and spec_m.get("orm"):  # the engine reads roughness and metal; occlusion (red) it does not
        orm = nt.nodes.new("ShaderNodeTexImage")
        orm.image = image(spec_m["orm"], False)
        split = nt.nodes.new("ShaderNodeSeparateColor")
        nt.links.new(orm.outputs["Color"], split.inputs["Color"])
        nt.links.new(split.outputs["Red"], aov.inputs["Value"])
        nt.links.new(split.outputs["Green"], bsdf.inputs["Roughness"])
        nt.links.new(split.outputs["Blue"], bsdf.inputs["Metallic"])
        gloss = nt.nodes.new("ShaderNodeMapRange")  # materialSpecularWeight: smoothstep(0.5, 0.8, roughness)
        gloss.interpolation_type = "SMOOTHSTEP"
        gloss.inputs["From Min"].default_value, gloss.inputs["From Max"].default_value = 0.5, 0.8
        gloss.inputs["To Min"].default_value, gloss.inputs["To Max"].default_value = 0.5, 0.0
        nt.links.new(split.outputs["Green"], gloss.inputs["Value"])
        nt.links.new(gloss.outputs["Result"], bsdf.inputs["Specular IOR Level"])
    if own and spec_m.get("normal"):
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = image(spec_m["normal"], False)
        nmap = nt.nodes.new("ShaderNodeNormalMap")
        nt.links.new(tex.outputs["Color"], nmap.inputs["Color"])
        nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
    nt.links.new(colour, bsdf.inputs["Base Color"])
    return m


def build(name, obj_spec, own):
    d = np.load(obj_spec["npz"])
    p, n, uv, tris, slots = d["positions"], d["normals"], d["uv"], d["triangles"], d["slots"]
    me = bpy.data.meshes.new(name)
    me.vertices.add(len(p))
    me.vertices.foreach_set("co", p.astype(np.float32).ravel())
    me.loops.add(tris.size)
    me.loops.foreach_set("vertex_index", tris.astype(np.int32).ravel())
    me.polygons.add(len(tris))
    me.polygons.foreach_set("loop_start", (np.arange(len(tris)) * 3).astype(np.int32))
    me.polygons.foreach_set("material_index", slots.astype(np.int32))
    layer = me.uv_layers.new(name="UVMap")
    corner = uv[tris.ravel()]
    layer.data.foreach_set("uv", np.c_[corner[:, 0], 1 - corner[:, 1]].astype(np.float32).ravel())
    me.update()
    me.validate()
    me.polygons.foreach_set("use_smooth", np.ones(len(me.polygons), dtype=bool))
    me.normals_split_custom_set_from_vertices([tuple(v) for v in n / np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)])
    for m in obj_spec["materials"]:
        me.materials.append(material(m, own))
    ob = bpy.data.objects.new(name, me)
    scene.collection.objects.link(ob)
    return ob


for view in spec["views"]:
    eye, target = Vector(view["eye"]), Vector(view["target"])
    cam.location = eye
    forward = (target - eye).normalized()
    cam.rotation_euler = forward.to_track_quat("-Z", "Y").to_euler()
    objects = {k: build(f"{view['name']}-{k}", view[k], k == "ours") for k in ("orig", "ours") if k in view}
    for key, ob in objects.items():
        for other in objects.values():
            other.hide_render = other is not ob
        scene.render.filepath = str(out / f"{view['name']}-{key}.exr")
        bpy.ops.render.render(write_still=True)
    for ob in objects.values():
        bpy.data.objects.remove(ob)
    print("VIEW", view["name"], flush=True)
print("GAME_VIEW DONE", flush=True)
