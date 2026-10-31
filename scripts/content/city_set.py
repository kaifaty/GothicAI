#!/usr/bin/env python3
"""Prepare and import text-only built-in image_gen batches for staged city-3.

Generation itself belongs to image_gen.imagegen. This command only reads its
retained PNG outputs, derives maps on CPU, and backs up every imported batch.
The registry may contain pending materials until all batches are complete.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
import csv
import hashlib
from io import BytesIO
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

import bakeoff
import alpha_material
import generate
from import_imagegen import MODEL, _write_immutable
from pilot import TARGET_LUMINANCE_TOLERANCE, antitile_mode, selected_rows
import registry as registry_module
import seam_fix
from material_colour import colour_target, correct_mean_colour

ROOT = Path(__file__).resolve().parents[2]
SET = 'city-3'
DEFAULT_STAGING = Path.home() / '.local/share/gothic-ai/content-staging' / SET
NO_BAKED_LIGHT = ('flat albedo only, no painted shadows, no shading under beams or overhangs, '
    'no dark contact lines, no ambient occlusion, no highlights')
SUBSTRATES = dict(timber_frame_tile='plaster', framed_masonry_tile='masonry_rubble',
    framed_boards_tile='planks_wall', window_wall_tile='plaster', quay_wall_tile='masonry_rubble',
    roof_structure_tile='planks_wall', stair_tile='flagstone', pavement_border_tile='cobble',
    bound_timber_tile='timber_beam', path_tile='dirt_path', paving_transition_tile='flagstone',
    ground_panel='dirt_path', stone_panel='flagstone', planks_panel='planks_wall',
    timber_strip='timber_beam', stone_strip='masonry_dressed', plaster_panel='plaster',
    masonry_panel='masonry_rubble', timber_frame_panel='plaster', roof_panel='planks_wall',
    painted_panel='plaster', roof_ridge_strip='roof_tiles', plaster_strip='plaster',
    rope_strip='timber_beam', cloth_panel='plaster', foliage_panel='plaster')

# U/V metres from the physical module analysis, before square UV storage.
PHYSICAL_METRES = {
    'NW_CITY_HAFENKAI_MAUER_01.TGA': (14.33, 19.98),
    'NW_CITY_WOODBEAM_02.TGA': (1.15, 10.14),
    'NW_CITY_ROOF_INSIDE_01.TGA': (14.48, 6.42),
    'NW_CITY_WALL_03.TGA': (2., 2.06),
    'NW_HARBOUR_WOOD_IRON_01.TGA': (5.2, 3.),
    'NW_CITY_INSIDEWALL_12.TGA': (3.53, 3.53),
    'NW_CITY_FACHWERKBASEMENT_EMPTY_01.TGA': (4., 4.),
    'NW_CITY_RICH_WALL_01.TGA': (3.71, 3.70),
    'NW_CITY_INSIDEWALL_13.TGA': (3.73, 3.50),
    'NW_CITY_INSIDEWALL_11.TGA': (4.11, 4.11),
    'NW_CITY_HAFEN_HOLZWAND_02.TGA': (3.64, 4.09),
    'NW_CITY_RICH_WINDOW_01.TGA': (2.38, 3.),
    'NW_HARBOUR_STAIRS_01.TGA': (3.4, 2.56),
    'NW_CITY_WOODBEAM_03.TGA': (1.38, 7.46),
    'NW_CITY_DIRTYGROUND_WEG_03.TGA': (5.93, 6.66),
    'NW_CITY_PFLASTERSTEIN_BORDER_01.TGA': (6., 6.),
    'NW_CITY_DIRTYGROUND_WEG_02.TGA': (5.34, 5.),
    'NW_CITY_WINDOW_03.TGA': (4., 3.75),
    'NW_CITY_RICH_WALL_HALF_01.TGA': (3.71, 1.39),
    'NW_CITY_WOODBEAM_01.TGA': (.3, 2.65),
    'NW_CITY_FACHWERKBASEMENT_WINDOW_01.TGA': (4., 4.),
    'NW_CITY_HAFENKAI_BODEN_TRANS_01.TGA': (7.97, 8.18),
    'NW_CITY_FACHWERKBASEMENT_CHAIN_01.TGA': (2.95, 2.61),
    'NW_CITY_WALL_05.TGA': (3.03, 1.29),
    'NW_HARBOUR_STAIRS_03.TGA': (1.31, 1.98),
    'NW_CITY_WALL_04.TGA': (3.83, 2.64),
    # Remaining opaque panels/strips: area-weighted median U/V metres per UV
    # unit, using matching-layout patches in texture-intent/final-measurements.
    'NW_CITY_DIRTYGROUND_WEG_END_01.TGA': (5.46567, 4.13799),
    'NW_CITY_DIRTYGROUND_WEG_TRANS_02.TGA': (4.78115, 5.83923),
    'NW_CITY_HAFENRANDSTEIN_01.TGA': (14.85073, 1.92817),
    'NW_CITY_RICH_BLACKWOOD_01.TGA': (.36577, 4.),
    'NW_CITY_SHRINEFLOOR_01.TGA': (7.39512, 7.39537),
    'NW_CITY_WOOD_01.TGA': (6.51346, .95533),
    'NW_CITY_WOOD_ORNAMENT_01.TGA': (.39163, 3.27),
    'NW_CITY_WOOD_ORNAMENT_02.TGA': (1.91819, 8.16460),
    'NW_HARBOUR_WOODCRATE_01.TGA': (2.13421, 3.45146),
    'NW_HARBOUR_WOODCRATE_02.TGA': (4.49052, 5.39176),
    'NW_CITY_FLAG_01.TGA': (2.00200, 4.00400),
    'NW_CITY_IVY_BORDER_01.TGA': (5.29006, 4.58294),
    'NW_CITY_IVY_FULL_01.TGA': (2.68061, 2.90555),
    'NW_CITY_ROPE_01.TGA': (.27497, 3.75734),
    'NW_HARBOUR_ROPEWALL_01.TGA': (1.75664, 2.15476),
    'NW_HARBOUR_ROPEWALL_02.TGA': (2.80746, 1.78725),
}
STRIP_AXES = {
    'NW_CITY_HAFENRANDSTEIN_01.TGA': 'u', 'NW_CITY_RICH_BLACKWOOD_01.TGA': 'v',
    'NW_CITY_WOOD_ORNAMENT_01.TGA': 'v', 'NW_CITY_WOOD_ORNAMENT_02.TGA': 'u',
    'NW_CITY_WALL_05.TGA': 'u', 'NW_CITY_WOODBEAM_01.TGA': 'v',
    'NW_CITY_WOODBEAM_02.TGA': 'v', 'NW_CITY_WOODBEAM_03.TGA': 'v',
    'NW_HARBOUR_STAIRS_01.TGA': 'v', 'NW_HARBOUR_WOOD_IRON_01.TGA': 'v',
    'NW_CITY_ROPE_01.TGA': 'v', 'NW_HARBOUR_ROPEWALL_01.TGA': 'u',
    'NW_HARBOUR_ROPEWALL_02.TGA': 'u',
}
SOURCE_NAME = re.compile(r'(NW_(?:CITY|HARBOUR)_[A-Z0-9_]+)-attempt-([1-3])\Z')
REGENERATE = frozenset({'NW_CITY_WALL_03.TGA', 'NW_CITY_WINDOW_03.TGA',
    'NW_CITY_FACHWERKBASEMENT_EMPTY_01.TGA', 'NW_CITY_ROOF_01.TGA',
    'NW_CITY_FACHWERKBASEMENT_WINDOW_01.TGA', 'NW_CITY_FACHWERKBASEMENT_CHAIN_01.TGA',
    'NW_CITY_WOODBEAM_02.TGA'})


def texture_name(value):
    value = value.upper()
    return value if value.endswith('.TGA') else value + '.TGA'


def recipe_id(texture):
    return SET + '-' + Path(texture).stem.lower().replace('_', '-')


def repeat_axes(row):
    if row['layout'] == 'tiled':
        return 'uv'
    if row['layout'] == 'panel':
        return ''
    if row['layout'] == 'strip' and row['texture'] in STRIP_AXES:
        return STRIP_AXES[row['texture']]
    raise ValueError(f'{row["texture"]}: no measured repeat axes for {row["layout"]}')


def recipes(mapping=ROOT/'data/content/city-materials.csv', bible=ROOT/'docs/content-style.md',
            textures=None, require_target=True):
    """Build recipes from semantic text and scalar measurements, never pixels."""
    mapping, bible = Path(mapping), Path(bible)
    classes, _ = generate.read_style(bible)
    cores = {}
    for line in bible.read_text().splitlines():
        cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
        if len(cells) == 7 and cells[0].strip('`') in SUBSTRATES:
            cores[cells[0].strip('`')] = cells[2]
    with mapping.open(newline='', encoding='utf-8') as stream:
        rows = selected_rows(list(csv.DictReader(stream)), SET)
    chosen = None if textures is None else {texture_name(name) for name in textures}
    available = {row['texture'] for row in rows}
    if chosen is not None and (not chosen or chosen - available):
        raise ValueError('requested texture is outside the nonmetal city-3 scope')
    result = []
    for row in rows:
        texture = row['texture']
        if chosen is not None and texture not in chosen:
            continue
        name = row['class']
        substrate = SUBSTRATES.get(name, name)
        if texture == 'NW_CITY_HAFEN_HOLZWAND_02.TGA':
            substrate = 'planks_wall'
        if substrate not in classes:
            raise ValueError(f'{texture}: no numeric substrate for {name}')
        style = classes[substrate]
        module = row['decision'] != 'include'
        metres = PHYSICAL_METRES[texture] if module else (style['tile_m'], style['tile_m'])
        aspect = metres[0] / metres[1]
        axes = repeat_axes(row)
        boundaries = {'uv': 'opposite left/right and top/bottom edges join seamlessly',
            'u': 'left/right edges join seamlessly; top/bottom edges are fixed',
            'v': 'top/bottom edges join seamlessly; left/right edges are fixed',
            '': 'one complete bounded panel; no wrapping at any edge'}[axes]
        core = cores.get(name, style['prompt'] if not module else row['depicts'])
        if texture == 'NW_CITY_HAFEN_GIEBEL_01.TGA':
            core = 'weathered rough horizontal overlapping handmade wooden boards'
        alpha_tested = texture in alpha_material.ALPHA_TEXTURES
        raw_coverage = row.get('target_coverage', '')
        target_coverage = None
        if alpha_tested:
            if raw_coverage:
                target_coverage = float(raw_coverage)
                if not math.isfinite(target_coverage) or not 0 < target_coverage < 1:
                    raise ValueError(f'{texture}: invalid target_coverage')
            elif require_target:
                raise ValueError(f'{texture}: alpha material requires target_coverage')
        composition = ('Front orthographic material elevation, surface fills every pixel, no scene or margins, '
            'muted earthy colours, small irregular handmade detail. ')
        if alpha_tested:
            density = (f'Visible foreground must occupy approximately {target_coverage:.1%} of the canvas. '
                if target_coverage is not None else 'Keep visible foreground and transparent gaps at the density of this material role. ')
            shape = {
                'NW_CITY_FLAG_01.TGA': 'One upright hanging cloth banner attached along its top edge, narrow woven edging, a plain unrecognizable nonfigurative charcoal weave patch, lightly frayed lower silhouette and transparent space outside the cloth; no heraldry or figurative ornament, no pole or wall.',
                'NW_CITY_IVY_BORDER_01.TGA': 'A cohesive ivy border: dense attached stems and overlapping leaves on the LEFT side, tapering toward an irregular open RIGHT side with trailing leafy tendrils and real transparent gaps; preserve this left-to-right UV silhouette, no wall or supporting background.',
                'NW_CITY_IVY_FULL_01.TGA': 'A dense cohesive climbing ivy patch with overlapping leaves, small irregular real transparent gaps between leaf clusters, and an organic leafy outer silhouette; no wall or supporting background.',
            }[texture]
            composition = ('Front orthographic flat material cutout. '+shape+' '+density+
                'Return real RGBA alpha transparency with transparent background; no painted checkerboard, '
                'no solid background colour, no border around the canvas. Opaque foliage or cloth carries all colour; '
                'transparent regions are holes. Muted earthy colours, fine handmade detail. ')
        prompt = (f'Create an original late medieval harbour town material. {core}. '
            f'Object: {row["object"]}; role: {row["role"]}. Depicts: {row["depicts"]}. '
            f'Material condition: {row["state"]}; colour: {row["colour"]}; '
            f'actual wear and sparse coloured deposits: {row["weathering"]}. '
            f'Orientation: {row["orientation"]}. Physical U:V proportions {metres[0]:g}:{metres[1]:g} '
            f'metres before square UV packing. {boundaries}. '
            f'{composition}'
            f'{NO_BAKED_LIGHT}. Constant colour response independent of depth; '
            'no dark halos at contacts, no underside strips, no edge vignette or relief shading. '
            'Dirt means sparse pigment deposits, never an illumination gradient. '
            'Engine normal maps and dynamic lights supply relief and all shadows. '
            'No perspective, text, logos or watermark. Text only; no image references.')
        if texture == 'NW_CITY_FLAG_01.TGA':
            prompt = prompt.replace('dark emblem', 'plain unrecognizable nonfigurative charcoal weave patch')
        params = dict(set=SET, material=recipe_id(texture), material_class=name, numeric_substrate=substrate,
            decision=row['decision'], layout=row['layout'], object=row['object'], role=row['role'],
            depicts=row['depicts'], state=row['state'], scale=1. if module else float(row['scale']),
            tile_m=style['tile_m'], luminance_range=style['luminance'],
            roughness_range=style['roughness'], roughness_base=sum(style['roughness'])/2,
            saturation_limit=.25 if name == 'roof_tiles' else style['saturation_limit'],
            metallic=0, resolution=1024, output_width=1024, output_height=1024,
            source_aspect=aspect, physical_uv_metres=list(metres), tiling_axes=axes, tileable=bool(axes),
            source_size_policy='nearest available generation aspect; Lanczos stretch to 1024 square',
            tiling='seamless requested in text; explicit CPU overlap cut along designated repeat axes',
            antitile=antitile_mode(row), antitile_cell=1.,
            input='text only; no original pixels or game screenshots',
            luminance_policy='original mean linear luminance only; class range is advisory',
            target_luminance_tolerance=TARGET_LUMINANCE_TOLERANCE,
            height_filter=dict(sigma_small_px_at_1024=2, sigma_large_px_at_1024=32, normal_strength=.025),
            normal_convention='OpenGL: X right, Y up, Z toward viewer; DDS pack flips Y for engine texture-V basis',
            mapping_sha256=hashlib.sha256(mapping.read_bytes()).hexdigest(),
            bible_sha256=hashlib.sha256(bible.read_bytes()).hexdigest())
        if alpha_tested:
            params.update(alpha_mode='test', alpha_threshold=alpha_material.ALPHA_TEST_THRESHOLD,
                coverage_tolerance=alpha_material.COVERAGE_TOLERANCE,
                transparent_render='albedo only; generated normal/ORM retained but unused by the engine alpha pass',
                numeric_substrate_note='neutral existing numeric range for retained unused maps; alpha pass renders only albedo')
            if target_coverage is not None:
                params['target_coverage'] = target_coverage
        raw_target = row.get('target_luminance', '')
        if raw_target:
            target = float(raw_target)
            if not math.isfinite(target) or not 0 <= target <= 1:
                raise ValueError(f'{texture}: invalid target_luminance')
            params['target_luminance'] = target
        elif require_target:
            raise ValueError(f'{texture}: city-3 requires target_luminance')
        target_colour = colour_target(row)
        if target_colour is not None:
            params.update(target_oklab=target_colour,
                colour_policy='original OKLab of mean linear RGB; per-channel linear gains; no saturation cap')
        result.append(dict(id=recipe_id(texture), kind='material', replaces=[texture],
            pipeline='imagegen-filter', models=[deepcopy(MODEL)], prompt=prompt, negative='',
            seed=None, status='draft', files={}, parameters=params,
            generation=dict(outcome='pending', attempts=[], attempt_limit=3)))
    return result


def write_registry(path, records, source_root):
    """Validate and atomically publish recipes, including pending batches."""
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name+'.',
                suffix='.tmp', delete=False) as stream:
            temporary = Path(stream.name)
            for record in records:
                stream.write((json.dumps(record, ensure_ascii=False, sort_keys=True,
                    allow_nan=False)+'\n').encode('utf-8'))
        registry_module.check_registry(temporary, source_root=source_root)
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def prepare(registry=registry_module.DEFAULT_REGISTRY, mapping=ROOT/'data/content/city-materials.csv',
            bible=ROOT/'docs/content-style.md', source_root=registry_module.DEFAULT_SOURCE_ROOT):
    records = registry_module.check_registry(registry, source_root=source_root)
    existing = {record['id']: record for record in records}
    for recipe in recipes(mapping, bible):
        old = existing.get(recipe['id'])
        if old is None:
            records.append(recipe)
        elif old['pipeline'] != 'imagegen-filter' or old['replaces'] != recipe['replaces']:
            raise ValueError(f'{recipe["id"]}: existing city-3 recipe has incompatible provenance')
        elif old['generation']['outcome'] == 'pending':
            # Preserve the first retained WALL_03 attempt and every later tool
            # call while replacing its superseded "waiting for metal" reason.
            old['parameters'] = recipe['parameters']
            old['generation'].pop('reason', None)
            old['generation']['attempt_limit'] = 3
    write_registry(registry, records, source_root)
    return [record for record in records if record['parameters'].get('set') == SET]


def read_sources(sources, textures, source_root):
    """Read one partial batch, requiring every retained call for each texture."""
    sources, source_root = Path(sources), Path(source_root).expanduser().resolve()
    textures = {texture_name(name) for name in textures}
    found = {}
    for path in sorted(sources.glob('*-attempt-*.json')):
        match = SOURCE_NAME.fullmatch(path.stem)
        if match is None:
            raise ValueError(f'invalid source metadata filename: {path.name}')
        texture, number = match[1]+'.TGA', int(match[2])
        if texture not in textures:
            continue
        metadata = json.loads(path.read_text(encoding='utf-8'))
        if not isinstance(metadata, dict) or metadata.get('texture') != texture or metadata.get('attempt') != number:
            raise ValueError(f'{path.name}: texture/attempt does not match filename')
        for field in ('prompt', 'utc'):
            if not isinstance(metadata.get(field), str) or not metadata[field].strip():
                raise ValueError(f'{path.name}: missing {field}')
        if metadata.get('tool', MODEL['id']) != MODEL['id']:
            raise ValueError(f'{path.name}: expected built-in image_gen.imagegen provenance')
        seconds = metadata.get('seconds')
        if isinstance(seconds, bool) or not isinstance(seconds, (int, float)) or not math.isfinite(seconds) or seconds < 0:
            raise ValueError(f'{path.name}: seconds must be finite and nonnegative')
        relative = registry_module._safe_path(metadata.get('source'), f'{path.name}.source')
        raw = (source_root / relative).resolve()
        if not raw.is_relative_to(source_root) or not raw.is_file() or raw.suffix.lower() != '.png':
            raise ValueError(f'{path.name}: source must be a PNG in the permanent source store')
        digest = hashlib.sha256(raw.read_bytes()).hexdigest()
        if metadata.get('source_sha256', digest) != digest:
            raise ValueError(f'{path.name}: source_sha256 differs from permanent PNG')
        with Image.open(raw) as image:
            if image.format != 'PNG':
                raise ValueError(f'{path.name}: source is not a PNG')
            size = image.size
            image.load()
            if texture in alpha_material.ALPHA_TEXTURES:
                if image.mode != 'RGBA':
                    raise ValueError(f'{path.name}: alpha material requires real RGBA PNG, not an opaque checkerboard')
                visible = alpha_material.coverage(np.asarray(image)[..., 3])
                if not 0 < visible < 1:
                    raise ValueError(f'{path.name}: alpha material requires visible foreground and transparent cutouts')
            elif 'A' in image.getbands() and image.getchannel('A').getextrema() != (255, 255):
                raise ValueError(f'{path.name}: generated albedo must be opaque')
        retained = path.with_suffix('.png')
        if retained.exists() and retained.read_bytes() != raw.read_bytes():
            raise ValueError(f'{path.name}: staging source differs from permanent PNG')
        found.setdefault(texture, []).append(dict(metadata=metadata, path=path, source=raw,
            relative=relative.as_posix(), sha256=digest, dimensions=size, retained=retained))
    missing = textures - found.keys()
    if missing:
        raise ValueError('batch has no source metadata for '+', '.join(sorted(missing)))
    for texture, attempts in found.items():
        numbers = [source['metadata']['attempt'] for source in attempts]
        if numbers != list(range(1, len(attempts)+1)) or not 1 <= len(attempts) <= 3:
            raise ValueError(f'{texture}: require 1..3 consecutive attempts starting at 1')
    return found


def save_maps(sources, record, maps, number):
    """Content-addressed directories keep old attempts and recipes immutable."""
    encoded, digest = {}, hashlib.sha256()
    for name in bakeoff.MAPS:
        value = np.asarray(maps[name])
        if not np.isfinite(value).all():
            raise ValueError(f'{record["id"]} attempt {number}: nonfinite {name}')
        stream = BytesIO()
        Image.fromarray(np.rint(np.clip(value, 0, 1)*255).astype('uint8')).save(stream, format='PNG')
        encoded[name] = stream.getvalue()
        digest.update(name.encode()+b'\0'+encoded[name])
    relative = Path(SET)/record['id']/f'attempt-{number}'/digest.hexdigest()
    for name, value in encoded.items():
        _write_immutable(sources/relative/(name+'.png'), value)
    return {name: (relative/(name+'.png')).as_posix() for name in bakeoff.MAPS}


def import_material(recipe, attempts, sources, source_root, *, axes=None, selected=None,
                    review=None, failed=False, previous=None):
    params = recipe['parameters']
    allowed = params['tiling_axes']
    axes = allowed if axes is None else axes
    if axes not in ('', 'u', 'v', 'uv') or not set(axes) <= set(allowed):
        raise ValueError(f'{recipe["id"]}: seam axes {axes!r} exceed repeat axes {allowed!r}')
    numbers = [source['metadata']['attempt'] for source in attempts]
    automatic_selection = selected is None
    selected = numbers[-1] if selected is None else selected
    if selected not in numbers:
        raise ValueError(f'{recipe["id"]}: selected attempt does not exist')
    if failed and not review:
        raise ValueError(f'{recipe["id"]}: selecting a failed attempt requires a review note')
    previous_attempts = (previous or {}).get('generation', {}).get('attempts', [])
    for old in previous_attempts:
        number = old.get('number')
        current = next((source for source in attempts if source['metadata']['attempt'] == number), None)
        if current is None or any(old.get(key) != value for key, value in (
                ('source_png', current['relative']), ('source_sha256', current['sha256']),
                ('prompt', current['metadata']['prompt']), ('utc', current['metadata']['utc']),
                ('seconds', current['metadata']['seconds']))):
            raise ValueError(f'{recipe["id"]}: retained attempt {number} cannot be removed or rewritten')
    result = deepcopy(recipe)
    generation = result['generation'] = dict(outcome='complete', attempts=[], selected_attempt=selected,
        attempt_limit=3, new_tool_calls=len(attempts),
        model_identity='Tool name image_gen.imagegen; underlying model/version not exposed',
        seed_policy='No seed exposed by the built-in tool',
        source_policy='Original generated PNG in permanent source store; text only, no reference images')
    for source in attempts:
        metadata = source['metadata']
        number = metadata['attempt']
        retained = source['retained']
        link = os.path.relpath(source['source'], retained.parent)
        if not retained.is_symlink() or os.readlink(retained) != link:
            retained.unlink(missing_ok=True)
            retained.symlink_to(link)
        metadata.update(source=source['relative'], source_sha256=source['sha256'], tool=MODEL['id'])
        source['path'].write_text(json.dumps(metadata, ensure_ascii=False, sort_keys=True)+'\n', encoding='utf-8')
        alpha_tested = recipe['replaces'][0] in alpha_material.ALPHA_TEXTURES
        with Image.open(source['source']) as raw:
            if alpha_tested:
                rgba = np.asarray(raw.convert('RGBA'))
                raw_coverage = alpha_material.coverage(rgba[..., 3])
                image = alpha_material.resize_rgba(rgba)
            else:
                image = raw.convert('RGB').resize((1024, 1024), Image.Resampling.LANCZOS)
        if alpha_tested:
            seam = dict(axes='', applied=False, reason='bounded alpha panel; silhouette retained')
            maps, correction, rejection = alpha_material.corrected_maps(image, result)
            correction['alpha']['source_coverage'] = raw_coverage
        else:
            overlap = 1/8 if params['decision'] == 'include' else 1/32
            image, seam = seam_fix.seamless(image, axes=axes, overlap_fraction=overlap)
            maps, correction, rejection = generate.target_corrected_maps(image, result)
        files = save_maps(sources, result, maps, number)
        attempt = dict(number=number, prompt=metadata['prompt'], seed=None, tool=MODEL['id'],
            utc=metadata['utc'], seconds=metadata['seconds'], source_png=source['relative'],
            source_sha256=source['sha256'], source_width=source['dimensions'][0],
            source_height=source['dimensions'][1], output_width=1024, output_height=1024,
            brightness_correction=correction, seam_fix=seam, files=files,
            numeric_valid=rejection is None, numeric_rejection=rejection,
            outcome='complete', selected=number == selected)
        prior = next((item for item in previous_attempts if item.get('number') == number), {})
        for field in ('review_note', 'timing_policy'):
            if metadata.get(field, prior.get(field)):
                attempt[field] = metadata.get(field, prior.get(field))
        generation['attempts'].append(attempt)
    if recipe['replaces'][0] in alpha_material.ALPHA_TEXTURES and automatic_selection:
        admitted = [attempt for attempt in generation['attempts'] if attempt['numeric_valid']]
        if admitted:
            selected = admitted[-1]['number']
        else:
            def error_score(attempt):
                correction = attempt['brightness_correction']
                return (correction['alpha']['absolute_error']/params['coverage_tolerance'] +
                    abs(correction['linear_mean_after']-params['target_luminance'])/params['target_luminance_tolerance'])
            selected = min(generation['attempts'], key=error_score)['number']
            generation['best_available'] = f'best available of {len(attempts)} retained attempts; admission failed'
    generation['selected_attempt'] = selected
    for attempt in generation['attempts']:
        attempt['selected'] = attempt['number'] == selected
        if not attempt['selected']:
            continue
        correction = attempt['brightness_correction']
        result.update(prompt=attempt['prompt'], files=deepcopy(attempt['files']))
        result['parameters'].update(source_width=attempt['source_width'], source_height=attempt['source_height'])
        generation.update(brightness_correction=deepcopy(correction),
            saturation_correction=deepcopy(correction['saturation']), seam_fix=deepcopy(attempt['seam_fix']))
        if not attempt['numeric_valid']:
            generation.update(selected_failed=True, failure_warning=f'Numeric admission failed: {attempt["numeric_rejection"]}')
    if review:
        generation['visual_review'] = review
    if failed:
        generation.update(selected_failed=True, failure_warning=review)
    registry_module.validate_entry(result, content_root=sources, source_root=source_root)
    return result


def _staging(path):
    from pack_materials import staging_output
    staging = staging_output(Path(path))
    if not (staging/'sources').resolve().is_relative_to(staging):
        raise ValueError('sources symlink escapes staging root')
    return staging


def import_batch(staging=DEFAULT_STAGING, registry=registry_module.DEFAULT_REGISTRY, *,
                 textures, mapping=ROOT/'data/content/city-materials.csv', bible=ROOT/'docs/content-style.md',
                 source_root=registry_module.DEFAULT_SOURCE_ROOT, fixes=None, selections=None,
                 reviews=None, failed=()):
    """Import explicit materials without requiring the remaining batches."""
    staging, source_root = _staging(staging), Path(source_root).expanduser().resolve()
    records = registry_module.check_registry(registry, source_root=source_root)
    fresh = recipes(mapping, bible, textures=textures)
    fixes, selections, reviews = fixes or {}, selections or {}, reviews or {}
    names = {recipe['replaces'][0] for recipe in fresh}
    failed = {texture_name(name) for name in failed}
    if (fixes.keys() | selections.keys() | reviews.keys() | failed) - names:
        raise ValueError('import options name a texture outside this batch')
    found = read_sources(staging/'sources', names, source_root)
    old = {record['id']: record for record in records}
    imported = [import_material(recipe, found[recipe['replaces'][0]], staging/'sources', source_root,
        axes=fixes.get(recipe['replaces'][0]), selected=selections.get(recipe['replaces'][0]),
        review=reviews.get(recipe['replaces'][0]), failed=recipe['replaces'][0] in failed,
        previous=old.get(recipe['id'])) for recipe in fresh]
    replacements = {record['id']: record for record in imported}
    output = [replacements.pop(record['id'], record) for record in records]
    output.extend(replacements.values())
    write_registry(registry, output, source_root)
    return imported


def _map_references(value):
    if isinstance(value, dict):
        if isinstance(value.get('files'), dict):
            yield from value['files'].values()
        for child in value.values():
            yield from _map_references(child)
    elif isinstance(value, list):
        for child in value:
            yield from _map_references(child)


def _correct_reused_colour(record, sources):
    """Apply the new mean colour to retained albedos, preserving other maps."""
    generation, params = record['generation'], record['parameters']
    corrected = {}

    def correct(files):
        source = files['albedo']
        if source not in corrected:
            with Image.open(sources/source) as image:
                rgb = np.asarray(image.convert('RGB'), dtype=float)/255
                alpha = np.asarray(image.getchannel('A')) if 'A' in image.getbands() else None
            pixels, correction = correct_mean_colour(rgb, params['target_oklab'],
                mask=alpha >= 128 if alpha is not None else None)
            if alpha is not None:
                pixels = np.dstack((pixels, alpha))
            stream = BytesIO()
            Image.fromarray(pixels).save(stream, format='PNG')
            encoded = stream.getvalue()
            relative = Path(SET)/record['id']/'reuse-colour'/hashlib.sha256(encoded).hexdigest()/'albedo.png'
            _write_immutable(sources/relative, encoded)
            correction.update(input=source, output=relative.as_posix(),
                target=params.get('target_luminance'), range=params['luminance_range'],
                class_range_advisory=True)
            corrected[source] = relative.as_posix(), correction
        output, correction = corrected[source]
        return dict(files, albedo=output), deepcopy(correction)

    for attempt in generation.get('attempts', []):
        if not attempt.get('files'):
            continue
        attempt['files'], correction = correct(attempt['files'])
        attempt['brightness_correction'] = correction
        rejection = generate.numeric_rejection(record, correction)
        attempt.update(numeric_valid=rejection is None, numeric_rejection=rejection)
    files, correction = correct(record['files'])
    rejection = generate.numeric_rejection(record, correction)
    if rejection is not None:
        raise ValueError(f'{record["id"]}: reused albedo fails current colour admission: {rejection}')
    record['files'] = files
    generation.update(brightness_correction=correction,
        saturation_correction=deepcopy(correction['saturation']),
        reuse_colour_correction=deepcopy(correction))


def reuse(staging, from_root, registry=registry_module.DEFAULT_REGISTRY, *, textures,
          mapping=ROOT/'data/content/city-materials.csv', bible=ROOT/'docs/content-style.md',
          source_root=registry_module.DEFAULT_SOURCE_ROOT):
    """Reuse reviewed comparison outputs and their original raw-source history."""
    staging, from_root = _staging(staging), Path(from_root).expanduser().resolve()
    if staging == from_root:
        raise ValueError('reuse requires separate source and destination staging roots')
    records = registry_module.check_registry(registry, source_root=source_root)
    fresh = recipes(mapping, bible, textures=textures)
    names = {recipe['replaces'][0] for recipe in fresh}
    if names & REGENERATE:
        raise ValueError('baked-light comparison materials must be regenerated for city-3')
    comparison = {record['replaces'][0]: record for record in records
        if record['parameters'].get('set') == 'city-2-imagegen' and record['pipeline'] == 'imagegen-filter'
        and record['generation']['outcome'] == 'complete' and len(record['replaces']) == 1}
    if names - comparison.keys():
        raise ValueError('reuse requires completed image_gen comparison materials')
    old = {record['id']: record for record in records}
    imported = []
    for recipe in fresh:
        texture = recipe['replaces'][0]
        original = comparison[texture]
        clone = deepcopy(original)
        clone['id'] = recipe['id']
        clone['parameters'].update(recipe['parameters'])
        clone['generation'].update(reused_from=original['id'], new_tool_calls=0)
        previous = old.get(clone['id'])
        if previous and previous['generation'].get('attempts') and previous['generation'].get('reused_from') != original['id']:
            raise ValueError(f'{texture}: cannot replace retained new generation with a reused selection')
        registry_module.validate_entry(original, content_root=from_root/'sources', source_root=source_root)
        for relative in set(_map_references(clone)):
            safe = registry_module._safe_path(relative, f'{original["id"]}.files')
            source = (from_root/'sources'/safe).resolve()
            if not source.is_relative_to(from_root/'sources'):
                raise ValueError('reused map path escapes source staging root')
            _write_immutable(staging/'sources'/safe, source.read_bytes())
        for relative, _ in registry_module.source_references(clone):
            permanent = (Path(source_root)/relative).resolve()
            retained = staging/'sources'/'raw-sources'/relative
            retained.parent.mkdir(parents=True, exist_ok=True)
            link = os.path.relpath(permanent, retained.parent)
            if retained.exists() and retained.read_bytes() != permanent.read_bytes():
                raise ValueError(f'refusing to replace differing reused source: {retained}')
            if not retained.is_symlink() or os.readlink(retained) != link:
                retained.unlink(missing_ok=True)
                retained.symlink_to(link)
        if 'target_oklab' in clone['parameters']:
            _correct_reused_colour(clone, staging/'sources')
        registry_module.validate_entry(clone, content_root=staging/'sources', source_root=source_root)
        imported.append(clone)
    replacements = {record['id']: record for record in imported}
    output = [replacements.pop(record['id'], record) for record in records]
    output.extend(replacements.values())
    write_registry(registry, output, source_root)
    return imported


def option_pairs(values, *, selection=False):
    result = {}
    for value in values:
        name, separator, choice = value.partition(':')
        name = texture_name(name)
        if not separator or not choice or name in result:
            raise ValueError(f'expected one unique TEXTURE:VALUE option: {value}')
        if selection:
            if choice not in ('1', '2', '3'):
                raise ValueError(f'{name}: selected attempt must be 1, 2 or 3')
            choice = int(choice)
        result[name] = choice
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for name in ('prepare', 'import', 'reuse'):
        command = commands.add_parser(name)
        command.add_argument('--registry', type=Path, default=registry_module.DEFAULT_REGISTRY)
        command.add_argument('--mapping', type=Path, default=ROOT/'data/content/city-materials.csv')
        command.add_argument('--bible', type=Path, default=ROOT/'docs/content-style.md')
        command.add_argument('--source-root', type=Path, default=registry_module.DEFAULT_SOURCE_ROOT)
        if name != 'prepare':
            command.add_argument('--staging', type=Path, default=DEFAULT_STAGING)
            command.add_argument('--texture', action='append', required=True)
        if name == 'reuse':
            command.add_argument('--from-root', type=Path, required=True)
        if name == 'import':
            command.add_argument('--fix', action='append', default=[], metavar='TEXTURE:uv|u|v')
            command.add_argument('--select', action='append', default=[], metavar='TEXTURE:N')
            command.add_argument('--review', action='append', default=[], metavar='TEXTURE:NOTE')
            command.add_argument('--failed', action='append', default=[], metavar='TEXTURE')
    args = parser.parse_args(argv)
    common = dict(registry=args.registry, mapping=args.mapping, bible=args.bible, source_root=args.source_root)
    try:
        if args.command == 'prepare':
            result = prepare(**common)
        elif args.command == 'reuse':
            result = reuse(args.staging, args.from_root, textures=args.texture, **common)
        else:
            result = import_batch(args.staging, textures=args.texture,
                fixes=option_pairs(args.fix), selections=option_pairs(args.select, selection=True),
                reviews=option_pairs(args.review), failed=args.failed, **common)
        if args.command != 'prepare':
            subprocess.run([str(Path(__file__).with_name('backup-sources.sh')),
                '--registry', str(args.registry), '--source-root', str(args.source_root)], check=True)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'city-3 {args.command} failed: {error}', file=sys.stderr)
        return 1
    print(f'city-3 {args.command}: {len(result)} material records')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
