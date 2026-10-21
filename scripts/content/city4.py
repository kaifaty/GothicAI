#!/usr/bin/env python3
"""Process all 65 city materials from retained text-only image_gen sources.

``prompt`` prints CSV-only prompts. ``prepare`` freezes corrected IID inputs;
``build`` verifies these bytes, applies low-frequency IID where flagged, fits
detail under the four measured CSV ceilings, and merges the city-4 registry.
The seven pilot-2 materials reuse their selected raw PNGs. Packaging is separate.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
import csv
import hashlib
from io import BytesIO
import json
import math
from pathlib import Path
import re
import shutil
import sys

import numpy as np
from PIL import Image

import alpha_material
import bakeoff
import city_set
import generate
from import_imagegen import MODEL, _write_immutable
from material_colour import _srgb, colour_target, oklab_to_linear
from material_detail import FIELDS, fit_material_detail, low_frequency_iid
from material_lighting import detect_baked_lighting, lighting_risk, measure_file as measure_light
from material_measures import measure_material
import marigold_iid
import pack_materials
import quality_pilot as pilot
import registry
from seam_fix import seamless

ROOT = Path(__file__).resolve().parents[2]
BASE = Path.home() / '.local/share/gothic-ai'
SET = 'city-4'
SOURCE_NAME = re.compile(r'(NW_(?:CITY|HARBOUR|MISC)_[A-Z0-9_]+)-attempt-([12])\Z')
GATE_WALLS = frozenset('NW_MISC_STRONG_STONEWALL' + suffix + '.TGA'
    for suffix in ('_01', '_SIDE_01', '_ENTRANCE_01', '_BLOCKBORDER_01', '_ROUNDBORDER_01'))


def rows(mapping):
    with Path(mapping).open(newline='', encoding='utf-8') as stream:
        selected = [row for row in csv.DictReader(stream)
            if row['decision'] in ('include', 'module', 'panel', 'strip')
            and row['uv_m_u'] and row['uv_m_v']]
    if len(selected) != 65 or len({row['texture'] for row in selected}) != 65:
        raise ValueError('city-4 requires exactly 65 distinct measured city materials')
    return selected


def number(row, key):
    if not row[key]:
        return None
    value = float(row[key])
    if not math.isfinite(value) or value <= 0:
        raise ValueError(f'{row["texture"]}: {key} must be positive and finite')
    return value


def repeat_axes(row):
    if row['layout'] != 'strip':
        return 'uv' if row['layout'] == 'tiled' else ''
    if row['texture'] in ('NW_MISC_STRONG_STONEWALL_BLOCKBORDER_01.TGA',
            'NW_MISC_STRONG_STONEWALL_ROUNDBORDER_01.TGA'):
        return 'u'
    return city_set.STRIP_AXES[row['texture']]


def recipe(row, classes, mapping, bible):
    name = row['texture']
    substrate = city_set.SUBSTRATES.get(row['class'], row['class'])
    if name == 'NW_CITY_HAFEN_HOLZWAND_02.TGA':
        substrate = 'planks_wall'
    style = classes[substrate]
    axes = repeat_axes(row)
    metres = [number(row, 'uv_m_' + axis) for axis in ('u', 'v')]
    counts = {axis: number(row, 'count_' + axis) for axis in ('u', 'v')}
    centimetres = {axis: number(row, 'element_cm_' + axis) for axis in ('u', 'v')}
    target = colour_target(row)
    if target is None:
        raise ValueError(f'{name}: target_oklab is required')
    detail_targets = {field: float(row['target_' + field]) for field in FIELDS}
    if any(not math.isfinite(value) or value < 0 for value in detail_targets.values()):
        raise ValueError(f'{name}: detail targets must be finite and nonnegative')
    highlight_target = float(row['target_highlight_y'])
    if not math.isfinite(highlight_target) or not 0 < highlight_target <= 1:
        raise ValueError(f'{name}: target_highlight_y must be in (0, 1]')
    encoded = np.rint(np.clip(_srgb(oklab_to_linear(target)), 0, 1) * 255).astype(np.uint8)
    hex_colour = '#' + ''.join(f'{value:02x}' for value in encoded)
    module = row['decision'] != 'include'
    subject = row['parts'] if module else row['depicts']
    if not subject:
        raise ValueError(f'{name}: subject/parts is required')
    dimensions = []
    for axis in ('u', 'v'):
        if counts[axis] is not None:
            if centimetres[axis] is None:
                raise ValueError(f'{name}: counted axis {axis} lacks element_cm')
            dimensions.append(f'{counts[axis]:g} details across {axis.upper()}, '
                f'each {centimetres[axis]:g} cm along {axis.upper()}, '
                f'count_direction {row["count_direction"]}')
    density = '; '.join(dimensions) or 'natural nonperiodic surface detail'
    if all(value is not None for value in counts.values()):
        density = f'{counts["u"] * counts["v"]:g} details total; ' + density
    layout = (f'scale 1; {density}; '
        f'physical U {metres[0]:g} m and V {metres[1]:g} m before square UV storage')
    if module:
        layout += f'; parts: {row["parts"]}'
    if substrate in ('cobble', 'flagstone', 'planks_floor', 'mud', 'dirt_path'):
        cause = 'foot traffic and ground moisture'
    elif row['class'] == 'foliage_panel':
        cause = 'leaf ageing and coastal exposure'
    elif row['class'] == 'cloth_panel':
        cause = 'wind and rain'
    else:
        cause = 'coastal moisture, weather exposure and handling'
    weathering = f'{cause} cause fine wear and pigment deposits: {row["weathering"]}'
    if row['layout'] == 'tiled':
        weathering += '; fine wear and pigment distributed evenly across the entire tile'
    boundary = {'uv': 'seamless on U and V', 'u': 'seamless on U; V boundaries fixed',
        'v': 'seamless on V; U boundaries fixed', '': 'complete bounded panel'}[axes]
    alpha = name in alpha_material.ALPHA_TEXTURES
    if alpha:
        boundary = (f'RGBA transparent background; opaque foreground coverage '
            f'{float(row["target_coverage"]):.1%}; hard silhouette and real transparent gaps')
    prompt = '\n'.join((
        f'Asset type: albedo material for {row["object"]}; {row["role"]}; {row["layout"]}.',
        f'Subject: {subject}.',
        'Medium: calibrated photographic material scan strictly from above, camera perpendicular, surface filling the image edge to edge.',
        'Lighting: even shadowless diffuse illumination, identical exposure from corner to corner.',
        f'Weathering: {weathering}. Material condition: {row["state"]}.',
        f'Colour and value: {row["colour"]}; average sRGB {hex_colour}.',
        f'Layout: {layout}; orientation: {row["orientation"]}.',
        f'Constraints: {boundary}; no text, decorative picture frame, loose objects or perspective.'))
    resource_id = SET + '-' + Path(name).stem.lower().replace('_', '-')
    params = dict(set=SET, material=resource_id, material_class=row['class'],
        numeric_substrate=substrate, decision=row['decision'], layout=row['layout'],
        object=row['object'], role=row['role'], depicts=row['depicts'], parts=row['parts'],
        state=row['state'], scale=1., tile_m=style['tile_m'],
        physical_uv_metres=metres, source_aspect=metres[0]/metres[1],
        detail_count_target=counts, detail_centimetres=centimetres,
        detail_targets=detail_targets, detail_ceiling=True,
        target_highlight_y=highlight_target,
        fine_gain_max=1. if name in GATE_WALLS else None,
        detail_policy='final quantized detail metrics never exceed CSV targets when feasible; bounded gains, shoulder and mean-colour correction included in fit',
        count_direction=row['count_direction'], tiling_axes=axes, tileable=bool(axes),
        antitile=city_set.antitile_mode(row), antitile_cell=1., metallic=0,
        luminance_range=style['luminance'], roughness_range=style['roughness'],
        roughness_base=sum(style['roughness'])/2, saturation_limit=style['saturation_limit'],
        target_luminance=float(row['target_luminance']), target_oklab=target,
        target_luminance_tolerance=city_set.TARGET_LUMINANCE_TOLERANCE,
        resolution=1024, output_width=1024, output_height=1024,
        input='text only; no original pixels or game screenshots',
        source_size_policy='physical canvas first; Lanczos to square UV storage; narrow seam quilting on permitted repeat axes',
        scale_policy='fixed one UV unit; visual counts govern admission',
        colour_policy='original OKLab of mean linear RGB; per-channel linear gains; no saturation cap',
        mapping_sha256=hashlib.sha256(Path(mapping).read_bytes()).hexdigest(),
        bible_sha256=hashlib.sha256(Path(bible).read_bytes()).hexdigest())
    if alpha:
        params.update(alpha_mode='test', alpha_threshold=alpha_material.ALPHA_TEST_THRESHOLD,
            target_coverage=float(row['target_coverage']),
            coverage_tolerance=alpha_material.COVERAGE_TOLERANCE,
            transparent_render='albedo only; generated normal/ORM retained but unused by the engine alpha pass')
    return dict(id=resource_id, kind='material', replaces=[name],
        pipeline='imagegen-filter', models=[deepcopy(MODEL)], prompt=prompt,
        negative='', seed=None, status='draft', files={}, parameters=params)


def recipes(mapping=ROOT/'data/content/city-materials.csv', bible=ROOT/'docs/content-style.md', textures=None):
    selected = rows(mapping)
    names = {row['texture'] for row in selected}
    chosen = names if textures is None else {pilot.texture_name(name) for name in textures}
    if not chosen or chosen - names:
        raise ValueError('requested texture is outside the 65 city-4 materials')
    classes, _ = generate.read_style(Path(bible))
    return [recipe(row, classes, mapping, bible) for row in selected if row['texture'] in chosen]


def _read_source_batch(directory, names, source_root, namespace):
    """Retain original tool PNG and metadata bytes; at most two calls per material."""
    found = {name: [] for name in names}
    for path in sorted(directory.glob('NW_*-attempt-*.json')):
        match = SOURCE_NAME.fullmatch(path.stem)
        if match is None:
            raise ValueError(f'invalid source name or third attempt: {path.name}')
        name, attempt = match[1] + '.TGA', int(match[2])
        if name not in found:
            continue
        raw_metadata = path.read_bytes()
        meta = json.loads(raw_metadata)
        if meta.get('texture') != name or meta.get('attempt') != attempt:
            raise ValueError(f'{path.name}: texture/attempt differs from filename')
        for field in ('prompt', 'utc'):
            if not isinstance(meta.get(field), str) or not meta[field].strip():
                raise ValueError(f'{path.name}: missing {field}')
        seconds = meta.get('seconds')
        if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or not math.isfinite(seconds) or seconds < 0:
            raise ValueError(f'{path.name}: invalid generation seconds')
        if meta.get('tool', MODEL['id']) != MODEL['id']:
            raise ValueError(f'{path.name}: expected built-in image_gen.imagegen provenance')
        source = path.with_suffix('.png')
        raw = source.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if meta.get('source_sha256', digest) != digest:
            raise ValueError(f'{path.name}: source_sha256 differs from PNG bytes')
        with Image.open(BytesIO(raw)) as image:
            if image.format != 'PNG':
                raise ValueError(f'{source}: expected original PNG')
            size = list(image.size)
            if name in alpha_material.ALPHA_TEXTURES:
                if image.mode != 'RGBA' or not 0 < alpha_material.coverage(np.asarray(image)[..., 3]) < 1:
                    raise ValueError(f'{name}: real RGBA foreground and transparent gaps required')
            elif 'A' in image.getbands() and image.getchannel('A').getextrema() != (255, 255):
                raise ValueError(f'{name}: opaque material has transparent pixels')
            image.load()
        relative = Path(namespace) / source.name
        _write_immutable(source_root / relative, raw)
        _write_immutable(source_root / relative.with_suffix('.json'), raw_metadata)
        found[name].append(dict(attempt=attempt, prompt=meta['prompt'], utc=meta['utc'],
            seconds=seconds, source_png=relative.as_posix(), source_sha256=digest, source_size=size))
    for name, attempts in found.items():
        if [item['attempt'] for item in attempts] != list(range(1, len(attempts)+1)) or not 1 <= len(attempts) <= 2:
            raise ValueError(f'{name}: require one or two consecutive attempts starting at 1')
    return found


def read_sources(directory, names, source_root):
    """Append at most two gate-wall retries from the fixed round-2 namespace."""
    found = _read_source_batch(directory, names, source_root, SET)
    round2 = directory / 'round-2'
    available = set()
    for path in round2.glob('NW_*-attempt-*.json'):
        match = SOURCE_NAME.fullmatch(path.stem)
        if match is None:
            raise ValueError(f'invalid round-2 source name or third attempt: {path.name}')
        name = match[1] + '.TGA'
        if name not in GATE_WALLS:
            raise ValueError(f'{name}: round-2 regeneration is limited to the five gate walls')
        if name in found:
            available.add(name)
    if available:
        replacements = _read_source_batch(round2, sorted(available), source_root, SET + '/round-2')
        for name, attempts in replacements.items():
            offset = len(found[name])
            for item in attempts:
                item.update(round=2, round_attempt=item['attempt'], attempt=offset + item['attempt'])
            found[name].extend(attempts)
    return found


def prepare(args, records):
    names = [record['replaces'][0] for record in records]
    fresh = [name for name in names if name not in pilot.TEXTURES]
    attempts = read_sources(args.input_dir, fresh, args.source_root) if fresh else {}
    reused = {}
    if any(name in pilot.TEXTURES for name in names):
        for line in args.pilot2_registry.read_text().splitlines():
            if line.strip():
                previous = json.loads(line)
                name = previous['replaces'][0]
                if name in names and name in pilot.TEXTURES:
                    if name in reused:
                        raise ValueError(f'{name}: duplicate pilot-2 source record')
                    reused[name] = previous
        for name in names:
            if name not in pilot.TEXTURES:
                continue
            previous = reused[name]
            attempts[name] = deepcopy(previous['generation']['attempts'])
            if any(not item['source_png'].startswith('quality-pilot-2/') for item in attempts[name]):
                raise ValueError(f'{name}: reused pilot source must remain in quality-pilot-2')
            registry.verify_sources(attempts[name], args.source_root)
    selected = {}
    for value in args.select:
        name, attempt = value.rsplit(':', 1)
        name = pilot.texture_name(name)
        allowed = ('1', '2', '3', '4') if name in GATE_WALLS else ('1', '2')
        if name not in names or name in selected or attempt not in allowed:
            raise ValueError(f'invalid or repeated selection {value!r}')
        selected[name] = int(attempt)
    previous_city = {}
    if getattr(args, 'registry_output', None) and args.registry_output.exists():
        previous_city = {record['replaces'][0]: record for record in
            registry.check_registry(args.registry_output, source_root=args.source_root)}
    args.prepared_dir.mkdir(parents=True, exist_ok=True)
    flagged_dir = args.prepared_dir / 'flagged'
    flagged_dir.mkdir(exist_ok=True)
    # This folder is an explicit batch, so a subsequent IID command cannot
    # accidentally rerun detector inputs from an earlier batch.
    if args.command == 'prepare':
        for stale in flagged_dir.glob('*.png'):
            stale.unlink()
    for record in records:
        name = record['replaces'][0]
        previous = reused.get(name)
        retained = previous['generation']['selected_attempt'] if previous else attempts[name][-1]['attempt']
        number = selected.get(name, retained)
        if previous and number != retained:
            raise ValueError(f'{name}: reuse requires the selected pilot-2 raw attempt {retained}')
        chosen = next((item for item in attempts[name] if item['attempt'] == number), None)
        if chosen is None:
            raise ValueError(f'{name}: selected attempt {number} does not exist')
        record['prompt'] = chosen['prompt']
        record['parameters']['source_attempt'] = number
        alpha = name in alpha_material.ALPHA_TEXTURES
        geometry = None
        with Image.open(args.source_root / chosen['source_png']) as opened:
            if alpha:
                image = alpha_material.resize_rgba(np.asarray(opened.convert('RGBA')))
            else:
                image = opened.convert('RGB')
                if name == pilot.ROOF_INSIDE:
                    image, geometry = pilot.roof_canvas(image)
                image = image.resize((1024, 1024), Image.Resampling.LANCZOS)
        if alpha:
            maps, correction, rejection = alpha_material.corrected_maps(image, record)
            seam = dict(axes='', applied=False, reason='bounded alpha panel; silhouette retained')
        else:
            image, seam = seamless(image, axes=record['parameters']['tiling_axes'], overlap_fraction=1/64)
            maps, correction, rejection = generate.target_corrected_maps(image, record)
        target = args.prepared_dir / (Path(name).stem + '.png')
        corrected = Image.fromarray(np.rint(maps['albedo'] * 255).astype(np.uint8))
        digest = pilot.prepared_png(corrected, target, frozen=args.command == 'build')
        lighting = dict(lighting_risk=False, applicable=False, reason='alpha cutout; IID excluded') if alpha else measure_light(target)
        flagged = flagged_dir / target.name
        if lighting['lighting_risk']:
            if args.command == 'build':
                if not flagged.is_file() or pilot.file_digest(flagged) != digest:
                    raise ValueError(f'{name}: flagged IID input differs from prepared PNG')
            else:
                shutil.copyfile(target, flagged)
        record['generation'] = dict(outcome='complete', attempts=attempts[name],
            selected_attempt=number, attempt_limit=2, colour_correction=correction,
            attempt_limit_scope='per generation round',
            numeric_rejection=rejection, geometry=geometry, seam=seam, prepared_sha256=digest,
            detector_before=lighting,
            source_policy='text-only image_gen; original generator PNG and metadata bytes retained and checksummed',
            model_identity='image_gen.imagegen; underlying model/version not exposed; no seed exposed')
        old_generation = previous_city.get(name, {}).get('generation', {})
        retained_iid = deepcopy(old_generation.get('retained_iid_sources', []))
        old_iid = old_generation.get('iid_source')
        if old_iid and old_iid not in retained_iid:
            retained_iid.append(deepcopy(old_iid))
        if retained_iid:
            record['generation']['retained_iid_sources'] = retained_iid
        if previous:
            record['models'] = [deepcopy(previous['models'][0])]
            record['generation'].update(
                source_policy='selected pilot-2 raw image_gen PNG reused without regeneration; immutable quality-pilot-2 references and generation attempts retained',
                source_registry_id=previous['id'], source_set='pilot-2',
                model_identity=previous['generation']['model_identity'])
        record['_maps'] = maps
    return records


def visual_measurement(record, observed):
    name, targets = record['replaces'][0], record['parameters']['detail_count_target']
    errors = {}
    for axis, target in targets.items():
        if target is None:
            continue
        value = observed.get(axis)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise ValueError(f'{name}: visual count {axis} must be positive and finite')
        errors[axis] = value / target - 1
    if not errors and not observed.get('note', '').strip():
        raise ValueError(f'{name}: nonperiodic material requires a visual inspection note')
    return dict(observed=observed, relative_errors=errors,
        visual_max_relative_error=max((abs(error) for error in errors.values()), default=0),
        visual_count_pass=all(abs(error) <= .2 + 1e-9 for error in errors.values()))


def apply_iid(record, args, iid_records):
    """Return prepared RGB with only the gated low-frequency IID correction."""
    name, generation = record['replaces'][0], record['generation']
    generation['marigold_applied'] = False
    flagged = generation['detector_before']['lighting_risk']
    if flagged and name in alpha_material.ALPHA_TEXTURES:
        raise ValueError(f'{name}: Marigold is excluded for alpha materials')
    prepared = args.prepared_dir / (Path(name).stem + '.png')
    if pilot.file_digest(prepared) != generation['prepared_sha256']:
        raise ValueError(f'{name}: prepared PNG no longer matches its frozen SHA')
    with Image.open(prepared) as corrected:
        pixels = np.asarray(corrected.convert('RGB')).copy()
    if not flagged:
        return pixels
    if args.marigold_dir is None or args.marigold_log is None:
        raise ValueError(f'{name}: flagged input requires --marigold-dir and --marigold-log')
    source = args.marigold_dir / (Path(name).stem + '-albedo.png')
    iid = iid_records.get(Path(name).stem, {})
    raw = source.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if iid.get('source_sha256') != generation['prepared_sha256'] or iid.get('output_sha256') != digest:
        raise ValueError(f'{name}: IID log does not match prepared input and output PNG bytes')
    relative = Path(SET) / 'marigold' / generation['prepared_sha256'] / source.name
    _write_immutable(args.source_root / relative, raw)
    with Image.open(BytesIO(raw)) as output:
        iid_pixels = np.asarray(output.convert('RGB'), dtype=np.float64) / 255
    pixels, correction = low_frequency_iid(pixels.astype(np.float64) / 255, iid_pixels)
    record['models'].append(dict(id=marigold_iid.MODEL_ID,
        revision=marigold_iid.MODEL_REVISION, license=marigold_iid.MODEL_LICENSE))
    record['parameters'].update(iid_model=marigold_iid.MODEL_ID,
        iid_revision=marigold_iid.MODEL_REVISION, iid_gaussian_sigma_px=32, iid_power=.5)
    generation.update(marigold_applied=True, iid_low_frequency=correction,
        iid_source=dict(source_png=relative.as_posix(), source_sha256=digest),
        iid_seconds=iid.get('seconds'))
    return pixels


def fit_detail(record, pixels):
    """Fit final pixels once, preserving alpha and recording actual measurements."""
    params, generation = record['parameters'], record['generation']
    alpha = record['replaces'][0] in alpha_material.ALPHA_TEXTURES
    alpha_bytes = (np.rint(record['_maps']['albedo'][..., 3] * 255).astype(np.uint8)
        if alpha else None)
    mask = alpha_material.visible_mask(alpha_bytes) if alpha else None
    before = np.dstack((pixels, alpha_bytes)) if alpha else pixels
    generation['detail_measures_before'] = measure_material(before)
    if alpha:
        generation['detector_after_iid'] = dict(lighting_risk=False, applicable=False,
            reason='alpha cutout; IID excluded')
    else:
        lighting = detect_baked_lighting(Image.fromarray(pixels))
        generation['detector_after_iid'] = dict(lighting, lighting_risk=lighting_risk(lighting))
    fitted, fit = fit_material_detail(pixels.astype(np.float64) / 255,
        params['detail_targets'], target_oklab=params['target_oklab'], ceiling=True, mask=mask,
        target_highlight_y=params['target_highlight_y'], fine_gain_max=params['fine_gain_max'])
    if not fit['highlight']['feasible']:
        raise ValueError(f'{record["replaces"][0]}: highlight ceiling is infeasible: '
            f'{fit["highlight"]["infeasibility"]}')
    output = (alpha_material.harden_and_dilate(np.dstack((fitted, alpha_bytes)))
        if alpha else fitted)
    maps = bakeoff.filtered_maps(Image.fromarray(output[..., :3]), record)
    maps['albedo'] = output.astype(np.float32) / 255
    rlo, rhi = params['roughness_range']
    maps['roughness'] = np.clip(maps['roughness'], np.ceil(rlo * 255) / 255, np.floor(rhi * 255) / 255)
    record['_maps'] = maps
    generation.update(detail_fit=fit, detail_measures_after=measure_material(output),
        colour_correction_before_fit=generation['colour_correction'],
        colour_correction=fit['colour'], numeric_rejection_after_fit=generate.numeric_rejection(record, fit['colour']))


def build(args, records):
    if args.visual_counts is None:
        raise ValueError('build requires --visual-counts JSON from inspection of generated images')
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
    for record in records:
        name, generation = record['replaces'][0], record['generation']
        visual = visual_measurement(record, counts[name])
        selected = next((attempt for attempt in generation.get('attempts', [])
            if attempt['attempt'] == generation['selected_attempt']), {})
        local_attempt = selected.get('round_attempt', generation['selected_attempt'])
        if (not visual['visual_count_pass'] and local_attempt != 2
                and name not in pilot.TEXTURES):
            raise ValueError(f'{name}: visual count differs by more than 20%; second attempt required')
        generation['visual_inspection'] = visual
        defects = [] if visual['visual_count_pass'] else ['visual detail count outside ±20% in selected retained source']
        pixels = apply_iid(record, args, iid_records)
        fit_detail(record, pixels)
        rejection = generation['numeric_rejection_after_fit']
        if rejection:
            defects.append(rejection)
        if name in alpha_material.ALPHA_TEXTURES:
            coverage = alpha_material.coverage(record['_maps']['albedo'][..., 3])
            if abs(coverage - record['parameters']['target_coverage']) > record['parameters']['coverage_tolerance'] + 1e-12:
                defects.append('visible alpha coverage differs from target by more than tolerance')
        if not generation['detail_fit']['converged']:
            defects.append('detail target tolerance not reached within capped gain bounds')
        generation['defects'] = defects
    existing = registry.check_registry(args.registry_output, source_root=args.source_root) if args.registry_output.exists() else []
    previous = {record['id']: record for record in existing}
    for record in records:
        old_attempts = previous.get(record['id'], {}).get('generation', {}).get('attempts', [])
        if old_attempts != record['generation']['attempts'][:len(old_attempts)]:
            raise ValueError(f'{record["id"]}: retained attempts cannot be removed or rewritten')
    for record in records:
        name, generation = record['replaces'][0], record['generation']
        pilot.save_maps(record, args.derived_root)
        albedo = args.derived_root / record['files']['albedo']
        generation['detector_after'] = (dict(lighting_risk=False, applicable=False,
            reason='alpha cutout; IID excluded') if name in alpha_material.ALPHA_TEXTURES else measure_light(albedo))
        registry.validate_entry(record, content_root=args.derived_root, source_root=args.source_root)
    replacement = {record['id']: record for record in records}
    output = [replacement.pop(record['id'], record) for record in existing]
    output.extend(replacement.values())
    args.registry_output.parent.mkdir(parents=True, exist_ok=True)
    city_set.write_registry(args.registry_output, output, args.source_root)
    return dict(batch=len(records), total=len(output),
        calls=sum(len(record['generation']['attempts']) for record in output),
        new_material_calls=sum(len(record['generation']['attempts']) for record in output
            if record['replaces'][0] not in pilot.TEXTURES),
        marigold_applied=sum(record['generation']['marigold_applied'] for record in records),
        defects=sum(bool(record['generation']['defects']) for record in records))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prompt', 'prepare', 'build'))
    batch = parser.add_mutually_exclusive_group()
    batch.add_argument('--texture', action='append', default=[])
    batch.add_argument('--all', action='store_true', help='explicitly process all 65 materials')
    parser.add_argument('--select', action='append', default=[])
    parser.add_argument('--input-dir', type=Path, default=registry.DEFAULT_SOURCE_ROOT / SET)
    parser.add_argument('--prepared-dir', type=Path, default=BASE / SET / 'prepared-detail')
    parser.add_argument('--source-root', type=Path, default=registry.DEFAULT_SOURCE_ROOT)
    parser.add_argument('--derived-root', type=Path, default=BASE / 'content-staging/city-4-sources')
    parser.add_argument('--registry-output', type=Path, default=ROOT / 'data/content/city-4.jsonl')
    parser.add_argument('--pilot2-registry', type=Path, default=ROOT / 'data/content/quality-pilot-2.jsonl')
    parser.add_argument('--mapping', type=Path, default=ROOT / 'data/content/city-materials.csv')
    parser.add_argument('--bible', type=Path, default=ROOT / 'docs/content-style.md')
    parser.add_argument('--visual-counts', type=Path)
    parser.add_argument('--marigold-dir', type=Path)
    parser.add_argument('--marigold-log', type=Path)
    args = parser.parse_args(argv)
    try:
        records = recipes(args.mapping, args.bible, args.texture or None)
        if args.command == 'prompt':
            for record in records:
                print(json.dumps(dict(texture=record['replaces'][0], prompt=record['prompt'],
                    transparent_background=record['replaces'][0] in alpha_material.ALPHA_TEXTURES), ensure_ascii=False))
            return 0
        if not args.texture and not args.all:
            raise ValueError('prepare/build require an explicit --texture batch or --all')
        if args.registry_output.resolve() in {path.resolve() for path in (
                registry.DEFAULT_REGISTRY, ROOT / 'data/content/quality-pilot.jsonl',
                ROOT / 'data/content/quality-pilot-2.jsonl', ROOT / 'data/content/quality-pilot-3.jsonl',
                args.pilot2_registry)}:
            raise ValueError('city-4 registry must not replace an existing set registry')
        for path in (args.prepared_dir, args.source_root / SET, args.derived_root, args.registry_output):
            output = pack_materials.staging_output(path)
            for protected in ('city-3', 'pilot-b', 'pilot-c', 'pilot-2', 'pilot-3a', 'pilot-3b',
                    'quality-pilot-sources', 'quality-pilot-2-sources', 'quality-pilot-3-sources'):
                base = (BASE / 'content-staging' / protected).resolve()
                if output == base or output.is_relative_to(base) or base.is_relative_to(output):
                    raise ValueError(f'output overlaps protected set: {path}')
        records = prepare(args, records)
        if args.command == 'build':
            print(json.dumps(build(args, records), sort_keys=True))
        else:
            for record in records:
                print(json.dumps(dict(texture=record['replaces'][0],
                    selected_attempt=record['generation']['selected_attempt'],
                    colour_max_error=record['generation']['colour_correction']['linear_rgb_max_error'],
                    **record['generation']['detector_before']), sort_keys=True))
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        print(f'city4: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
