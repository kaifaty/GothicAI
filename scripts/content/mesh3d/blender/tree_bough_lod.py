"""Measure foliage LOD per primary branch on a fir GLB, without Blender or the game.

The original config and the GLB's seed/scale recover the branch frames and card roles.
Faces, positions, UV and wind channels are checked against the GLB before using those frames.
The comparison uses the unlit albedo and binary cutout at mip 0, with box-integrated screen pixels.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import fir_tree
import tree_density
import tree_glb
import tree_prefix

TREE_PX = (800, 400, 200, 100, 50)
BOUGH_PX = (1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128)
THRESHOLDS = ((48, 16), (32, 8), (16, 4))
OPTIONS = ('single', 'cross', 'three crosses')
BAND = .10
BAKE_PX = 192
RENDER_PX = 256
LUMA = np.array((.2126, .7152, .0722))
BACKGROUND = .18
QUAD = np.array(((0, 1, 2), (0, 2, 3)))
UV = np.array(((0., 0.), (1., 0.), (1., 1.), (0., 1.)))


def direction(vector):
    vector = np.asarray(vector)
    return vector / np.linalg.norm(vector)


def basis(camera):
    # Right, up and depth increasing away from the camera.
    right = direction(np.cross(camera, (0, 0, 1)))
    return np.stack((right, np.cross(right, camera), -camera), axis=1)


BAKE_ANGLE = math.radians(55)
FRONT = np.array((0., math.cos(BAKE_ANGLE), math.sin(BAKE_ANGLE)))
SIDE = np.array((0., math.sin(BAKE_ANGLE), -math.cos(BAKE_ANGLE)))
VIEWS = {
    'front': FRONT, 'side': SIDE, '45 degrees': direction(FRONT + (1., 0., 0.)),
    'below': direction((-.35, -.25, -.9)),
}


@dataclass
class Bough:
    phase: int
    kind: str
    position: np.ndarray
    uv: np.ndarray
    index: np.ndarray

    @property
    def bounds(self):
        return self.position.min(axis=0), self.position.max(axis=0)

    @property
    def span(self):
        return np.ptp(self.position, axis=0)

    @property
    def diameter(self):
        return float(np.linalg.norm(self.span))


def measure_branches(glb: Path, config: dict):
    data = glb.read_bytes()
    length = struct.unpack_from('<I', data, 12)[0]
    extras = json.loads(data[20:20 + length])['meshes'][0]['extras']
    if extras['tree'] != config['id']:
        raise ValueError('The config does not match the GLB tree ID')
    tree = fir_tree.build(config, extras['seed'], extras['scale'])
    parts = tree_glb.read(glb)
    if len(parts) != len(tree.parts):
        raise ValueError('The GLB does not have the original fir materials')
    for part, generated in zip(parts, tree.parts):
        if part['name'] != generated['name'] or any(
                not np.array_equal(part[key], generated[key]) for key in ('position', 'uv', 'index', 'colour')):
            raise ValueError('The GLB does not match the full fir geometry, UVs and wind channels')
    wood, foliage = parts
    position = foliage['position']
    vertex_phase = foliage['colour'][:, 1]
    phases = vertex_phase[foliage['index']]
    if np.any(phases != phases[:, :1]):
        raise ValueError('A foliage triangle belongs to more than one branch')
    branches = tree.branches[tree.stubs:]
    by_phase = {round(branch.phase * 255): branch for branch in branches}
    if len(by_phase) != len(branches) or set(phases[:, 0]) != set(by_phase) | {0}:
        raise ValueError('Wind phases must uniquely identify the living branches')

    boughs, world = [], []
    for phase, branch in by_phase.items():
        indices = foliage['index'][phases[:, 0] == phase]
        vertices, local = np.unique(indices, return_inverse=True)
        az = branch.azimuth
        frame = np.array(((math.cos(az), -math.sin(az), 0.), (math.sin(az), math.cos(az), 0.), (0., 0., 1.)))
        world.append(position[vertices])
        local_positions = (world[-1] - branch.points[0] * extras['scale']) @ frame
        if branch.height >= config['cards']['top'][0]:
            kind = 'top'
        elif branch.whorl < config['whorl']['skirt']:
            kind = 'skirt'
        elif branch.length < 2.:
            kind = 'short'
        else:
            kind = 'main'
        boughs.append(Bough(phase, kind, local_positions, foliage['uv'][vertices], local.reshape(-1, 3)))

    tree_height = np.ptp(np.concatenate([p['position'] for p in parts])[:, 2])
    shapes = []
    angles = np.radians(np.arange(0, 360, 45))
    for elevation in np.radians((0, 30, -30)):
        for angle in angles:
            camera = np.array((np.cos(angle) * np.cos(elevation), np.sin(angle) * np.cos(elevation),
                               np.sin(elevation)))
            projection = basis(camera)[:, :2]
            shapes.append([float(np.ptp(points @ projection, axis=0).max()) for points in world])
    fixed_foliage = int(np.count_nonzero(phases[:, 0] == 0))
    census = dict(sha256=hashlib.sha256(data).hexdigest(), seed=extras['seed'], scale=extras['scale'],
                  height_m=float(tree_height), wood=len(wood['index']), fixed_foliage=fixed_foliage,
                  full=len(wood['index']) + len(foliage['index']), branches=len(boughs), stubs=tree.stubs,
                  cards=tree.metrics['cards_total'], source_atlas_kinds=tree.metrics['cards'],
                  source_roles=tree.metrics['cards_by_role'], types=dict(Counter(b.kind for b in boughs)),
                  hanging_clusters=len({r['cluster'] for r in tree.cards if r['role'] == 'hang'}),
                  end_clusters=len({r['cluster'] for r in tree.cards if r['role'] == 'end'}))
    return boughs, np.array(shapes) / tree_height, census, (foliage, tree)


def budgets(boughs, projected, census):
    diameter = np.array([b.diameter for b in boughs]) / census['height_m']
    tris = np.array([len(b.index) for b in boughs])
    costs = np.column_stack((tris, np.full_like(tris, 12), np.full_like(tris, 4)))
    fixed = census['wood'] + census['fixed_foliage']
    for height in TREE_PX:
        px = diameter * height
        options = {}
        for medium, far in THRESHOLDS:
            stage = (px < medium).astype(int) + (px < far)
            steady = fixed + int(costs[np.arange(len(px)), stage].sum())
            submitted = fixed
            fading = 0
            for index, threshold in enumerate((medium, far)):
                overlap = np.abs(px - threshold) < threshold * BAND
                fading += np.count_nonzero(overlap)
                submitted += int(costs[overlap, index].sum() + costs[overlap, index + 1].sum())
            settled = (np.abs(px - medium) >= medium * BAND) & (np.abs(px - far) >= far * BAND)
            submitted += int(costs[np.flatnonzero(settled), stage[settled]].sum())
            options[f'{medium}/{far}'] = dict(steady=steady, submitted=submitted, fading=int(fading),
                                             stages=np.bincount(stage, minlength=3).tolist())
        quantiles = (0, .1, .5, .9, 1)
        yield dict(tree_px=height, projected=np.quantile(projected * height, quantiles).tolist(),
                   selector=np.quantile(px, quantiles).tolist(), options=options)


@dataclass
class CutNode:
    low: np.ndarray
    high: np.ndarray
    cost: tuple[int, int]  # foliage cards, triangles
    leaves: int  # number of original cards represented
    ready_at: float = math.inf


@dataclass
class CutMerge:
    high: float
    low: float
    before: tuple[int, int]
    after: tuple[int, int]
    leaves: int


def combine_cut_nodes(children):
    return CutNode(np.min([n.low for n in children], axis=0), np.max([n.high for n in children], axis=0),
                   (sum(n.cost[0] for n in children), sum(n.cost[1] for n in children)),
                   sum(n.leaves for n in children), min(n.ready_at for n in children))


def cut_merge(node, target, screen_px, centres, height, ordinal):
    """Schedule a local merge after all descendants have completed their own transitions."""
    diameter = np.linalg.norm(node.high - node.low)
    centre = np.clip(screen_px * height / diameter, *centres)
    stagger = 1. + .24 * ((ordinal * .61803398875) % 1. - .5)
    high = min(centre * stagger * (1 + BAND), node.ready_at)
    low = high * (1 - BAND) / (1 + BAND)
    return CutMerge(high, low, node.cost, target, node.leaves)


def hierarchy_merges(foliage, tree, census):
    """Cost model of a disjoint foliage forest. Nodes use GLB bounds, proxies use assumed budgets."""
    first, _, by_face = tree_prefix.cards(foliage)
    if not np.array_equal(first, [card['first'] for card in tree.cards]):
        raise ValueError('The hierarchy requires the original card ranges')
    lows = np.minimum.reduceat(foliage['position'], first, axis=0)
    highs = np.maximum.reduceat(foliage['position'], first, axis=0)
    costs = np.bincount(by_face, minlength=len(first))
    height = census['height_m']
    clusters = defaultdict(list)
    stems = defaultdict(list)
    tiers = defaultdict(list)
    branches = {b.number: b for b in tree.branches[tree.stubs:]}
    merges, ends = [], []

    def cell(z, azimuth):
        return math.floor(z / 1.5), math.floor(3 * (azimuth % (2 * math.pi)) / (2 * math.pi))

    def combine(children, cards, px, centres):
        node = combine_cut_nodes(children)
        cost = (cards, 2 * cards)
        if cost[0] < node.cost[0] and cost[1] < node.cost[1]:
            merge = cut_merge(node, cost, px, centres, height, len(merges) + 1)
            merges.append(merge)
            return CutNode(node.low, node.high, cost, node.leaves, merge.low)
        return node

    for i, record in enumerate(tree.cards):
        node = CutNode(lows[i], highs[i], (1, int(costs[i])), 1)
        branch = record['branch']
        if branch:
            group = record['cluster']
            clusters[branch, group if group else -i - 1].append(node)
        else:
            x, y, z = (node.low + node.high) / 2
            stems[cell(z, math.atan2(y, x))].append(node)

    boughs = defaultdict(list)
    for (branch, _), children in clusters.items():
        boughs[branch].append(combine(children, 2, 18, (130, 630)))
    for number, children in boughs.items():
        branch = branches[number]
        group = cell(branch.points[0, 2] * census['scale'], branch.azimuth)
        tiers[group].append(combine(children, 6, 30, (85, 420)))
    for group, children in stems.items():
        for i in range(0, len(children), 4):
            tiers[group].append(combine(children[i:i + 4], 2, 18, (130, 630)))
    for children in tiers.values():
        root = combine(children, 4, 22, (55, 115))
        ends.append(cut_merge(root, (0, 0), 15, (38, 58), height, len(ends) + 1))
    return merges, ends


def _merge_curve(merges, heights, full_cost):
    effective = np.tile(full_cost, (len(heights), 1)).astype(float)
    submitted = np.tile(full_cost, (len(heights), 1))
    churn = np.zeros(len(heights))
    impostor = np.zeros(len(heights))
    for merge in merges:
        t = np.clip((merge.high - heights) / (merge.high - merge.low), 0., 1.)
        t = t * t * (3. - 2. * t)
        delta = np.subtract(merge.after, merge.before)
        effective += t[:, None] * delta
        submitted += (t[:, None] > 0) * merge.after - (t[:, None] == 1) * merge.before
        churn += np.abs(np.diff(t, prepend=t[0])) * merge.leaves / full_cost[0]
        if merge.after == (0, 0):
            impostor += t * merge.leaves
    return effective, submitted, churn, impostor / full_cost[0]


def hierarchy_curve(merges, ends, census, levels, heights):
    """Compare weighted costs, actual submissions and original-card turnover at each sample."""
    heights = np.asarray(heights)
    full_cost = (census['cards'], census['full'])
    effective, submitted, churn, impostor = _merge_curve((*merges, *ends), heights, full_cost)
    effective += impostor[:, None] * (1, 2 - census['wood'])
    submitted += (impostor[:, None] > 0) * (1, 2)
    submitted -= np.isclose(impostor[:, None], 1., atol=1e-12, rtol=0.) * (0, census['wood'])

    stage = np.count_nonzero(heights[:, None] < (400, 200, 100, 50, 49), axis=1)
    current = np.array([full_cost, *levels, (1, 2)])[stage]
    current_churn = np.abs(np.diff(stage, prepend=stage[0])) > 0
    return dict(samples_px=heights.tolist(), effective=effective.round(6).tolist(), submitted=submitted.tolist(),
                turnover=churn.tolist(), impostor_share=impostor.tolist(),
                current=current.tolist(), current_turnover=current_churn.astype(int).tolist(),
                local_merges=len(merges), root_transitions=len(ends))


def progressive_merges(nodes, height_m, parents):
    """Whole-tree bands of the saved hierarchy and the cost of its original cards."""
    settled = {}
    merges = []
    cards, triangles = 0, 0
    for node in nodes:
        if not node.children:
            count = node.index_count // 3
            settled[node.id] = (1, count)
            cards += 1
            triangles += count
            continue
        before = tuple(map(sum, zip(*(settled[child] for child in node.children))))
        settled[node.id] = before
        if node.high_px:
            parent = parents[node.id]
            after = (len(parent['position']) // 4, len(parent['index']))
            scale = height_m / node.diameter
            merges.append(CutMerge(node.high_px * scale, node.low_px * scale, before, after, node.leaf_count))
            settled[node.id] = after
    return merges, (cards, triangles)


def turnover_steps(merges, cards, start, end):
    """Weighted share of the original cards changing over each full 1% step from start down to end."""
    steps = math.floor(math.log(end / start) / math.log(.99))
    grid = np.r_[start * .99 ** np.arange(steps + 1), end / .99, end]
    _, _, turnover, _ = _merge_curve(merges, grid, (cards, 0))
    turnover[-2] = 0.  # The jump from the regular grid to the end interval is not a sample.
    if end / .99 > start:
        turnover[-1] = 0.
    return turnover, grid


def progressive_curve(nodes, height_m, parents, wood, level_wood, heights):
    """Measure the saved 3D bands, retaining both geometry ranges during a fade."""
    merges, (cards, triangles) = progressive_merges(nodes, height_m, parents)
    heights = np.asarray(heights)
    full_cost = (cards, triangles + wood)
    effective, submitted, _, _ = _merge_curve(merges, heights, full_cost)
    limits = sorted(level_wood, reverse=True)
    stage = np.count_nonzero(heights[:, None] < limits, axis=1)
    wood_delta = np.asarray([wood, *(level_wood[px] for px in limits)])[stage] - wood
    delta = np.column_stack((np.zeros(len(heights), dtype=int), wood_delta))

    # Use full 1% intervals independently of the requested cost samples.
    turnover, grid = turnover_steps(merges, cards, heights.max(), heights.min())
    peak = int(np.argmax(turnover))
    return dict(samples_px=heights.tolist(), effective=effective.round(6).tolist(), submitted=submitted.tolist(),
                effective_level_wood=(effective + delta).round(6).tolist(),
                submitted_level_wood=(submitted + delta).tolist(), max_turnover=float(turnover[peak]),
                turnover_above_px=float(grid[max(0, peak - 1)]), turnover_below_px=float(grid[peak]))


def sample(texture, u, v):
    height, width = texture.shape[:2]
    return texture[np.clip((v * height).astype(int), 0, height - 1),
                   np.clip((u * width).astype(int), 0, width - 1)]


def raster(surfaces, projection, low, ppm, shape):
    """The front cutout hit, as premultiplied linear RGB and coverage."""
    depth = np.full(math.prod(shape), np.inf, np.float32)
    rgba = np.zeros((len(depth), 4), np.float32)
    for position, index, uv, texture in surfaces:
        points = position @ projection
        points[:, :2] = (points[:, :2] - low) * ppm
        for _, pixels, z, u, v in tree_density.fragments(points, index, shape, uv):
            texels = sample(texture, u, v)
            use = (texels[:, 3] >= .5) & (z < depth[pixels])
            pixels, z, texels = pixels[use], z[use], texels[use]
            # The same pixel can occur more than once in a batch.
            order = np.argsort(z, kind='stable')
            _, first = np.unique(pixels[order], return_index=True)
            winners = order[first]
            dst = pixels[winners]
            depth[dst] = z[winners]
            rgba[dst, :3] = texels[winners, :3]
            rgba[dst, 3] = 1.
    return rgba.reshape(*shape, 4), depth.reshape(shape)


def resize(image, shape):
    width, height = shape[::-1]
    return np.stack([np.asarray(Image.fromarray(channel).resize((width, height), Image.Resampling.BOX))
                     for channel in np.moveaxis(image, -1, 0)], axis=-1)


def bake(bough, texture, planes, segments):
    """One orthographic texture per strip/plane, positioned at its median front-hit depth."""
    ppm = BAKE_PX / bough.diameter
    surfaces = [(bough.position, bough.index, bough.uv, texture)]
    cards = []
    for camera in planes:
        projection = basis(camera)
        xy = bough.position @ projection[:, :2]
        start, end = xy.min(axis=0), xy.max(axis=0)
        cuts = np.linspace(start[0], end[0], segments + 1)
        for left, right in zip(cuts[:-1], cuts[1:]):
            low = np.array((left, start[1]))
            high = np.array((right, end[1]))
            shape = tuple(np.maximum(1, np.ceil((high - low) * ppm).astype(int))[::-1])
            extent = np.array(shape[::-1]) / ppm
            rgba, depth = raster(surfaces, projection, low, ppm * 2, tuple(2 * x for x in shape))
            hits = depth[np.isfinite(depth)]
            if not len(hits):
                continue
            tile = resize(rgba, shape)
            tile[..., :3] /= np.maximum(tile[..., 3:], 1e-8)
            xyz = np.column_stack((low + UV * extent, np.full(4, np.median(hits))))
            cards.append((xyz @ projection.T, QUAD, UV, tile))
    return cards


def templates(boughs):
    for kind in sorted({b.kind for b in boughs}):
        group = [b for b in boughs if b.kind == kind]
        features = np.array([b.span for b in group])
        relative = features / np.median(features, axis=0)
        order = np.argsort(np.linalg.norm(relative - 1., axis=1), kind='stable')
        # One typical bake and one different instance near the 90th percentile of shape deviation.
        yield group[order[0]], group[order[math.ceil(.9 * (len(order) - 1))]]


def fit(cards, source, target):
    low, _ = source.bounds
    target_low, _ = target.bounds
    scale = target.span / source.span
    return [(target_low + (position - low) * scale, index, uv, texture) for position, index, uv, texture in cards]


def difference(full, coarse, footprint):
    alpha_full, alpha_coarse = full[..., 3], coarse[..., 3]
    hit_full, hit_coarse = alpha_full.sum(), alpha_coarse.sum()
    full_luma, coarse_luma = full[..., :3] @ LUMA, coarse[..., :3] @ LUMA
    mean_full = full_luma.sum() / max(hit_full, 1e-8)
    tone = None if hit_coarse == 0 else float((coarse_luma.sum() / hit_coarse / mean_full - 1) * 100)
    contrast = full_luma - coarse_luma + BACKGROUND * (alpha_coarse - alpha_full)
    return dict(open_pp=float((hit_full - hit_coarse) / footprint * 100), tone_pct=tone,
                coverage_px2=float(np.abs(alpha_full - alpha_coarse).sum()),
                luma_px2=float(np.abs(contrast).sum()))


def measure_views(source, target, texture, variants):
    full = [(target.position, target.index, target.uv, texture)]
    proxies = [fit(cards, source, target) for cards in variants]
    ppm = RENDER_PX / target.diameter
    for view, camera in VIEWS.items():
        projection = basis(camera)
        xy = np.concatenate([surface[0] for surface in full + sum(proxies, [])]) @ projection[:, :2]
        low, high = xy.min(axis=0), xy.max(axis=0)
        low -= 4 / ppm
        shape = tuple(np.ceil((high - low) * ppm + 4).astype(int)[::-1])
        renders = [raster(surfaces, projection, low, ppm, shape)[0] for surfaces in (full, *proxies)]
        rows, columns = np.nonzero(renders[0][..., 3])
        footprint_px2 = (np.ptp(rows) + 1) * (np.ptp(columns) + 1) / ppm ** 2
        result = {}
        for px in BOUGH_PX:
            scale = px / target.diameter
            size = tuple(np.maximum(1, np.ceil(np.array(shape) * px / RENDER_PX).astype(int)))
            pictures = [resize(image, size) for image in renders]
            # Box resizing preserves the shared frame aspect up to integer rounding.
            cell_area = math.prod(shape) * (px / RENDER_PX) ** 2 / math.prod(size)
            metrics = {}
            for name, coarse in zip(OPTIONS, pictures[1:]):
                delta = difference(pictures[0], coarse, footprint_px2 * scale ** 2 / cell_area)
                for key in ('coverage_px2', 'luma_px2'):
                    delta[key] *= cell_area
                metrics[name] = delta
            result[px] = metrics
        yield view, result, renders


def atlas_sizes(groups):
    """Estimate power-of-two square atlases at 1 texel/pixel at the top of each transition band."""
    for medium, far in THRESHOLDS:
        rectangles = []
        for source, targets, cards in groups:
            stretch = max(float((target.span / source.span).max()) * source.diameter / target.diameter
                          for target in targets)
            for px, surfaces in ((medium, cards[2]), (far, cards[1])):
                factor = px * (1 + BAND) * stretch / BAKE_PX
                rectangles.extend(tuple(math.ceil(side * factor) + 8 for side in tile.shape[:2])
                                  for _, _, _, tile in surfaces)
        area = sum(h * w for h, w in rectangles)
        size = 2 ** math.ceil(math.log2(max(max(max(r) for r in rectangles), math.sqrt(area))))
        while not fits_atlas(rectangles, size):
            size *= 2
        yield f'{medium}/{far}', dict(tiles=len(rectangles), padded_area=area, side=size)


def fits_atlas(rectangles, side):
    used_height, row_height, row_width = 0, 0, 0
    for height, width in sorted(rectangles, reverse=True):
        if row_width + width > side:
            used_height += row_height
            row_height, row_width = 0, 0
        row_height = max(row_height, height)
        row_width += width
    return used_height + row_height <= side


def screening_limits(results):
    """Largest contiguous sampled size below the spatial coverage and luma error limits."""
    limits = {}
    for label in ('source', 'holdout', 'both'):
        views = [view for row in results if label in (row['sample'], 'both') for view in row['metrics'].values()]
        limits[label] = {}
        for name in OPTIONS:
            passed = 0
            for px in BOUGH_PX:
                if any(view[px][name]['coverage_px2'] > 1. or view[px][name]['luma_px2'] > .10 for view in views):
                    break
                passed = px
            limits[label][name] = passed
    return limits


def save_sheet(path, label, views):
    width, height = 215, 139
    sheet = Image.new('RGB', (95 + 4 * width, 32 + len(views) * height), (246, 246, 246))
    draw = ImageDraw.Draw(sheet)
    for col, title in enumerate(('Full bough', *OPTIONS)):
        draw.text((101 + col * width, 11), title, fill='black')
    for row, (view, renders) in enumerate(views):
        y = 32 + row * height
        draw.text((7, y + 60), view, fill='black')
        for col, rgba in enumerate(renders):
            rgb = rgba[..., :3] + BACKGROUND * (1 - rgba[..., 3:])
            srgb = np.where(rgb <= .0031308, rgb * 12.92, 1.055 * rgb ** (1 / 2.4) - .055)
            image = Image.fromarray(np.uint8(np.clip(srgb, 0, 1) * 255))
            image.thumbnail((width - 5, height - 5), Image.Resampling.LANCZOS)
            sheet.paste(image, (95 + col * width + (width - image.width) // 2, y + (height - image.height) // 2))
    draw.text((7, 11), label, fill='black')
    sheet.save(path, quality=85, optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('glb', type=Path)
    parser.add_argument('--config', type=Path, default=Path(__file__).with_name('fir.json'))
    parser.add_argument('--atlas', type=Path, required=True)
    parser.add_argument('--levels', type=Path, nargs=4, required=True)
    parser.add_argument('--sheets', type=Path)
    args = parser.parse_args()
    boughs, projected, census, (foliage, tree) = measure_branches(args.glb, json.loads(args.config.read_text()))
    with Image.open(args.atlas) as im:
        rgba = np.array(im.convert('RGBA'), dtype=np.float32) / 255
    rgb = rgba[..., :3]
    rgb[:] = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    results, groups = [], []
    for source, holdout in templates(boughs):
        cross = bake(source, rgba, (FRONT, SIDE), 1)
        variants = [cross[:1], cross, bake(source, rgba, (FRONT, SIDE), 3)]
        groups.append((source, [b for b in boughs if b.kind == source.kind], variants))
        for label, target in (('source', source), ('holdout', holdout)):
            metrics, sheet_views = {}, []
            for view, result, renders in measure_views(source, target, rgba, variants):
                metrics[view] = result
                sheet_views.append((view, renders))
            results.append(dict(kind=source.kind, sample=label, phase=target.phase, metrics=metrics))
            if args.sheets is not None:
                args.sheets.mkdir(parents=True, exist_ok=True)
                save_sheet(args.sheets / f'tree-bough-{source.kind}-{label}.jpg',
                           f'{source.kind} {target.phase}', sheet_views)
    parts = [tree_glb.read(path) for path in args.levels]
    levels = [[len(p['index']) for p in level] for level in parts]
    old_costs = [(len(tree_prefix.cards(tree_prefix.split_parts(level)[1])[0]), sum(cost))
                 for level, cost in zip(parts, levels)]
    steps = math.floor(math.log(30 / 800) / math.log(.99))
    heights = np.r_[800 * .99 ** np.arange(steps + 1), 30]
    merges, ends = hierarchy_merges(foliage, tree, census)
    report = dict(census=census, budgets=list(budgets(boughs, projected, census)), current_levels=levels,
                  atlas=dict(atlas_sizes(groups)), screening_px=screening_limits(results), views=results,
                  hierarchy=hierarchy_curve(merges, ends, census, old_costs, heights))
    print(json.dumps(report, indent=1))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
