"""Three views of a prop's debris (model.debris.glb) next to the whole prop, for judging them by eye.

blender -b --factory-startup -P debris_views.py -- WHOLE.glb DEBRIS.glb OUT_DIR [MAPS_DIR ...] [--focus NAME,NAME]
MAPS_DIR: a baked atlas (bake_atlas.py OUT/maps) with <material key lower>/{albedo,normal,roughness}.png;
without one every material gets a flat colour by its name, to check the zones.
Writes OUT_DIR/assembled.png (the whole and the pieces in its pose side by side, one camera and light:
silhouette and tone must match), exploded.png (every piece moved off the centre: staves out, hoops and
heads up and down) and closeup.png (the --focus pieces, by default the first broken stave's halves
and one board, turned so their inner side and the break face the camera).
"""
import math
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector

argv = sys.argv[sys.argv.index("--") + 1:]
focus = None
if "--focus" in argv:
    k = argv.index("--focus")
    focus = argv[k + 1].split(",")
    argv = argv[:k] + argv[k + 2:]
whole_path, debris_path, out = Path(argv[0]), Path(argv[1]), Path(argv[2])
maps_dirs = [Path(p) for p in argv[3:]]
out.mkdir(parents=True, exist_ok=True)
FLAT = {"STAVE": (0.12, 0.08, 0.05), "HEAD": (0.09, 0.06, 0.035), "BAND": (0.05, 0.05, 0.05),
        "INNER": (0.05, 0.12, 0.2), "END": (0.6, 0.35, 0.12), "RUST": (0.25, 0.06, 0.03)}


def reset(ground=True):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    scene = bpy.context.scene
    scene.render.engine = "CYCLES"
    scene.cycles.samples = 48
    scene.cycles.use_denoising = True
    scene.render.resolution_x, scene.render.resolution_y = 1100, 760
    scene.view_settings.view_transform = "AgX"
    world = bpy.data.worlds.new("world")
    world.use_nodes = True
    world.node_tree.nodes["Background"].inputs[0].default_value = (0.55, 0.6, 0.68, 1)
    world.node_tree.nodes["Background"].inputs[1].default_value = 0.6
    scene.world = world
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 3.5
    sun.data.angle = math.radians(3)
    sun.rotation_euler = (math.radians(50), 0, math.radians(35))
    scene.collection.objects.link(sun)
    if ground:
        mesh = bpy.data.meshes.new("ground")
        mesh.from_pydata([(-60, -60, -0.559), (60, -60, -0.559), (60, 60, -0.559), (-60, 60, -0.559)], [], [(0, 1, 2, 3)])
        mat = bpy.data.materials.new("ground")
        mat.use_nodes = True
        mat.node_tree.nodes["Principled BSDF"].inputs["Base Color"].default_value = (0.18, 0.17, 0.16, 1)
        mesh.materials.append(mat)
        scene.collection.objects.link(bpy.data.objects.new("ground", mesh))
    return scene


def dress(objects):
    """Material by its name: the baked atlas when a maps dir has it, else a flat colour of its zone."""
    for o in objects:
        for slot in o.material_slots:
            mat = slot.material
            if mat is None or mat.get("_dressed"):
                continue
            mat["_dressed"] = True
            key = mat.name.split(".")[0]
            mat.use_nodes = True
            nodes, links = mat.node_tree.nodes, mat.node_tree.links
            bsdf = nodes["Principled BSDF"]
            folder = next((d / key.lower() for d in maps_dirs if (d / key.lower() / "albedo.png").exists()), None)
            if folder is None:
                colour = next((c for s, c in FLAT.items() if key.endswith(s)), (0.5, 0.5, 0.5))
                bsdf.inputs["Base Color"].default_value = (*colour, 1)
                continue
            for name, socket, colour in (("albedo", "Base Color", True), ("roughness", "Roughness", False),
                                         ("metallic", "Metallic", False)):
                tex = nodes.new("ShaderNodeTexImage")
                tex.image = bpy.data.images.load(str(folder / f"{name}.png"))
                tex.image.colorspace_settings.name = "sRGB" if colour else "Non-Color"
                links.new(tex.outputs[0], bsdf.inputs[socket])
            tex = nodes.new("ShaderNodeTexImage")
            tex.image = bpy.data.images.load(str(folder / "normal.png"))
            tex.image.colorspace_settings.name = "Non-Color"
            nmap = nodes.new("ShaderNodeNormalMap")
            links.new(tex.outputs[0], nmap.inputs["Color"])
            links.new(nmap.outputs[0], bsdf.inputs["Normal"])


def load(path):
    before = set(bpy.context.scene.objects)
    bpy.ops.import_scene.gltf(filepath=str(path))
    objects = [o for o in bpy.context.scene.objects if o not in before and o.type == "MESH"]
    for o in objects:  # bake the import's axis turn into the mesh, so pieces move in world axes
        o.data.transform(o.matrix_world)
        o.matrix_world = Matrix.Identity(4)
    for o in [o for o in bpy.context.scene.objects if o not in before and o.type != "MESH"]:
        bpy.data.objects.remove(o)
    dress(objects)
    return objects


def centre(o):
    return sum((v.co for v in o.data.vertices), Vector()) / len(o.data.vertices)


def camera(scene, target, distance, elevation, azimuth, lens=50):
    cam = bpy.data.objects.new("camera", bpy.data.cameras.new("camera"))
    cam.data.lens = lens
    e, a = math.radians(elevation), math.radians(azimuth)
    cam.location = target + Vector((math.cos(e) * math.sin(a), -math.cos(e) * math.cos(a), math.sin(e))) * distance
    cam.rotation_euler = (target - cam.location).to_track_quat("-Z", "Y").to_euler()
    scene.collection.objects.link(cam)
    scene.camera = cam


def render(scene, name):
    scene.render.filepath = str(out / f"{name}.png")
    bpy.ops.render.render(write_still=True)


# 1. the whole and the pieces in its pose
scene = reset()
whole = load(whole_path)
for o in whole:
    o.location.x -= 0.5
pieces = load(debris_path)
for o in pieces:
    o.location.x += 0.5
camera(scene, Vector((0, 0, 0.0)), 3.6, 14, 0)
render(scene, "assembled")

# 2. exploded: staves out, broken halves apart, hoops spread up the axis, lid up, bottom down
scene = reset(ground=False)
pieces = load(debris_path)
axis = Vector((0, 0, 0))
for o in pieces:
    c = centre(o)
    radial = Vector((c.x, c.y, 0))
    name = o.name
    if name.startswith("stave"):
        o.location = radial.normalized() * 0.38 + Vector((0, 0, 0.1 if name.endswith("-top") else 0))
    elif name.startswith("hoop"):
        k = int(name.split("-")[1])
        o.location = Vector((0, 0, (k - 2.5) * 0.28))
    elif name.startswith("lid"):
        k = int(name.split("-")[1])
        o.location = Vector(((k - 2.5) * 0.05, 0, 0.55))
    elif name.startswith("bottom"):
        k = int(name.split("-")[1])
        o.location = Vector(((k - 2.5) * 0.05, 0, -0.55))
camera(scene, Vector((0, 0, 0)), 4.6, 22, 20)
render(scene, "exploded")

# 3. close: a broken stave's halves turned inner side out, its neighbour outer side out, one lid board
scene = reset(ground=False)
pieces = {o.name: o for o in load(debris_path)}
if focus is None:
    half = next(n for n in sorted(pieces) if n.endswith("-top"))
    stave = half[:-4]
    focus = [stave, half, "lid-2"]
shown = [pieces[n] for n in focus]
for o in pieces.values():
    if o not in shown:
        bpy.data.objects.remove(o)
c0 = centre(pieces[focus[0]])
# turn about the barrel's axis so the first piece's inner side faces the camera (-Y), a little to the side
spin = Matrix.Rotation(math.pi / 2 - math.atan2(c0.y, c0.x) + math.radians(25), 4, "Z")
c0 = spin @ c0
tops = [o for o in shown if o.name.endswith("-top")]
z_look = min(v.co.z for v in tops[0].data.vertices) if tops else c0.z  # the break
for o in shown:
    if o.name.startswith(("lid", "bottom")):
        c = centre(o)
        o.data.transform(Matrix.Translation(-c))
        o.data.transform(Matrix.Rotation(math.radians(-65), 4, "X"))
        o.location = Vector((0.26, 0.05, z_look))
    else:
        o.data.transform(spin)
        o.data.transform(Matrix.Translation(-Vector((c0.x, c0.y, 0))))
        if o.name.endswith("-top"):
            o.location = Vector((0.0, 0.0, 0.05))
camera(scene, Vector((0.11, 0, z_look + 0.02)), 0.95, 32, 0, lens=50)
render(scene, "closeup")
print("VIEWS", out)
