#!/usr/bin/env python3
"""Prepare, measure and stage the seven-material quality pilot without models.

``prepare`` retains image_gen PNG bytes and writes colour-corrected B inputs for
the separate dev-ubuntu Marigold command. ``build`` adds C, two city-3 staging
copies, a separate draft registry and final CSV/contact sheets. The live content
root and the city-3 registry are never changed.
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
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import gaussian_filter1d

import bakeoff
import city_set
import generate
from import_imagegen import MODEL, _write_immutable
from material_colour import oklab_to_linear
from material_lighting import measure_file as measure_light
from material_measures import measure_material
from material_scale import panel_canvas
import marigold_iid
import pack_materials
import registry as registry_module
from seam_fix import _minimum_path, seamless


ROOT = Path(__file__).resolve().parents[2]
BASE = Path.home() / '.local/share/gothic-ai'
TEXTURES = ('NW_CITY_ROOF_01.TGA', 'NW_CITY_PFLASTERSTEIN_01.TGA',
    'NW_CITY_BODENDIELEN_01.TGA', 'NW_CITY_ROOF_INSIDE_01.TGA',
    'NW_CITY_WALL_03.TGA', 'NW_HARBOUR_STAIRS_01.TGA', 'NW_CITY_HAFENKAI_BODEN_01.TGA')
ROOF_INSIDE = 'NW_CITY_ROOF_INSIDE_01.TGA'
SOURCE_NAME = re.compile(r'(NW_(?:CITY|HARBOUR)_[A-Z0-9_]+)-attempt-([12])\Z')


def csv_rows(path):
    with Path(path).open(newline='', encoding='utf-8') as stream:
        return {row['texture']: row for row in csv.DictReader(stream)}


def texture_name(value):
    value = value.upper()
    return value if value.endswith('.TGA') else value + '.TGA'


def scope(ceiling):
    result = list(TEXTURES)
    result[2] = texture_name(ceiling)
    if len(set(result)) != 7:
        raise ValueError('ceiling replacement must leave seven distinct materials')
    return result


def selections(values, names):
    result = {}
    for value in values:
        name, attempt = value.rsplit(':', 1)
        name = texture_name(name)
        if name not in names or name in result or attempt not in ('1', '2'):
            raise ValueError(f'invalid or repeated selection {value!r}')
        result[name] = int(attempt)
    return result


def validate_outputs(args):
    """Validate every writable destination before retaining or preparing files."""
    outputs = [args.prepared_dir, args.source_root, args.derived_root,
        args.report_dir, args.registry_output]
    if args.command == 'build':
        outputs += [args.staging_parent / ('pilot-'+variant) for variant in ('b', 'c')]
    base = args.city3.expanduser().resolve()
    for path in outputs:
        output = pack_materials.staging_output(path)
        if output == base or output.is_relative_to(base) or base.is_relative_to(output):
            raise ValueError(f'output must not overlap city-3: {path}')
    if args.registry_output.resolve() == args.base_registry.resolve():
        raise ValueError('pilot registry must not replace the base registry')


def prepared_png(image, target, *, frozen):
    """Build verifies the exact PNG supplied to IID without rewriting it."""
    encoded = BytesIO()
    image.save(encoded, format='PNG')
    data = encoded.getvalue()
    digest = hashlib.sha256(data).hexdigest()
    if frozen:
        if not target.is_file() or file_digest(target) != digest:
            raise ValueError(f'{target}: rebuilt B differs from prepared IID input; prepare final selection and rerun IID')
    else:
        target.write_bytes(data)
    return digest


def read_sources(directory, names, source_root, *, namespace='quality-pilot'):
    found = {name: [] for name in names}
    for path in sorted(directory.glob('NW_*-attempt-*.json')):
        match = SOURCE_NAME.fullmatch(path.stem)
        if match is None:
            raise ValueError(f'invalid source name or third attempt: {path.name}')
        name, number = match[1] + '.TGA', int(match[2])
        if name not in found:
            continue
        meta = json.loads(path.read_text())
        if meta.get('texture') != name or meta.get('attempt') != number:
            raise ValueError(f'{path.name}: texture/attempt differs from filename')
        for field in ('prompt', 'utc'):
            if not isinstance(meta.get(field), str) or not meta[field].strip():
                raise ValueError(f'{path.name}: missing {field}')
        seconds = meta.get('seconds')
        if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or not math.isfinite(seconds) or seconds < 0:
            raise ValueError(f'{path.name}: invalid generation seconds')
        source = path.with_suffix('.png')
        raw = source.read_bytes()
        with Image.open(BytesIO(raw)) as image:
            if image.format != 'PNG':
                raise ValueError(f'{source}: expected original PNG')
            size = list(image.size)
            image.verify()
        relative = Path(namespace) / source.name
        _write_immutable(source_root / relative, raw)
        _write_immutable(source_root / relative.with_suffix('.json'), path.read_bytes())
        found[name].append(dict(attempt=number, prompt=meta['prompt'], utc=meta['utc'],
            seconds=seconds, source_png=relative.as_posix(),
            source_sha256=hashlib.sha256(raw).hexdigest(), source_size=size))
    for name, attempts in found.items():
        if [item['attempt'] for item in attempts] != list(range(1, len(attempts)+1)) or not 1 <= len(attempts) <= 2:
            raise ValueError(f'{name}: require one or two consecutive attempts starting at 1')
    return found


def roof_canvas(image):
    """Three square modules; shifts and overlap cuts preserve native proportions.

    The single support in the square source is placed at the centres of thirds
    of the whole panel. The wrap boundaries introduced by those translations
    lie inside overlaps and are discarded by the constrained quilting cuts.
    """
    geometry = panel_canvas(*city_set.PHYSICAL_METRES[ROOF_INSIDE])
    width, side = geometry['width'], geometry['height']
    if image.width != image.height:
        raise ValueError('ROOF_INSIDE source must be one square module')
    square = np.asarray(image.resize((side, side), Image.Resampling.LANCZOS))
    luminance = square.astype(float) @ np.array([.2126, .7152, .0722])
    profile = gaussian_filter1d(luminance.mean(axis=0), max(1, side/80))
    margin = round(side*.1)
    support = margin + int(profile[margin:-margin].argmin())
    origins = [0, round((width-side)/2), width-side]
    centres = [round((index+.5)*width/3) for index in range(3)]
    shifts = [centre-origin-support for centre, origin in zip(centres, origins)]
    modules = [np.roll(square, shift, axis=1) for shift in shifts]
    result = np.zeros((side, width, 3), dtype=np.uint8)
    result[:, :side] = modules[0]
    cuts = []
    for index in (1, 2):
        left, right = origins[index], origins[index-1]+side
        # Discard the previous module's tail wrap and the next module's head
        # wrap. Source support stays outside these overlap intervals.
        if shifts[index-1] < 0:
            right = min(right, origins[index-1]+side+shifts[index-1]-2)
        if shifts[index] > 0:
            left = max(left, origins[index]+shifts[index]+2)
        if right-left < 3:
            raise ValueError('support location leaves no safe roof module overlap')
        incoming = modules[index][:, left-origins[index]:right-origins[index]].astype(float)
        error = np.mean((result[:, left:right].astype(float)-incoming)**2, axis=2)
        cut, _ = _minimum_path(error)
        cut += left
        for y, x in enumerate(cut):
            result[y, x:origins[index]+side] = modules[index][y, x-origins[index]:]
        cuts.append(dict(min_x=int(cut.min()), max_x=int(cut.max())))
    return Image.fromarray(result), dict(geometry, module_origins=origins,
        source_support_x=support, support_centres=centres, source_shifts_x=shifts,
        cuts=cuts, source_to_canvas_stretch=1.,
        method='three uniformly resized square modules; horizontal translations; minimum-error overlap cuts; no anisotropic stretch')


def scale_measurement(name, image, mapping, plan):
    measured = measure_material(image)
    errors, result = [], dict(texture=name, scale=float(plan['scale']),
        measured_direction=measured['direction'], target_direction=mapping['target_direction'],
        direction_pass=mapping['target_direction'] not in ('U', 'V') or measured['direction'] == mapping['target_direction'])
    for axis in ('u', 'v'):
        target = float(plan[f'planned_detail_{axis}_metres'])
        period = measured[f'period_{axis}_uv']
        actual = period*float(plan['tile_metres'])
        error = actual/target-1 if target and period else None
        if target:
            errors.append(error)
        result.update({f'target_{axis}_metres': target, f'measured_period_{axis}_uv': period,
            f'measured_{axis}_metres': actual, f'relative_error_{axis}': error,
            f'period_{axis}_confidence': measured[f'period_{axis}_confidence']})
    result['scale_pass'] = all(error is not None and abs(error) <= .15+1e-9 for error in errors)
    result['known_axes'] = len(errors)
    result['scale_status'] = ('no_known_period' if not errors else
        'within_15_percent' if result['scale_pass'] else
        'missing_period' if None in errors else 'outside_15_percent')
    return result


def prepared_records(args, names):
    mapping, plans = csv_rows(args.mapping), csv_rows(args.scale_plan)
    attempts = read_sources(args.input_dir, names, args.source_root)
    selected = selections(args.select, names)
    recipes = {r['replaces'][0]: r for r in city_set.recipes(args.mapping, args.bible, textures=names)}
    records, measurements = [], []
    args.prepared_dir.mkdir(parents=True, exist_ok=True)
    for name in names:
        number = selected.get(name, attempts[name][-1]['attempt'])
        chosen = next((item for item in attempts[name] if item['attempt'] == number), None)
        if chosen is None:
            raise ValueError(f'{name}: selected attempt {number} does not exist')
        record = deepcopy(recipes[name])
        resource_id = 'pilot-b-' + Path(name).stem.lower().replace('_', '-')
        record.update(id=resource_id, prompt=chosen['prompt'], pipeline='imagegen-filter',
            models=[deepcopy(MODEL)], seed=None)
        record['parameters'].update(set='pilot-b', material=resource_id,
            scale=float(plans[name]['scale']), source_size_policy='physical canvas first; Lanczos to square UV storage; narrow seam quilting on permitted repeat axes',
            scale_plan=dict(plans[name]), source_attempt=number)
        with Image.open(args.source_root / chosen['source_png']) as opened:
            image = opened.convert('RGB')
        geometry = None
        if name == ROOF_INSIDE:
            image, geometry = roof_canvas(image)
        elif record['parameters']['decision'] != 'include':
            u, v = city_set.PHYSICAL_METRES[name]
            aspect = u/v
            source_aspect = image.width/image.height
            stretch = max(source_aspect/aspect, aspect/source_aspect)
            geometry = dict(physical_uv_metres=[u, v], source_to_physical_stretch=stretch)
        image = image.resize((1024, 1024), Image.Resampling.LANCZOS)
        axes = '' if record['parameters']['layout'] == 'panel' else record['parameters']['tiling_axes']
        image, seam = seamless(image, axes=axes, overlap_fraction=1/64)
        maps, correction, rejection = generate.target_corrected_maps(image, record)
        # A failed admission remains a measurable candidate, as required by the
        # pilot's two-attempt limit. It is never silently claimed to pass.
        record['generation'] = dict(outcome='complete', attempts=attempts[name],
            selected_attempt=number, attempt_limit=2, colour_correction=correction,
            numeric_rejection=rejection, geometry=geometry, seam=seam,
            source_policy='text-only image_gen; original PNG bytes retained and checksummed',
            model_identity='image_gen.imagegen; underlying model/version not exposed; no seed exposed')
        image = Image.fromarray(np.rint(maps['albedo']*255).astype(np.uint8))
        target = args.prepared_dir / (Path(name).stem+'.png')
        record['generation']['prepared_b_sha256'] = prepared_png(image, target, frozen=args.command == 'build')
        record['_maps'] = maps
        measurement = scale_measurement(name, image, mapping[name], plans[name])
        measurement.update(colour_max_error=correction['linear_rgb_max_error'],
            colour_pass=correction['linear_rgb_max_error'] <= .005,
            selected_attempt=number)
        measurement['b_pass'] = measurement['colour_pass'] and measurement['scale_pass'] and measurement['direction_pass']
        measurements.append(measurement)
        records.append(record)
    return records, measurements


def save_maps(record, directory):
    maps = record.pop('_maps')
    record['files'] = {}
    for name, value in maps.items():
        relative = Path(record['parameters']['set']) / record['id'] / (name+'.png')
        target = directory/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(np.rint(np.clip(value, 0, 1)*255).astype(np.uint8)).save(target)
        record['files'][name] = relative.as_posix()


def file_digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def clone_and_pack(records, args, variant):
    output = pack_materials.staging_output(args.staging_parent / ('pilot-'+variant))
    base = args.city3.resolve()
    if output == base or output.is_relative_to(base) or base.is_relative_to(output):
        raise ValueError('pilot staging output must not overlap city-3')
    if output.exists():
        raise ValueError(f'{output}: staging exists; use a fresh --staging-parent')
    shutil.copytree(base, output, symlinks=False)
    index_path = output/'materials/materials.json'
    original = json.loads(index_path.read_text())
    index = deepcopy(original)
    write_dds = pack_materials.load_compressor()
    for record in records:
        maps = pack_materials.read_maps(record, args.derived_root)
        resource_id = record['id']
        directory = output/'materials'/resource_id
        directory.mkdir(parents=True, exist_ok=True)
        write_dds(directory/'albedo.dds', maps['albedo'])
        write_dds(directory/'normal.dds', maps['normal'], normal=True)
        write_dds(directory/'orm.dds', np.stack([maps[name] for name in ('ao', 'roughness', 'metallic')], axis=2))
        texture = record['replaces'][0]
        index[texture] = dict(original[texture],
            **{name: f'{resource_id}/{name}.dds' for name in ('albedo', 'normal', 'orm')},
            scale=record['parameters']['scale'])
    names = {record['replaces'][0] for record in records}
    if set(original) != set(index) or {name for name in index if index[name] != original[name]} != names:
        raise ValueError('staged index does not replace exactly the pilot materials')
    verified = 0
    for name in original.keys()-names:
        if index[name] != original[name]:
            raise ValueError(f'{name}: unrelated material index changed')
        for channel in ('albedo', 'normal', 'orm'):
            path = Path('materials') / original[name][channel]
            if file_digest(base/path) != file_digest(output/path):
                raise ValueError(f'{name}: unrelated DDS changed')
        verified += 1
    index_path.write_text(json.dumps(index, indent=2)+'\n')
    return dict(variant=variant.upper(), materials=len(index), replacements=len(names),
        unchanged_materials_verified=verified, unchanged_dds_verified=verified*3)


def write_csv(path, rows):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open('w', newline='', encoding='utf-8') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]), lineterminator='\n')
        writer.writeheader()
        writer.writerows(rows)


def contact_sheet(names, images, target, *, repeat=False):
    side, header = 384, 34
    sheet = Image.new('RGB', (side*3, (side+header)*len(names)), '#252626')
    draw = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype('/System/Library/Fonts/Menlo.ttc', 12)
    except OSError:
        font = ImageFont.load_default()
    for y, name in enumerate(names):
        for x, label in enumerate(('A', 'B', 'C')):
            with Image.open(images[(name, label)]) as opened:
                image = opened.convert('RGB')
            tile = image.resize((side//2 if repeat else side,)*2, Image.Resampling.LANCZOS)
            for ty in range(2 if repeat else 1):
                for tx in range(2 if repeat else 1):
                    sheet.paste(tile, (x*side+tx*tile.width, y*(side+header)+header+ty*tile.height))
            title = f'{label} {Path(name).stem}'
            draw.text((x*side+4, y*(side+header)+8), title, fill='white', font=font)
    sheet.save(target, quality=94)


def build(args, records, measurements, names):
    if args.marigold_dir is None:
        raise ValueError('build requires --marigold-dir with B → Marigold albedos')
    iid_measurements = {}
    if args.marigold_log:
        for line in args.marigold_log.read_text().splitlines():
            if line.strip():
                item = json.loads(line)
                if 'input' in item:
                    iid_measurements[Path(item['input']).stem] = item
    all_records = list(records)
    for record in records:
        name = record['replaces'][0]
        c = deepcopy(record)
        c['id'] = c['id'].replace('pilot-b-', 'pilot-c-', 1)
        c['parameters'].update(set='pilot-c', material=c['id'], iid_input='colour-corrected B before IID',
            iid_model=marigold_iid.MODEL_ID, iid_revision=marigold_iid.MODEL_REVISION)
        c['models'].append(dict(id=marigold_iid.MODEL_ID, revision=marigold_iid.MODEL_REVISION,
            license=marigold_iid.MODEL_LICENSE))
        source = args.marigold_dir / (Path(name).stem+'-albedo.png')
        raw = source.read_bytes()
        if args.marigold_log:
            iid = iid_measurements.get(Path(name).stem, {})
            if (iid.get('source_sha256') != record['generation']['prepared_b_sha256'] or
                    iid.get('output_sha256') != hashlib.sha256(raw).hexdigest()):
                raise ValueError(f'{name}: IID log does not match the prepared B and output C PNG bytes')
        relative = Path('quality-pilot/marigold')/source.name
        _write_immutable(args.source_root/relative, raw)
        with Image.open(BytesIO(raw)) as opened:
            if opened.size != (1024, 1024):
                raise ValueError(f'{source}: Marigold output must match prepared B 1024 square')
            c['_maps'], correction, rejection = generate.target_corrected_maps(opened, c)
        c['generation'].update(colour_correction=correction, numeric_rejection=rejection,
            iid_source=dict(source_png=relative.as_posix(), source_sha256=hashlib.sha256(raw).hexdigest()),
            iid_input_sha256=file_digest(args.prepared_dir/(Path(name).stem+'.png')))
        all_records.append(c)
    for record in all_records:
        save_maps(record, args.derived_root)
        registry_module.validate_entry(record, content_root=args.derived_root, source_root=args.source_root)
    args.registry_output.parent.mkdir(parents=True, exist_ok=True)
    args.registry_output.write_text(''.join(json.dumps(record, ensure_ascii=False, sort_keys=True)+'\n' for record in all_records))
    invariants = [clone_and_pack([r for r in all_records if r['parameters']['set'] == 'pilot-'+variant], args, variant)
                  for variant in ('b', 'c')]
    a_records = {r['replaces'][0]: r for r in bakeoff.read_registry(args.base_registry)
                 if r.get('parameters', {}).get('set') == 'city-3' and r.get('parameters', {}).get('selected') is not False}
    images, lights = {}, []
    for name in names:
        images[(name, 'A')] = args.city3/'sources'/a_records[name]['files']['albedo']
    for record in all_records:
        images[(record['replaces'][0], record['parameters']['set'][-1].upper())] = args.derived_root/record['files']['albedo']
    target_by_name = {r['replaces'][0]: oklab_to_linear(r['parameters']['target_oklab']) for r in records}
    for name in names:
        for label in ('A', 'B', 'C'):
            path = images[(name, label)]
            material = measure_material(path)
            lights.append(dict(texture=name, variant=label, **measure_light(path),
                colour_max_error=float(np.abs(np.array(material['mean_linear_rgb'])-target_by_name[name]).max()),
                albedo_sha256=file_digest(path)))
    args.report_dir.mkdir(parents=True, exist_ok=True)
    write_csv(args.report_dir/'lighting-abc.csv', lights)
    write_csv(args.report_dir/'scale-b.csv', measurements)
    write_csv(args.report_dir/'generation-cost.csv', [dict(texture=r['replaces'][0],
        calls=len(r['generation']['attempts']), selected_attempt=r['generation']['selected_attempt'],
        seconds=sum(a['seconds'] for a in r['generation']['attempts'])) for r in records])
    contact_sheet(names, images, args.report_dir/'materials-abc.jpg')
    contact_sheet(names, images, args.report_dir/'materials-abc-repeat-2x2.jpg', repeat=True)
    return dict(staging=invariants, registry=str(args.registry_output), derived_root=str(args.derived_root),
        generation_calls=sum(len(r['generation']['attempts']) for r in records),
        b_pass=sum(m['b_pass'] for m in measurements), b_materials=len(measurements))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('prepare', 'build'))
    parser.add_argument('--input-dir', required=True, type=Path)
    parser.add_argument('--prepared-dir', required=True, type=Path)
    parser.add_argument('--source-root', type=Path, default=registry_module.DEFAULT_SOURCE_ROOT)
    parser.add_argument('--city3', type=Path, default=BASE/'content-staging/city-3')
    parser.add_argument('--staging-parent', type=Path, default=BASE/'content-staging')
    parser.add_argument('--derived-root', type=Path, default=BASE/'content-staging/quality-pilot-sources')
    parser.add_argument('--marigold-dir', type=Path)
    parser.add_argument('--marigold-log', type=Path, help='optional JSONL stdout from marigold_iid.py; verify B input/C output checksums')
    parser.add_argument('--mapping', type=Path, default=ROOT/'data/content/city-materials.csv')
    parser.add_argument('--bible', type=Path, default=ROOT/'docs/content-style.md')
    parser.add_argument('--scale-plan', type=Path, default=ROOT/'reports/content/2026-10-pilot-measures/scale-plan.csv')
    parser.add_argument('--base-registry', type=Path, default=registry_module.DEFAULT_REGISTRY)
    parser.add_argument('--registry-output', type=Path, default=ROOT/'data/content/quality-pilot.jsonl')
    parser.add_argument('--report-dir', type=Path, default=ROOT/'reports/content/2026-10-quality-pilot')
    parser.add_argument('--ceiling', default=TEXTURES[2])
    parser.add_argument('--texture', action='append', default=[], help='prepare only these textures; build always needs seven')
    parser.add_argument('--select', action='append', default=[], help='TEXTURE:N; default latest available attempt')
    args = parser.parse_args(argv)
    try:
        validate_outputs(args)
        names = scope(args.ceiling)
        if args.texture:
            chosen = {texture_name(name) for name in args.texture}
            if args.command != 'prepare' or chosen-set(names):
                raise ValueError('--texture is only valid for known pilot prepare materials')
            names = [name for name in names if name in chosen]
        records, measurements = prepared_records(args, names)
        for measurement in measurements:
            print(json.dumps(measurement, sort_keys=True), flush=True)
        if args.command == 'build':
            print(json.dumps(build(args, records, measurements, names), sort_keys=True), flush=True)
    except (OSError, ValueError, KeyError, TypeError, StopIteration) as error:
        print(f'quality_pilot: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
