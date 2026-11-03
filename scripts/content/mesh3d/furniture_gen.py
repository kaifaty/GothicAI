#!/usr/bin/env python3
"""The generator pilot of the house furniture (asset city-furniture): image_gen picture -> TRELLIS.2 -> game prop.

    R="uv run --no-project --with numpy --with pillow --with scipy python scripts/content/mesh3d/furniture_gen.py"
    $R cutout ITEM PICTURE.png          # white background -> RGBA input for run-mesh3d.sh
    # on dev-ubuntu: run-mesh3d.sh (raw.npz), then cumesh_clean.py RAW OUT BUDGET (clean.npz, low.npz); copy all three
    $R models [ITEM ...]                # -> model.glb + maps (blender/gen_prop.py)
    $R pack --set NAME [ITEM ...]       # meshes.json / materials.json for building_set.py compose

The user opened the pilot on 2026-11-02 after the code-built set lost to the original in game
(reports/content/remaster/city-furniture/decisions.md). Inputs: a clay render of the mesh-measured model, turned
into a photograph of the finished piece by image_gen (no pixels of the original), then lifted to 3D by TRELLIS.2 on
dev-ubuntu. Outputs: ~/.local/share/gothic-ai/content-staging/city-furniture/gen/<item>/.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
import furniture_set as fs  # noqa: E402
import rigging_set as rig  # noqa: E402

GEN = fs.STAGING / 'city-furniture/gen'
INPUTS = Path.home() / '.local/share/gothic-ai/content-sources/mesh3d-pilot'
# item -> (material key, sound group, roughness, the original's linear mean the albedo is scaled to)
ITEMS = {
    'chair': ('OWN_FURN_CHAIR_GEN', 'WOOD', 0.85, fs.TABLEPEASANT),
    'throne': ('OWN_FURN_THRONE_GEN', 'UNDEF', 0.6, [0.1172, 0.0323, 0.0149]),
    'cupboard-rich-1': ('OWN_FURN_CUPBOARD_RICH_1_GEN', 'WOOD', 0.8, fs.CUPBOARD),
    # From the original (the user, 2026-11-02, a derivative kept local): a render of the original armchair with its
    # texture upscaled 4x, straight into TRELLIS.2 (a), or first through image_gen as a photograph of it (b).
    'throne-a': ('OWN_FURN_THRONE_ORIG_A', 'UNDEF', 0.6, [0.1172, 0.0323, 0.0149]),
    'throne-b': ('OWN_FURN_THRONE_ORIG_B', 'UNDEF', 0.6, [0.1172, 0.0323, 0.0149]),
    # Pixal3D (setup-pixal3d.sh): four posed renders of the original from the sides (mv), and the picture of b.
    'throne-pmv': ('OWN_FURN_THRONE_PIXAL_MV', 'UNDEF', 0.6, [0.1172, 0.0323, 0.0149]),
    'throne-pb': ('OWN_FURN_THRONE_PIXAL_B', 'UNDEF', 0.6, [0.1172, 0.0323, 0.0149]),
}
# The whole set by route B (the user, 2026-11-03): a render of each original with its texture upscaled 4x, an image_gen
# photograph of that piece, TRELLIS.2. The albedo is scaled to the original's area-weighted linear mean over all its
# textures (from its OBJ, 2026-11-03); the armchair is throne-b.
ORIGINAL_MEANS = {
    'bed-rich': [0.1034, 0.0614, 0.0312], 'bed-master': [0.2575, 0.2144, 0.1538], 'bed': [0.0509, 0.0354, 0.0175],
    'bench-2': [0.0893, 0.0588, 0.0396], 'bookshelf': [0.1072, 0.0883, 0.0769], 'chair-rich': [0.0524, 0.0262, 0.01],
    'chair': [0.0723, 0.0531, 0.0374], 'cupboard-poor': [0.044, 0.0349, 0.0265], 'cupboard-rich-1': [0.0691, 0.0423, 0.0274],
    'cupboard-rich-2': [0.0678, 0.0411, 0.0262], 'table': [0.0611, 0.0323, 0.0126], 'table-rich': [0.1132, 0.0542, 0.0303],
}
for _item, _mean in ORIGINAL_MEANS.items():
    ITEMS[f'{_item}-o'] = (f"OWN_FURN_{_item.upper().replace('-', '_')}_ORIG", 'WOOD', 0.85, _mean)
VARIANT_OF = {'throne-a': 'throne', 'throne-b': 'throne', 'throne-pmv': 'throne', 'throne-pb': 'throne',
              **{f'{item}-o': item for item in ORIGINAL_MEANS}}  # the target and the visual a variant replaces


def cmd_cutout(args) -> int:
    """Cut the object from its white background: near-white pixels connected to the border become transparent."""
    import numpy as np
    from PIL import Image
    from scipy import ndimage
    rgb = np.asarray(Image.open(args.picture).convert('RGB'), dtype=np.float32) / 255
    white = (rgb.min(axis=2) > 0.90) & (rgb.max(axis=2) - rgb.min(axis=2) < 0.06)
    labels, _ = ndimage.label(white)
    border = set(np.unique(np.r_[labels[0], labels[-1], labels[:, 0], labels[:, -1]])) - {0}
    background = np.isin(labels, list(border))
    background = ndimage.binary_opening(background, iterations=1)
    alpha = ndimage.gaussian_filter((~background).astype(np.float32), 0.7)
    out = INPUTS / f'{args.item}.png'
    out.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray((np.dstack([rgb, alpha]) * 255).round().astype(np.uint8), 'RGBA').save(out)
    print(f'{out}: object {float((alpha > 0.5).mean()):.3f} of the frame')
    return 0


def colour_clean(out: Path) -> Path:
    """The bake source: the clean dual-contouring skin from cumesh_clean.py with each vertex's colour taken from
    the nearest vertex of the generator's raw mesh (the remesh keeps the surface, not the colour)."""
    import numpy as np
    from scipy.spatial import cKDTree
    raw, clean = np.load(out / 'raw.npz'), np.load(out / 'clean.npz')
    _, nearest = cKDTree(raw['vertices']).query(clean['vertices'], workers=-1)
    path = out / 'high.npz'
    np.savez(path, vertices=clean['vertices'], faces=clean['faces'], color=raw['color'][nearest])
    return path


LUMA = (0.2126, 0.7152, 0.0722)


def zone_mask(rgb, kind: str):
    """Soft mask of a zone in the generator's albedo: iron is its unsaturated part (bluish grey hinges reach 0.2),
    light is cloth or paper, far lighter than the wood around it."""
    import numpy as np
    top = rgb.max(axis=2)
    if kind == 'iron':
        saturation = (top - rgb.min(axis=2)) / np.maximum(top, 1e-3)
        return np.clip((0.3 - saturation) / 0.15, 0, 1) * (top > 0.12)
    return np.clip((rgb @ np.array(LUMA, dtype=np.float32) - 0.3) / 0.1, 0, 1)


def grade(build: Path, mean: list[float], zones: dict, scale: list[float] | None = None) -> Path:
    """The maps the game gets, in build/game: the albedo scaled to the original's linear mean, then each zone of
    gen.json given the original's own colour. One scale kept what the generator paints relative to the wood: iron 2.3
    times as light, a tan canopy where the original's sail is cream, tan pages where its booklet is cool grey
    (2026-11-03). A zone: {"colour": linear mean, "contrast": exponent on its luminance, "normal": share of the relief
    kept, "fill": fill the bake misses inside it (black specks where rays missed the thin cloth), "hue": keep the zone's
own shifts of hue and only bring its mean to the colour (the canopy's unbleached and sun-greyed linen)}. A zone whose mask
    gen_prop baked (maps/<zone>.png, parts built in Blender) takes that mask. scale: a fixed linear factor instead of
    the mean, so the generator's part keeps the colour it was accepted with when own parts change the atlas."""
    import numpy as np
    from PIL import Image
    from scipy import ndimage
    maps, out = build / 'maps', build / 'game'
    out.mkdir(exist_ok=True)
    rgb = np.asarray(Image.open(maps / 'albedo.png').convert('RGB'), dtype=np.float32) / 255
    lin = rgb ** 2.2
    lin *= np.array(scale) if scale else np.array(mean) / lin.reshape(-1, 3).mean(0)  # over the atlas, as rig.write_maps
    normal = np.asarray(Image.open(maps / 'normal.png').convert('RGB'), dtype=np.float32) / 255 * 2 - 1
    for kind, zone in zones.items():
        baked = maps / f'{kind}.png'
        mask = (np.asarray(Image.open(baked).convert('L'), dtype=np.float32) / 255 if baked.exists()
                else zone_mask(rgb, kind))
        if zone.get('fill'):
            inside = mask > 0.5
            holes = ndimage.binary_closing(inside, iterations=6) & ~inside
            _, (rows, cols) = ndimage.distance_transform_edt(~inside, return_indices=True)
            lin[holes], normal[holes], mask[holes] = lin[rows, cols][holes], normal[rows, cols][holes], 1
        if zone.get('hue'):
            target = lin * (np.array(zone['colour']) / np.maximum(lin[mask > 0.5].mean(0), 1e-4))
        else:
            luminance = lin @ np.array(LUMA, dtype=np.float32)
            ratio = (luminance / max(float(np.median(luminance[mask > 0.5])), 1e-4)) ** zone.get('contrast', 1.0)
            target = np.array(zone['colour']) * ratio[..., None]
        lin += mask[..., None] * (target - lin)
        normal[..., :2] *= 1 - mask[..., None] * (1 - zone.get('normal', 1.0))
        print(f'{build.parent.name}: {kind} {float((mask > 0.5).mean()):.3f} of the atlas')
    srgb = np.clip(lin, 0, 1) ** (1 / 2.2)
    Image.fromarray((srgb * 255).round().astype(np.uint8), 'RGB').save(out / 'albedo.png')
    normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    Image.fromarray(((normal + 1) / 2 * 255).round().astype(np.uint8), 'RGB').save(out / 'normal.png')
    for name in ('ao', 'roughness', 'metallic'):
        shutil.copy(maps / f'{name}.png', out / f'{name}.png')
    return out


def cmd_models(args) -> int:
    for item in args.items or ITEMS:
        key, group, rough, _ = ITEMS[item]
        out = GEN / item
        conf = json.loads((out / 'gen.json').read_text())  # yaw_deg, symmetrize, zones: chosen per item by eye
        cfg = {'id': f'furn-{VARIANT_OF.get(item, item)}', 'target': str(fs.ROOT / 'data/content/mesh3d-furniture.json'), 'material': key,
               'group': group, 'roughness': rough, **conf, 'raw': str(colour_clean(out)), 'low': str(out / 'low.npz')}
        path = out / 'config.json'
        path.write_text(json.dumps(cfg, indent=1))
        rig.run([rig.BLENDER, '-b', '--factory-startup', '--python-exit-code', '1', '-P', fs.BLENDER_DIR / 'gen_prop.py', '--', path, out / 'build'],
                cwd=fs.ROOT, stdout=subprocess.DEVNULL)
        print(item, json.loads((out / 'build/metrics.json').read_text()))
    return 0


def flat_to_atlas(maps: Path) -> None:
    """Constant roughness and metallic maps at the atlas size: the packer stacks them with the AO into ORM."""
    from PIL import Image
    size = Image.open(maps / 'ao.png').size
    for name in ('roughness', 'metallic'):
        image = Image.open(maps / f'{name}.png')
        if image.size != size:
            image.convert('L').resize(size, Image.NEAREST).save(maps / f'{name}.png')


FOLD_SHADE, TONE = 0.6, 0.06  # the coverlet's hollows: share of their occlusion in the colour; tone unevenness


def own_maps(folder: Path, kind: str) -> Path:
    """The maps of parts with a texture of their own (gen_prop.py "own" "materials"): <kind>/<kind>.png with its
    .normal and .roughness (edit_texture.py), and the AO gen_prop baked into build/<kind>/."""
    from PIL import Image
    src, maps = folder / kind, folder / 'build' / kind
    import numpy as np
    from scipy import ndimage
    albedo = Image.open(src / f'{kind}.png')
    size = (max(albedo.size),) * 2  # square: the DDS writer's mip chain breaks on 1024 x 2048 (UVs are 0..1 anyway)
    lin = (np.asarray(albedo.convert('RGB').resize(size, Image.LANCZOS), dtype=np.float32) / 255) ** 2.2
    normal = np.asarray(Image.open(src / f'{kind}.normal.png').convert('RGB').resize(size, Image.BILINEAR),
                        dtype=np.float32) / 255 * 2 - 1
    sim = src / 'sim'
    if (sim / 'folds.ao.png').exists():
        # the coverlet (coverlet_sim.py): its folds darken the cloth in the hollows as the original's painted creases
        # do (shade without a direction), a hand-sized unevenness of tone from wear and dye, and the weave rides on
        # the folds' normals (whiteout blend)
        ao = np.asarray(Image.open(sim / 'folds.ao.png').convert('L').resize(size), dtype=np.float32) / 255
        tone = ndimage.gaussian_filter(np.random.default_rng(7).standard_normal(size[::-1]), size[0] * 0.04, mode='wrap')
        lin *= ((1 - FOLD_SHADE * (1 - ao)) * (1 + TONE * tone / tone.std()))[..., None]
        folds = np.asarray(Image.open(sim / 'folds.normal.png').convert('RGB').resize(size, Image.BILINEAR),
                           dtype=np.float32) / 255 * 2 - 1
        normal = np.dstack([folds[..., :2] + normal[..., :2], folds[..., 2] * normal[..., 2]])
        normal /= np.linalg.norm(normal, axis=2, keepdims=True)
    Image.fromarray((np.clip(lin, 0, 1) ** (1 / 2.2) * 255).round().astype(np.uint8)).save(maps / 'albedo.png')
    Image.fromarray(((normal + 1) / 2 * 255).round().astype(np.uint8)).save(maps / 'normal.png')
    for name, image in (('roughness', Image.open(src / f'{kind}.roughness.png')), ('metallic', Image.new('L', size, 0)),
                        ('ao', Image.open(maps / 'ao.png'))):
        image.convert('L').resize(size).save(maps / f'{name}.png')
    return maps


def cmd_pack(args) -> int:
    write_dds = rig.load_compressor()
    output = fs.STAGING / args.set
    if output.exists():
        shutil.rmtree(output)
    materials, meshes = {}, {}
    for item in args.items or ITEMS:
        key, _, _, colour = ITEMS[item]
        name = f'furniture-gen-{item}'
        build = GEN / item / 'build'
        (output / 'meshes' / name).mkdir(parents=True, exist_ok=True)
        flat_to_atlas(build / 'maps')
        conf = json.loads((GEN / item / 'gen.json').read_text())
        zones = conf.get('zones')
        graded = zones or conf.get('scale')  # a fixed scale: own parts left the atlas, so its mean no longer holds
        maps, colour = (grade(build, colour, zones or {}, conf.get('scale')), None) if graded else (build / 'maps', colour)
        entry = rig.write_maps(write_dds, output / 'materials', output / 'materials' / name, maps, colour=colour)
        materials[f'{key}.TGA'] = {**entry, 'scale': 1.0, 'antitile': 'off', 'antitile_cell': 1}
        for kind, own_key in conf.get('own', {}).get('materials', {}).items():
            entry = rig.write_maps(write_dds, output / 'materials', output / 'materials' / f'{name}-{kind}',
                                   own_maps(GEN / item, kind))
            materials[f'{own_key}.TGA'] = {**entry, 'scale': 1.0, 'antitile': 'off', 'antitile_cell': 1}
        rig.glb_edit(build / 'model.glb', output / 'meshes' / name / 'model.glb', {}, set())
        visual = fs.ITEMS[VARIANT_OF.get(item, item)]
        meshes[visual] = {'glb': f'{name}/model.glb'}
        print(f'{item}: {visual} <- {name}')
    (output / 'materials' / 'materials.json').write_text(json.dumps(materials, indent=2) + '\n')
    (output / 'meshes' / 'meshes.json').write_text(json.dumps(meshes, indent=2) + '\n')
    print(f'{args.set}: {len(meshes)} meshes -> {output}')
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(required=True)
    p = sub.add_parser('cutout'); p.add_argument('item'); p.add_argument('picture', type=Path); p.set_defaults(func=cmd_cutout)
    p = sub.add_parser('models'); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_models)
    p = sub.add_parser('pack'); p.add_argument('--set', required=True); p.add_argument('items', nargs='*'); p.set_defaults(func=cmd_pack)
    args = parser.parse_args()
    return args.func(args)


if __name__ == '__main__':
    sys.exit(main())
