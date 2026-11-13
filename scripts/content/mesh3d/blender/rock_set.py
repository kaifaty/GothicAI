"""Split a scanned set of rocks (one glTF, several meshes, one texture atlas) into one GLB per rock for the game.

    blender -b -P rock_set.py -- --source rock_moss_set_01/rock_moss_set_01_4k.gltf --out work/rocks \
        --material OWN_LANDSCAPE_ROCKSET_01 --prefix OWN_LANDSCAPE_ROCK --scales 1 0.6

Every rock keeps the scan's UV, so the set's colour, normal and roughness maps serve all of them through one
material. A rock is centred on its own footprint and keeps the set's ground plane at height 0: the part the scan
has below it sinks into the ground when the rock is placed at ground height. The object layer of a content set
has no scale, so each scale is its own GLB. `rocks.json` lists the meshes with their sizes.
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
    parser.add_argument('--out', required=True)
    parser.add_argument('--material', required=True)
    parser.add_argument('--prefix', required=True)
    parser.add_argument('--scales', type=float, nargs='+', default=[1.])
    return parser.parse_args(sys.argv[sys.argv.index('--') + 1:])


def main():
    args = arguments()
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=args.source)
    material = bpy.data.materials.new(args.material)
    material['group'] = 'STONE'
    rocks = sorted((o for o in bpy.context.scene.objects if o.type == 'MESH'), key=lambda o: o.name)
    report = []
    for number, rock in enumerate(rocks, 1):
        bpy.ops.object.select_all(action='DESELECT')
        rock.select_set(True)
        bpy.context.view_layer.objects.active = rock
        bpy.ops.object.parent_clear(type='CLEAR_KEEP_TRANSFORM')
        bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
        corners = [Vector(c) for c in rock.bound_box]
        low = Vector((min(c.x for c in corners), min(c.y for c in corners), min(c.z for c in corners)))
        high = Vector((max(c.x for c in corners), max(c.y for c in corners), max(c.z for c in corners)))
        rock.data.transform(Matrix.Translation((-(low.x + high.x) / 2, -(low.y + high.y) / 2, 0)))
        rock.data.materials.clear()
        rock.data.materials.append(material)
        for polygon in rock.data.polygons:
            polygon.use_smooth = True
        rock.data.calc_loop_triangles()
        for scale in args.scales:
            name = f'{args.prefix}_{number:02d}' + ('' if scale == 1 else f'_S{round(scale * 100):03d}')
            copy = rock.copy()
            copy.data = rock.data.copy()
            copy.data.transform(Matrix.Diagonal((scale, scale, scale, 1)))
            bpy.context.scene.collection.objects.link(copy)
            bpy.ops.object.select_all(action='DESELECT')
            copy.select_set(True)
            bpy.context.view_layer.objects.active = copy
            bpy.ops.export_scene.gltf(filepath=str(out / f'{name}.glb'), export_format='GLB', use_selection=True,
                                      export_apply=True, export_yup=True, export_extras=True,
                                      export_image_format='NONE', export_tangents=False)
            report.append(dict(visual=name, glb=f'{name}.glb', source=rock.name, scale=scale,
                               tris=len(rock.data.loop_triangles),
                               size_m=[round((high - low)[axis] * scale, 2) for axis in range(3)],
                               below_ground_m=round(-low.z * scale, 2)))
            bpy.data.objects.remove(copy)
    (out / 'rocks.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


main()
