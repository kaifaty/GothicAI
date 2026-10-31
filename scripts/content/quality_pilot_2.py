#!/usr/bin/env python3
"""Prepare and stage seven text-only pilot materials with gated, half-strength IID.

prepare writes colour-corrected PNGs and copies only detector-flagged inputs to
prepared/flagged for the separate dev-ubuntu Marigold run. build verifies those
exact bytes, blends flagged outputs 50/50 in linear RGB, corrects colour again,
and writes one isolated registry/staging set plus final comparison sheets.
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
import city_set
import generate
from import_imagegen import MODEL, _write_immutable
from material_colour import _linear, _srgb
from material_lighting import measure_file as measure_light
from material_measures import measure_material
import marigold_iid
import pack_materials
import quality_pilot as pilot
import registry as registry_module
from seam_fix import seamless


SET = 'pilot-2'
NAMESPACE = 'quality-pilot-2'
# Counts are per original UV unit; a full-width board has one U segment.
TARGET_COUNTS = dict(zip(pilot.TEXTURES, (
    dict(u=14, v=10), dict(u=16, v=16), dict(u=1, v=5),
    dict(u=3, v=20), dict(u=1, v=1),
    dict(u=2.5, v=6), dict(u=5, v=6))))
PHYSICAL_METRES = dict(zip(pilot.TEXTURES, (
    (13.435398081906335, 6.787173953337865),
    (6.000002610045845, 6.000000333024118),
    (6.5936656579303055, 3.10605552903998),
    (14.48, 6.42), (2., 2.06), (3.4, 2.56),
    (8.001205491327342, 7.997257527846163))))


def validate_outputs(args):
    base = args.city3.expanduser().resolve()
    outputs = [args.prepared_dir, args.source_root / NAMESPACE,
        args.derived_root, args.report_dir, args.registry_output]
    if args.command == 'build':
        outputs.append(args.staging_parent / SET)
    for path in outputs:
        output = pack_materials.staging_output(path)
        if output == base or output.is_relative_to(base) or base.is_relative_to(output):
            raise ValueError(f'output must not overlap city-3: {path}')
    if args.registry_output.resolve() in (args.base_registry.resolve(), args.iteration1_registry.resolve()):
        raise ValueError('pilot-2 registry must not replace an existing registry')


def prepare(args, names):
    attempts = pilot.read_sources(args.input_dir, names, args.source_root, namespace=NAMESPACE)
    selected = pilot.selections(args.select, names)
    recipes = {r['replaces'][0]: r for r in city_set.recipes(args.mapping, args.bible, textures=names)}
    args.prepared_dir.mkdir(parents=True, exist_ok=True)
    flagged_dir = args.prepared_dir / 'flagged'
    flagged_dir.mkdir(exist_ok=True)
    records = []
    for name in names:
        number = selected.get(name, attempts[name][-1]['attempt'])
        chosen = next((a for a in attempts[name] if a['attempt'] == number), None)
        if chosen is None:
            raise ValueError(f'{name}: selected attempt {number} does not exist')
        record = deepcopy(recipes[name])
        resource_id = SET + '-' + Path(name).stem.lower().replace('_', '-')
        record.update(id=resource_id, prompt=chosen['prompt'], pipeline='imagegen-filter',
            models=[deepcopy(MODEL)], seed=None)
        record['parameters'].update(set=SET, material=resource_id, scale=1.,
            detail_count_target=TARGET_COUNTS[name], source_attempt=number,
            physical_uv_metres=list(PHYSICAL_METRES[name]),
            source_aspect=PHYSICAL_METRES[name][0] / PHYSICAL_METRES[name][1],
            detail_centimetres={axis: PHYSICAL_METRES[name][index] * 100 / count
                for index, (axis, count) in enumerate(TARGET_COUNTS[name].items())},
            scale_policy='fixed one UV unit; visual counts govern admission; spectrum is reference only',
            source_size_policy='physical canvas first; Lanczos to square UV storage; narrow seam quilting on permitted repeat axes')
        with Image.open(args.source_root / chosen['source_png']) as opened:
            image = opened.convert('RGB')
        geometry = None
        if name == pilot.ROOF_INSIDE:
            image, geometry = pilot.roof_canvas(image)
        image = image.resize((1024, 1024), Image.Resampling.LANCZOS)
        axes = '' if record['parameters']['layout'] == 'panel' else record['parameters']['tiling_axes']
        image, seam = seamless(image, axes=axes, overlap_fraction=1/64)
        maps, correction, rejection = generate.target_corrected_maps(image, record)
        target = args.prepared_dir / (Path(name).stem + '.png')
        corrected = Image.fromarray(np.rint(maps['albedo'] * 255).astype(np.uint8))
        digest = pilot.prepared_png(corrected, target, frozen=args.command == 'build')
        lighting = measure_light(target)
        flagged = flagged_dir / target.name
        if lighting['lighting_risk']:
            if args.command == 'build':
                if not flagged.is_file() or pilot.file_digest(flagged) != digest:
                    raise ValueError(f'{name}: flagged IID input differs from prepared colour-corrected PNG')
            else:
                shutil.copyfile(target, flagged)
        elif args.command == 'prepare':
            flagged.unlink(missing_ok=True)
        record['generation'] = dict(outcome='complete', attempts=attempts[name],
            selected_attempt=number, attempt_limit=2, colour_correction=correction,
            numeric_rejection=rejection, geometry=geometry, seam=seam,
            prepared_sha256=digest, detector_before=lighting,
            source_policy='text-only image_gen; original generator PNG bytes retained and checksummed',
            model_identity='image_gen.imagegen; underlying model/version not exposed; no seed exposed')
        record['_maps'] = maps
        records.append(record)
    return records


def blend_linear_half(corrected, iid):
    """Half-strength IID in radiometric colour space, encoded back to uint8 sRGB."""
    if corrected.size != iid.size:
        raise ValueError('IID output must match the prepared image dimensions')
    before = np.asarray(corrected.convert('RGB'), dtype=np.float64) / 255
    after = np.asarray(iid.convert('RGB'), dtype=np.float64) / 255
    linear = (_linear(before) + _linear(after)) / 2
    return Image.fromarray(np.rint(np.clip(_srgb(linear), 0, 1) * 255).astype(np.uint8))


def apply_iid(record, args, iid_records):
    """The detector gate is evaluated on the stored colour-corrected PNG."""
    name = record['replaces'][0]
    generation = record['generation']
    generation['marigold_applied'] = False
    if not generation['detector_before']['lighting_risk']:
        return
    if args.marigold_dir is None or args.marigold_log is None:
        raise ValueError(f'{name}: flagged input requires --marigold-dir and --marigold-log')
    source = args.marigold_dir / (Path(name).stem + '-albedo.png')
    iid = iid_records.get(Path(name).stem, {})
    raw = source.read_bytes()
    output_digest = pilot.file_digest(source)
    if (iid.get('source_sha256') != generation['prepared_sha256'] or
            iid.get('output_sha256') != output_digest):
        raise ValueError(f'{name}: IID log does not match prepared input and output PNG bytes')
    relative = Path(NAMESPACE) / 'marigold' / source.name
    _write_immutable(args.source_root / relative, raw)
    with Image.open(args.prepared_dir / (Path(name).stem + '.png')) as corrected:
        with Image.open(BytesIO(raw)) as output:
            blended = blend_linear_half(corrected, output)
    maps, correction, rejection = generate.target_corrected_maps(blended, record)
    record['_maps'] = maps
    record['models'].append(dict(id=marigold_iid.MODEL_ID,
        revision=marigold_iid.MODEL_REVISION, license=marigold_iid.MODEL_LICENSE))
    record['parameters'].update(iid_model=marigold_iid.MODEL_ID,
        iid_revision=marigold_iid.MODEL_REVISION, iid_linear_weight=.5)
    generation.update(marigold_applied=True, colour_correction_after_iid=correction,
        numeric_rejection_after_iid=rejection,
        iid_source=dict(source_png=relative.as_posix(), source_sha256=output_digest),
        iid_seconds=iid.get('seconds'))


def visual_measurement(name, observed):
    targets = TARGET_COUNTS[name]
    errors = {}
    for axis, target in targets.items():
        if target is None:
            continue
        value = observed.get(axis)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not np.isfinite(value) or value <= 0:
            raise ValueError(f'{name}: visual count {axis} must be a positive finite number')
        errors[axis] = value / target - 1
    return dict(target_counts=json.dumps(targets, sort_keys=True),
        visual_counts=json.dumps(observed, sort_keys=True, ensure_ascii=False),
        visual_max_relative_error=max(abs(error) for error in errors.values()),
        visual_count_pass=all(abs(error) <= .2 + 1e-9 for error in errors.values()))


def contact_sheet(names, images, target, *, repeat=False):
    side, header = 384, 34
    sheet = Image.new('RGB', (side * 2, (side + header) * len(names)), '#252626')
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 12)
    except OSError:
        font = ImageFont.load_default()
    for row, name in enumerate(names):
        for column, label in enumerate(('iteration-1-B', 'iteration-2')):
            with Image.open(images[(name, label)]) as opened:
                image = opened.convert('RGB')
            tile = image.resize((side // 2 if repeat else side,) * 2, Image.Resampling.LANCZOS)
            for y in range(2 if repeat else 1):
                for x in range(2 if repeat else 1):
                    sheet.paste(tile, (column * side + x * tile.width,
                        row * (side + header) + header + y * tile.height))
            draw.text((column * side + 4, row * (side + header) + 8),
                f'{label} {Path(name).stem}', fill='white', font=font)
    sheet.save(target, quality=94)


def build(args, records):
    if args.visual_counts is None:
        raise ValueError('build requires --visual-counts JSON from inspection of our generated images')
    counts = json.loads(args.visual_counts.read_text())
    iid_records = {}
    if args.marigold_log:
        for line in args.marigold_log.read_text().splitlines():
            if line.strip():
                item = json.loads(line)
                if 'input' in item:
                    key = Path(item['input']).stem
                    if key in iid_records:
                        raise ValueError(f'duplicate IID log input: {key}')
                    iid_records[key] = item
    # Check supplied observations before writing the final registry or staging.
    visual = {r['replaces'][0]: visual_measurement(r['replaces'][0], counts[r['replaces'][0]]) for r in records}
    for record in records:
        apply_iid(record, args, iid_records)
        name = record['replaces'][0]
        generation = record['generation']
        generation['visual_inspection'] = dict(observed=counts[name], **visual[name])
        if not visual[name]['visual_count_pass'] and generation['selected_attempt'] != 2:
            raise ValueError(f'{name}: visual detail count differs by more than 20%; second attempt required')
    metrics = []
    for record in records:
        name = record['replaces'][0]
        generation = record['generation']
        pilot.save_maps(record, args.derived_root)
        albedo = args.derived_root / record['files']['albedo']
        if not generation['marigold_applied']:
            # Preserve PNG bytes too, rather than merely equivalent pixels.
            shutil.copyfile(args.prepared_dir / (Path(name).stem + '.png'), albedo)
        after = measure_light(albedo)
        generation['detector_after'] = after
        final_correction = generation.get('colour_correction_after_iid', generation['colour_correction'])
        material = measure_material(albedo)
        metrics.append(dict(texture=name, scale=1, selected_attempt=generation['selected_attempt'],
            calls=len(generation['attempts']), **visual[name],
            physical_u_metres=PHYSICAL_METRES[name][0], physical_v_metres=PHYSICAL_METRES[name][1],
            target_detail_u_cm=record['parameters']['detail_centimetres']['u'],
            target_detail_v_cm=record['parameters']['detail_centimetres']['v'],
            spectrum_u_period_uv=material['period_u_uv'], spectrum_v_period_uv=material['period_v_uv'],
            spectrum_u_count=1 / material['period_u_uv'] if material['period_u_uv'] else None,
            spectrum_v_count=1 / material['period_v_uv'] if material['period_v_uv'] else None,
            spectrum_u_confidence=material['period_u_confidence'], spectrum_v_confidence=material['period_v_confidence'],
            spectrum_direction=material['direction'],
            colour_max_error=final_correction['linear_rgb_max_error'],
            colour_pass=final_correction['linear_rgb_max_error'] <= .005,
            marigold_applied=generation['marigold_applied'],
            **{'detector_before_' + key: value for key, value in generation['detector_before'].items()},
            **{'detector_after_' + key: value for key, value in after.items()},
            albedo_sha256=pilot.file_digest(albedo)))
        registry_module.validate_entry(record, content_root=args.derived_root, source_root=args.source_root)
    args.registry_output.parent.mkdir(parents=True, exist_ok=True)
    args.registry_output.write_text(''.join(json.dumps(record, ensure_ascii=False, sort_keys=True) + '\n' for record in records))
    invariants = pilot.clone_and_pack(records, args, '2')
    old = {r['replaces'][0]: r for r in bakeoff.read_registry(args.iteration1_registry)
        if r.get('parameters', {}).get('set') == 'pilot-b'}
    images = {}
    for record in records:
        name = record['replaces'][0]
        images[(name, 'iteration-1-B')] = args.iteration1_root / old[name]['files']['albedo']
        images[(name, 'iteration-2')] = args.derived_root / record['files']['albedo']
    args.report_dir.mkdir(parents=True, exist_ok=True)
    pilot.write_csv(args.report_dir / 'material-metrics.csv', metrics)
    contact_sheet(pilot.TEXTURES, images, args.report_dir / 'materials-b-v2.jpg')
    contact_sheet(pilot.TEXTURES, images, args.report_dir / 'materials-b-v2-repeat-2x2.jpg', repeat=True)
    return dict(staging=invariants, registry=str(args.registry_output),
        calls=sum(row['calls'] for row in metrics), materials=len(metrics),
        visual_pass=sum(row['visual_count_pass'] for row in metrics),
        colour_pass=sum(row['colour_pass'] for row in metrics),
        marigold_applied=sum(row['marigold_applied'] for row in metrics))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'build'))
    parser.add_argument('--input-dir', type=Path, default=registry_module.DEFAULT_SOURCE_ROOT / NAMESPACE)
    parser.add_argument('--prepared-dir', type=Path, default=pilot.BASE / NAMESPACE / 'prepared')
    parser.add_argument('--source-root', type=Path, default=registry_module.DEFAULT_SOURCE_ROOT)
    parser.add_argument('--city3', type=Path, default=pilot.BASE / 'content-staging/city-3')
    parser.add_argument('--staging-parent', type=Path, default=pilot.BASE / 'content-staging')
    parser.add_argument('--derived-root', type=Path, default=pilot.BASE / 'content-staging/quality-pilot-2-sources')
    parser.add_argument('--marigold-dir', type=Path)
    parser.add_argument('--marigold-log', type=Path)
    parser.add_argument('--visual-counts', type=Path)
    parser.add_argument('--mapping', type=Path, default=pilot.ROOT / 'data/content/city-materials.csv')
    parser.add_argument('--bible', type=Path, default=pilot.ROOT / 'docs/content-style.md')
    parser.add_argument('--base-registry', type=Path, default=registry_module.DEFAULT_REGISTRY)
    parser.add_argument('--registry-output', type=Path, default=pilot.ROOT / 'data/content/quality-pilot-2.jsonl')
    parser.add_argument('--iteration1-registry', type=Path, default=pilot.ROOT / 'data/content/quality-pilot.jsonl')
    parser.add_argument('--iteration1-root', type=Path, default=pilot.BASE / 'content-staging/quality-pilot-sources')
    parser.add_argument('--report-dir', type=Path, default=pilot.ROOT / 'reports/content/2026-10-quality-pilot-2')
    parser.add_argument('--texture', action='append', default=[])
    parser.add_argument('--select', action='append', default=[])
    args = parser.parse_args(argv)
    try:
        validate_outputs(args)
        names = list(pilot.TEXTURES)
        if args.texture:
            chosen = {pilot.texture_name(name) for name in args.texture}
            if args.command != 'prepare' or chosen - set(names):
                raise ValueError('--texture is only valid for known pilot prepare materials')
            names = [name for name in names if name in chosen]
        records = prepare(args, names)
        for record in records:
            print(json.dumps(dict(texture=record['replaces'][0],
                selected_attempt=record['generation']['selected_attempt'],
                colour_max_error=record['generation']['colour_correction']['linear_rgb_max_error'],
                **record['generation']['detector_before']), sort_keys=True), flush=True)
        if args.command == 'build':
            print(json.dumps(build(args, records), sort_keys=True), flush=True)
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        print(f'quality_pilot_2: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
