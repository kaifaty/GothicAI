"""Scan rock for the game: a dense photogrammetry mesh to a low mesh with a normal map baked from it.

    blender -b -P scan_rock.py -- --source boulder/vivvecldw_tier_0.gltf --normal boulder/Textures/T_..._N.png \
        --out work/boulder_low --material OWN_MEGASCANS_BOULDER --low work/low.obj --scale 0.9 --offset 0.05 -1.12 -0.15

The low mesh keeps the scan's UV, so its colour and ORM maps stay usable as they are; only the normal map is
new, because it must also carry the shape the decimation removed. With --diffuse, for a scan whose dense UV
seams stop the decimation, the low mesh gets a new unwrap and the colour and ORM maps are baked onto it too. Source and output stay outside git: the
scan's licence forbids sharing the files. Metrics go to metrics.json next to the GLB.
"""
import argparse
import json
import sys
from pathlib import Path

import bpy
from mathutils import Matrix, Vector


def arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', required=True)
    parser.add_argument('--normal', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--material', required=True)
    parser.add_argument('--low', required=True, help='decimated OBJ in the scan\'s own space, with its UV')
    parser.add_argument('--scale', type=float, default=1.)
    parser.add_argument('--offset', type=float, nargs=3, default=(0., 0., 0.))
    parser.add_argument('--size', type=int, default=4096)
    parser.add_argument('--diffuse', help='re-unwrap the low mesh and bake this colour map and --arm onto it')
    parser.add_argument('--arm', help='AO, roughness, metal map of the scan, for --diffuse')
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:])


def tris(obj) -> int:
    return sum(len(p.vertices) - 2 for p in obj.data.polygons)


def main():
    args = arguments()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=args.source)
    high = next(o for o in bpy.data.objects if o.type == 'MESH')
    bpy.context.view_layer.objects.active = high
    high.select_set(True)
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    centre = sum((Vector(c) for c in high.bound_box), Vector()) / 8
    centre.z = min(Vector(c).z for c in high.bound_box)
    placement = (Matrix.Translation(Vector(args.offset)) @ Matrix.Diagonal((args.scale, args.scale, args.scale, 1))
                 @ Matrix.Translation(-centre))
    high.data.transform(placement)
    high.data.update()
    high_tris = tris(high)

    # Decimated outside Blender: its collapse folds triangles over, and the engine culls back faces, so each
    # fold is a hole. MeshLab's edge collapse keeps the normals and the UV; see scan_decimate.py.
    bpy.ops.wm.obj_import(filepath=args.low, forward_axis='Y', up_axis='Z')
    low = next(o for o in bpy.context.selected_objects if o.type == 'MESH')
    low.name = 'scan_low'
    low.data.transform(placement)
    low.data.update()
    for polygon in low.data.polygons:
        polygon.use_smooth = True
    low.data.update()
    low_tris = tris(low)
    print(f'[scan_rock] {high_tris} -> {low_tris} tris', flush=True)

    # The dense mesh keeps its fine relief in the scan's own normal map: both go into the bake.
    material = bpy.data.materials.new('scan_high')
    material.use_nodes = True
    nodes = material.node_tree.nodes
    bsdf = nodes['Principled BSDF']
    image = bpy.data.images.load(args.normal)
    image.colorspace_settings.name = 'Non-Color'
    texture = nodes.new('ShaderNodeTexImage')
    texture.image = image
    normal_map = nodes.new('ShaderNodeNormalMap')
    material.node_tree.links.new(texture.outputs['Color'], normal_map.inputs['Color'])
    material.node_tree.links.new(normal_map.outputs['Normal'], bsdf.inputs['Normal'])
    high.data.materials.clear()
    high.data.materials.append(material)

    if args.diffuse:
        bpy.ops.object.select_all(action='DESELECT')
        low.select_set(True)
        bpy.context.view_layer.objects.active = low
        bpy.ops.object.mode_set(mode='EDIT')
        bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.uv.smart_project(angle_limit=1.15, island_margin=.002)
        bpy.ops.object.mode_set(mode='OBJECT')

    baked = bpy.data.images.new('baked_normal', args.size, args.size, alpha=False, float_buffer=True)
    baked.colorspace_settings.name = 'Non-Color'
    target = bpy.data.materials.new(args.material)
    target.use_nodes = True
    target_nodes = target.node_tree.nodes
    target_image = target_nodes.new('ShaderNodeTexImage')
    target_image.image = baked
    target_nodes.active = target_image
    low.data.materials.clear()
    low.data.materials.append(target)
    target['group'] = 'STONE'

    scene = bpy.context.scene
    scene.render.engine = 'CYCLES'
    scene.cycles.device = 'CPU'
    scene.cycles.samples = 1
    bake = scene.render.bake
    bake.use_selected_to_active = True
    bake.cage_extrusion = .06
    bake.max_ray_distance = .15
    bake.margin = 16
    bake.normal_space = 'TANGENT'
    bpy.ops.object.select_all(action='DESELECT')
    high.select_set(True)
    low.select_set(True)
    bpy.context.view_layer.objects.active = low
    bpy.ops.object.bake(type='NORMAL')
    baked.filepath_raw = str(out / 'normal.png')
    baked.file_format = 'PNG'
    baked.save()

    # Raw values through emission: no colour transform, so the PNG holds the same numbers as the scan's map.
    for source, name in ((args.diffuse, 'diffuse'), (args.arm, 'arm')) if args.diffuse else ():
        image = bpy.data.images.load(source)
        image.colorspace_settings.name = 'Non-Color'
        nodes.clear()
        output = nodes.new('ShaderNodeOutputMaterial')
        emission = nodes.new('ShaderNodeEmission')
        texture = nodes.new('ShaderNodeTexImage')
        texture.image = image
        material.node_tree.links.new(texture.outputs['Color'], emission.inputs['Color'])
        material.node_tree.links.new(emission.outputs['Emission'], output.inputs['Surface'])
        target_image.image = bpy.data.images.new(name, args.size, args.size, alpha=False, float_buffer=True)
        target_image.image.colorspace_settings.name = 'Non-Color'
        target_nodes.active = target_image
        bpy.ops.object.bake(type='EMIT')
        target_image.image.filepath_raw = str(out / f'{name}.png')
        target_image.image.file_format = 'PNG'
        target_image.image.save()
        print(f'[scan_rock] baked {name}', flush=True)

    bpy.ops.object.select_all(action='DESELECT')
    low.select_set(True)
    glb = out / 'boulder.glb'
    bpy.ops.export_scene.gltf(filepath=str(glb), export_format='GLB', use_selection=True, export_apply=True,
                              export_yup=True, export_extras=True, export_image_format='NONE', export_tangents=False)
    dimensions = [round(v, 3) for v in low.dimensions]
    (out / 'metrics.json').write_text(json.dumps(dict(tris=low_tris, source_tris=high_tris, dimensions_m=dimensions,
                                                      glb_bytes=glb.stat().st_size, normal_size=args.size), indent=2))
    print(f'[scan_rock] done {dimensions}', flush=True)


main()
