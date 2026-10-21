#!/usr/bin/env python3
"""Prepare concept-derived textures and build isolated house-1 / house-2 sets."""
from __future__ import annotations

import argparse
from copy import deepcopy
from io import BytesIO
import json
from pathlib import Path
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw
from scipy.ndimage import gaussian_filter

import bakeoff
import city_set
from material_colour import _linear, _srgb, oklab_to_linear
from material_lighting import measure_file as measure_light
import pack_materials
import quality_pilot as pilot
import registry as registry_module
from seam_fix import seamless


TEXTURES = tuple(name + '.TGA' for name in (
    'NW_CITY_WALL_03', 'NW_CITY_WINDOW_03',
    'NW_CITY_FACHWERKBASEMENT_WINDOW_01', 'NW_CITY_FACHWERKBASEMENT_EMPTY_01',
    'NW_CITY_FACHWERKBASEMENT_CHAIN_01', 'NW_CITY_WOODBEAM_02'))
LUMINANCE = np.array([.2126, .7152, .0722])
REPORT = pilot.ROOT / 'reports/content/2026-10-market-house-concept'


def tiling_axes(name):
    if name not in TEXTURES:
        raise ValueError(f'unknown house texture: {name}')
    return 'u' if name == 'NW_CITY_WINDOW_03.TGA' else 'uv'


def prepare_image(image, name):
    image = image.convert('RGB').resize((1024, 1024), Image.Resampling.LANCZOS)
    return seamless(image, axes=tiling_axes(name), overlap_fraction=1/64)


def apply_gains(pixels, gains):
    linear = _linear(np.asarray(pixels, dtype=np.float64)/255)
    return np.rint(_srgb(np.clip(linear*np.asarray(gains), 0, 1))*255).astype(np.uint8)


def oak_masks(images):
    """One Otsu threshold on pooled, seam-aware blurred log luminance."""
    blurred = {}
    for name, pixels in images.items():
        linear = _linear(np.asarray(pixels, dtype=float)/255)
        log_y = np.log(np.maximum(linear @ LUMINANCE, 1e-8))
        axes = tiling_axes(name)
        modes = ('wrap' if 'v' in axes else 'reflect', 'wrap' if 'u' in axes else 'reflect')
        blurred[name] = gaussian_filter(log_y, sigma=6, mode=modes)
    histogram, edges = np.histogram(np.concatenate([v.ravel() for v in blurred.values()]), bins=256)
    centres = (edges[:-1]+edges[1:])/2
    count = histogram.cumsum()
    sums = (histogram*centres).cumsum()
    score = (sums[-1]*count-sums*count[-1])**2 / np.maximum(count*(count[-1]-count), 1)
    # A two-colour input has an entire optimal threshold interval. Its centre
    # keeps the sigmoid away from either material's flat interior.
    threshold = float(centres[np.isclose(score, score.max(), rtol=1e-12, atol=0)].mean())
    masks = {name: 1/(1+np.exp(np.clip((log_y-threshold)/.3, -60, 60)))
        for name, log_y in blurred.items()}
    return masks, dict(log_y_threshold=threshold, log_y_width=.3, gaussian_sigma_px=6,
        method='shared pooled Otsu after Gaussian of log linear luminance; sigmoid; no colour condition')


def correct_house_colour(images, areas, targets):
    """Solve one RGB multiplier against the area-weighted house, including clipping."""
    names = list(images)
    weights = np.array([areas[name] for name in names], dtype=float)
    if not np.isfinite(weights).all() or np.any(weights <= 0):
        raise ValueError('house texture areas must be finite and positive')
    weights /= weights.sum()
    target = weights @ np.array([targets[name] for name in names])
    if not np.isfinite(target).all() or np.any(target < 0) or np.any(target > 1):
        raise ValueError('house target must be an in-gamut linear RGB mean')
    # All prepared inputs are uint8. Histograms retain every sample and weight
    # textures by world area, independently of their pixel dimensions.
    hist = np.zeros((3, 256), dtype=float)
    for weight, name in zip(weights, names):
        pixels = np.asarray(images[name])
        for channel in range(3):
            hist[channel] += weight*np.bincount(pixels[..., channel].ravel(), minlength=256)/pixels[..., channel].size
    values = _linear(np.arange(256, dtype=float)/255)
    gains = np.ones(3)
    for channel in range(3):
        left, right = 0., 1.
        while hist[channel] @ np.minimum(values*right, 1) < target[channel] and right < 65536:
            right *= 2
        for _ in range(48):
            middle = (left+right)/2
            if hist[channel] @ np.minimum(values*middle, 1) < target[channel]:
                left = middle
            else:
                right = middle
        gains[channel] = (left+right)/2
    corrected = {name: apply_gains(images[name], gains) for name in names}
    after = weights @ np.array([_linear(corrected[name].astype(float)/255).mean(axis=(0, 1)) for name in names])
    error = float(np.abs(after-target).max())
    metadata = dict(method='one shared linear RGB multiplier for the supplied texture group; hard gamut clipping only',
        channel_gains=gains.tolist(), target_linear_rgb=target.tolist(),
        linear_rgb_mean_before=(hist @ values).tolist(), linear_rgb_mean_after=after.tolist(),
        linear_rgb_max_error=error, tolerance=.005, area_m2=dict(areas))
    if error > .005:
        raise ValueError(f'weighted house colour error {error:.6f} exceeds .005')
    return corrected, metadata


def correct_material_colours(images, areas, targets):
    """Match oak, stone and the two painted modules with shared material gains."""
    oak = TEXTURES[-1]
    corrected, oak_colour = correct_house_colour({oak: images[oak]}, {oak: areas[oak]}, {oak: targets[oak]})
    oak_target = _linear(corrected[oak].astype(float)/255).mean(axis=(0, 1))
    stone = TEXTURES[2:5]
    stone_images, stone_colour = correct_house_colour({n: images[n] for n in stone},
        {n: areas[n] for n in stone}, {n: targets[n] for n in stone})
    corrected.update(stone_images)
    panels = TEXTURES[:2]
    masks, mask_info = oak_masks({n: images[n] for n in panels})
    weight = {n: areas[n]/sum(areas[k] for k in panels) for n in panels}
    linear = {n: _linear(images[n].astype(float)/255) for n in panels}
    # Total colour and the final colour on wood (mask > .5) are two linear
    # constraints per channel. The mask still blends every boundary softly.
    moments = np.zeros((2, 2, 3))
    wood_share = 0.
    target = sum(weight[n]*targets[n] for n in panels)
    for name in panels:
        m, pixels = masks[name][..., None], linear[name]
        core = m > .5
        moments[0, 0] += weight[name]*(pixels*m).mean(axis=(0, 1))
        moments[0, 1] += weight[name]*(pixels*(1-m)).mean(axis=(0, 1))
        moments[1, 0] += weight[name]*(pixels*m*core).mean(axis=(0, 1))
        moments[1, 1] += weight[name]*(pixels*(1-m)*core).mean(axis=(0, 1))
        wood_share += weight[name]*float(core.mean())
    gains = np.stack([np.linalg.solve(moments[:, :, c], [target[c], wood_share*oak_target[c]])
        for c in range(3)], axis=1)
    if not np.isfinite(gains).all() or np.any(gains < 0):
        raise ValueError(f'painted oak/plaster colour is unattainable with nonnegative gains: {gains.tolist()}')
    clipped = {}
    for name in panels:
        m = masks[name][..., None]
        mixed = linear[name]*(m*gains[0]+(1-m)*gains[1])
        clipped[name] = float(np.any(mixed > 1, axis=2).mean())
        corrected[name] = np.rint(_srgb(np.clip(mixed, 0, 1))*255).astype(np.uint8)
    final = {n: _linear(corrected[n].astype(float)/255) for n in panels}
    after = sum(weight[n]*final[n].mean(axis=(0, 1)) for n in panels)
    wood_after = sum(weight[n]*(final[n]*(masks[n] > .5)[..., None]).mean(axis=(0, 1)) for n in panels)/wood_share
    error = float(np.max(np.abs(after-target)))
    wood_error = float(np.max(np.abs(wood_after-oak_target)))
    if max(error, wood_error) > .005:
        raise ValueError(f'painted colour error after quantization: total={error:.6f}, oak={wood_error:.6f}')
    metadata = dict(oak_target_linear_rgb=oak_target.tolist(), oak=oak_colour, stone=stone_colour,
        mask=mask_info, painted=dict(oak_channel_gains=gains[0].tolist(), plaster_channel_gains=gains[1].tolist(),
            target_linear_rgb=target.tolist(), linear_rgb_mean_after=after.tolist(), linear_rgb_max_error=error,
            oak_mean_linear_rgb=wood_after.tolist(), oak_rgb_max_error=wood_error,
            wood_measurement='final pixels with mask > 0.5; area-weighted across WALL/WINDOW',
            clipped_pixel_fraction=clipped))
    return corrected, metadata, masks


def mask_sheet(images, masks):
    side, header = 512, 32
    sheet = Image.new('RGB', (side*2, (side+header)*2), '#252626')
    draw = ImageDraw.Draw(sheet)
    for row, name in enumerate(TEXTURES[:2]):
        y = row*(side+header)
        sheet.paste(Image.fromarray(images[name]).resize((side, side), Image.Resampling.LANCZOS), (0, y+header))
        mask = Image.fromarray(np.rint(masks[name]*255).astype(np.uint8)).convert('RGB')
        sheet.paste(mask.resize((side, side), Image.Resampling.LANCZOS), (side, y+header))
        draw.text((8, y+8), f'{Path(name).stem} | white = oak', fill='white')
    sheet.save(REPORT / 'house-2-masks.jpg', 'JPEG', quality=90, optimize=True)


def upper_plaster_spill(name, mask):
    """Measure the requested damp check in manually marked plaster fields.

    These polygons only measure the two known source layouts; they never
    participate in masking or colour correction.
    """
    polygons = ([[(.07, .09), (.93, .09), (.93, .50), (.70, .91), (.30, .91), (.07, .50)]]
        if name == TEXTURES[0] else [
            [(.07, .055), (.93, .055), (.93, .13), (.07, .13)],
            [(.065, .22), (.22, .22), (.22, .60), (.065, .60)],
            [(.80, .22), (.94, .22), (.94, .60), (.80, .60)],
            [(.12, .68), (.88, .68), (.73, .925), (.27, .925)]])
    height, width = mask.shape
    image = Image.new('L', (width, height))
    draw = ImageDraw.Draw(image)
    for polygon in polygons:
        draw.polygon([(round(x*(width-1)), round(y*(height-1))) for x, y in polygon], fill=255)
    plaster = np.asarray(image) > 0
    upper = np.arange(height)[:, None]/height < (.30 if name == TEXTURES[0] else .41)
    marked = plaster & upper & (mask > .5)
    return dict(manual_plaster_polygons_uv=[[list(point) for point in polygon] for polygon in polygons],
        plaster_pixels=int(plaster.sum()),
        upper_pixels_marked_oak=int(marked.sum()), fraction_of_plaster=float(marked.sum()/plaster.sum()),
        advisory_limit=.02, exceeds_2_percent=bool(marked.sum()/plaster.sum() > .02))


def prepare(args):
    attempts = pilot.read_sources(args.source_root / 'market-house', TEXTURES,
        args.source_root, namespace='market-house')
    selection = json.loads(args.selection.read_text())
    measurement = json.loads(args.measurement.read_text())
    areas = {name: sum(t['area_m2'] for t in measurement['candidate_house_triangles']
        if t['texture'] == name) for name in TEXTURES}
    mapping = pilot.csv_rows(pilot.ROOT / 'data/content/city-materials.csv')
    recipes = {r['replaces'][0]: r for r in city_set.recipes(textures=TEXTURES)}
    records, images = [], {}
    args.prepared_dir.mkdir(parents=True, exist_ok=True)
    for name in TEXTURES:
        number = selection['selected_attempts'][name]
        chosen = next(a for a in attempts[name] if a['attempt'] == number)
        record = recipes[name]
        resource_id = args.set + '-' + Path(name).stem.lower().replace('_', '-')
        record.update(id=resource_id, prompt=chosen['prompt'])
        row = mapping[name]
        record['parameters'].update(set=args.set, material=resource_id, scale=1.,
            tiling_axes=tiling_axes(name), tileable=True, antitile='off',
            physical_uv_metres=[float(row['uv_m_u']), float(row['uv_m_v'])],
            source_attempt=number, input='edit of accepted facade concept; no original texture pixels',
            colour_policy=('shared oak/stone/plaster gains with a pooled blurred-luminance oak mask'
                if args.set == 'house-2' else 'one shared area-weighted linear RGB multiplier for the whole house'),
            luminance_policy=('individual target ratio within 0.75–1.25 required'
                if args.set == 'house-2' else 'individual target ratio within 0.75–1.25 is advisory only'),
            source_size_policy='one Lanczos resize to 1024 square, then narrow permitted-axis seam cuts')
        with Image.open(args.source_root / chosen['source_png']) as source:
            image, seam = prepare_image(source, name)
        images[name] = np.asarray(image)
        record['generation'] = dict(outcome='complete', attempts=attempts[name],
            selected_attempt=number, attempt_limit=2,
            selection_reason=selection.get('reasons', {}).get(name, 'accepted concept-thread selection'),
            source_policy='image_gen edits in the accepted facade concept thread; immutable original PNGs',
            model_identity='image_gen.imagegen; underlying model/version and seed not exposed',
            seam=seam, house_area_m2=areas[name], measurement_sha256=pilot.file_digest(args.measurement),
            detail_fit_applied=False, marigold_applied=False, shoulder_applied=False)
        records.append(record)
    targets = {r['replaces'][0]: oklab_to_linear(r['parameters']['target_oklab']) for r in records}
    masks = {}
    if args.set == 'house-2':
        corrected, common, masks = correct_material_colours(images, areas, targets)
        mask_sheet(images, masks)
    else:
        corrected, common = correct_house_colour(images, areas, targets)
    for record in records:
        name = record['replaces'][0]
        image = Image.fromarray(corrected[name])
        path = args.prepared_dir / (Path(name).stem + '.png')
        image.save(path)
        actual = _linear(corrected[name].astype(float)/255).mean(axis=(0, 1))
        ratio = float(actual @ LUMINANCE / (targets[name] @ LUMINANCE))
        measurement = dict(
            target_linear_rgb=targets[name].tolist(), mean_linear_rgb=actual.tolist(),
            luminance_ratio=ratio, within_25_percent=.75 <= ratio <= 1.25)
        if name in masks:
            core = masks[name] > .5
            wood = _linear(corrected[name].astype(float)/255)[core].mean(axis=0)
            oak_target = np.array(common['oak_target_linear_rgb'])
            wood_ratio = wood/oak_target
            measurement.update(oak_mean_linear_rgb=wood.tolist(), oak_target_linear_rgb=oak_target.tolist(),
                oak_channel_ratios=wood_ratio.tolist(), oak_luminance_ratio=float(wood @ LUMINANCE/(oak_target @ LUMINANCE)),
                oak_within_25_percent=bool(np.all((wood_ratio >= .75) & (wood_ratio <= 1.25))),
                mask_above_half_fraction=float(core.mean()), upper_plaster_spill=upper_plaster_spill(name, masks[name]))
        record['generation'].update(**{('material_colour' if masks else 'house_colour'): common},
            colour_measurement=measurement,
            prepared_sha256=pilot.file_digest(path), detector=measure_light(path))
    if args.set == 'house-2':
        failed = [dict(texture=r['replaces'][0], **r['generation']['colour_measurement']) for r in records
            if not r['generation']['colour_measurement']['within_25_percent']
            or not r['generation']['colour_measurement'].get('oak_within_25_percent', True)]
        if failed:
            raise ValueError(f'house-2 brightness / oak outside 25%: {json.dumps(failed)}')
    return records, common


def clone_and_pack(records, args):
    output = pack_materials.staging_output(args.pilot3b.parent / args.set)
    if output.exists():
        raise ValueError(f'{output}: staging exists; refusing to overwrite')
    shutil.copytree(args.pilot3b, output, symlinks=False)
    index_path = output / 'materials/materials.json'
    before = json.loads(index_path.read_text())
    if len(before) != 58:
        raise ValueError(f'pilot-3b: expected 58 entries, got {len(before)}')
    index = deepcopy(before)
    write_dds = pack_materials.load_compressor()
    for record in records:
        maps = pack_materials.read_maps(record, args.derived_root)
        directory = output / 'materials' / record['id']
        directory.mkdir()
        write_dds(directory / 'albedo.dds', maps['albedo'])
        write_dds(directory / 'normal.dds', maps['normal'], normal=True)
        write_dds(directory / 'orm.dds', np.stack([maps[k] for k in ('ao', 'roughness', 'metallic')], axis=2))
        name = record['replaces'][0]
        index[name] = dict(before[name], **{k: f'{record["id"]}/{k}.dds' for k in ('albedo', 'normal', 'orm')},
            scale=1., antitile='off')
    if set(index) != set(before) or {k for k in index if index[k] != before[k]} != set(TEXTURES):
        raise ValueError(f'{args.set} must change exactly six pilot-3b entries')
    for name in before.keys()-set(TEXTURES):
        for channel in ('albedo', 'normal', 'orm'):
            relative = Path('materials') / before[name][channel]
            if pilot.file_digest(args.pilot3b / relative) != pilot.file_digest(output / relative):
                raise ValueError(f'{name}: unrelated DDS changed')
    index_path.write_text(json.dumps(index, indent=2)+'\n')
    return dict(entries=len(index), changed_from_pilot3b=6, unchanged_entries=52, unchanged_dds=156,
        target_replacements=12)


def contact_sheet(records, args):
    side, header = 400, 32
    sheet = Image.new('RGB', (side*3, (side+header)*2), '#252626')
    draw = ImageDraw.Draw(sheet)
    for i, record in enumerate(records):
        x, y = i%3*side, i//3*(side+header)
        with Image.open(args.derived_root / record['files']['albedo']) as image:
            sheet.paste(image.resize((side, side), Image.Resampling.LANCZOS), (x, y+header))
        draw.text((x+4, y+8), Path(record['replaces'][0]).stem.removeprefix('NW_CITY_'), fill='white')
    for quality in range(90, 39, -5):
        data = BytesIO()
        sheet.save(data, 'JPEG', quality=quality, optimize=True)
        if data.tell() <= 1_000_000:
            filename = 'house-2-albedos.jpg' if args.set == 'house-2' else 'house-albedos.jpg'
            (REPORT / filename).write_bytes(data.getvalue())
            return
    raise ValueError('house albedo sheet exceeds 1 MB')


def build(args, records):
    for record in records:
        path = args.prepared_dir / (Path(record['replaces'][0]).stem + '.png')
        with Image.open(path) as image:
            record['_maps'] = bakeoff.filtered_maps(image, record)
        rlo, rhi = record['parameters']['roughness_range']
        record['_maps']['roughness'] = np.clip(record['_maps']['roughness'], np.ceil(rlo*255)/255, np.floor(rhi*255)/255)
        pilot.save_maps(record, args.derived_root)
        record['generation']['albedo_sha256'] = pilot.file_digest(args.derived_root / record['files']['albedo'])
        registry_module.validate_entry(record, content_root=args.derived_root, source_root=args.source_root)
    invariants = clone_and_pack(records, args)
    previous = args.registry_output.read_text() if args.registry_output.exists() else ''
    ids = {json.loads(line)['id'] for line in previous.splitlines()}
    if any(r['id'] in ids for r in records):
        raise ValueError('registry already contains this house set')
    args.registry_output.write_text(previous+''.join(json.dumps(r, ensure_ascii=False, sort_keys=True)+'\n' for r in records))
    contact_sheet(records, args)
    return invariants


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'build'))
    parser.add_argument('--set', choices=('house-1', 'house-2'), default='house-1')
    parser.set_defaults(source_root=registry_module.DEFAULT_SOURCE_ROOT,
        selection=registry_module.DEFAULT_SOURCE_ROOT / 'market-house-selection.json',
        measurement=pilot.BASE / 'texture-intent/market-house-measurement.json',
        prepared_dir=pilot.BASE / 'market-house/prepared',
        derived_root=pilot.BASE / 'content-staging/market-house-sources',
        pilot3b=pilot.BASE / 'content-staging/pilot-3b',
        registry_output=pilot.ROOT / 'data/content/market-house.jsonl')
    args = parser.parse_args(argv)
    if args.set == 'house-2':
        args.prepared_dir = pilot.BASE / 'market-house/house-2/prepared'
    try:
        # The source store is immutable; every writable output stays outside
        # the live content root and all existing staging sets.
        for path in (args.prepared_dir, args.derived_root, args.registry_output):
            output = pack_materials.staging_output(path)
            for existing in args.pilot3b.parent.iterdir():
                if not existing.is_dir() or existing.name == 'market-house-sources':
                    continue
                base = existing.resolve()
                if output == base or output.is_relative_to(base) or base.is_relative_to(output):
                    raise ValueError(f'output overlaps staging: {path}')
        records, common = prepare(args)
        print(json.dumps(dict(house_colour=common), sort_keys=True), flush=True)
        for record in records:
            print(json.dumps(dict(texture=record['replaces'][0], **record['generation']['colour_measurement']), sort_keys=True), flush=True)
        if args.command == 'build':
            print(json.dumps(build(args, records), sort_keys=True), flush=True)
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        print(f'market_house: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
