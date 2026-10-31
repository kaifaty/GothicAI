#!/usr/bin/env python3
"""Generate text-only material sets from the mapping CSV and the style bible.

prepare is CPU-only; run uses the pinned bakeoff FLUX adapter on dev-ubuntu.
All attempts, including rejected tiles, remain in each material's registry row.
"""
from __future__ import annotations

import argparse
import csv
from copy import deepcopy
import hashlib
import json
from pathlib import Path
import platform
import re
import time

import bakeoff
from pilot import (MODULE_ASPECT_RATIOS, MODULE_REPEAT_AXES,
    TARGET_LUMINANCE_TOLERANCE, antitile_mode, selected_rows)

ROOT = Path(__file__).resolve().parents[2]

MODULE_SUBSTRATES = dict(timber_frame_tile='plaster', framed_masonry_tile='masonry_rubble',
    window_wall_tile='plaster', bound_timber_tile='timber_beam')


def read_style(path):
    text = path.read_text()
    classes = {}
    for line in text.splitlines():
        cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
        if len(cells) != 6 or not re.fullmatch(r'`[a-z_]+`', cells[0]):
            continue
        def pair(value):
            values = [float(v.replace(',', '.')) for v in re.split('[–—]', value)]
            if len(values) != 2 or not 0 <= values[0] < values[1] <= 1:
                raise ValueError(f'invalid style range: {value}')
            return values
        classes[cells[0].strip('`')] = dict(tile_m=float(cells[2].replace(',', '.')),
            luminance=pair(cells[3]), roughness=pair(cells[4]), prompt=cells[5])
    template = re.search(r'^`(<ядро класса>, <состояние из записи>, .+)`$', text, re.M)
    if not classes or template is None:
        raise ValueError('style bible has no class table or prompt template')
    limits = re.search(r'Средняя насыщенность HSV S.*?не больше ([0-9]+(?:[,\.][0-9]+)?).*?`roof_tiles`.*?не больше ([0-9]+(?:[,\.][0-9]+)?)', text)
    if limits is None:
        raise ValueError('style bible has no HSV saturation limits')
    default_limit, roof_limit = (float(v.replace(',', '.')) for v in limits.groups())
    for name, style in classes.items():
        style['saturation_limit'] = roof_limit if name == 'roof_tiles' else default_limit
    # Module composition is a separate table, with a substrate supplying only
    # numeric roughness/colour guidance. The words always come from its own core.
    for line in text.splitlines():
        cells = [cell.strip() for cell in line.strip().strip('|').split('|')]
        if len(cells) == 7 and cells[0].strip('`') in MODULE_SUBSTRATES:
            name = cells[0].strip('`')
            classes[name] = dict(classes[MODULE_SUBSTRATES[name]], prompt=cells[2])
    return classes, template[1]


def generation_size(aspect):
    """Use physical U/V proportions before the original UV square stretch."""
    import math
    height = 1024 / math.sqrt(aspect)
    width = height * aspect
    if min(width, height) < 256:
        factor = 256 / min(width, height)
        width, height = width * factor, height * factor
    width = int(round(width/16))*16
    # Keep the physical ratio after quantizing the short dimension. Rounding
    # both independently noticeably distorts narrow members such as beams.
    height = int(round(width/aspect/16))*16
    return width, height


def recipe_prompt(row, style, template, set_name):
    if row['decision'] != 'include':
        axes = MODULE_REPEAT_AXES.get(row['texture'], 'uv' if row['layout'] == 'tiled' else '')
        seamless = {'uv': 'seamless at left and right and top and bottom boundaries',
            'u': 'seamless along the horizontal length only, fixed top and bottom boundaries',
            'v': 'seamless along the vertical length only, fixed left and right boundaries',
            '': 'complete bounded panel, fixed boundaries'}[axes]
        return (f'{style["prompt"]}, {row["depicts"]}, {seamless}, '
            'late medieval poor harbour town, weathered, damp, muted earthy colors, '
            'architectural material albedo texture, surface fills the entire image, no background, '
            'front orthographic view, even overcast light, no shadows, no highlights, no text, no logos')
    core = style['prompt']
    prompt = template.replace('<ядро класса>', core).replace('<состояние из записи>', row['state'])
    return prompt.replace(', grime in the cracks', '') if row['class'] in ('dirt_path', 'mud') else prompt


def seed_for(texture, attempt=1):
    # Explicit byte order and encoding make seeds stable across Python/hosts.
    return int.from_bytes(hashlib.sha256(f'{texture.upper()}:{attempt}'.encode('utf-8')).digest()[:4], 'big')


def recipes(mapping, bible, set_name):
    from material_colour import colour_target
    classes, template = read_style(bible)
    with mapping.open(newline='') as stream:
        rows = list(csv.DictReader(stream))
    result = []
    seen = set()
    for row in selected_rows(rows, set_name):
        texture = row['texture']
        if texture in seen:
            raise ValueError(f'duplicate included texture: {texture}')
        seen.add(texture)
        style = classes[row['class']]
        resource_id = set_name + '-' + Path(texture).stem.lower().replace('_', '-')
        prompt = recipe_prompt(row, style, template, set_name)
        result.append(dict(id=resource_id, kind='material', replaces=[texture], pipeline='flux-filter',
            models=[dict(id=bakeoff.MODELS['flux'][0], revision=bakeoff.FLUX_REVISION, license='Apache-2.0')],
            prompt=prompt, negative='', seed=seed_for(texture), status='draft', files={},
            parameters=dict(set=set_name, material=resource_id, material_class=row['class'], state=row['state'],
                scale=float(row['scale']), tile_m=style['tile_m'], luminance_range=style['luminance'],
                saturation_limit=style['saturation_limit'],
                roughness_range=style['roughness'], roughness_base=sum(style['roughness'])/2,
                resolution=1024, steps=4, guidance_scale=0.0, max_sequence_length=256,
                quantization=bakeoff.FLUX_QUANTIZATION, tiling=bakeoff.FLUX_TILING, torch_dtype='bfloat16',
                tileable=True, negative_prompt_used=False, metallic=0,
                height_filter=dict(sigma_small_px_at_1024=2, sigma_large_px_at_1024=32, normal_strength=.025),
                normal_convention='OpenGL: X right, Y up, Z toward viewer; DDS pack flips Y for engine texture-V basis',
                input='text only; no original pixels', seed_algorithm='first 32 bits SHA256(uppercase texture + colon + 1-based attempt), big endian',
                mapping_sha256=hashlib.sha256(mapping.read_bytes()).hexdigest(), bible_sha256=hashlib.sha256(bible.read_bytes()).hexdigest()),
            generation=dict(outcome='pending', attempts=[])))
        if set_name == 'city-2':
            record = result[-1]
            try:
                target = float(row['target_luminance'])
            except (KeyError, ValueError) as error:
                raise ValueError(f'{texture}: city-2 requires target_luminance') from error
            if not 0 < target < 1:
                raise ValueError(f'{texture}: invalid target_luminance')
            is_module = row['decision'] != 'include'
            aspect = MODULE_ASPECT_RATIOS[texture] if is_module else 1.
            axes = MODULE_REPEAT_AXES.get(texture, 'uv' if row['layout'] == 'tiled' else '') if is_module else 'uv'
            width, height = generation_size(aspect)
            record['parameters'].update(decision=row['decision'], layout=row['layout'],
                object=row['object'], role=row['role'], depicts=row['depicts'],
                target_luminance=target, target_luminance_tolerance=TARGET_LUMINANCE_TOLERANCE,
                luminance_policy='original mean linear luminance only; class range is advisory',
                source_aspect=aspect, source_width=width, source_height=height,
                output_width=1024, output_height=1024,
                tiling_axes=axes, tileable=bool(axes),
                tiling='periodic latent roll and VAE padding only along designated UV axes',
                scale=1. if is_module else float(row['scale']),
                antitile=antitile_mode(row), antitile_cell=1.,
                saturation_limit=.25 if row['class'] == 'roof_tiles' else style['saturation_limit'])
        target_colour = colour_target(row)
        if target_colour is not None:
            result[-1]['parameters'].update(target_oklab=target_colour,
                colour_policy='original OKLab of mean linear RGB; per-channel linear gains; no saturation cap')
    if not result:
        raise ValueError('mapping has no included materials')
    return result


def prepare(registry, mapping, bible, set_name):
    records = bakeoff.read_registry(registry) if registry.exists() else []
    existing = {r['id']: r for r in records}
    for recipe in recipes(mapping, bible, set_name):
        if recipe['id'] in existing:
            old = existing[recipe['id']]
            acknowledged = old['generation'].get('prepared_inputs') == input_signature(recipe)
            if not acknowledged and any(old[key] != recipe[key] for key in ('parameters', 'prompt', 'models', 'replaces')):
                raise ValueError(f'{recipe["id"]}: inputs changed; use a new set name')
        else:
            records.append(recipe)
    bakeoff.write_registry(registry, records)
    return records


def input_signature(recipe):
    value = {k: recipe[k] for k in ('parameters', 'prompt', 'models', 'replaces')}
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()


def archive_recipe(record):
    generation = record['generation']
    if 'recipes' not in generation:
        generation['recipes'] = [dict(revision=1, **deepcopy({k: record[k] for k in ('prompt', 'negative', 'models', 'parameters')}))]
        for attempt in generation['attempts']:
            attempt['recipe_revision'] = 1
    return generation['recipes']


def refresh(records, mapping, bible, set_name, textures, reason):
    """Explicitly authorize three new attempts only for the selected textures.

    Other rows retain their actual generation prompt, source maps and seed;
    only their colour postprocess policy changes. Prior recipes remain pinned.
    """
    fresh = {r['id']: r for r in recipes(mapping, bible, set_name)}
    selected = set(textures)
    available = {r['replaces'][0] for r in fresh.values()}
    if not selected or selected - available:
        raise ValueError('refresh requires known include texture names')
    for record in records:
        if record['id'] not in fresh:
            continue
        recipe = fresh[record['id']]
        history = archive_recipe(record)
        generation = record['generation']
        signature = input_signature(recipe)
        if record['replaces'][0] in selected:
            refreshed = generation.get('refreshed_inputs')
            # Older registries acknowledged postprocessing for every row in
            # prepared_inputs. Only an explicitly refreshed recipe granted a
            # generation budget; recover that distinction without rewriting it.
            if refreshed is None and 'refresh_reason' in generation:
                revised = next((r for r in history if r['revision'] == generation.get('recipe_revision')), None)
                if revised is not None:
                    refreshed = input_signature(dict(revised, replaces=record['replaces']))
            if refreshed == signature:
                continue  # Repeating refresh cannot grant another attempt budget.
            generation['prepared_inputs'] = signature
            generation.setdefault('previous_selections', []).append(deepcopy({
                'seed': record['seed'], 'files': record['files'],
                'brightness_correction': generation.get('brightness_correction'),
                'visual_review': generation.get('visual_review'), 'reason': reason}))
            for key in ('prompt', 'negative', 'models', 'parameters'):
                record[key] = deepcopy(recipe[key])
            revision = len(history)+1
            history.append(dict(revision=revision, **deepcopy({k: record[k] for k in ('prompt', 'negative', 'models', 'parameters')})))
            generation.update(outcome='pending', recipe_revision=revision,
                series_start=len(generation['attempts']), attempt_limit=len(generation['attempts'])+3,
                refresh_reason=reason, refreshed_inputs=signature)
            generation.pop('visual_review', None)
            record['files'] = {}
        else:
            if generation.get('prepared_inputs') == signature:
                continue
            generation['prepared_inputs'] = signature
            record['parameters'].update(saturation_limit=recipe['parameters']['saturation_limit'],
                postprocess_bible_sha256=recipe['parameters']['bible_sha256'])
            for key in ('target_oklab', 'colour_policy'):
                if key in recipe['parameters']:
                    record['parameters'][key] = deepcopy(recipe['parameters'][key])


def postprocess(records, root, set_name):
    """Correct selected albedo colour without regenerating spatial detail."""
    import numpy as np
    from PIL import Image
    from material_colour import correct_mean_colour, limit_saturation
    for record in records:
        if record['parameters'].get('set') != set_name or record['generation']['outcome'] != 'complete':
            continue
        generation = record['generation']
        limit = record['parameters']['saturation_limit']
        signature = generation.get('prepared_inputs', '')
        if generation.get('saturation_correction', {}).get('prepared_inputs') == signature:
            continue
        source = root/'out'/record['files']['albedo']
        with Image.open(source) as image:
            rgb = np.asarray(image.convert('RGB'))/255
            if 'target_oklab' in record['parameters']:
                pixels, correction = correct_mean_colour(rgb, record['parameters']['target_oklab'])
            else:
                pixels, correction = limit_saturation(rgb, limit)
        relative = Path(set_name)/record['id']/('colour-'+signature[:12])/'albedo.png'
        target = root/'out'/relative
        target.parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(pixels).save(target)
        correction.update(input=record['files']['albedo'], output=relative.as_posix(), prepared_inputs=signature)
        generation.setdefault('postprocess', []).append(deepcopy(correction))
        if 'target_oklab' in record['parameters']:
            generation['mean_colour_correction'] = correction
            generation['saturation_correction'] = dict(correction['saturation'], prepared_inputs=signature)
        else:
            generation['saturation_correction'] = correction
        record['files']['albedo'] = relative.as_posix()


def corrected_maps(image, record):
    if 'target_luminance' in record['parameters'] or 'target_oklab' in record['parameters']:
        return target_corrected_maps(image, record)
    import numpy as np
    from PIL import Image
    from material_colour import limit_saturation
    rgb = np.asarray(image.convert('RGB'), dtype=np.float32) / 255
    linear = np.where(rgb <= .04045, rgb/12.92, ((rgb+.055)/1.055)**2.4)
    weights = np.array([.2126, .7152, .0722])
    before = float((linear @ weights).mean())
    lo, hi = record['parameters']['luminance_range']
    # Aim at the class midpoint when correcting, leaving room for 8-bit rounding.
    target = (lo+hi)/2 if not lo <= before <= hi else before
    for _ in range(2):
        gain = 1.0
        if target != before:
            left, right = 0., 1.
            while float((np.clip(linear*right, 0, 1) @ weights).mean()) < target and right < 65536:
                right *= 2
            for _ in range(40):
                gain = (left+right)/2
                if float((np.clip(linear*gain, 0, 1) @ weights).mean()) < target:
                    left = gain
                else:
                    right = gain
        adjusted = np.clip(linear*gain, 0, 1)
        encoded = np.where(adjusted <= .0031308, adjusted*12.92, 1.055*adjusted**(1/2.4)-.055)
        pixels = np.rint(encoded*255).astype('uint8')
        normalized = pixels.astype(np.float32)/255
        quantized_linear = np.where(normalized <= .04045, normalized/12.92, ((normalized+.055)/1.055)**2.4)
        after = float((quantized_linear @ weights).mean())
        limited_pixels, saturation = limit_saturation(pixels.astype(np.float64)/255,
            record['parameters']['saturation_limit'])
        # An initially in-range level can cross a boundary after desaturation's
        # 8-bit rounding. Correct that level to the midpoint before deriving maps.
        if lo <= saturation['linear_mean_after'] <= hi or target == (lo+hi)/2:
            break
        target = (lo+hi)/2
    correction = dict(method='uniform linear RGB gain with clipping; no spatial edits',
        linear_mean_before=before, linear_mean_after_level=after, target=target, gain=gain,
        clipped_channel_fraction=float((linear*gain > 1).mean()), range=[lo, hi])
    maps = bakeoff.filtered_maps(Image.fromarray(pixels), record)
    maps['albedo'] = limited_pixels.astype(np.float32)/255
    correction['saturation'] = saturation
    # Admission uses the actual stored albedo after saturation and quantization.
    correction['linear_mean_after'] = saturation['linear_mean_after']
    rlo, rhi = record['parameters']['roughness_range']
    # Quantized endpoints must also lie inside the class range.
    maps['roughness'] = np.clip(maps['roughness'], np.ceil(rlo*255)/255, np.floor(rhi*255)/255)
    return maps, correction, numeric_rejection(record, correction)


def target_corrected_maps(image, record):
    """Match original numeric colour or luminance; never use original pixels."""
    import numpy as np
    from PIL import Image
    from material_colour import correct_mean_colour, limit_saturation
    rgb = np.asarray(image.convert('RGB'), dtype=np.float32)/255
    if 'target_oklab' in record['parameters']:
        pixels, correction = correct_mean_colour(rgb, record['parameters']['target_oklab'])
        lo, hi = record['parameters']['luminance_range']
        correction.update(target=record['parameters'].get('target_luminance'),
            range=[lo, hi], class_range_advisory=True,
            outside_class_range=not lo <= correction['linear_mean_after'] <= hi)
        maps = bakeoff.filtered_maps(Image.fromarray(pixels), record)
        maps['albedo'] = pixels.astype(np.float32)/255
        rlo, rhi = record['parameters']['roughness_range']
        maps['roughness'] = np.clip(maps['roughness'], np.ceil(rlo*255)/255, np.floor(rhi*255)/255)
        return maps, correction, numeric_rejection(record, correction)
    linear = np.where(rgb <= .04045, rgb/12.92, ((rgb+.055)/1.055)**2.4)
    weights = np.array([.2126, .7152, .0722])
    before = float((linear @ weights).mean())
    target = record['parameters']['target_luminance']
    left, right = 0., 1.
    while float((np.clip(linear*right, 0, 1) @ weights).mean()) < target and right < 65536:
        right *= 2
    for _ in range(36):
        gain = (left+right)/2
        if float((np.clip(linear*gain, 0, 1) @ weights).mean()) < target:
            left = gain
        else:
            right = gain
    adjusted = np.clip(linear*gain, 0, 1)
    encoded = np.where(adjusted <= .0031308, adjusted*12.92, 1.055*adjusted**(1/2.4)-.055)
    pixels, saturation = limit_saturation(np.clip(encoded, 0, 1), record['parameters']['saturation_limit'])
    lo, hi = record['parameters']['luminance_range']
    correction = dict(method='uniform linear RGB gain with clipping; no spatial edits',
        linear_mean_before=before, linear_mean_after=saturation['linear_mean_after'],
        target=target, gain=gain, clipped_channel_fraction=float((linear*gain > 1).mean()),
        range=[lo, hi], class_range_advisory=True,
        outside_class_range=not lo <= saturation['linear_mean_after'] <= hi,
        target_tolerance=record['parameters'].get('target_luminance_tolerance', TARGET_LUMINANCE_TOLERANCE),
        saturation=saturation)
    maps = bakeoff.filtered_maps(Image.fromarray(pixels), record)
    maps['albedo'] = pixels.astype(np.float32)/255
    rlo, rhi = record['parameters']['roughness_range']
    maps['roughness'] = np.clip(maps['roughness'], np.ceil(rlo*255)/255, np.floor(rhi*255)/255)
    return maps, correction, numeric_rejection(record, correction)


def save_attempt(root, record, maps, number):
    import numpy as np
    from PIL import Image
    relative = Path(record['parameters']['set']) / record['id'] / f'attempt-{number}'
    directory = root/'out'/relative
    directory.mkdir(parents=True, exist_ok=True)
    for name, value in maps.items():
        Image.fromarray(np.rint(np.clip(value, 0, 1)*255).astype('uint8')).save(directory/f'{name}.png')
    return {name: (relative/f'{name}.png').as_posix() for name in bakeoff.MAPS}


def reject(records, resource_id, reason, explore_remaining=False):
    record = next(r for r in records if r['id'] == resource_id)
    attempts = record['generation']['attempts']
    if record['generation']['outcome'] != 'complete' or not attempts:
        raise ValueError('only a completed tile can be rejected')
    if len(attempts) >= record['generation'].get('attempt_limit', 3):
        if record['parameters'].get('set') == 'city-2':
            attempts[-1].update(outcome='rejected', reason=reason)
            retain_best_candidate(record)
            record['generation']['failure_warning'] = 'Visual review failed at attempt limit: '+reason
            return
        raise ValueError('three attempts exhausted; a decision is required')
    attempts[-1].update(outcome='rejected', reason=reason)
    record['generation']['outcome'] = 'pending'
    record['generation']['explore_remaining'] = explore_remaining
    record['files'] = {}


def numeric_rejection(record, correction):
    """Validate final stored colour measurements, independently of visual review."""
    import math
    if 'target_oklab' in record['parameters']:
        import numpy as np
        from material_colour import TARGET_COLOUR_TOLERANCE, oklab_to_linear
        after_colour = np.asarray(correction.get('linear_rgb_mean_after', []), dtype=float)
        target_colour = oklab_to_linear(record['parameters']['target_oklab'])
        tolerance = record['parameters'].get('target_colour_tolerance', TARGET_COLOUR_TOLERANCE)
        if (after_colour.shape != (3,) or not np.isfinite(after_colour).all() or
                np.max(np.abs(after_colour-target_colour)) > tolerance):
            return 'colour correction could not reach original mean linear RGB within quantization tolerance'
    saturation = correction.get('saturation', {})
    # Old attempts measured luminance before saturation; its own measurement is
    # authoritative when present, including the final 8-bit rounding.
    after = saturation.get('linear_mean_after', correction.get('linear_mean_after'))
    lo, hi = record['parameters']['luminance_range']
    if 'target_oklab' in record['parameters']:
        # Colour measures the engine-visible pixels. Its RGB mean supersedes
        # legacy luminance, which may have used continuous alpha weighting.
        if not isinstance(after, (int, float)) or not math.isfinite(after):
            return 'final mean luminance is not finite or is not measured'
    elif 'target_luminance' in record['parameters']:
        target = record['parameters']['target_luminance']
        tolerance = record['parameters'].get('target_luminance_tolerance', TARGET_LUMINANCE_TOLERANCE)
        if not isinstance(after, (int, float)) or not math.isfinite(after) or abs(after-target) > tolerance:
            return 'brightness correction could not reach original mean within quantization tolerance'
    elif not isinstance(after, (int, float)) or not math.isfinite(after) or not lo <= after <= hi:
        return 'brightness correction could not reach class range'
    clipped = correction.get('clipped_channel_fraction')
    if not isinstance(clipped, (int, float)) or not math.isfinite(clipped) or not 0 <= clipped <= .4:
        return 'more than 40% of colour channels clipped after level correction'
    mean_s = saturation.get('mean_hsv_s_after')
    limit = 1. if 'target_oklab' in record['parameters'] else record['parameters']['saturation_limit']
    if not isinstance(mean_s, (int, float)) or not math.isfinite(mean_s) or not 0 <= mean_s <= limit:
        return 'final mean HSV saturation exceeds class limit or is not measured'
    return None


def retain_best_candidate(record):
    """At the limit, visual compromises must still obey numerical limits."""
    def distance(attempt):
        import math
        correction = attempt.get('brightness_correction', {})
        if 'target_oklab' in record['parameters']:
            import numpy as np
            from material_colour import oklab_to_linear
            after = np.asarray(correction.get('linear_rgb_mean_after', []), dtype=float)
            if after.shape != (3,) or not np.isfinite(after).all():
                return math.inf
            # Rank against the current recipe, just like numerical admission;
            # cached errors or a legacy luminance target may describe old inputs.
            target = oklab_to_linear(record['parameters']['target_oklab'])
            return float(np.max(np.abs(after-target)))
        after = correction.get('saturation', {}).get('linear_mean_after',
            correction.get('linear_mean_after'))
        target = record['parameters'].get('target_luminance', correction.get('target'))
        if not all(isinstance(value, (int, float)) and math.isfinite(value) for value in (after, target)):
            return math.inf
        return abs(after-target)

    generation = record['generation']
    series = generation['attempts'][generation.get('series_start', 0):]
    candidates = []
    for attempt in series:
        reason = numeric_rejection(record, attempt.get('brightness_correction', {}))
        attempt.update(numeric_valid=reason is None, numeric_rejection=reason)
        if attempt.get('files') and reason is None:
            candidates.append(attempt)
    if not candidates:
        details = sorted({a['numeric_rejection'] for a in series if a['numeric_rejection']})
        reason = f'{len(series)} attempts in current series exhausted with no numerically valid candidate'
        if details:
            reason += ': ' + '; '.join(details)
        # City-2 keeps the strongest available result with an explicit warning,
        # so a single difficult surface does not stop the rest of the pilot.
        available = [a for a in series if a.get('files')]
        if record['parameters'].get('set') == 'city-2' and available:
            best = min(available, key=distance)
            select_attempt(record, best, 'Attempt limit reached; best failed candidate retained: '+reason,
                allow_failed=True)
            generation['failure_warning'] = reason
            return None
        generation.update(outcome='pending', failure_reason=reason)
        for key in ('selected_attempt', 'brightness_correction', 'saturation_correction', 'selection_note'):
            generation.pop(key, None)
        record['files'] = {}
        return reason
    best = min(candidates, key=distance)
    select_attempt(record, best, 'Attempt limit reached; best numerically valid candidate retained')
    return None


def select_attempt(record, attempt, note='', allow_failed=False):
    reason = numeric_rejection(record, attempt.get('brightness_correction', {}))
    if reason and not allow_failed:
        raise ValueError(f'{record["id"]}: cannot select numerically invalid attempt: {reason}')
    record.update(seed=attempt['seed'], files=deepcopy(attempt['files']))
    generation = record['generation']
    generation.pop('failure_reason', None)
    generation.update(outcome='complete', selected_attempt=attempt['number'],
        brightness_correction=deepcopy(attempt['brightness_correction']))
    if record['parameters'].get('set') == 'city-2':
        generation['selected_failed'] = reason is not None or attempt.get('outcome') == 'rejected'
    if 'saturation' in attempt['brightness_correction']:
        generation['saturation_correction'] = deepcopy(attempt['brightness_correction']['saturation'])
        generation['saturation_correction']['prepared_inputs'] = generation.get('prepared_inputs', '')
    if note:
        generation['selection_note'] = note


def run(records, registry, root, set_name):
    if platform.system() != 'Linux' or root.resolve() != (Path.home()/'gothic-ai/materials').resolve():
        raise RuntimeError('models run only in ~/gothic-ai/materials on dev-ubuntu')
    pending = [r for r in records if r['parameters'].get('set') == set_name and r['generation']['outcome'] == 'pending']
    # A retry after an exhausted failure must not load FLUX or grant more seeds.
    for record in pending:
        if len(record['generation']['attempts']) >= record['generation'].get('attempt_limit', 3):
            reason = retain_best_candidate(record)
            bakeoff.write_registry(registry, records)
            if reason:
                raise RuntimeError(f'{record["id"]}: {reason}')
    pending = [r for r in pending if r['generation']['outcome'] == 'pending']
    if not pending:
        print('No pending materials', flush=True)
        return
    import torch
    from PIL import Image
    torch.set_num_threads(4)
    torch.backends.cuda.matmul.allow_tf32 = True
    bakeoff.gpu_state(root, set_name, 'load', 10000)
    started = time.monotonic()
    pipe, embeddings, encoding = bakeoff.load_flux(records, root, pending)
    print(f'FLUX ready in {time.monotonic()-started:.1f}s', flush=True)
    for record in pending:
        attempts = record['generation']['attempts']
        while len(attempts) < record['generation'].get('attempt_limit', 3):
            number = len(attempts)+1
            seed = seed_for(record['replaces'][0], number)
            attempt = dict(number=number, seed=seed, outcome='started',
                recipe_revision=record['generation'].get('recipe_revision', 1),
                utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))
            attempts.append(attempt)
            bakeoff.write_registry(registry, records)
            prompt, pooled = embeddings[record['parameters']['material']]
            pipe.transformer._tile_step = 0
            width = record['parameters'].get('source_width', 1024)
            height = record['parameters'].get('source_height', 1024)
            if set_name == 'city-2':
                bakeoff.flux_layout(pipe, width, height, record['parameters']['tiling_axes'])
            torch.cuda.reset_peak_memory_stats()
            start = time.monotonic()
            image = pipe(height=height, width=width, guidance_scale=0., num_inference_steps=4,
                generator=torch.Generator('cpu').manual_seed(seed), prompt_embeds=prompt.to('cuda:0', dtype=pipe.transformer.dtype),
                pooled_prompt_embeds=pooled.to('cuda:0', dtype=pipe.transformer.dtype), max_sequence_length=256).images[0]
            square = (image.resize((1024, 1024), Image.Resampling.LANCZOS)
                if set_name == 'city-2' and image.size != (1024, 1024) else image)
            maps, correction, reason = corrected_maps(square, record)
            files = save_attempt(root, record, maps, number)
            image.save(root/'out'/Path(files['albedo']).parent/'source.png')
            attempt.update(seconds=round(time.monotonic()-start, 3), brightness_correction=correction, files=files,
                outcome='rejected' if reason else 'complete', numeric_valid=reason is None, numeric_rejection=reason,
                peak_allocated_mib=round(torch.cuda.max_memory_allocated()/1024**2,1))
            if set_name == 'city-2':
                attempt.update(source_width=image.width, source_height=image.height,
                    output_width=square.width, output_height=square.height)
            if reason:
                attempt['reason'] = reason
            else:
                select_attempt(record, attempt)
                record['generation'].update(encoding=encoding[record['parameters']['material']], host='dev-ubuntu')
            bakeoff.write_registry(registry, records)
            print(record['id'], json.dumps({k: v for k, v in attempt.items() if k != 'files'}), flush=True)
            if not reason and not record['generation'].get('explore_remaining', False):
                break
        if record['generation']['outcome'] != 'complete':
            reason = retain_best_candidate(record)
            bakeoff.write_registry(registry, records)
            if reason:
                raise RuntimeError(f'{record["id"]}: {reason}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['prepare', 'run', 'reject', 'refresh', 'postprocess'])
    parser.add_argument('--registry', type=Path, default=ROOT/'data/content/registry.jsonl')
    parser.add_argument('--mapping', type=Path, default=ROOT/'data/content/city-materials.csv')
    parser.add_argument('--bible', type=Path, default=ROOT/'docs/content-style.md')
    parser.add_argument('--set', default='city-1', dest='set_name')
    parser.add_argument('--root', type=Path, default=Path.home()/'gothic-ai/materials')
    parser.add_argument('--id')
    parser.add_argument('--reason')
    parser.add_argument('--texture', action='append', default=[])
    args = parser.parse_args()
    if not re.fullmatch('[a-z0-9]+(?:-[a-z0-9]+)*', args.set_name):
        parser.error('unsafe set name')
    if args.command == 'prepare':
        records = prepare(args.registry, args.mapping, args.bible, args.set_name)
        print(f'{sum(r["parameters"].get("set") == args.set_name for r in records)} set materials prepared')
    else:
        records = bakeoff.read_registry(args.registry)
        if args.command == 'refresh':
            if not args.reason:
                parser.error('refresh requires --reason and --texture')
            refresh(records, args.mapping, args.bible, args.set_name, args.texture, args.reason)
            bakeoff.write_registry(args.registry, records)
        elif args.command == 'postprocess':
            postprocess(records, args.root, args.set_name)
            bakeoff.write_registry(args.registry, records)
        elif args.command == 'reject':
            if not args.id or not args.reason:
                parser.error('reject requires --id and --reason')
            reject(records, args.id, args.reason)
            bakeoff.write_registry(args.registry, records)
        else:
            run(records, args.registry, args.root, args.set_name)


if __name__ == '__main__':
    main()
