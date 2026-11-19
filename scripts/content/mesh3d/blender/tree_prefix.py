"""Measure ranked prefixes of a full spruce GLB against its four designed levels, without the game."""
from __future__ import annotations

import argparse
import heapq
import json
from pathlib import Path

import numpy as np

import tree_density
import tree_glb

RAYS = tree_density.RAYS
LUMA = np.array((.2126, .7152, .0722))
# Extra samples on either side of the nominal budget, with intermediate heights between the old switches.
SWEEP = {
    400: (.20, .25, .33, .50, .67, .80),
    300: (.25, .33),
    200: (.08, .125, .1875, .25, .33, .50, .80),
    150: (.125, .1875),
    100: (.04, .0625, .09375, .125, .1875, .25, .50, .80),
    75: (.0625, .09375),
    50: (.025, .04, .0625, .09375, .125, .1875, .25, .50, .80),
}
CHECKPOINTS = (400, 200, 100, 50)
VIEWS = ((0, 0), (180, 0), (90, 0), (270, 0), (0, 30))
ORDER_VIEWS = ((30, 0), (120, 0), (210, 0), (300, 0), (45, 25), (225, -15))


def split_parts(parts):
    wood, = (part for part in parts if part['material'].get('alphaMode') != 'MASK')
    leaves, = (part for part in parts if part['material'].get('alphaMode') == 'MASK')
    return wood, leaves


def cards(leaves):
    """The first vertex of each contiguous fir card is its only B=0 stem."""
    first = np.flatnonzero(leaves['colour'][:, 2] == 0)
    by_vertex = np.searchsorted(first, np.arange(len(leaves['position'])), side='right') - 1
    by_face = by_vertex[leaves['index']]
    if not len(first) or first[0] or np.any(by_face != by_face[:, :1]):
        raise ValueError('Expected contiguous spruce cards with one B=0 stem each')
    return first, by_vertex, by_face[:, 0]


def projection(full, px, azimuth, elevation):
    azimuth, elevation = np.radians((azimuth, elevation))
    x, y = np.sin(azimuth), np.cos(azimuth)
    s, c = np.sin(elevation), np.cos(elevation)
    basis = np.array([(y, -x, 0), (-x*s, -y*s, c), (-x*c, -y*c, -s)]).T
    bounds = full @ basis[:, :2]
    low = bounds.min(axis=0)
    span = bounds.max(axis=0) - low
    ppm = px / span[1]
    # Room for card growth, including at the leader and at the bottom of the tree.
    margin = int(np.ceil(.2 * px)) + 2
    shape = tuple((np.ceil(span[::-1] * ppm).astype(int) + 2 * margin) * RAYS)

    def project(position):
        return (position @ basis - np.append(low - margin / ppm, 0)) * (ppm * RAYS, ppm * RAYS, 1)

    return project, shape, ppm


def texture_indices(uv, shape):
    x = np.clip((uv[:, 0] * shape[1]).astype(int), 0, shape[1] - 1)
    y = np.clip((uv[:, 1] * shape[0]).astype(int), 0, shape[0] - 1)
    return x, y


def rank_cards(full, wood, leaves, mask, by_face, costs):
    """Greedy multiview coverage with a small bonus for originally frontmost rays."""
    card_pixels = []
    pixel_weights = []
    front_score = np.zeros(len(costs))
    offset = 0
    for azimuth, elevation in ORDER_VIEWS:
        project, shape, _ = projection(full, 96, azimuth, elevation)
        bark = tree_density.raster(project(wood['position']), wood['index'], shape).ravel()
        depth = bark.copy()
        front = np.full(bark.size, -1, int)
        keys = []
        points = project(leaves['position'])
        for face, pixel, z, _, _ in tree_density.fragments(points, leaves['index'], shape, leaves['uv'], mask):
            visible = z < bark[pixel]
            owner = by_face[face]
            keys.append((owner[visible] * bark.size + pixel[visible]))
            np.minimum.at(depth, pixel, z)
            nearest = (z == depth[pixel]) & visible
            front[pixel[nearest]] = owner[nearest]
        keys = np.unique(np.concatenate(keys))
        ray = keys % bark.size
        overlap = np.bincount(ray, minlength=bark.size)
        weight = 1 / np.sqrt(np.maximum(overlap, 1))
        cover = (overlap > 0).reshape(shape)
        edge = cover & tree_density.grown(~cover, 2)
        weight[edge.ravel()] *= 2
        weight /= max(1, cover.sum())
        hit = front >= 0
        front_score += np.bincount(front[hit], weights=weight[hit], minlength=len(costs))
        card_pixels.append(keys // bark.size)
        pixel_weights.append((offset + ray, weight))
        offset += bark.size
    owners = np.concatenate(card_pixels)
    rays = np.concatenate([ray for ray, _ in pixel_weights])
    weight = np.concatenate([weight for _, weight in pixel_weights])
    grouped = rays[np.argsort(owners, kind='stable')]
    cuts = np.cumsum(np.bincount(owners, minlength=len(costs)))[:-1]
    footprints = np.split(grouped, cuts)
    front_score *= .1
    cost_weight = np.sqrt(costs)
    # Cached gains are upper bounds as rays get covered. Refresh the leader only when needed.
    queue = [(-(weight[ray].sum() + front_score[i]) / cost_weight[i], i, 0)
             for i, ray in enumerate(footprints)]
    heapq.heapify(queue)
    order = []
    while queue:
        _, card, generation = heapq.heappop(queue)
        ray = footprints[card]
        if generation != len(order):
            gain = (weight[ray].sum() + front_score[card]) / cost_weight[card]
            heapq.heappush(queue, (-gain, card, len(order)))
        else:
            order.append(card)
            weight[ray] = 0
    return np.asarray(order)


def render(leaves, project, shape, wood_depth, mask, atlas_luma):
    canopy = np.full(wood_depth.size, np.inf, np.float32)
    luma = np.zeros(canopy.size)
    openness = np.zeros(canopy.size)
    index = leaves['index']
    points = project(leaves['position'])
    for face, pixel, z, u, v in tree_density.fragments(points, index, shape, leaves['uv'], mask):
        np.minimum.at(canopy, pixel, z)
        front = (z == canopy[pixel]) & (z < wood_depth[pixel])
        visible_pixels = pixel[front]
        x, y = texture_indices(np.column_stack((u[front], v[front])), atlas_luma.shape)
        luma[visible_pixels] = atlas_luma[y, x]
        corners = index[face[front]]
        a, b, c = points[corners, :2].transpose(1, 0, 2)
        y, x = np.divmod(visible_pixels, shape[1])
        dx, dy = x + .5 - c[:, 0], y + .5 - c[:, 1]
        ab, bc, ca = a - b, b - c, c - a
        area = ab[:, 0] * bc[:, 1] - ab[:, 1] * bc[:, 0]
        w0 = (bc[:, 1] * dx - bc[:, 0] * dy) / area
        w1 = (ca[:, 1] * dx - ca[:, 0] * dy) / area
        opened = leaves['colour'][corners, 3] / 255.
        openness[visible_pixels] = w0 * opened[:, 0] + w1 * opened[:, 1] + (1 - w0 - w1) * opened[:, 2]
    seen = canopy < wood_depth
    hits = (seen | np.isfinite(wood_depth)).reshape(shape[0] // RAYS, RAYS, shape[1] // RAYS, RAYS)
    solid = hits.sum(axis=(1, 3)) > RAYS * RAYS // 2
    return solid, np.array([luma[seen].sum(), openness[seen].sum(), seen.sum()])


def variant(leaves, order, count, first, by_vertex, by_face, growth):
    keep = np.zeros(len(order), bool)
    keep[order[:count]] = True
    position = leaves['position']
    if growth != 1:
        pivot = position[first[by_vertex]]
        position = pivot + growth * (position - pivot)
    return dict(leaves, position=position, index=leaves['index'][keep[by_face]])


def measure(full_path, levels, mask, atlas_luma, heights=SWEEP):
    wood, leaves = split_parts(tree_glb.read(full_path))
    first, by_vertex, by_face = cards(leaves)
    costs = np.bincount(by_face, minlength=len(first))
    if np.any(costs == 0):
        raise ValueError('Expected every spruce card to have triangles')
    full = np.concatenate((wood['position'], leaves['position']))
    order = rank_cards(full, wood, leaves, mask, by_face, costs)
    cumulative = np.cumsum(costs[order])
    results = {}
    for px, shares in heights.items():
        # Equivalent bark controls isolate the canopy at the exact designed foliage budget.
        cases = [('full', wood, leaves, len(first), 1.)]
        counts = [(f'{share:g}', max(1, round(share * len(first))), wood) for share in shares]
        if px in levels:
            level_wood, level_leaves = split_parts(tree_glb.read(levels[px]))
            level_cost = len(level_wood['index']) + len(level_leaves['index'])
            cases.append(('designed', level_wood, level_leaves, len(cards(level_leaves)[0]), 1.))
            for name, bark in (('same_total', wood), ('same_canopy', level_wood)):
                count = int(np.searchsorted(cumulative, level_cost - len(bark['index']), side='right'))
                if count:
                    counts.append((name, count, bark))
        for name, count, bark in counts:
            for suffix, growth in (('', 1.), ('_grown', min(1.6, (len(first) / count) ** .25))):
                crown = variant(leaves, order, count, first, by_vertex, by_face, growth)
                cases.append((name + suffix, bark, crown, count, growth))
        rows = {name: dict(cards=count, growth=growth, wood_triangles=len(bark['index']),
                           triangles=len(bark['index']) + len(crown['index']), views=[])
                for name, bark, crown, count, growth in cases}
        tones = {name: np.zeros(3) for name in rows}
        for azimuth, elevation in VIEWS:
            project, shape, ppm = projection(full, px, azimuth, elevation)
            depths = {}
            for name, bark, crown, _, _ in cases:
                if id(bark) not in depths:
                    depths[id(bark)] = tree_density.raster(project(bark['position']), bark['index'], shape).ravel()
                solid, tone = render(crown, project, shape, depths[id(bark)], mask, atlas_luma)
                tones[name] += tone
                outline = tree_density.outline(solid, ppm, tree_density.CLOSE)
                if name == 'full':
                    reference = outline
                size = max(1, int(reference.sum()))
                rows[name]['views'].append(dict(
                    azimuth=azimuth, elevation=elevation,
                    opening=float((outline & ~solid).sum() / max(1, outline.sum())),
                    reference_opening=float((reference & ~solid).sum() / size),
                    missing_outline=float((reference & ~outline).sum() / size),
                    extra_outline=float((outline & ~reference).sum() / size),
                ))
        for name, row in rows.items():
            luma, openness, rays = tones[name]
            row['luma_srgb'] = float(luma / rays) if rays else None
            row['mean_openness'] = float(openness / rays) if rays else None
        results[px] = rows
    return results


def main():
    from PIL import Image

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('full', type=Path)
    parser.add_argument('--levels', required=True, nargs=4, type=Path, metavar='GLB',
                        help='Designed GLBs in order: 400, 200, 100, 50 pixels')
    parser.add_argument('--mask', required=True, type=Path)
    parser.add_argument('--atlas', required=True, type=Path, help='Foliage albedo DDS or PNG')
    args = parser.parse_args()
    with Image.open(args.atlas) as image:
        luma = np.asarray(image.convert('RGB')) @ LUMA
    print(json.dumps(measure(args.full, dict(zip(CHECKPOINTS, args.levels)),
                             tree_density.load_mask(args.mask), luma), indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
