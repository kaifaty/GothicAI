#!/usr/bin/env python3
"""Import built-in image_gen PNGs into the city-2 comparison, without models.

Each ``sources/TEXTURE-attempt-N.json`` describes its neighbouring source PNG:
texture, attempt, prompt, utc, seconds, and source (relative to the permanent
source store; absolute tool output paths are accepted only for initial import).
This CPU-only command never calls a generator or reads provider credentials.
It copies the ten unchanged FLUX selections and filters all imported attempts
with the same target luminance and derived-map filter as the city-2 pilot.
"""

from __future__ import annotations

import argparse
from copy import deepcopy
import hashlib
from io import BytesIO
import json
import math
import os
from pathlib import Path
import re
import sys
import subprocess
import tempfile

import numpy as np
from PIL import Image

import bakeoff
import generate
from pilot import CITY_2_MODULES, MODULE_REPEAT_AXES
import registry as registry_module
import seam_fix


SET = 'city-2-imagegen'
SOURCE_SET = 'city-2'
TEXTURES = frozenset((*CITY_2_MODULES, 'NW_CITY_HAFENKAI_BODEN_01.TGA',
    'NW_CITY_DUNGEONFLOOR_02.TGA', 'NW_CITY_DUNGEONFLOOR_03.TGA',
    'NW_CITY_DIRTYGROUND_02.TGA'))
SOURCE_NAME = re.compile(r'(NW_CITY_[A-Z0-9_]+)-attempt-([1-3])\Z')
MODEL = dict(id='image_gen.imagegen', revision=None,
    license='OpenAI Terms of Use; output owned by user')
# These describe FLUX execution, not the shared numeric map filter.
FLUX_PARAMETERS = frozenset({'steps', 'guidance_scale', 'max_sequence_length',
    'quantization', 'torch_dtype', 'seed_algorithm', 'tiling',
    'negative_prompt_used'})


def texture_name(value):
    value = value.upper()
    return value if value.endswith('.TGA') else value + '.TGA'


def option_pairs(values, *, selection=False):
    """Parse repeatable explicit selections without silently overwriting one."""
    result = {}
    for value in values:
        try:
            name, choice = value.rsplit(':', 1)
        except ValueError as error:
            raise ValueError(f'expected TEXTURE:{"N" if selection else "uv|u|v"}: {value}') from error
        name = texture_name(name)
        if name not in TEXTURES:
            raise ValueError(f'unknown imported texture: {name}')
        if name in result:
            raise ValueError(f'duplicate option for {name}')
        if selection:
            if choice not in ('1', '2', '3'):
                raise ValueError(f'{name}: attempt must be 1, 2 or 3')
            choice = int(choice)
        elif choice not in ('u', 'v', 'uv'):
            raise ValueError(f'{name}: seam axes must be u, v or uv')
        result[name] = choice
    return result


def permitted_axes(record):
    params = record['parameters']
    layout = params['layout']
    if layout == 'tiled':
        return 'uv'
    if layout == 'panel':
        return ''
    if layout == 'strip':
        axes = MODULE_REPEAT_AXES.get(record['replaces'][0], params.get('tiling_axes', ''))
        if axes in ('u', 'v'):
            return axes
    raise ValueError(f'{record["id"]}: no valid repeat axes for layout {layout!r}')


def read_sources(sources, source_root=None):
    """Require the exact comparison scope and every 1-based attempt's recipe."""
    source_root = Path(source_root or registry_module.DEFAULT_SOURCE_ROOT).expanduser().resolve()
    found = {}
    metadata_paths = set()
    for path in sorted(sources.glob('NW_CITY_*-attempt-*.json')):
        match = SOURCE_NAME.fullmatch(path.stem)
        if match is None:
            raise ValueError(f'invalid source attempt filename: {path.name}')
        expected_texture = match[1] + '.TGA'
        expected_attempt = int(match[2])
        metadata = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(metadata, dict):
            raise ValueError(f'{path.name}: expected a JSON object')
        if metadata.get('texture') != expected_texture or metadata.get('attempt') != expected_attempt:
            raise ValueError(f'{path.name}: texture/attempt does not match filename')
        if expected_texture not in TEXTURES:
            raise ValueError(f'{path.name}: not one of the 12 imported materials')
        for field in ('prompt', 'utc'):
            if not isinstance(metadata.get(field), str) or not metadata[field].strip():
                raise ValueError(f'{path.name}: missing {field}')
        seconds = metadata.get('seconds')
        if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or not math.isfinite(seconds) or seconds < 0:
            raise ValueError(f'{path.name}: seconds must be a finite nonnegative number')
        raw = metadata.get('source')
        if not isinstance(raw, str):
            raise ValueError(f'{path.name}: source must name a PNG path')
        if Path(raw).is_absolute():
            raw = Path(raw).resolve()
        else:
            relative = registry_module._safe_path(raw, f'{path.name}.source')
            raw = (source_root / relative).resolve()
            if not raw.is_relative_to(source_root):
                raise ValueError(f'{path.name}: source escapes permanent store')
        if not raw.is_file() or raw.suffix.lower() != '.png':
            raise ValueError(f'{path.name}: source PNG does not exist: {raw}')
        # A tool may save its result elsewhere first; retain a staging copy too.
        retained = path.with_suffix('.png')
        if retained.exists() and retained.resolve() != raw:
            if retained.read_bytes() != raw.read_bytes():
                raise ValueError(f'{path.name}: source differs from retained neighbouring PNG')
        expected_digest = metadata.get('source_sha256')
        if expected_digest is not None and hashlib.sha256(raw.read_bytes()).hexdigest() != expected_digest:
            raise ValueError(f'{path.name}: source_sha256 does not match PNG')
        with Image.open(raw) as image:
            if image.format != 'PNG' or min(image.size) < 1:
                raise ValueError(f'{path.name}: expected a PNG image')
            dimensions = image.size
            image.verify()
        attempt = dict(metadata=metadata, metadata_path=path, source=raw,
            retained=retained, dimensions=dimensions)
        found.setdefault(expected_texture, []).append(attempt)
        metadata_paths.add(path.with_suffix('.png'))
    png_paths = set(sources.glob('NW_CITY_*-attempt-*.png'))
    if png_paths - metadata_paths:
        raise ValueError('source PNGs have no attempt metadata: ' +
            ', '.join(sorted(path.name for path in png_paths - metadata_paths)))
    missing = TEXTURES - found.keys()
    if missing:
        raise ValueError(f'expected exactly 12 materials; missing {len(missing)}: ' + ', '.join(sorted(missing)))
    for name, attempts in found.items():
        numbers = [attempt['metadata']['attempt'] for attempt in attempts]
        if numbers != list(range(1, len(numbers) + 1)) or not 1 <= len(numbers) <= 3:
            raise ValueError(f'{name}: require one through three consecutive attempts starting at 1')
    return found


def _new_id(record):
    prefix = SOURCE_SET + '-'
    if not record['id'].startswith(prefix):
        raise ValueError(f'{record["id"]}: city-2 id has unexpected prefix')
    return SET + '-' + record['id'][len(prefix):]


def _write_immutable(path, data):
    """Reuse equal bytes, but never replace a file an older recipe can name."""
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open('xb') as stream:
            stream.write(data)
    except FileExistsError:
        if path.read_bytes() != data:
            raise ValueError(f'refusing to overwrite retained file: {path}')


def _save_maps(sources, record, maps, number):
    encoded = {}
    digest = hashlib.sha256()
    for name in bakeoff.MAPS:
        value = np.asarray(maps[name])
        if not np.isfinite(value).all():
            raise ValueError(f'{record["id"]} attempt {number}: {name} contains nonfinite values')
        stream = BytesIO()
        Image.fromarray(np.rint(np.clip(value, 0, 1)*255).astype('uint8')).save(stream, format='PNG')
        encoded[name] = stream.getvalue()
        digest.update(name.encode() + b'\0' + encoded[name])
    # A later failure must leave even the previous successful import's files
    # intact. New pixels get a new directory; equal outputs reuse their paths.
    relative = Path(SET) / record['id'] / f'attempt-{number}' / digest.hexdigest()
    for name, data in encoded.items():
        _write_immutable(sources / relative / f'{name}.png', data)
    return {name: (relative / f'{name}.png').as_posix() for name in bakeoff.MAPS}


def validate_choice(record, attempts, axes='', selected=None):
    """Check every requested choice before any material files are written."""
    texture = record['replaces'][0]
    allowed = permitted_axes(record)
    if axes not in ('', 'u', 'v', 'uv'):
        raise ValueError(f'{texture}: seam axes must be u, v or uv')
    if not set(axes) <= set(allowed):
        raise ValueError(f'{texture}: seam fix {axes!r} exceeds {record["parameters"]["layout"]} repeat axes {allowed!r}')
    selected = attempts[-1]['metadata']['attempt'] if selected is None else selected
    if selected not in [attempt['metadata']['attempt'] for attempt in attempts]:
        raise ValueError(f'{texture}: selected attempt {selected} does not exist')
    return selected


def import_material(record, attempts, sources, axes='', selected=None, source_root=None):
    """Retain all source attempts, and select an explicitly chosen/latest one."""
    texture = record['replaces'][0]
    source_root = Path(source_root or registry_module.DEFAULT_SOURCE_ROOT).expanduser().resolve()
    selected = validate_choice(record, attempts, axes, selected)
    result = deepcopy(record)
    result.update(id=_new_id(record), pipeline='imagegen-filter', models=[deepcopy(MODEL)],
        seed=None, negative='', files={})
    params = result['parameters']
    for field in FLUX_PARAMETERS:
        params.pop(field, None)
    params.update(set=SET, material=result['id'], input='text only; no original pixels or game screenshots',
        tiling='seamless requested in text; explicit CPU overlap cut where reviewed',
        source_size_policy='nearest available generation aspect; Lanczos stretch to 1024 square',
        output_width=1024, output_height=1024)
    generation = dict(outcome='complete', attempts=[], selected_attempt=selected,
        model_identity='Tool name image_gen.imagegen; underlying model/version not exposed',
        seed_policy='No seed exposed by the built-in tool',
        source_policy='Original generated PNG retained outside git; no reference images supplied')
    result['generation'] = generation
    for source in attempts:
        metadata = source['metadata']
        number = metadata['attempt']
        retained = source['retained']
        relative = Path(SET) / retained.name
        permanent = source_root / relative
        raw_bytes = source['source'].read_bytes()
        _write_immutable(permanent, raw_bytes)
        digest = hashlib.sha256(raw_bytes).hexdigest()
        # Staging is disposable; its relative symlink points at the durable PNG.
        link_target = os.path.relpath(permanent, retained.parent)
        if not retained.is_symlink() or os.readlink(retained) != link_target:
            if retained.exists() and retained.read_bytes() != raw_bytes:
                raise ValueError(f'refusing to replace differing staging source: {retained}')
            retained.unlink(missing_ok=True)
            retained.symlink_to(link_target)
        metadata.update(source=relative.as_posix(), source_sha256=digest)
        source['metadata_path'].write_text(json.dumps(metadata, ensure_ascii=False,
            sort_keys=True) + '\n', encoding='utf-8')
        with Image.open(permanent) as opened:
            image = opened.convert('RGB').resize((1024, 1024), Image.Resampling.LANCZOS)
        # A broad overlap can eat a module's thin structural head/sill rail.
        # Keep its cut in a narrow boundary strip; homogeneous surfaces have
        # more freedom to find a low-error path through a wider overlap.
        overlap = 1 / 32 if params['decision'] == 'module' else 1 / 8
        image, seam = seam_fix.seamless(image, axes=axes, overlap_fraction=overlap)
        maps, correction, rejection = generate.target_corrected_maps(image, result)
        if rejection is not None:
            raise ValueError(f'{texture} attempt {number}: numeric admission failed: {rejection}')
        files = _save_maps(sources, result, maps, number)
        attempt = dict(number=number, prompt=metadata['prompt'], seed=None,
            utc=metadata['utc'], seconds=metadata['seconds'],
            source_png=relative.as_posix(), source_sha256=digest,
            source_width=source['dimensions'][0], source_height=source['dimensions'][1],
            output_width=1024, output_height=1024,
            brightness_correction=correction, seam_fix=seam, files=files,
            numeric_valid=True, numeric_rejection=None, outcome='complete', selected=number == selected)
        generation['attempts'].append(attempt)
        if number == selected:
            result.update(prompt=metadata['prompt'], files=deepcopy(files))
            params.update(source_width=attempt['source_width'], source_height=attempt['source_height'])
            generation.update(brightness_correction=deepcopy(correction),
                saturation_correction=deepcopy(correction['saturation']), seam_fix=deepcopy(seam))
    registry_module.validate_entry(result, content_root=sources, source_root=source_root)
    return result


def import_set(staging, flux_root, registry, fixes=None, selections=None, source_root=None):
    """Replace only this set's registry rows; preserve every other row verbatim."""
    staging = Path(staging).expanduser().resolve()
    flux_root = Path(flux_root).expanduser().resolve()
    sources = staging / 'sources'
    live = (Path.home() / '.local/share/gothic-ai/content').resolve()
    if staging.is_relative_to(live) or live.is_relative_to(staging) or staging == flux_root:
        raise ValueError('staging must be separate from the live content root and city-2')
    registry = Path(registry)
    source_root = Path(source_root or registry_module.DEFAULT_SOURCE_ROOT).expanduser().resolve()
    records = registry_module.check_registry(registry, source_root=source_root)
    flux = [record for record in records if record['parameters'].get('set') == SOURCE_SET]
    if len(flux) != 22 or len({tuple(record['replaces']) for record in flux}) != 22:
        raise ValueError(f'city-2 must contain exactly 22 distinct materials, found {len(flux)}')
    if any(len(record['replaces']) != 1 or record['generation'].get('outcome') != 'complete' for record in flux):
        raise ValueError('every city-2 record must be complete and replace one texture')
    if not TEXTURES <= {record['replaces'][0] for record in flux}:
        raise ValueError('city-2 does not contain all 12 comparison materials')
    fixes, selections = fixes or {}, selections or {}
    if (fixes.keys() | selections.keys()) - TEXTURES:
        raise ValueError('fix/selection contains a texture outside the comparison scope')
    found = read_sources(sources, source_root=source_root)
    # Preflight the entire set, not just the next material in CSV order.
    for record in flux:
        texture = record['replaces'][0]
        if texture in TEXTURES:
            validate_choice(record, found[texture], fixes.get(texture, ''), selections.get(texture))
        else:
            registry_module.validate_entry(record, content_root=flux_root / 'sources')
    retained_ids = {record['id'] for record in records if record['parameters'].get('set') != SET}
    if retained_ids & {_new_id(record) for record in flux}:
        raise ValueError('new resource ids conflict with records outside this set')
    imported = []
    for record in flux:
        texture = record['replaces'][0]
        if texture in TEXTURES:
            imported.append(import_material(record, found[texture], sources,
                axes=fixes.get(texture, ''), selected=selections.get(texture), source_root=source_root))
        else:
            clone = deepcopy(record)
            clone['id'] = _new_id(record)
            clone['parameters']['set'] = SET
            for relative in clone['files'].values():
                target = sources / relative
                _write_immutable(target, (flux_root / 'sources' / relative).read_bytes())
            registry_module.validate_entry(clone, content_root=sources)
            imported.append(clone)
    if len(imported) != 22:
        raise ValueError(f'expected 22 output records, found {len(imported)}')
    kept_lines = [line for line in registry.read_bytes().splitlines(keepends=True)
        if json.loads(line)['parameters'].get('set') != SET]
    serialized = b''.join(line if line.endswith(b'\n') else line + b'\n' for line in kept_lines)
    serialized += ''.join(json.dumps(record, ensure_ascii=False, sort_keys=True, allow_nan=False) + '\n'
        for record in imported).encode('utf-8')
    with tempfile.NamedTemporaryFile(dir=registry.parent, prefix=registry.name + '.', suffix='.tmp', delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(serialized)
    try:
        registry_module.check_registry(temporary, source_root=source_root)
        temporary.replace(registry)
    finally:
        temporary.unlink(missing_ok=True)
    return imported


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    command = commands.add_parser('import', help='filter already-generated PNG sources and register the complete comparison set')
    command.add_argument('--staging', type=Path, required=True)
    command.add_argument('--flux-root', type=Path, required=True)
    command.add_argument('--registry', type=Path, default=registry_module.DEFAULT_REGISTRY)
    command.add_argument('--source-root', type=Path, default=registry_module.DEFAULT_SOURCE_ROOT)
    command.add_argument('--fix', action='append', default=[], metavar='TEXTURE:uv|u|v')
    command.add_argument('--select', action='append', default=[], metavar='TEXTURE:N')
    args = parser.parse_args(argv)
    try:
        records = import_set(args.staging, args.flux_root, args.registry,
            option_pairs(args.fix), option_pairs(args.select, selection=True), source_root=args.source_root)
        subprocess.run([str(Path(__file__).with_name('backup-sources.sh')),
            '--registry', str(args.registry), '--source-root', str(args.source_root)], check=True)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f'Imagegen import failed: {error}', file=sys.stderr)
        return 1
    attempts = sum(len(record['generation']['attempts']) for record in records if record['pipeline'] == 'imagegen-filter')
    print(f'Imported {len(TEXTURES)} imagegen materials ({attempts} attempts) and {len(records)-len(TEXTURES)} unchanged FLUX selections; {len(records)} records')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
