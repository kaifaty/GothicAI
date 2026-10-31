"""Physical detail scale policy; reads measurements, never edits staged materials.

One original UV unit occupies ``uv_m_per_unit_median`` metres. A generated
period p at render scale s occupies p * metres / s. Modules retain scale one.
"""
from __future__ import annotations

import math


def target_detail_plan(row, tile_metres=None):
    """Return scale and required detail counts for a future text-only tile.

    A repeating tile needs whole detail counts. Search nearby integer counts
    and one shared scale, accepting only plans within 15% on every known axis.
    The generator must be measured afterwards; this plan is not an acceptance.
"""
    uv = float(row['uv_m_per_unit_median'])
    if not math.isfinite(uv) or uv <= 0:
        raise ValueError('UV metres must be finite and positive')
    module = row['decision'] != 'include'
    if tile_metres is None:
        from map_textures import TILE_METERS
        tile_metres = float(TILE_METERS.get(row['class'], uv))
    if not math.isfinite(tile_metres) or tile_metres <= 0:
        raise ValueError('tile metres must be finite and positive')
    ideal_scale = 1. if module else uv / tile_metres
    counts = {}
    for axis in ('u', 'v'):
        period = float(row[f'target_period_{axis}_uv'])
        if not math.isfinite(period) or not 0 <= period <= 1:
            raise ValueError('target periods must be finite and in [0, 1]')
        counts[axis] = 1 / period if period else 0.
    scale, chosen = ideal_scale, dict(counts)
    if not module and any(counts.values()):
        # Bounded search: the decoder resolves at most 32 periods per UV axis.
        options = {axis: range(1, min(256, max(4, math.ceil(count / ideal_scale * 2) + 3)) + 1)
                   if count else (0,) for axis, count in counts.items()}
        candidates = []
        for nu in options['u']:
            for nv in options['v']:
                proposed = dict(u=nu, v=nv)
                ratios = [count / proposed[axis] for axis, count in counts.items() if count]
                candidate = math.exp(sum(map(math.log, ratios)) / len(ratios))
                error = max(abs(ratio / candidate - 1) for ratio in ratios)
                if error <= .15:
                    distance = abs(math.log(candidate / ideal_scale))
                    # Prefer a modest tile-size change over avoidable detail
                    # distortion (e.g. equal stone periods remain equal).
                    candidates.append((distance + 2 * error, error, candidate, nu, nv))
        if not candidates:
            raise ValueError('no integer detail plan can satisfy both axes within 15%')
        _, _, scale, nu, nv = min(candidates)
        chosen = dict(u=nu, v=nv)
    result = dict(scale=scale, ideal_class_scale=ideal_scale, tile_metres=uv / scale,
                  eligible=row['decision'] != 'exclude')
    for axis in ('u', 'v'):
        result[f'details_{axis}_per_original_uv'] = counts[axis]
        result[f'details_{axis}_per_tile'] = chosen[axis]
        result[f'detail_{axis}_metres'] = uv / counts[axis] if counts[axis] else 0.
        result[f'planned_detail_{axis}_metres'] = uv / (scale * chosen[axis]) if chosen[axis] else 0.
    return result


def match_detail_scale(row, measured, *, tolerance=.15):
    """Find a single scale satisfying every observed axis within ±tolerance.

No period is represented by zero, and is never treated as a measured detail.
Missing periods, conflicting axes and direction mismatches require a new tile;
none can be fixed by silently changing the texture's architectural mapping.
    """
    if not 0 < tolerance < 1:
        raise ValueError('tolerance must be between zero and one')
    ratios, axes, missing = [], [], []
    for axis in ('u', 'v'):
        target = float(row[f'target_period_{axis}_uv'])
        own = float(measured[f'period_{axis}_uv'])
        if not all(math.isfinite(x) and 0 <= x <= 1 for x in (target, own)):
            raise ValueError('periods must be finite and in [0, 1]')
        if target and own:
            ratios.append(own / target)
            axes.append(axis)
        elif target:
            missing.append(axis)
    module = row['decision'] != 'include'
    lower = max((r / (1 + tolerance) for r in ratios), default=0.)
    upper = min((r / (1 - tolerance) for r in ratios), default=math.inf)
    scale = 1. if module or not ratios else math.exp(sum(map(math.log, ratios))/len(ratios))
    if not module and lower <= upper:
        scale = min(max(scale, lower), upper)
    errors = {axis: ratio / scale - 1 for axis, ratio in zip(axes, ratios)}
    expected_direction = row['target_direction']
    direction_mismatch = (expected_direction in ('U', 'V') and
                          measured['direction'] != expected_direction)
    if missing:
        status = 'missing_period'
    elif not ratios:
        status = 'no_period'
    elif direction_mismatch:
        status = 'direction_mismatch'
    elif any(abs(error) > tolerance + 1e-12 for error in errors.values()):
        status = 'regenerate_details' if module else 'incompatible_axes'
    else:
        status = 'within_15_percent'
    return dict(scale=scale, status=status, relative_errors=errors,
                missing_axes=missing, direction_mismatch=direction_mismatch,
                scale_interval=[lower, upper if math.isfinite(upper) else None])


def panel_canvas(physical_u, physical_v, *, max_side=1536, max_stretch=1.25):
    """Use an exact-aspect working canvas assembled from overlapping modules.

The canvas can be larger than an imagegen output: generate square modules,
assemble at native proportions, then resample once to UV storage dimensions.
Sampling into square storage is intentional; the inverse physical UV aspect
restores proportions in game. The limit concerns source art → physical panel.
    """
    if min(physical_u, physical_v) <= 0 or not all(math.isfinite(x) for x in (physical_u, physical_v)):
        raise ValueError('panel dimensions must be finite and positive')
    ratio = physical_u / physical_v
    width, height = (max_side, round(max_side / ratio)) if ratio >= 1 else (round(max_side * ratio), max_side)
    stretch = max((width / height) / ratio, ratio / (width / height))
    if stretch > max_stretch:
        raise ValueError('rounded canvas exceeds maximum stretch')
    return dict(width=width, height=height, source_to_physical_stretch=stretch,
                square_modules=math.ceil(max(ratio, 1 / ratio)),
                policy='overlapping square source modules on exact physical-aspect canvas; resample to UV storage only after assembly')
