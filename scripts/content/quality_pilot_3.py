#!/usr/bin/env python3
"""Stage pilot-3a/3b with low-frequency IID and measured contrast/detail fitting.

prepare reproduces the iteration-2 canvas, seams and colour correction. build
accepts IID only for those exact flagged PNG bytes, fits the four CSV targets,
and packs two isolated copies of city-3. No models run in this process.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
from io import BytesIO
import json
from pathlib import Path
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFont

import bakeoff
import generate
from import_imagegen import _write_immutable
from material_lighting import measure_file as measure_light, detect_baked_lighting, lighting_risk
from material_measures import measure_material
import marigold_iid
import pack_materials
import quality_pilot as pilot
import quality_pilot_2 as iteration2
import registry as registry_module
from seam_fix import _minimum_path, seamless


NAMESPACE = 'quality-pilot-3'
SETS = ('pilot-3a', 'pilot-3b')
METRICS = ('log_spread', 'detail_k3', 'detail_k9', 'detail_k27')
WINDOW = 'NW_CITY_WINDOW_03.TGA'
TEXTURES = (*pilot.TEXTURES, WINDOW)
NEW_TEXTURES = tuple(name for name in TEXTURES if name not in (
    'NW_CITY_BODENDIELEN_01.TGA', 'NW_CITY_HAFENKAI_BODEN_01.TGA'))


def validate_outputs(args):
    protected = [args.staging_parent / name for name in ('city-3', 'pilot-b', 'pilot-c', 'pilot-2')]
    protected.append(args.city3)
    outputs = [args.source_root / NAMESPACE, args.prepared_dir, args.derived_root, args.report_dir,
        args.registry_output, *(args.staging_parent / name for name in SETS)]
    for path in outputs:
        output = pack_materials.staging_output(path)
        for existing in protected:
            base = existing.expanduser().resolve()
            if output == base or output.is_relative_to(base) or base.is_relative_to(output):
                raise ValueError(f'output overlaps a protected staging set: {path}')
    if args.registry_output.resolve() in (args.base_registry.resolve(), args.iteration2_registry.resolve()):
        raise ValueError('iteration-3 registry must be separate')


def prepared_input(image, target, *, frozen):
    if not frozen:
        return pilot.prepared_png(image, target, frozen=False)
    # Linux and macOS PNG encoders can emit different bytes for identical
    # pixels. Keep the actual IID input bytes and verify preprocessing pixels.
    with Image.open(target) as stored:
        if stored.mode != 'RGB' or not np.array_equal(np.asarray(stored), np.asarray(image)):
            raise ValueError(f'{target}: rebuilt pixels differ from prepared IID input')
    return pilot.file_digest(target)


def window_canvas(image, physical):
    """Cut the native U join, then sample the physical panel to UV storage once."""
    source = np.asarray(image)
    height, width = source.shape[:2]
    overlap = max(3, round(width/64))
    period = width-overlap
    error = np.mean(((source[:, :overlap].astype(float)-source[:, period:])/255)**2, axis=2)
    path, cost = _minimum_path(error[:, 1:-1])
    path += 1
    canvas = source[:, :period].copy()
    canvas[:, :overlap] = np.where((np.arange(overlap)[None] <= path[:, None])[..., None],
        source[:, period:], source[:, :overlap])
    # Periodic neighbours belong only to U. This single 2D Lanczos sample also
    # packs the 193 x 214 cm physical rectangle into square UV storage.
    periodic = Image.fromarray(np.concatenate([canvas]*3, axis=1))
    packed = periodic.resize((3072, 1024), Image.Resampling.LANCZOS).crop((1024, 0, 2048, 1024))
    aspect = physical[0]/physical[1]
    geometry = dict(physical_uv_metres=physical, modules_u=1, modules_v=1,
        source_size=[width, height], canvas_size=[period, height],
        source_to_physical_stretch=max(period/height/aspect, aspect/(period/height)), resamples_to_uv=1)
    seam = dict(applied=True, axes='u', overlap_fraction=1/64, source_size=[width, height],
        output_size=[1024, 1024], method='native U minimum-error cut; single periodic-U Lanczos resample to square UV',
        operations=[dict(axis='u', overlap_px=overlap, period_before_resize_px=period,
            path=path.tolist(), mean_squared_rgb_error=cost/height)])
    return packed, geometry, seam


def prepare(args):
    old = {r['replaces'][0]: r for r in bakeoff.read_registry(args.iteration2_registry)}
    city_window = next(r for r in pack_materials.select_set_records(
        bakeoff.read_registry(args.base_registry), 'city-3', args.mapping) if r['replaces'][0] == WINDOW)
    old[WINDOW] = city_window
    new = pilot.read_sources(args.input_dir, NEW_TEXTURES, args.source_root, namespace=NAMESPACE)
    selection = json.loads(args.selection.read_text())
    mapping = pilot.csv_rows(args.mapping)
    records = []
    for variant in SETS:
        directory = args.prepared_dir / variant
        directory.mkdir(parents=True, exist_ok=True)
        flagged = directory / 'flagged'
        flagged.mkdir(exist_ok=True)
        for name in TEXTURES:
            previous = old[name]
            fresh = variant == 'pilot-3b' and name in NEW_TEXTURES
            attempts = new[name] if fresh else previous['generation']['attempts']
            if name == WINDOW and not fresh:
                attempts = [dict(a, attempt=a['number']) for a in attempts]
            number = (selection['selected_attempts'][name] if fresh
                else previous['generation']['selected_attempt'])
            chosen = next(a for a in attempts if a['attempt'] == number)
            record = deepcopy(previous)
            resource_id = variant + '-' + Path(name).stem.lower().replace('_', '-')
            record.update(id=resource_id, prompt=chosen['prompt'], files={})
            record['models'] = [deepcopy(previous['models'][0])]
            params = record['parameters']
            for key in list(params):
                if key.startswith('iid_'):
                    del params[key]
            targets = {key: float(mapping[name]['target_' + key]) for key in METRICS}
            params.update(set=variant, material=resource_id, scale=1., source_attempt=number,
                detail_targets=targets, detail_fit_tolerance=.03, detail_fit_max_iterations=8,
                detail_gain_bounds=dict(g0=[.8, 1.6], g1=[.5, 3.], g2=[.5, 3.], g3=[.5, 3.]))
            input_path = args.source_root / chosen['source_png']
            input_source = None
            if name == WINDOW:
                row = mapping[name]
                physical = [float(row['uv_m_u']), float(row['uv_m_v'])]
                params.update(decision='module', layout='panel', antitile='off', tiling_axes='u',
                    target_oklab=[float(row['target_oklab_' + axis]) for axis in ('l', 'a', 'b')],
                    detail_count_target=dict(u=1, v=1), physical_uv_metres=physical,
                    source_aspect=physical[0]/physical[1],
                    parts=row['parts'], source_size_policy='one physical window-wall canvas; one Lanczos resample to 1024-square UV storage')
                if not fresh:
                    input_path = args.city3 / 'sources' / previous['files']['albedo']
                    relative = Path(NAMESPACE) / 'city-3-selected-window-albedo.png'
                    _write_immutable(args.source_root / relative, input_path.read_bytes())
                    input_source = dict(source_png=relative.as_posix(), source_sha256=pilot.file_digest(input_path),
                        registry_id=previous['id'], selected_albedo=previous['files']['albedo'])
                    input_path = args.source_root / relative
            with Image.open(input_path) as source:
                image = source.convert('RGB')
            geometry = None
            if name == pilot.ROOF_INSIDE:
                image, geometry = pilot.roof_canvas(image)
            if name == WINDOW:
                params.update(source_width=image.width, source_height=image.height)
                image, geometry, seam = window_canvas(image, physical)
            else:
                image = image.resize((1024, 1024), Image.Resampling.LANCZOS)
                axes = '' if params['layout'] == 'panel' else params['tiling_axes']
                image, seam = seamless(image, axes=axes, overlap_fraction=1/64)
            maps, colour, rejection = generate.target_corrected_maps(image, record)
            pixels = Image.fromarray(np.rint(maps['albedo'] * 255).astype(np.uint8))
            target = directory / (Path(name).stem + '.png')
            digest = prepared_input(pixels, target, frozen=args.command == 'build')
            lighting = measure_light(target)
            iid_input = flagged / (variant + '-' + target.name)
            if lighting['lighting_risk']:
                if args.command == 'build':
                    if not iid_input.is_file() or pilot.file_digest(iid_input) != digest:
                        raise ValueError(f'{variant}/{name}: flagged input differs from prepared PNG')
                else:
                    shutil.copyfile(target, iid_input)
            elif args.command == 'prepare':
                iid_input.unlink(missing_ok=True)
            record['generation'] = dict(outcome='complete', attempts=deepcopy(attempts),
                selected_attempt=number, attempt_limit=2,
                selection_reason=selection.get('reasons', {}).get(name) if fresh else 'iteration-2 selection retained',
                source_iteration=3 if fresh else 2, colour_correction_before=colour,
                numeric_rejection_before=rejection, geometry=geometry, seam=seam,
                prepared_sha256=digest, detector_before=lighting,
                source_policy='text-only image_gen; original generator PNG bytes retained and checksummed',
                model_identity=previous['generation']['model_identity'])
            if name == WINDOW and not fresh:
                record['generation'].update(source_iteration='city-3', input_source=input_source,
                    selection_reason='selected city-3 final albedo retained as input')
            records.append(record)
    return records


def iid_candidates(args):
    """Index both runs by exact input SHA, never by a texture name alone."""
    found = {}
    for directory, log, reused in ((args.iteration2_marigold_dir, args.iteration2_marigold_log, True),
            (args.marigold_dir, args.marigold_log, False)):
        if not log.is_file():
            continue
        for line in log.read_text().splitlines():
            item = json.loads(line)
            if 'input' not in item:
                continue
            source = directory / Path(item['output']).name
            if not source.is_file() or pilot.file_digest(source) != item['output_sha256']:
                raise ValueError(f'IID output does not match log: {source}')
            found[item['source_sha256']] = (source, item, reused)
    return found


def apply_iid(record, args, candidates):
    from material_detail import low_frequency_iid
    name = record['replaces'][0]
    variant = record['parameters']['set']
    generation = record['generation']
    with Image.open(args.prepared_dir / variant / (Path(name).stem + '.png')) as source:
        pixels = np.asarray(source.convert('RGB'))
    generation['marigold_applied'] = False
    if not generation['detector_before']['lighting_risk']:
        return pixels
    candidate = candidates.get(generation['prepared_sha256'])
    if candidate is None:
        raise ValueError(f'{variant}/{name}: no IID output for exact prepared SHA')
    source, item, reused = candidate
    relative = Path(NAMESPACE) / 'marigold' / (variant + '-' + source.name)
    _write_immutable(args.source_root / relative, source.read_bytes())
    with Image.open(source) as output:
        iid = np.asarray(output.convert('RGB'), dtype=np.float64) / 255
    pixels, correction = low_frequency_iid(pixels.astype(np.float64) / 255, iid)
    record['models'].append(dict(id=marigold_iid.MODEL_ID,
        revision=marigold_iid.MODEL_REVISION, license=marigold_iid.MODEL_LICENSE))
    record['parameters'].update(iid_model=marigold_iid.MODEL_ID,
        iid_revision=marigold_iid.MODEL_REVISION, iid_gaussian_sigma_px=32, iid_power=.5)
    generation.update(marigold_applied=True, iid_reused_iteration2=reused,
        iid_source=dict(source_png=relative.as_posix(), source_sha256=item['output_sha256']),
        iid_seconds=item['seconds'], iid_low_frequency=correction)
    return pixels


def contact_sheet(records, args):
    old = {r['replaces'][0]: r for r in bakeoff.read_registry(args.iteration2_registry)}
    current = {(r['parameters']['set'], r['replaces'][0]): r for r in records}
    side, header = 400, 28
    sheet = Image.new('RGB', (side * 3, (side + header) * len(TEXTURES)), '#252626')
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 11)
    except OSError:
        font = ImageFont.load_default()
    city_window = next(r for r in pack_materials.select_set_records(
        bakeoff.read_registry(args.base_registry), 'city-3', args.mapping) if r['replaces'][0] == WINDOW)
    for row, name in enumerate(TEXTURES):
        paths = [args.city3 / 'sources' / city_window['files']['albedo'] if name == WINDOW
                 else args.iteration2_root / old[name]['files']['albedo']]
        paths += [args.derived_root / current[(variant, name)]['files']['albedo'] for variant in SETS]
        for column, (label, path) in enumerate(zip(('iteration-2', *SETS), paths)):
            with Image.open(path) as opened:
                image = opened.convert('RGB').resize((side, side), Image.Resampling.LANCZOS)
            x, y = column * side, row * (side + header)
            sheet.paste(image, (x, y + header))
            draw.text((x + 5, y + 10), f'{label} {Path(name).stem}', fill='white', font=font)
    for quality in range(90, 39, -5):
        encoded = BytesIO()
        sheet.save(encoded, format='JPEG', quality=quality, optimize=True)
        if encoded.tell() <= 1_000_000:
            (args.report_dir / 'materials-2-3a-3b.jpg').write_bytes(encoded.getvalue())
            return
    raise ValueError('material sheet exceeds 1 MB')


def build(args, records):
    from material_detail import fit_material_detail
    counts = json.loads(args.visual_counts.read_text())
    candidates = iid_candidates(args)
    rows = []
    for record in records:
        variant, name = record['parameters']['set'], record['replaces'][0]
        generation = record['generation']
        if name == WINDOW:
            observed = counts[variant][name]
            errors = [abs(observed[axis]-1) for axis in ('u', 'v')]
            visual = dict(target_counts=json.dumps(dict(u=1, v=1)),
                visual_counts=json.dumps(observed, sort_keys=True, ensure_ascii=False),
                visual_max_relative_error=max(errors), visual_count_pass=max(errors) <= .2)
            if variant == 'pilot-3b' and observed['parts'] != record['parameters']['parts'].split('; '):
                raise ValueError('pilot-3b/window: observed parts differ from CSV')
        else:
            visual = iteration2.visual_measurement(name, counts[variant][name])
        if not visual['visual_count_pass']:
            raise ValueError(f'{variant}/{name}: visual count outside 20%')
        pixels = apply_iid(record, args, candidates)
        after_iid = detect_baked_lighting(Image.fromarray(pixels))
        generation['detector_after_iid'] = dict(after_iid, lighting_risk=lighting_risk(after_iid))
        fitted, fit = fit_material_detail(pixels.astype(float) / 255,
            record['parameters']['detail_targets'], target_oklab=record['parameters']['target_oklab'])
        if name == WINDOW and fit['clipped_pixel_fraction'] > .01:
            raise ValueError(f'{variant}/{name}: clipped pixels exceed 1%')
        generation.update(detail_fit=fit, colour_correction=fit['colour'],
            visual_inspection=dict(observed=counts[variant][name], **visual))
        record['_maps'] = bakeoff.filtered_maps(Image.fromarray(fitted), record)
        record['_maps']['albedo'] = fitted.astype(np.float32) / 255
        rlo, rhi = record['parameters']['roughness_range']
        record['_maps']['roughness'] = np.clip(record['_maps']['roughness'], np.ceil(rlo*255)/255, np.floor(rhi*255)/255)
        pilot.save_maps(record, args.derived_root)
        albedo = args.derived_root / record['files']['albedo']
        generation['detector_after'] = measure_light(albedo)
        measured = measure_material(albedo)
        colour_error = fit['colour']['linear_rgb_max_error']
        if colour_error > .005:
            raise ValueError(f'{variant}/{name}: colour error {colour_error} exceeds .005')
        rows.append(dict(variant=variant, texture=name,
            **{'target_' + key: record['parameters']['detail_targets'][key] for key in METRICS},
            **{key: measured[key] for key in METRICS},
            gains=json.dumps(fit['gains'], sort_keys=True), fit=json.dumps(fit, sort_keys=True),
            shoulder_pixel_fraction=fit['shoulder_pixel_fraction'],
            colour_max_error=colour_error, colour_pass=colour_error <= .005, **visual,
            detector_before=generation['detector_before']['lighting_risk'],
            detector_after_iid=generation['detector_after_iid']['lighting_risk'],
            detector_after=generation['detector_after']['lighting_risk'],
            marigold_applied=generation['marigold_applied'],
            iid_reused_iteration2=generation.get('iid_reused_iteration2', False),
            albedo_sha256=pilot.file_digest(albedo)))
        registry_module.validate_entry(record, content_root=args.derived_root, source_root=args.source_root)
        print(json.dumps(rows[-1], sort_keys=True), flush=True)
    invariants = [pilot.clone_and_pack([r for r in records if r['parameters']['set'] == variant],
        args, variant.removeprefix('pilot-')) for variant in SETS]
    args.registry_output.parent.mkdir(parents=True, exist_ok=True)
    args.registry_output.write_text(''.join(json.dumps(r, ensure_ascii=False, sort_keys=True) + '\n' for r in records))
    args.report_dir.mkdir(parents=True, exist_ok=True)
    pilot.write_csv(args.report_dir / 'material-metrics.csv', rows)
    contact_sheet(records, args)
    return dict(staging=invariants, materials=len(rows), colour_pass=sum(r['colour_pass'] for r in rows),
        visual_pass=sum(r['visual_count_pass'] for r in rows))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'build'))
    base = pilot.BASE / NAMESPACE
    parser.add_argument('--input-dir', type=Path, default=registry_module.DEFAULT_SOURCE_ROOT / NAMESPACE)
    parser.add_argument('--selection', type=Path, default=registry_module.DEFAULT_SOURCE_ROOT / 'quality-pilot-3-selection.json')
    parser.add_argument('--source-root', type=Path, default=registry_module.DEFAULT_SOURCE_ROOT)
    parser.add_argument('--prepared-dir', type=Path, default=base / 'prepared')
    parser.add_argument('--visual-counts', type=Path, default=base / 'visual-counts.json')
    parser.add_argument('--city3', type=Path, default=pilot.BASE / 'content-staging/city-3')
    parser.add_argument('--staging-parent', type=Path, default=pilot.BASE / 'content-staging')
    parser.add_argument('--derived-root', type=Path, default=pilot.BASE / 'content-staging/quality-pilot-3-sources')
    parser.add_argument('--marigold-dir', type=Path, default=base / 'marigold-output')
    parser.add_argument('--marigold-log', type=Path, default=base / 'marigold.jsonl')
    parser.add_argument('--iteration2-marigold-dir', type=Path, default=pilot.BASE / 'quality-pilot-2/marigold-output')
    parser.add_argument('--iteration2-marigold-log', type=Path, default=pilot.BASE / 'quality-pilot-2/marigold.jsonl')
    parser.add_argument('--iteration2-registry', type=Path, default=pilot.ROOT / 'data/content/quality-pilot-2.jsonl')
    parser.add_argument('--iteration2-root', type=Path, default=pilot.BASE / 'content-staging/quality-pilot-2-sources')
    parser.add_argument('--base-registry', type=Path, default=registry_module.DEFAULT_REGISTRY)
    parser.add_argument('--registry-output', type=Path, default=pilot.ROOT / 'data/content/quality-pilot-3.jsonl')
    parser.add_argument('--mapping', type=Path, default=pilot.ROOT / 'data/content/city-materials.csv')
    parser.add_argument('--report-dir', type=Path, default=pilot.ROOT / 'reports/content/2026-10-quality-pilot-3')
    args = parser.parse_args(argv)
    try:
        validate_outputs(args)
        records = prepare(args)
        for record in records:
            print(json.dumps(dict(variant=record['parameters']['set'], texture=record['replaces'][0],
                prepared_sha256=record['generation']['prepared_sha256'],
                lighting_risk=record['generation']['detector_before']['lighting_risk']), sort_keys=True), flush=True)
        if args.command == 'build':
            print(json.dumps(build(args, records), sort_keys=True), flush=True)
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        print(f'quality_pilot_3: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
