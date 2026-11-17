"""Build a spruce's near level of detail: the GLB with wind data in COLOR_0, its numbers and the Blender scene.

    blender -b --factory-startup -P fir.py -- --out DIR [--config fir.json] [--seed N] [--scale S] [--name NAME]
        [--pairs]

Writes DIR/NAME.glb, DIR/metrics.json, DIR/NAME-density.png and DIR/NAME.blend (the tree as one object, its cards
one-sided). NAME is the config's id unless given. A bough's triangles are listed once and its material is
two-sided (`"two_sided": "same"` in its record, scripts/content/fir_set.py); --pairs lists them twice for a
one-sided material, as before reports/engine/2026-11-16-modern-trees/two-sided.md. The shape is fir_tree.py, the file and its layout tree_glb.py;
metrics.json holds the generator's numbers and what is read back from the written file: by tree_check.py the
triangles, the bounding box, inside_out_parts and the layout of COLOR_0, by tree_density.py (`density`) how much
of the background and of the trunk shows through the crown in two side views and the crown's width at the
ground, by fir_tree.py how far from one plane the boughs of each role are (`card_planes`) and how much of the
branches' axes is under boughs seen from above (`axis_cover`). Exits 1 when the file breaks a rule, the budget
of triangles, a limit of the config's `density` or a rule of its `cards.rules`.

The same runs without Blender (python fir.py ...), then no .blend is written.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import fir_tree  # noqa: E402
import tree_check  # noqa: E402
import tree_density  # noqa: E402
import tree_glb  # noqa: E402

try:
    import bpy
except ImportError:
    bpy = None


def front_faces(index: np.ndarray) -> np.ndarray:
    """A card's triangles once each: in the mode of pairs the first of each pair is its front."""
    _, first = np.unique(np.sort(index, axis=1), axis=0, return_index=True)
    return index[np.sort(first)]


def mesh_object(parts: list[dict], name: str):
    """The tree as one Blender object: a material slot per part, authored normals, UV, the colour as `Col`."""
    position = np.concatenate([p['position'] for p in parts]).astype(np.float64)
    normal = np.concatenate([p['normal'] for p in parts]).astype(np.float64)
    uv = np.concatenate([p['uv'] for p in parts]).astype(np.float64)
    colour = np.concatenate([p['colour'] for p in parts]).astype(np.float64) / 255
    faces, slots, offset = [], [], 0
    for slot, part in enumerate(parts):
        index = front_faces(np.asarray(part['index'])) if part.get('cutout') else np.asarray(part['index'])
        faces.append(index + offset)
        slots.append(np.full(len(index), slot))
        offset += len(part['position'])
    faces, slots = np.concatenate(faces), np.concatenate(slots)
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(position.tolist(), [], faces.tolist())
    mesh.polygons.foreach_set('material_index', slots.astype(np.int32))
    mesh.polygons.foreach_set('use_smooth', np.ones(len(faces), bool))
    corners = faces.reshape(-1)
    layer = mesh.uv_layers.new(name='UVMap')
    flipped = uv[corners] * (1, -1) + (0, 1)
    layer.data.foreach_set('uv', flipped.reshape(-1))
    attribute = mesh.color_attributes.new('Col', 'BYTE_COLOR', 'POINT')
    attribute.data.foreach_set('color', colour.reshape(-1))
    mesh.update()
    mesh.normals_split_custom_set_from_vertices((normal / np.linalg.norm(normal, axis=1, keepdims=True)).tolist())
    obj = bpy.data.objects.new(name, mesh)
    for part in parts:
        obj.data.materials.append(bpy.data.materials.get(part['name']) or bpy.data.materials.new(part['name']))
    bpy.context.scene.collection.objects.link(obj)
    return obj


def build(config: dict, out: Path, name: str, seed: int | None = None, scale: float = 1., pairs: bool = False) -> dict:
    tree = fir_tree.build(config, seed, scale, pairs)
    glb = out / f'{name}.glb'
    tree_glb.write(glb, [{k: v for k, v in p.items() if k != 'owner'} for p in tree.parts], name,
                   extras=dict(tree=config['id'], seed=config['seed'] if seed is None else seed, scale=scale))
    read = tree_check.check(glb)
    metrics = tree.metrics | dict(
        file=glb.name, bytes=glb.stat().st_size, seed=config['seed'] if seed is None else seed,
        triangles=read['triangles_total'], inside_out_parts=read['inside_out_parts'],
        closed_parts=read['closed_parts'], colour_0=read, fails=list(read['fails']))
    if metrics['triangles'] > config['budget_tris']:
        metrics['fails'].append(f"{metrics['triangles']} triangles, the budget is {config['budget_tris']}")
    mask = tree_density.load_mask(HERE / config['atlas']['mask'])
    metrics['density'] = tree_density.measure(glb, mask, picture=out / f'{name}-density.png')
    metrics['fails'] += tree_density.rules(metrics['density'], config['density'], scale)
    rules = config['cards']['rules']
    metrics['card_planes'] = fir_tree.card_planes(tree)
    metrics['axis_cover'] = fir_tree.axis_cover(tree, mask, config['cards']['cover_from'])
    for role, deg in rules['planes_deg'].items():
        spread = metrics['card_planes'][role]['spread_min_deg']
        if spread < deg:
            metrics['fails'].append(f'a bough of the role {role} is within {spread} degrees of one plane, the least is {deg}')
    if metrics['axis_cover']['covered'] < rules['cover_min']:
        metrics['fails'].append(f"{metrics['axis_cover']['covered']:.1%} of the branches' axes is under boughs, "
                                f"the least is {rules['cover_min']:.0%}")
    (out / 'metrics.json').write_text(json.dumps(metrics, indent=1, ensure_ascii=False) + '\n')
    return dict(tree=tree, metrics=metrics, glb=glb)


def main() -> int:
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else sys.argv[1:]
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--config', type=Path, default=HERE / 'fir.json')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--seed', type=int)
    parser.add_argument('--scale', type=float, default=1.)
    parser.add_argument('--name')
    parser.add_argument('--pairs', action='store_true',
                        help="list a bough's triangles twice for a one-sided material")
    args = parser.parse_args(argv)
    config = json.loads(args.config.read_text())
    name = args.name or config['id']
    args.out.mkdir(parents=True, exist_ok=True)
    result = build(config, args.out, name, args.seed, args.scale, args.pairs)
    metrics = result['metrics']
    if bpy:
        bpy.ops.wm.read_factory_settings(use_empty=True)
        mesh_object(result['tree'].parts, name)
        bpy.ops.wm.save_as_mainfile(filepath=str(args.out / f'{name}.blend'))
    print(json.dumps({k: metrics[k] for k in ('file', 'triangles', 'triangles_by_material', 'height_m',
                                              'crown_width_m', 'trunk_diameter_m', 'root_spread_m', 'branches',
                                              'cards_total', 'inside_out_parts', 'density', 'fails')}, indent=1))
    return 1 if metrics['fails'] else 0


if __name__ == '__main__':
    code = main()
    if code:
        sys.exit(code)
