"""Build and measure the spruce A hierarchy without Blender or the game."""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path
import sys

import numpy as np
from PIL import Image

import fir_hierarchy
import tree_bough_lod as lod
import tree_glb
import tree_hierarchy_bake as bake
import tree_hierarchy_raster as raster
import tree_prefix

HEIGHTS = (800, 400, 300, 200, 150, 100, 70, 50)
LEVEL_HEIGHTS = (400, 200, 100, 50)
EYE_FACTORS = (4, 8, 16)
# Merge moments of the parents of original cards, log-uniform in whole-tree pixels.
# The geometric mean, 200 px, stays at the median of the accepted ×4 hang and end bands.
STAGGER_PX = (800., 50.)
STAGGER_GAP = 2.  # octaves below the latest moment an older node may wait
# Spruce A's impostor (49 px) takes over completely below 46 px and starts below 56 px.
SOLID_PX = 56


def read_rgba(path):
    with Image.open(path) as image:
        rgba = np.asarray(image.convert('RGBA'), dtype=np.float32) / 255
    rgb = rgba[..., :3]
    rgb[:] = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    return rgba


def read_normal(path):
    """Decode the source DXT5nm layout (R and B are unused)."""
    with Image.open(path) as image:
        xy = np.asarray(image.convert('RGBA'), dtype=np.float32)[..., [3, 1]] * (2 / 255) - 1
    z = np.sqrt(np.maximum(0, 1 - np.sum(xy ** 2, axis=-1)))
    return np.dstack((xy, z))


def schedule(nodes, fits, parents, height_m, certify):
    """Schedule certified bands and leave the children active when a proxy saves neither cost."""
    merges = []
    # The costs describe a subtree after all its accepted merges are complete.
    states = {}
    for node in nodes:
        if not node.children:
            states[node.id] = ((1, node.index_count // 3), math.inf)
            continue
        children = [states[child] for child in node.children]
        before = tuple(map(sum, zip(*(cost for cost, _ in children))))
        ready_px = min(low_px for _, low_px in children)
        states[node.id] = (before, ready_px)
        fit = fits[node.id]
        node.max_px = fit.max_px
        after = (len(fit.cards), len(parents[node.id]['index']))
        if not fit.max_px or any(a >= b for a, b in zip(after, before)):
            continue

        # Leave room for both the +12% ID stagger and the upper edge of the band.
        stagger = 1 + .24 * ((node.id * .61803398875) % 1 - .5)
        # Keep node-local bounds decreasing toward the root. A parent's larger
        # box also puts its band below the descendants in whole-tree pixels.
        upper_limit = min(fit.max_px, ready_px)
        centre = min(fit.target_px, upper_limit / (1.12 * (1 + lod.BAND))) * stagger
        high = certify(node, centre * (1 + lod.BAND))
        if not high:
            continue
        centre = high / (1 + lod.BAND)
        node.threshold_px = centre
        node.high_px = high
        node.low_px = centre * (1 - lod.BAND)
        scale = height_m / node.diameter
        merges.append(lod.CutMerge(node.high_px * scale, node.low_px * scale, before, after, node.leaf_count))
        states[node.id] = (after, node.low_px)
    return merges


def stable_unit(ident):
    """A fixed pseudo-random fraction for a node ID (the splitmix64 finalizer)."""
    z = (ident * 0x9E3779B97F4A7C15 + 0x9E3779B97F4A7C15) % 2 ** 64
    z = (z ^ z >> 30) * 0xBF58476D1CE4E5B9 % 2 ** 64
    z = (z ^ z >> 27) * 0x94D049BB133111EB % 2 ** 64
    return (z ^ z >> 31) / 2 ** 64


def stagger(nodes, height_m, ratio):
    """Spread the merge moments of the enabled bands over the whole flight in whole-tree pixels.

    Each level of the hierarchy, counted up from the original cards, takes the order of its moments from
    a hash of the node IDs, so that neighbours in the crown do not merge one after another. Parents of
    cards are spread evenly in log height over STAGGER_PX. An older node merges from zero to STAGGER_GAP
    octaves below the latest moment at which its band stays under the lower edges of all bands below it
    in node-local pixels, which is the loader's rule for an ancestor chain.
    """
    band = (ratio - 1) / (ratio + 1)
    level, ready = {}, {}
    for node in nodes:
        level[node.id] = 1 + max(level[child] for child in node.children) if node.children else 0
    order = defaultdict(list)
    for node in nodes:
        if node.children and node.high_px:
            order[level[node.id]].append(node.id)
    unit = {ident: (rank + .5) / len(ids) for ids in order.values()
            for rank, ident in enumerate(sorted(ids, key=stable_unit))}
    top, bottom = STAGGER_PX
    for node in nodes:
        # Lowest band edge below the node in its own pixels.
        ready[node.id] = min((ready[child] for child in node.children), default=math.inf)
        if not node.children or not node.high_px:
            continue
        scale = node.diameter / height_m
        latest = ready[node.id] / (1 + band) / scale
        if math.isinf(latest):
            moment = top * (bottom / top) ** unit[node.id]
        else:
            moment = latest * 2 ** (-STAGGER_GAP * unit[node.id])
        node.threshold_px = moment * scale
        node.high_px = node.threshold_px * (1 + band)
        node.low_px = node.threshold_px * (1 - band)
        ready[node.id] = node.low_px


def cut(nodes, roots, height_px, height_m, *, k=1):
    """Take the binary cut through the centres of the measured transition bands."""
    height_px /= k
    selected = []
    pending = list(roots)
    while pending:
        node = nodes[pending.pop() - 1]
        if node.children and height_px * node.diameter > node.threshold_px * height_m:
            pending.extend(node.children)
        else:
            selected.append(node.id)
    return sorted(selected)


def join_parts(parts):
    """Concatenate different ranges sampling a single atlas page."""
    columns = ('position', 'normal', 'uv', 'colour')
    first = np.cumsum([0, *(len(part['position']) for part in parts[:-1])])
    return dict({key: np.concatenate([part[key] for part in parts]) for key in columns},
                index=np.concatenate([part['index'] + offset for part, offset in zip(parts, first)]))


def cut_parts(nodes, ids, foliage, parents, textures):
    leaves = [node_id for node_id in ids if not nodes[node_id - 1].children]
    parts = [dict(fir_hierarchy.leaf_part(foliage, nodes, leaves),
                  rgba=foliage['rgba'], tangent_normal=foliage['tangent_normal'])] if leaves else []
    pages = defaultdict(list)
    for node_id in ids:
        if node_id in parents:
            part = parents[node_id]
            pages[part['page']].append(part)
    for page, group in sorted(pages.items()):
        parts.append(dict(join_parts(group) | textures[page], cutout=True))
    return parts


def read_pages(output, atlas):
    pages = []
    for page in atlas['pages']:
        rgba = read_rgba(output / page['rgba'])
        with Image.open(output / page['attributes']) as image:
            attributes = np.asarray(image.convert('RGBA'), dtype=np.float32) / 255
        attributes[..., :3] = attributes[..., :3] * 2 - 1
        pages.append(dict(rgba=rgba, attributes=attributes))
    return pages


def compare_merge(node, fit, size_px, children, original):
    """Check the upper edge against both the scheduled children and the original leaves."""
    views = [bake.viewport((node.rest_low, node.rest_high), plane, bake.SCREEN / node.diameter)
             for plane in fit.holdouts]
    renders = [raster.raster(fit.cards, *view)[0] for view in views]
    references = [raster.raster(children, *view)[0] for view in views]
    original_refs = references if original is children else [raster.raster(original, *view)[0] for view in views]
    # Check the exact staggered edge as well as smaller grid points. If the edge
    # fails, choose a smaller measured size without rebuilding the candidate.
    sizes = [px for px in bake.SIZES if px < size_px] + [size_px]
    high, failure_px, failure = bake.screen(references + original_refs, renders * 2, sizes)
    diagnostic_px = high or size_px
    summary = bake.summarize(references, renders, diagnostic_px)
    worst = max(range(len(views)), key=lambda i: summary['views'][i]['luma_px2'])
    target, _ = bake.dimensions(references[worst].shape[:2], diagnostic_px)
    pair = [raster.resize(images[worst], target) for images in (references, renders)]
    return high, dict(requested_px=size_px, compared_px=diagnostic_px, failure_px=failure_px, failure=failure,
                      children=summary, original=bake.summarize(original_refs, renders, diagnostic_px)), pair


def save_node_examples(examples, destination):
    pairs = []
    for kind, group in sorted(examples.items()):
        active = [row for row in group if row[0]]
        if len(active) >= 2:
            group = active
        group.sort(key=lambda row: row[:3])
        picks = sorted({len(group) // 4, 3 * len(group) // 4})
        for i in picks:
            bound, size, node_id, pair = group[i]
            label = f'{kind} #{node_id}   {size:.2f} px'
            pairs.append((label if bound else label + ' (inactive)', *pair))
    raster.node_sheet(pairs, destination)


def parts_for_comparison(parts, crown, bark):
    return [dict(part, **(crown if part['material'].get('alphaMode') == 'MASK' else bark))
            for part in parts]


def read_eye_hierarchy(directory, source_path, foliage, *, k=1):
    """Recover the exact saved cut thresholds and parent geometry from the hierarchy probe."""
    saved = json.loads((directory / 'hierarchy.json').read_text())
    if hashlib.sha256(source_path.read_bytes()).hexdigest() != saved['source']['source']:
        raise ValueError('The hierarchy belongs to a different source GLB')
    primitives = tree_glb.read(directory / saved['glb'])
    nodes, parents = [], {}
    for record in saved['nodes']:
        ident, children = record['id'], tuple(record['children'])
        geometry = record['geometry']
        first, count = geometry['first_vertex'], geometry['vertex_count']
        first_index, index_count = geometry['first_index'], geometry['index_count']
        vertices = slice(first, first + count)
        low, high = np.asarray(record['box'])
        if children:
            leaves = tuple(leaf for i in children for leaf in nodes[i - 1].leaves)
            rest_low = np.min([nodes[i - 1].rest_low for i in children], axis=0)
            rest_high = np.max([nodes[i - 1].rest_high for i in children], axis=0)
            primitive = primitives[geometry['primitive']]
            faces = slice(first_index // 3, (first_index + index_count) // 3)
            parent = {key: primitive[key][vertices] for key in fir_hierarchy.VERTEX_FIELDS}
            parent['index'] = primitive['index'][faces] - first
            parent['uv'] = np.tile(lod.UV, (count // 4, 1)).astype(np.float32)
            parents[ident] = parent
        else:
            positions = foliage['position'][vertices]
            leaves = (ident,)
            rest_low, rest_high = positions.min(axis=0), positions.max(axis=0)
        nodes.append(fir_hierarchy.Node(ident, record['kind'], children, leaves, record['branch'],
                                       rest_low, rest_high, low, high, first, count, first_index,
                                       index_count, max_px=record['max_px'],
                                       threshold_px=k * record['threshold_px'], high_px=k * record['high_px'],
                                       low_px=k * record['low_px']))
    return nodes, saved['roots'], parents, saved['source']['height_m']


def eye_material(name, k):
    return f'{name}_EYE_{k}'


def write_eye_level(path, wood, parts, k):
    name = eye_material(parts[0]['name'], k)
    needles = dict(join_parts(parts), material=dict(parts[0]['material'], name=name), name=name,
                   preserve_normals=True)
    tree_glb.write(path, [dict(wood, preserve_normals=True), needles])


def eye_levels(args, output):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from fir_eye_frames import FOV_DEGREES, SCENARIO
    from fir_set import EYE_ROW

    start = next(line.split() for line in SCENARIO.read_text().splitlines() if line.startswith('cam at '))
    eye, aim = np.asarray(start[2:8], dtype=float).reshape(2, 3) / 100
    outward = lod.direction(eye - aim)
    focal = 540 / math.tan(math.radians(FOV_DEGREES / 2))
    source = tree_glb.read(args.glb)
    crown = dict(rgba=read_rgba(args.atlas), tangent_normal=read_normal(args.normal))
    wood, foliage = parts_for_comparison(source, crown, dict(rgba=read_rgba(args.bark)))
    positions = np.concatenate([part['position'] for part in source])
    centre = (positions.min(axis=0) + positions.max(axis=0)) * .5
    nodes, roots, parents, height_m = read_eye_hierarchy(args.eye, args.glb, foliage)
    level_wood = {height: len(tree_prefix.split_parts(tree_glb.read(path))[0]['index'])
                  for height, path in zip(LEVEL_HEIGHTS, args.levels)}
    alternatives, costs = {}, {}
    for k, root in zip(EYE_FACTORS, EYE_ROW[1:], strict=True):
        cuts = {height: cut(nodes, roots, height, height_m, k=k) for height in LEVEL_HEIGHTS}
        px_per_m = {}
        for height, ids in cuts.items():
            for ident in ids:
                if ident in parents:
                    # On the return leg a level persists to 110% of its nominal onset.
                    px_per_m.setdefault(ident, 1.1 * height / height_m)
        print(f'k={k}: baking {len(px_per_m)} selected parent nodes', flush=True)
        folder = output / f'eye-{k}'
        camera = (aim, outward, np.asarray(root) / 100, centre, focal)
        needles, proxies, atlas_bytes = bake.rebake_variant(folder, foliage, nodes, parents, px_per_m,
                                                           args.atlas.parent, jobs=args.jobs, camera=camera)
        write_eye_level(folder / 'base.glb', wood, [needles], k)
        alternatives[k], costs[k] = {}, {}
        for number, (height, ids) in enumerate(cuts.items(), 1):
            leaves = [ident for ident in ids if not nodes[ident - 1].children]
            merged = [proxies[ident] for ident in ids if nodes[ident - 1].children]
            if merged:
                merged = [dict(merged[0], **join_parts(merged))]
            source_leaves = fir_hierarchy.leaf_part(foliage, nodes, leaves)
            write_eye_level(folder / f'L{number}.glb', wood,
                            [fir_hierarchy.leaf_part(needles, nodes, leaves), *merged], k)
            alternatives[k][height] = [wood, source_leaves, *merged]
            triangles = sum(len(part['index']) for part in (source_leaves, *merged))
            cards = len(leaves) + sum(len(part['position']) // 4 for part in merged)
            costs[k][height] = dict(cards=cards, triangles=triangles + len(wood['index']),
                                    current_wood=triangles + level_wood[height])
        print(f'k={k}: atlas {atlas_bytes} bytes', flush=True)

    print('Measuring 24 whole-tree cameras', flush=True)
    views = raster.tree_comparisons([wood, foliage], alternatives)
    for k, heights in views.items():
        for height, rows in heights.items():
            modes = [mode for row in rows for mode in (row['unlit'], *row['lights'])]
            ranges = {}
            for name, samples in (('open_pp', rows), ('tone_pct', modes),
                                  ('bright_pp', modes), ('mask_residual_pp', rows)):
                values = [row[name] for row in samples]
                ranges[name] = [min(values), max(values)]
            print(json.dumps(dict(k=k, height=height, views=len(rows), **costs[k][height], **ranges)), flush=True)


def level_spread(nodes, height_m):
    """Merge moments in whole-tree pixels per level, counted up from the original cards."""
    level, moments = {}, defaultdict(list)
    for node in nodes:
        level[node.id] = 1 + max(level[child] for child in node.children) if node.children else 0
        if node.children and node.high_px:
            moments[level[node.id]].append(node.threshold_px * height_m / node.diameter)
    spread = {}
    for depth, values in sorted(moments.items()):
        values = np.sort(values)
        octave = np.searchsorted(values, 2 * values, side='left') - np.arange(len(values))
        spread[depth] = dict(nodes=len(values), geometric_mean_px=float(np.exp(np.log(values).mean())),
                             p5_p50_p95_px=np.percentile(values, (5, 50, 95)).tolist(),
                             octave_max_share=float(octave.max() / len(values)))
    return spread


def band_measures(nodes, height_m, parents):
    """Turnover per 1% of height over the 3D range and with the solid crown only, and the level spread."""
    merges, (cards, _) = lod.progressive_merges(nodes, height_m, parents)
    turnover = {}
    for low in (30, SOLID_PX):
        steps, grid = lod.turnover_steps(merges, cards, 800, low)
        regular = steps[1:-2]
        peak = int(np.argmax(regular)) + 1
        turnover[f'{low}-800'] = dict(max_pct=100 * float(steps[peak]), p95_pct=100 * float(np.percentile(regular, 95)),
                                      above_px=float(grid[peak - 1]), below_px=float(grid[peak]))
    return dict(turnover=turnover, levels=level_spread(nodes, height_m))


def bough_nodes(args, foliage):
    """Read the strict hierarchy with its bands scaled by k or rescheduled by the staggered law."""
    if args.stagger:
        nodes, roots, parents, height_m = read_eye_hierarchy(args.bough, args.glb, foliage)
        stagger(nodes, height_m, args.stagger)
        return nodes, roots, parents, height_m, f'S{round(100 * args.stagger)}'
    return (*read_eye_hierarchy(args.bough, args.glb, foliage, k=args.k), f'X{args.k}')


def bough(args, output):
    source = tree_glb.read(args.glb)
    crown = dict(rgba=read_rgba(args.atlas), tangent_normal=read_normal(args.normal))
    wood, foliage = parts_for_comparison(source, crown, dict(rgba=read_rgba(args.bark)))
    nodes, roots, parents, height_m, label = bough_nodes(args, foliage)
    if args.bands_only:
        print(json.dumps(band_measures(nodes, height_m, parents), allow_nan=False))
        return
    px_per_m = {ident: nodes[ident - 1].high_px / nodes[ident - 1].diameter for ident in parents}
    print(f'Baking all {len(parents)} parent nodes', flush=True)
    fits = bake.rebake_cards(foliage, nodes, parents, px_per_m, jobs=args.jobs, samples=4)
    mapped, atlas, textures, coverage, area = bake.pack_bough(output, fits, parents)
    unpadded = sum(card['rgba'].shape[0] * card['rgba'].shape[1] for cards in fits.values() for card in cards)
    sizes = fir_hierarchy.export(output / f'OWN_TREE_FIR_A_BOUGH_{label}_H.glb', source, nodes, roots, mapped, atlas,
                                 metadata=dict(height_m=height_m, source=hashlib.sha256(args.glb.read_bytes()).hexdigest()),
                                 label=label)
    print(f"{atlas['tiles']} cards on {len(atlas['pages'])} pages, {atlas['bytes']} DDS bytes", flush=True)

    level_wood = {height: len(tree_prefix.split_parts(tree_glb.read(path))[0]['index'])
                  for height, path in zip(LEVEL_HEIGHTS, args.levels, strict=True)}
    steps = math.floor(math.log(30 / 800) / math.log(.99))
    curve = lod.progressive_curve(nodes, height_m, mapped, len(wood['index']), level_wood,
                                  sorted(set(800 * .99 ** np.arange(steps + 1)) | {30, *HEIGHTS}, reverse=True))
    cuts = {height: [wood, *cut_parts(nodes, cut(nodes, roots, height, height_m), foliage, mapped, textures)]
            for height in HEIGHTS if height != 800}
    print(f'Measuring hard centre cuts at {len(cuts)} heights in 24 cameras', flush=True)
    views = raster.tree_comparisons([wood, foliage], {'cut': cuts}, jpeg_dir=args.sheets,
                                    sheet_prefix=f'tree-bough-{label.lower()}',
                                    sheet_label=f'Stagger {args.stagger:.2f}' if args.stagger else f'Cut ×{args.k}')
    result = dict(views=views['cut'], curve=curve, coverage=coverage, atlas=atlas, sizes=sizes,
                  padded_texels=area, content_texels=unpadded)
    if args.stagger:
        result['bands'] = band_measures(nodes, height_m, parents)
    (output / 'result.json').write_text(json.dumps(result, allow_nan=False, separators=(',', ':')) + '\n')
    print(f'Data and metrics in {output}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('glb', type=Path)
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('fir.json'))
    parser.add_argument('--atlas', type=Path, required=True)
    parser.add_argument('--normal', type=Path, required=True)
    parser.add_argument('--bark', type=Path, required=True)
    parser.add_argument('--levels', type=Path, nargs=4, required=True)
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--sheets', type=Path)
    parser.add_argument('--jobs', type=int, default=1)
    parser.add_argument('--k', type=int, choices=EYE_FACTORS, default=4, help='tolerance multiplier for --bough')
    parser.add_argument('--stagger', type=float, metavar='RATIO',
                        help='for --bough: staggered merge moments with this high/low band ratio instead of --k')
    parser.add_argument('--bands-only', action='store_true', help='for --bough: print the band measures and stop')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--eye', type=Path, help='read a saved hierarchy and bake and measure its relaxed cuts')
    mode.add_argument('--bough', '--bough-x4', type=Path, help='reprint all parents for the scaled bands')
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if args.eye:
        eye_levels(args, output)
        return
    if args.bough:
        bough(args, output)
        return
    if args.sheets:
        args.sheets.mkdir(parents=True, exist_ok=True)

    config = json.loads(args.config.read_text())
    boughs, _, census, (foliage, tree) = lod.measure_branches(args.glb, config)
    source = tree_glb.read(args.glb)
    nodes, roots = fir_hierarchy.build(foliage, tree, census)
    crown = dict(rgba=read_rgba(args.atlas), tangent_normal=read_normal(args.normal))
    bark = dict(rgba=read_rgba(args.bark))
    source_wood, foliage = parts_for_comparison(source, crown, bark)
    level_source = [tree_glb.read(path) for path in args.levels]
    level_costs = [(len(tree_prefix.cards(tree_prefix.split_parts(parts)[1])[0]),
                    sum(len(part['index']) for part in parts)) for parts in level_source]
    level_parts = {height: parts_for_comparison(parts, crown, bark)
                   for height, parts in zip(LEVEL_HEIGHTS, level_source)}
    print(f'Fitting {len(nodes) - census["cards"]} parents on {args.jobs} workers', flush=True)
    fits = bake.fit_all(nodes, foliage, crown['rgba'], jobs=args.jobs)
    parents, atlas = bake.pack(output, fits)
    print('Parent fits and atlas complete', flush=True)
    height_m = census['height_m']
    textures = read_pages(output, atlas)
    reports, examples = {}, defaultdict(list)

    def certify(node, high):
        below = cut(nodes, node.children, high * height_m / node.diameter, height_m)
        children = cut_parts(nodes, below, foliage, parents, textures)
        original = children if len(below) == node.leaf_count else cut_parts(nodes, node.leaves, foliage, parents, textures)
        passed, result, pair = compare_merge(node, fits[node.id], high, children, original)
        reports[node.id] = result
        if args.sheets:
            examples[node.kind].append((passed, result['compared_px'], node.id, pair))
        return passed

    print('Certifying band edges against finished child cuts', flush=True)
    merges = schedule(nodes, fits, parents, height_m, certify)
    sizes = fir_hierarchy.export(output / 'hierarchy.glb', source, nodes, roots, parents, atlas,
                                 metadata=dict(height_m=height_m, source=census['sha256']))

    def parts_at(height):
        ids = cut(nodes, roots, height, height_m)
        if len(ids) == census['cards'] and ids[-1] == census['cards']:
            return [source_wood, foliage]
        return [source_wood, *cut_parts(nodes, ids, foliage, parents, textures)]

    if args.sheets:
        for kind in {node.kind for node in nodes if node.children}:
            if len(examples[kind]) >= 2:
                continue
            sampled = {row[2] for row in examples[kind]}
            for node in nodes:
                if node.kind != kind or node.id in sampled:
                    continue
                fit = fits[node.id]
                screen_px = fit.max_px or bake.SIZES[0]
                original = cut_parts(nodes, node.leaves, foliage, parents, textures)
                _, result, pair = compare_merge(node, fit, screen_px, original, original)
                examples[kind].append((0., result['compared_px'], node.id, pair))
                if len(examples[kind]) >= 2:
                    break
        save_node_examples(examples, args.sheets / 'tree-hierarchy-nodes.jpg')

    types = dict((bough.phase, bough.kind) for bough in boughs)
    transfer_groups = defaultdict(list)
    for node in nodes:
        if node.kind == 'bough':
            phase = int(foliage['colour'][nodes[node.leaves[0] - 1].first, 1])
            transfer_groups[types[phase]].append(node.id)
    transfer = bake.measure_transfer(nodes, foliage, crown['rgba'], fits, transfer_groups)
    print(f'Measuring 24 whole-tree cameras with {len(merges)} certified bands', flush=True)
    cuts = {height: parts_at(height) for height in HEIGHTS}
    views = raster.tree_comparisons([source_wood, foliage], {'cut': cuts, 'levels': level_parts}, jpeg_dir=args.sheets)
    steps = math.floor(math.log(30 / 800) / math.log(.99))
    heights = np.r_[800 * .99 ** np.arange(steps + 1), 30]
    model_merges, model_ends = lod.hierarchy_merges(foliage, tree, census)
    model = lod.hierarchy_curve(model_merges, model_ends, census, level_costs, heights)
    measured = lod.hierarchy_curve(merges, [], census, level_costs, heights)
    comparison = lod.hierarchy_curve(merges, [], census, level_costs, HEIGHTS)
    exact_model = lod.hierarchy_curve(model_merges, model_ends, census, level_costs, HEIGHTS)
    measured['exact'] = comparison
    model['exact'] = exact_model
    comparison['cut'] = []
    for height in HEIGHTS:
        ids = cut(nodes, roots, height, height_m)
        comparison['cut'].append([
            sum(len(fits[i].cards) if i in fits else 1 for i in ids),
            census['wood'] + sum(len(parents[i]['index']) if i in parents else nodes[i - 1].index_count // 3
                                 for i in ids)])
    report = dict(census=census, level_costs=level_costs, views=views, atlas=atlas, sizes=sizes,
                  fits={node.id: dict(fits[node.id].metrics, scheduled_px=node.high_px,
                                      certification=reports.get(node.id))
                        for node in nodes if node.children},
                  measured=measured, model=model, transfer=transfer)
    (output / 'result.json').write_text(json.dumps(report, allow_nan=False, separators=(',', ':')) + '\n')
    if args.sheets:
        raster.cost_sheet(measured, model, args.sheets / 'tree-hierarchy-cost.jpg')
    print(f'Data and metrics in {output}', flush=True)


if __name__ == '__main__':
    main()
