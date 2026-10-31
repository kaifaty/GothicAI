#!/usr/bin/env python3
"""Build a building's texture set from its remaster asset spec (stage 5 of docs/remaster-pipeline.md).

Input: `reports/content/remaster/<asset>/build.json` and the sources selected in the
asset's `asset.json`. Per texture: one Lanczos resize to 1024² (or `size`, a power of two up to
2048, for a wide texture whose detail would not fit), narrow seam cuts on the permitted axes, then
colour by material:

- `target` — linear RGB gains so the texture's mean matches its own `target_oklab`;
- `group`  — one shared set of gains for several textures of one material, toward
             their area-weighted targets (the house-1 method);
- `match`  — gains so the texture's pixels outside `exclude_uv` match the corrected mean
             of another texture of the same material (boards around a window).

`relief` (0..1, default 1) scales the tilt of the filtered normal map: the CPU filter
normalises fine detail to full contrast, which turns wood grain into bark.

`"maps": "source"` takes the normal, roughness and optional AO baked from our own model together
with the albedo (`remaster.py import --tool … --map normal=… --map roughness=…`, for example the
roofs of `shingle_bake.py`, `slate_bake.py` and `inside_bake.py`) instead of deriving them from the albedo. Those maps line up with the
albedo pixel for pixel and tile by construction, so such a texture takes no seam cut or crop
(`axes: ""`); its roughness is not clipped to the class range, and AO defaults to 1 because the
bake already holds its occlusion in the albedo.

Numbers are guard rails: luminance outside the ratio range or clipped pixels are recorded,
never "fixed" here. No detail fit, Marigold or highlight shoulder. Maps come from the
existing CPU filter unless the texture takes them from its model bake (below). With `"base": "original"` the set holds only this asset's entries;
with a staging set as base, it is that set with only these entries replaced.

`compose SET PART...` joins the sets of several assets into one staging set for the game:
each texture belongs to exactly one part, so a shared texture lives in its own asset. Meshes of a
part (a prop's GLB) and particle descriptions (`particles.d/*.json`) come along.

`enable SET` copies a staging set into the working content `~/.local/share/gothic-ai/content`, which
the game reads by default; only on the user's word (gate `enable`).
"""
from __future__ import annotations

import argparse
from copy import deepcopy
from datetime import datetime, timezone
from io import BytesIO
import json
from pathlib import Path
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw

import bakeoff
import city_set
from market_house import apply_gains, correct_house_colour
from material_colour import _linear, oklab_to_linear
from material_lighting import measure_file as measure_light
import pack_materials
import quality_pilot as pilot
import registry as registry_module
import remaster
from seam_fix import seamless

LUMINANCE = np.array([.2126, .7152, .0722])
MODES = ('target', 'group', 'match')


def load_spec(asset: str) -> dict:
    spec = json.loads((remaster.asset_dir(asset) / 'build.json').read_text())
    for key in ('set', 'base', 'registry', 'textures'):
        if key not in spec:
            raise ValueError(f'build.json: missing {key}')
    for name, item in spec['textures'].items():
        if item.get('axes', 'uv') not in ('', 'u', 'v', 'uv'):
            raise ValueError(f'{name}: axes must be "", "u", "v" or "uv"')
        mode = item.get('colour', {}).get('mode')
        if mode not in MODES:
            raise ValueError(f'{name}: colour.mode must be one of {MODES}')
        if mode == 'match' and item['colour'].get('to') not in spec['textures']:
            raise ValueError(f'{name}: colour.to must name another texture of this set')
        if item.get('maps', 'filter') not in ('filter', 'source'):
            raise ValueError(f'{name}: maps must be "filter" or "source"')
        if item.get('size', 1024) not in (256, 512, 1024, 2048):
            raise ValueError(f'{name}: size must be 256, 512, 1024 or 2048')
    return spec


EDIT_OF_ORIGINAL = 'scripts/content/edit_texture.py'


def source_input(name: str, source: dict) -> str:
    """What the texture was made from, for the registry: an edit of the original is a derivative (the user's
    decision of 2026-10-23, docs/own-content.md, «Граница с оригиналом»)."""
    if source.get('tool') == EDIT_OF_ORIGINAL:
        return f'original texture {name}: image_gen edit of it, colour taken back from it ({EDIT_OF_ORIGINAL}); derivative'
    return 'image_gen edit of the accepted remaster concept; no original texture pixels'


def mask_outside(shape: tuple[int, int], exclude_uv) -> np.ndarray:
    h, w = shape
    mask = np.ones((h, w), dtype=bool)
    if exclude_uv:
        u0, v0, u1, v1 = exclude_uv
        mask[round(v0 * h):round(v1 * h), round(u0 * w):round(u1 * w)] = False
    return mask


def gains_for_mean(pixels: np.ndarray, mask: np.ndarray, target: np.ndarray) -> np.ndarray:
    """Per-channel gain g so that mean(min(linear*g, 1)) over the mask equals target."""
    linear = _linear(pixels.astype(float) / 255)[mask]
    gains = np.ones(3)
    for c in range(3):
        values = linear[:, c]
        lo, hi = 0., 1.
        while np.minimum(values * hi, 1).mean() < target[c] and hi < 65536:
            hi *= 2
        for _ in range(60):
            mid = (lo + hi) / 2
            if np.minimum(values * mid, 1).mean() < target[c]:
                lo = mid
            else:
                hi = mid
        gains[c] = (lo + hi) / 2
    return gains


def clipped_fraction(pixels: np.ndarray, gains) -> float:
    linear = _linear(pixels.astype(float) / 255) * np.asarray(gains)
    return float(np.any(linear > 1, axis=2).mean())


def correct_colours(images: dict, spec: dict, targets: dict) -> tuple[dict, dict]:
    corrected, info = {}, {}
    textures = spec['textures']
    groups = {}
    for name, item in textures.items():
        colour = item['colour']
        if colour['mode'] == 'target':
            out, meta = correct_house_colour({name: images[name]}, {name: 1.}, {name: targets[name]})
            corrected[name] = out[name]
            info[name] = dict(mode='target', gains=meta['channel_gains'],
                              clipped_fraction=clipped_fraction(images[name], meta['channel_gains']))
        elif colour['mode'] == 'group':
            groups.setdefault(colour['group'], []).append(name)
    for group, names in groups.items():
        areas = {n: float(textures[n]['colour']['area_m2']) for n in names}
        out, meta = correct_house_colour({n: images[n] for n in names}, areas, {n: targets[n] for n in names})
        corrected.update(out)
        for n in names:
            info[n] = dict(mode='group', group=group, gains=meta['channel_gains'], area_m2=areas[n],
                           clipped_fraction=clipped_fraction(images[n], meta['channel_gains']))
    pending = [n for n, item in textures.items() if item['colour']['mode'] == 'match']
    while pending:
        progressed = False
        for name in list(pending):
            colour = textures[name]['colour']
            if colour['to'] not in corrected:
                continue
            reference = _linear(corrected[colour['to']].astype(float) / 255).mean(axis=(0, 1))
            mask = mask_outside(images[name].shape[:2], colour.get('exclude_uv'))
            gains = gains_for_mean(images[name], mask, reference)
            corrected[name] = apply_gains(images[name], gains)
            after = _linear(corrected[name].astype(float) / 255)[mask].mean(axis=0)
            error = float(np.abs(after - reference).max())
            if error > .005:
                raise ValueError(f'{name}: matched colour error {error:.6f} exceeds .005')
            info[name] = dict(mode='match', to=colour['to'], exclude_uv=colour.get('exclude_uv'),
                              gains=gains.tolist(), reference_linear_rgb=reference.tolist(),
                              matched_linear_rgb=after.tolist(), max_error=error,
                              masked_fraction=float(mask.mean()),
                              clipped_fraction=clipped_fraction(images[name], gains))
            pending.remove(name)
            progressed = True
        if not progressed:
            raise ValueError(f'match cycle or missing reference: {pending}')
    return corrected, info


def crop_band(image: Image.Image, tolerance: int = 6, floor: int = 170) -> tuple[Image.Image, list]:
    """Drop the plain light grey margins around a texture drawn as a band on a larger canvas."""
    pixels = np.asarray(image.convert('RGB')).astype(int)
    grey = (np.abs(pixels - pixels.mean(axis=2, keepdims=True)).max(axis=2) < tolerance) & (pixels.mean(axis=2) > floor)
    rows = np.where(grey.mean(axis=1) < .9)[0]
    cols = np.where(grey.mean(axis=0) < .9)[0]
    if not len(rows) or not len(cols):
        raise ValueError('crop: no band found')
    box = (int(cols[0]), int(rows[0]), int(cols[-1]) + 1, int(rows[-1]) + 1)
    return image.crop(box), list(box)      # a list: the registry takes JSON values only


def source_attempts(manifest: dict, texture: str) -> list[dict]:
    attempts = []
    for source in manifest['sources']:
        if source.get('texture') != texture:
            continue
        meta_path = (remaster.sources_root() / source['file']).with_suffix('.json')
        meta = json.loads(meta_path.read_text())
        attempts.append(dict(attempt=source['attempt'], prompt=meta['prompt'], utc=meta.get('utc'),
            seconds=meta.get('seconds'), source_png=source['file'], source_sha256=source['sha256'],
            model=source.get('model'), parent=source.get('parent')))
    return sorted(attempts, key=lambda a: a['attempt'])


def load_model_maps(source: dict, texture: dict) -> dict:
    """Normal (0..1 encoded), roughness and optional AO baked from our model with this albedo."""
    if texture.get('axes', 'uv') or texture.get('crop'):
        raise ValueError(f'{source["file"]}: model maps need axes "" and no crop: the bake tiles by construction')
    maps = source.get('maps') or {}
    if {'normal', 'roughness'} - set(maps):
        raise ValueError(f'{source["file"]}: "maps": "source" needs normal and roughness from import --map')
    out = {}
    for name, meta in maps.items():
        path = remaster.sources_root() / meta['file']
        if remaster.sha256(path) != meta['sha256']:
            raise ValueError(f'{path}: SHA-256 differs from asset.json')
        size = texture.get('size', 1024)
        with Image.open(path) as image:
            image = image.convert('RGB' if name == 'normal' else 'L')
            if image.size != (size, size):
                image = image.resize((size, size), Image.Resampling.LANCZOS)
            value = np.asarray(image, dtype=np.float64) / 255
        if name == 'normal':
            vector = value * 2 - 1
            value = vector / (np.linalg.norm(vector, axis=-1, keepdims=True) + 1e-9) * .5 + .5
        out[name] = value
    return out


def prepare(asset: str, spec: dict) -> tuple[list[dict], dict]:
    manifest = remaster.load_manifest(asset)
    names = list(spec['textures'])
    recipes = {r['replaces'][0]: r for r in city_set.recipes(textures=names)}
    prepared_dir = remaster.work_root(asset) / 'prepared' / spec['set']
    prepared_dir.mkdir(parents=True, exist_ok=True)
    images, records = {}, []
    for name in names:
        chosen = manifest['selected'].get(name)
        if chosen is None:
            raise ValueError(f'{name}: no selected source in asset.json')
        source = next(s for s in manifest['sources'] if s['file'] == chosen['file'])
        path = remaster.sources_root() / source['file']
        if remaster.sha256(path) != source['sha256']:
            raise ValueError(f'{path}: SHA-256 differs from asset.json')
        axes = spec['textures'][name].get('axes', 'uv')
        model_maps = (load_model_maps(source, spec['textures'][name])
                      if spec['textures'][name].get('maps') == 'source' else None)
        with Image.open(path) as image:
            image = image.convert('RGB')
            band = None
            if spec['textures'][name].get('crop') == 'band':
                image, band = crop_band(image)
            size = spec['textures'][name].get('size', 1024)
            resized = image.resize((size, size), Image.Resampling.LANCZOS)
            joined, seam = seamless(resized, axes=axes, overlap_fraction=1/64)
        images[name] = np.asarray(joined)
        record = recipes[name]
        record['id'] = spec['set'] + '-' + Path(name).stem.lower().replace('_', '-')
        record['prompt'] = json.loads(path.with_suffix('.json').read_text())['prompt']
        record['parameters'].update(set=spec['set'], material=record['id'], scale=1., tiling_axes=axes,
            tileable=bool(axes), antitile='off', source_attempt=source['attempt'],
            input=source_input(name, source),
            colour_policy=f"remaster building_set: {spec['textures'][name]['colour']['mode']}", source_band=band,
            luminance_policy='guard rail only: ratio to the original target is recorded, not enforced',
            source_size_policy=f'one Lanczos resize to {size} square, then narrow permitted-axis seam cuts')
        record['generation'] = dict(outcome='complete', attempts=source_attempts(manifest, name),
            selected_attempt=source['attempt'], selection_reason=chosen.get('reason'),
            source_policy='image_gen edits of the accepted concept; immutable PNGs with C2PA',
            model_identity=source.get('model'), seam=seam, remaster_asset=asset,
            detail_fit_applied=False, marigold_applied=False, shoulder_applied=False)
        if model_maps:
            record['_model_maps'] = model_maps
            record['parameters']['maps'] = 'model: ' + ', '.join(sorted(model_maps)) + ' baked with the albedo'
            record['generation']['model_maps'] = source['maps']
        records.append(record)
    targets = {r['replaces'][0]: oklab_to_linear(r['parameters']['target_oklab']) for r in records}
    corrected, colour = correct_colours(images, spec, targets)
    low, high = spec.get('guard', {}).get('luminance_ratio', [.75, 1.25])
    clip_max = spec.get('guard', {}).get('clipped_max', .01)
    report = {}
    for record in records:
        name = record['replaces'][0]
        out = prepared_dir / (Path(name).stem + '.png')
        Image.fromarray(corrected[name]).save(out)
        mean = _linear(corrected[name].astype(float) / 255).mean(axis=(0, 1))
        ratio = float(mean @ LUMINANCE / (targets[name] @ LUMINANCE))
        measurement = dict(target_linear_rgb=targets[name].tolist(), mean_linear_rgb=mean.tolist(),
            luminance_ratio=ratio, luminance_within_guard=low <= ratio <= high,
            clipped_within_guard=colour[name]['clipped_fraction'] <= clip_max)
        record['generation'].update(colour=colour[name], colour_measurement=measurement,
            prepared_sha256=pilot.file_digest(out), detector=measure_light(out))
        report[name] = dict(colour[name], **measurement)
    return records, report


def scale_relief(normal_map: np.ndarray, relief: float) -> np.ndarray:
    """Scale the tangent-space tilt of a 0..1 encoded normal map; 0 is flat, 1 unchanged."""
    if not 0 <= relief <= 1:
        raise ValueError(f'relief must be within 0..1, got {relief}')
    vector = normal_map.astype(np.float64) * 2 - 1
    vector[..., :2] *= relief
    vector /= np.linalg.norm(vector, axis=-1, keepdims=True)
    return (vector * .5 + .5).astype(normal_map.dtype)


def new_staging(name: str) -> Path:
    output = pack_materials.staging_output(remaster.data_root() / 'content-staging' / name)
    if output.exists():
        raise ValueError(f'{output}: staging set exists; choose a new set name')
    return output


def pack(records: list[dict], spec: dict, derived_root: Path) -> dict:
    output = new_staging(spec['set'])
    if spec['base'] == 'original':
        base = output
        (output / 'materials').mkdir(parents=True)
        (output / 'materials/materials.json').write_text('{}\n')
    else:
        base = remaster.data_root() / 'content-staging' / spec['base']
        shutil.copytree(base, output, symlinks=False)
    index_path = output / 'materials/materials.json'
    before = json.loads(index_path.read_text())
    index = deepcopy(before)
    write_dds = pack_materials.load_compressor()
    names = {r['replaces'][0] for r in records}
    missing = names - before.keys()
    if missing and spec['base'] != 'original':
        raise ValueError(f'{spec["base"]} has no entries for {sorted(missing)}')
    entry = dict(albedo=None, normal=None, orm=None, scale=1., antitile='off', antitile_cell=1.)
    for record in records:
        maps = pack_materials.read_maps(record, derived_root)
        directory = output / 'materials' / record['id']
        directory.mkdir()
        write_dds(directory / 'albedo.dds', maps['albedo'])
        write_dds(directory / 'normal.dds', maps['normal'], normal=True)
        write_dds(directory / 'orm.dds', np.stack([maps[k] for k in ('ao', 'roughness', 'metallic')], axis=2))
        name = record['replaces'][0]
        index[name] = dict(before.get(name, entry), **{k: f'{record["id"]}/{k}.dds' for k in ('albedo', 'normal', 'orm')},
                           scale=1., antitile='off')
    for name in before.keys() - names:
        for channel in ('albedo', 'normal', 'orm'):
            relative = Path('materials') / before[name][channel]
            if pilot.file_digest(base / relative) != pilot.file_digest(output / relative):
                raise ValueError(f'{name}: unrelated DDS changed')
    index_path.write_text(json.dumps(index, indent=2) + '\n')
    return dict(entries=len(index), replaced=sorted(names), unchanged_entries=len(index) - len(names))


def compose(name: str, parts: list[str]) -> dict:
    """Join the staging sets `parts` into a new set; a texture or a visual may come from one part only.

    A part's meshes (`meshes/meshes.json` and its GLB files, a prop like the barrel) come along with its
    materials. Entries of one part may share files: the barrel's wood and iron use one atlas. A part's
    world chunk edits (`worlds/<WORLD>/chunks/*.glb`, like the harbour stairs) and vob layer files
    (`worlds/<WORLD>/vobs.d/*.json`, like the harbour's traces of life) come along too; such a part may have no
    materials at all. Particle descriptions (`particles.d/*.json`) come along as well; file and effect
    names must belong to one part only."""
    if len(parts) < 2 or len(set(parts)) != len(parts):
        raise ValueError('compose needs two or more distinct parts')
    staging = remaster.data_root() / 'content-staging'
    indexes, meshes, chunks, particles, owner = {}, {}, {}, {}, {}
    for part in parts:
        material_index = staging / part / 'materials/materials.json'
        indexes[part] = json.loads(material_index.read_text()) if material_index.is_file() else {}
        worlds = staging / part / 'worlds'
        chunks[part] = sorted(f.relative_to(worlds) for f in worlds.rglob('*') if f.is_file() and (
            f.suffix == '.glb' or f.suffix == '.json' and f.parent.name == 'vobs.d')) if worlds.is_dir() else []
        mesh_index = staging / part / 'meshes/meshes.json'
        meshes[part] = json.loads(mesh_index.read_text()) if mesh_index.is_file() else {}
        particle_dir = staging / part / 'particles.d'
        particles[part] = sorted(p.name for p in particle_dir.glob('*.json') if p.is_file())
        if not material_index.is_file() and not meshes[part] and not chunks[part] and not particles[part]:
            raise ValueError(f'{part}: no materials, meshes, world chunks or particle descriptions')
        # the engine finds a visual by name without case
        for key in [*indexes[part], *(f'visual {visual.upper()}' for visual in meshes[part]),
                    *(f'chunk {chunk.as_posix().upper()}' for chunk in chunks[part])]:
            if key in owner:
                raise ValueError(f'{key}: in both {owner[key]} and {part}')
            owner[key] = part
        for filename in particles[part]:
            definitions = json.loads((particle_dir / filename).read_text())
            keys = [f'particle file {filename}',
                    *(f'particle effect {effect["name"].upper()}' for effect in definitions['effects'])]
            for key in keys:
                if key in owner:
                    raise ValueError(f'{key}: in both {owner[key]} and {part}')
                owner[key] = part
    output = new_staging(name)
    (output / 'materials').mkdir(parents=True)
    copied, digests = {}, {'materials': {}, 'meshes': {}, 'worlds': {}, 'particles.d': {}}

    def take(part: str, kind: str, relative: Path) -> None:
        key = f'{kind}/{relative}'
        if key in copied:
            if copied[key] != part:
                raise ValueError(f'{relative}: path used by two parts')
            return
        source, target = staging / part / kind / relative, output / kind / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        digests[kind][str(relative)] = pilot.file_digest(target)
        if digests[kind][str(relative)] != pilot.file_digest(source):
            raise ValueError(f'{relative}: copy differs from {part}')
        copied[key] = part

    index, mesh_index = {}, {}
    for part, part_index in indexes.items():
        for texture, entry in part_index.items():
            for channel in ('albedo', 'normal', 'orm'):
                take(part, 'materials', Path(entry[channel]))
            index[texture] = entry
        for visual, entry in meshes[part].items():
            take(part, 'meshes', Path(entry['glb']))
            mesh_index[visual] = entry
        for chunk in chunks[part]:
            take(part, 'worlds', chunk)
        for filename in particles[part]:
            take(part, 'particles.d', Path(filename))
    (output / 'materials/materials.json').write_text(json.dumps(index, indent=2) + '\n')
    if mesh_index:
        (output / 'meshes/meshes.json').write_text(json.dumps(mesh_index, indent=2) + '\n')
    composition = dict(set=name, date=datetime.now(timezone.utc).strftime('%Y-%m-%d'),
                       parts={part: sorted(part_index) + sorted(meshes[part]) + [f'worlds/{c.as_posix()}' for c in chunks[part]]
                                    + [f'particles.d/{f}' for f in particles[part]]
                              for part, part_index in indexes.items()},
                       dds_sha256=digests['materials'])
    if mesh_index:
        composition['glb_sha256'] = digests['meshes']
    if digests['worlds']:
        composition['world_sha256'] = digests['worlds']
    if digests['particles.d']:
        composition['particles_sha256'] = digests['particles.d']
    (output / 'composition.json').write_text(json.dumps(composition, indent=2) + '\n')
    result = dict(set=name, entries=len(index), meshes=len(mesh_index),
                  parts={part: len(i) + len(meshes[part]) + len(chunks[part]) + len(particles[part]) for part, i in indexes.items()})
    if digests['worlds']:
        result['chunks'] = len(digests['worlds'])
    if digests['particles.d']:
        result['particles'] = len(digests['particles.d'])
    return result


def enable(name: str) -> dict:
    """Make the staging set `name` the game's working content: `<data>/content`, which the engine reads
    when OPENGOTHIC_CONTENT is unset. Its `materials/`, `meshes/`, `worlds/` and `particles.d/` are replaced; other folders
    there stay; `enabled.json` records the set. Enabling is the user's decision (gate `enable`)."""
    staging = remaster.data_root() / 'content-staging' / name
    if not (staging / 'materials/materials.json').is_file():
        raise ValueError(f'{staging}: no materials/materials.json')
    live = remaster.data_root() / 'content'
    incoming = live / '.enable'
    shutil.rmtree(incoming, ignore_errors=True)
    incoming.mkdir(parents=True)
    kinds = [kind for kind in ('materials', 'meshes', 'worlds', 'particles.d') if (staging / kind).is_dir()]
    for kind in kinds:
        shutil.copytree(staging / kind, incoming / kind)
        for source in (staging / kind).rglob('*'):
            if source.is_file() and pilot.file_digest(source) != pilot.file_digest(incoming / kind / source.relative_to(staging / kind)):
                raise ValueError(f'{source}: copy differs')
    for kind in ('materials', 'meshes', 'worlds', 'particles.d'):
        shutil.rmtree(live / kind, ignore_errors=True)
        if kind in kinds:
            (incoming / kind).rename(live / kind)
    incoming.rmdir()
    composition = staging / 'composition.json'
    record = dict(set=name, date=datetime.now(timezone.utc).strftime('%Y-%m-%d'),
                  materials=len(json.loads((live / 'materials/materials.json').read_text())),
                  meshes=len(json.loads((live / 'meshes/meshes.json').read_text())) if 'meshes' in kinds else 0,
                  chunks=sum(1 for _ in (live / 'worlds').rglob('*.glb')) if 'worlds' in kinds else 0,
                  parts=json.loads(composition.read_text())['parts'] if composition.is_file() else None)
    (live / 'enabled.json').write_text(json.dumps(record, indent=2, ensure_ascii=False) + '\n')
    return record


def contact_sheet(records: list[dict], derived_root: Path, target: Path) -> None:
    side, header = 400, 32
    cols = min(3, len(records))
    rows = -(-len(records) // cols)
    sheet = Image.new('RGB', (side * cols, (side + header) * rows), '#252626')
    draw = ImageDraw.Draw(sheet)
    for i, record in enumerate(records):
        x, y = i % cols * side, i // cols * (side + header)
        with Image.open(derived_root / record['files']['albedo']) as image:
            sheet.paste(image.resize((side, side), Image.Resampling.LANCZOS), (x, y + header))
        draw.text((x + 4, y + 8), Path(record['replaces'][0]).stem.removeprefix('NW_CITY_'), fill='white')
    for quality in range(90, 39, -5):
        data = BytesIO()
        sheet.save(data, 'JPEG', quality=quality, optimize=True)
        if data.tell() <= 1_000_000:
            target.write_bytes(data.getvalue())
            return
    raise ValueError('albedo sheet exceeds 1 MB')


def build(asset: str, spec: dict, records: list[dict]) -> dict:
    derived_root = remaster.data_root() / 'content-staging' / f'{asset}-sources'
    for record in records:
        path = remaster.work_root(asset) / 'prepared' / spec['set'] / (Path(record['replaces'][0]).stem + '.png')
        with Image.open(path) as image:
            record['_maps'] = bakeoff.filtered_maps(image, record)
        model = record.pop('_model_maps', None)
        if model:
            # the model's own normal, roughness and AO; its occlusion is already in the albedo
            record['_maps']['ao'] = np.ones_like(model['roughness'])
            record['_maps'].update(model)
        relief = float(spec['textures'][record['replaces'][0]].get('relief', 1.))
        if relief != 1.:
            record['_maps']['normal'] = scale_relief(record['_maps']['normal'], relief)
        record['parameters']['relief'] = relief
        if not model:
            lo, hi = record['parameters']['roughness_range']
            record['_maps']['roughness'] = np.clip(record['_maps']['roughness'], np.ceil(lo * 255) / 255,
                                                   np.floor(hi * 255) / 255)
        pilot.save_maps(record, derived_root)
        record['generation']['albedo_sha256'] = pilot.file_digest(derived_root / record['files']['albedo'])
        registry_module.validate_entry(record, content_root=derived_root, source_root=remaster.sources_root())
    registry_path = remaster.ROOT / spec['registry']
    previous = registry_path.read_text() if registry_path.exists() else ''
    ids = {json.loads(line)['id'] for line in previous.splitlines() if line.strip()}
    if any(r['id'] in ids for r in records):
        raise ValueError(f'{registry_path}: already contains set {spec["set"]}')
    invariants = pack(records, spec, derived_root)
    registry_path.parent.mkdir(parents=True, exist_ok=True)
    registry_path.write_text(previous + ''.join(json.dumps(r, ensure_ascii=False, sort_keys=True) + '\n'
                                                for r in records))
    contact_sheet(records, derived_root, remaster.asset_dir(asset) / f'{spec["set"]}-albedos.jpg')
    manifest = remaster.load_manifest(asset)
    manifest['build'] = dict(set=spec['set'], base=spec['base'],
        date=datetime.now(timezone.utc).strftime('%Y-%m-%d'),
        albedo_sha256={r['replaces'][0]: r['generation']['albedo_sha256'] for r in records}, **invariants)
    remaster.save_manifest(asset, manifest)
    return invariants


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'build', 'compose', 'enable'))
    parser.add_argument('asset', help='asset, the new set name for compose, or the set to enable')
    parser.add_argument('parts', nargs='*', help='compose: staging sets to join')
    args = parser.parse_args(argv)
    try:
        if args.command == 'compose':
            print(json.dumps(compose(args.asset, args.parts), sort_keys=True), flush=True)
            return 0
        if args.command == 'enable':
            print(json.dumps(enable(args.asset), sort_keys=True, ensure_ascii=False), flush=True)
            return 0
        spec = load_spec(args.asset)
        records, report = prepare(args.asset, spec)
        for name, row in report.items():
            print(json.dumps(dict(texture=name, **row), sort_keys=True), flush=True)
        if args.command == 'build':
            print(json.dumps(build(args.asset, spec, records), sort_keys=True), flush=True)
    except (OSError, ValueError, KeyError, StopIteration) as error:
        print(f'building_set: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
