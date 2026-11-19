"""Build and measure the spruce A hierarchy without Blender or the game."""
from __future__ import annotations

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path

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


def cut(nodes, roots, height_px, height_m):
    """Take the binary cut through the centres of the measured transition bands."""
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
        parts.append(dict(join_parts(group), cutout=True, **textures[page]))
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
    args = parser.parse_args()
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=True)
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
    views = raster.tree_comparisons([source_wood, foliage], level_parts, parts_at, HEIGHTS, jpeg_dir=args.sheets)
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
