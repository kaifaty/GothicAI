"""CPU Workbench comparison sheets, one row per generator/concept/seed or control.

Blender CLI: -b -P sheet.py -- --variants variants.json --out sheet.png
The input is {"title": "...", "variants": [{"label": "...", "concept": "...png",
"raw": "...npz", "meta": "...json", "result": "...output-directory"}]}.
Paths are relative to the JSON file. Only result is required for a control row.
"""

import argparse
import json
import os
from pathlib import Path
import sys
import tempfile

# Force Mesa's CPU renderer before Blender initializes its render context.
os.environ["LIBGL_ALWAYS_SOFTWARE"] = "1"

import bpy
from mathutils import Vector
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import geometry
import postprocess


CELL = 360
GAP = 8
HEADER = 82
ROW_CAPTION = 64
BACKGROUND = np.array([0.945, 0.95, 0.96, 1.0], dtype=np.float32)


def clear_scene():
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    # Forty pilot variants must not retain all dense raw mesh datablocks.
    for blocks in (bpy.data.meshes, bpy.data.curves, bpy.data.materials, bpy.data.cameras):
        for block in list(blocks):
            if block.users == 0:
                blocks.remove(block)


def configure(width, height, transparent=False):
    scene = bpy.context.scene
    scene.render.engine = "BLENDER_WORKBENCH"
    scene.render.resolution_x = width
    scene.render.resolution_y = height
    scene.render.resolution_percentage = 100
    scene.render.image_settings.file_format = "PNG"
    scene.render.film_transparent = transparent
    scene.view_settings.view_transform = "Standard"
    shading = scene.display.shading
    shading.light = "STUDIO"
    shading.color_type = "MATERIAL"
    shading.background_type = "WORLD"
    shading.show_shadows = True
    shading.show_cavity = True
    shading.cavity_type = "BOTH"
    shading.show_specular_highlight = False
    scene.world.color = tuple(BACKGROUND[:3])
    return scene


def render_pixels(directory, name):
    path = Path(directory) / f"{name}.png"
    bpy.context.scene.render.filepath = str(path)
    bpy.ops.render.render(write_still=True)
    image = bpy.data.images.load(str(path), check_existing=False)
    width, height = image.size
    pixels = np.asarray(image.pixels[:], dtype=np.float32).reshape(height, width, 4)
    bpy.data.images.remove(image)
    return pixels


def camera_for(objects):
    points = np.array([obj.matrix_world @ Vector(corner)
                       for obj in objects for corner in obj.bound_box])
    center = (points.min(axis=0) + points.max(axis=0)) * 0.5
    extent = max(float(np.linalg.norm(points.max(axis=0) - points.min(axis=0))), 0.01)
    bpy.ops.object.camera_add(location=center + np.array([1.0, -1.6, 1.0]) * extent)
    camera = bpy.context.object
    camera.rotation_euler = (Vector(center) - camera.location).to_track_quat("-Z", "Y").to_euler()
    camera.data.type = "ORTHO"
    camera.data.ortho_scale = extent * 1.03
    camera.data.clip_start = extent * 0.001
    camera.data.clip_end = extent * 20
    bpy.context.scene.camera = camera
    return extent


def read_result(path):
    directory = path if path.is_dir() else path.parent
    model = directory / "model.glb" if path.is_dir() else path
    metrics = json.loads((directory / "metrics.json").read_text())
    clear_scene()
    configure(CELL, CELL)
    bpy.ops.import_scene.gltf(filepath=str(model))
    objects = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
    if not objects:
        raise ValueError(f"No mesh objects in {model}")
    extent = camera_for(objects)
    return objects, extent, metrics, directory


def checker_material():
    # Ten alternating squares per UV unit; one UV unit is one metre.
    image = bpy.data.images.get("mesh3d-checker-10cm")
    if image is None:
        image = bpy.data.images.new("mesh3d-checker-10cm", width=100, height=100)
        yy, xx = np.indices((100, 100))
        shade = np.where((xx // 10 + yy // 10) % 2, 0.12, 0.90)
        pixels = np.ones((100, 100, 4), dtype=np.float32)
        pixels[:, :, :3] = shade[:, :, None]
        image.pixels.foreach_set(pixels.ravel())
        image.pack()
    material = bpy.data.materials.new("10 cm UV checker")
    material.use_nodes = True
    texture = material.node_tree.nodes.new("ShaderNodeTexImage")
    texture.image = image
    texture.interpolation = "Closest"
    texture.extension = "REPEAT"
    material.node_tree.nodes.active = texture
    material.node_tree.links.new(texture.outputs["Color"],
                                 material.node_tree.nodes.get("Principled BSDF").inputs["Base Color"])
    return material


def plain_material(name, color):
    material = bpy.data.materials.new(name)
    material.diffuse_color = (*color, 1.0)
    return material


def render_result_cells(objects, extent, directory):
    zone = render_pixels(directory, "zones")
    checker = checker_material()
    for obj in objects:
        obj.data.materials.clear()
        obj.data.materials.append(checker)
        for face in obj.data.polygons:
            face.material_index = 0
    bpy.context.scene.display.shading.color_type = "TEXTURE"
    uv = render_pixels(directory, "uv")

    fill = plain_material("wire fill", [0.77, 0.79, 0.81])
    edge = plain_material("wire edges", [0.025, 0.035, 0.045])
    for obj in objects:
        obj.data.materials.clear()
        obj.data.materials.append(fill)
        obj.data.materials.append(edge)
        wire = obj.modifiers.new("Visible triangle edges", "WIREFRAME")
        wire.thickness = extent * 0.0008
        wire.use_replace = False
        wire.use_even_offset = False
        wire.use_boundary = True
        wire.material_offset = 1
    bpy.context.scene.display.shading.color_type = "MATERIAL"
    wireframe = render_pixels(directory, "wire")
    return zone, uv, wireframe


def render_raw(path, meta_path, target, directory):
    clear_scene()
    configure(CELL, CELL)
    with np.load(path) as raw:
        vertices = geometry.fit_bbox(raw["vertices"], json.loads(meta_path.read_text()), target)
        obj = postprocess.make_object("Raw vertex color", vertices, raw["faces"], raw["color"],
                                      raw["metallic"] if "metallic" in raw else None)
    camera_for([obj])
    bpy.context.scene.display.shading.color_type = "VERTEX"
    return render_pixels(directory, "raw")


def concept_pixels(path):
    image = bpy.data.images.load(str(path), check_existing=False)
    width, height = image.size
    pixels = np.asarray(image.pixels[:], dtype=np.float32).reshape(height, width, 4)
    scale = min(CELL / width, CELL / height)
    out_width, out_height = max(1, round(width * scale)), max(1, round(height * scale))
    # Nearest sampling suffices for a small diagnostic thumbnail and avoids a dependency.
    xs = np.minimum((np.arange(out_width) / scale).astype(int), width - 1)
    ys = np.minimum((np.arange(out_height) / scale).astype(int), height - 1)
    thumbnail = pixels[ys[:, None], xs[None, :]]
    cell = np.broadcast_to(BACKGROUND, (CELL, CELL, 4)).copy()
    x, y = (CELL - out_width) // 2, (CELL - out_height) // 2
    alpha = thumbnail[:, :, 3:4]
    cell[y:y + out_height, x:x + out_width, :3] = (
        thumbnail[:, :, :3] * alpha + BACKGROUND[:3] * (1 - alpha))
    bpy.data.images.remove(image)
    return cell


def caption_metrics(metrics):
    triangles = metrics["triangles"]
    opened = metrics["open_edges"]
    nonmanifold = metrics["non_manifold_edges"]
    seconds = metrics["seconds"]
    zones = metrics["zone_area_fractions"]
    zone_text = ", ".join(f"{key} {value:.0%}" for key, value in zones.items())
    return (f"{triangles:,} tris | open {opened} | non-manifold {nonmanifold} | {seconds:.1f} s"
            + (f" | {zone_text}" if zone_text else ""))


def add_text(body, x, y, size, width=None):
    curve = bpy.data.curves.new("Sheet caption", "FONT")
    curve.body = body
    curve.size = size
    curve.align_y = "BOTTOM_BASELINE"
    font_path = Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
    if font_path.exists():
        font = bpy.data.fonts.get("DejaVu Sans") or bpy.data.fonts.load(str(font_path))
        curve.font = font
    obj = bpy.data.objects.new("Sheet caption", curve)
    bpy.context.collection.objects.link(obj)
    obj.location = (x, y, 0)
    curve.materials.append(plain_material("Caption ink", [0.025, 0.035, 0.045]))
    bpy.context.view_layer.update()
    if width and obj.dimensions.x > width:
        obj.scale *= width / obj.dimensions.x


def render_captions(width, height, captions, directory):
    clear_scene()
    scene = configure(width, height, transparent=True)
    scene.display.shading.light = "FLAT"
    scene.display.shading.show_shadows = False
    scene.display.shading.show_cavity = False
    for body, x, y, size, max_width in captions:
        add_text(body, x, y, size, max_width)
    bpy.ops.object.camera_add(location=(width * 0.5, height * 0.5, 100))
    camera = bpy.context.object
    camera.rotation_euler = (0, 0, 0)
    camera.data.type = "ORTHO"
    camera.data.sensor_fit = "HORIZONTAL"
    camera.data.ortho_scale = width
    scene.camera = camera
    return render_pixels(directory, "captions")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variants", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    manifest = json.loads(args.variants.read_text())
    variants = manifest["variants"]
    if not variants:
        raise ValueError("At least one comparison variant is required")
    base = args.variants.resolve().parent

    def path(value):
        return (base / value).resolve()

    width = CELL * 5 + GAP * 6
    row_height = CELL + ROW_CAPTION + GAP
    height = HEADER + len(variants) * row_height + GAP
    canvas = np.broadcast_to(BACKGROUND, (height, width, 4)).copy()
    captions = [(manifest.get("title", "3D generator pilot"), GAP, height - 30, 22, width - 2 * GAP)]
    for column, title in enumerate(["Concept", "Raw / vertex color", "Final / zones",
                                    "UV checker / 10 cm", "Final / wireframe"]):
        captions.append((title, GAP + column * (CELL + GAP), height - 66, 17, CELL))

    with tempfile.TemporaryDirectory(prefix="mesh3d-sheet-") as temporary:
        for row, variant in enumerate(variants):
            objects, extent, metrics, result_dir = read_result(path(variant["result"]))
            zone, uv, wire = render_result_cells(objects, extent, temporary)
            blank = np.broadcast_to(BACKGROUND, (CELL, CELL, 4)).copy()
            concept = concept_pixels(path(variant["concept"])) if variant.get("concept") else blank.copy()
            raw_path = path(variant["raw"]) if variant.get("raw") else result_dir / "raw.npz"
            if raw_path.exists():
                meta_path = path(variant["meta"]) if variant.get("meta") else raw_path.parent / "meta.json"
                raw = render_raw(raw_path, meta_path, metrics["target"], temporary)
            else:
                raw = blank.copy()
            y = height - HEADER - row * row_height - CELL
            for column, pixels in enumerate([concept, raw, zone, uv, wire]):
                x = GAP + column * (CELL + GAP)
                canvas[y:y + CELL, x:x + CELL] = pixels
            label = variant.get("label", result_dir.name)
            captions.append((label, GAP, y - 23, 18, width - GAP * 2))
            captions.append((caption_metrics(metrics), GAP, y - 47, 13, width - GAP * 2))
            if not variant.get("concept"):
                captions.append(("No concept", GAP + 26, y + CELL * 0.5, 21, CELL - 52))
            if not raw_path.exists():
                captions.append(("No raw mesh", 2 * GAP + CELL + 70, y + CELL * 0.5, 18, CELL - 100))
        overlay = render_captions(width, height, captions, temporary)
        alpha = overlay[:, :, 3:4]
        canvas[:, :, :3] = overlay[:, :, :3] * alpha + canvas[:, :, :3] * (1 - alpha)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    image = bpy.data.images.new("3D pilot comparison", width=width, height=height, alpha=True)
    image.pixels.foreach_set(canvas.ravel())
    image.file_format = "PNG"
    image.filepath_raw = str(args.out.resolve())
    image.save()
    print(json.dumps({"sheet": str(args.out.resolve()), "rows": len(variants),
                      "width": width, "height": height}))


if __name__ == "__main__":
    main()
