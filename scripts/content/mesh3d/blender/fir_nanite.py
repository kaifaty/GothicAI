"""Build the stage 2 opaque spruce A and the bind skin for its accepted card control.

    uv run --no-project --with numpy --with pillow --with scipy --with contourpy --with 'shapely>=2.1' \
        python scripts/content/mesh3d/blender/fir_nanite.py ACCEPTED.glb BOUGHS-ALBEDO.dds OUTPUT
"""
from __future__ import annotations

import argparse
import json
import math
import struct
from itertools import pairwise
from pathlib import Path

import fir_tree
import numpy as np
import shapely
import tree_glb
import tree_prefix
from PIL import Image
from scipy.ndimage import gaussian_filter
from shapely.geometry import Polygon

HERE = Path(__file__).resolve().parent
# The pilot interchange keeps the packed vertex binding independent of glTF's 16-bit MRM index limit.
VERTEX = np.dtype([('position', '<f4', 3), ('normal', '<f4', 3), ('uv', '<f4', 2), ('colour', 'u1', 4),
                   ('bones', '<u2', 2), ('weight', 'u1'), ('lock', 'u1'), ('padding', 'V2')])


def engine_axes(value):
    return np.asarray(value)[..., (0, 2, 1)] * (-1, 1, -1)


def binding(parent, child, child_weight):
    """Two nonnegative weights with exactly 255 total and one encoding per pair."""
    weight = int(np.rint(255 * child_weight))
    if parent == child or weight == 0:
        return parent, parent, 255
    if weight == 255:
        return child, child, 255
    return parent, child, 255 - weight


def rig(tree: fir_tree.Tree):
    bones = []

    def bone(parent, kind, pivot, axis):
        bones.append([parent, kind, *(100 * engine_axes(pivot)), *engine_axes(axis)])
        return len(bones) - 1

    height = tree.metrics['height_m']
    bone(-1, 0, np.zeros(3), fir_tree.UP)
    pivots = np.array([.23, height * .25, height * .50, height * .75])
    for i, z in enumerate(pivots):
        bone(i, 1, (0., 0., z), fir_tree.UP)

    def trunk(z):
        i = int(np.searchsorted(pivots, z, side='right'))
        if i == 0:
            return 0, 0, 255
        end = pivots[i] if i < len(pivots) else height
        return binding(i - 1, i, fir_tree.smooth((z - pivots[i - 1]) / (end - pivots[i - 1])))

    branches = tree.branches[tree.stubs:]
    whorls = {}
    for whorl in sorted({b.whorl for b in branches}):
        z = np.median([b.points[0, 2] for b in branches if b.whorl == whorl])
        parent = int(np.searchsorted(pivots, z, side='right'))
        whorls[whorl] = bone(parent, 2, (0., 0., z), fir_tree.UP)
    shoots = {b.number: bone(whorls[b.whorl], 3, *b.at(0.)) for b in branches}

    def attach(branch, s):
        base = branch.points[0, 2]
        if branch.number not in shoots:
            return trunk(base)
        if s <= 0.:
            return trunk(base)
        whorl = whorls[branch.whorl]
        return binding(whorl, shoots[branch.number], fir_tree.smooth(s))

    wood, crown = tree.parts
    bark = np.empty((len(wood['position']), 4), dtype=np.uint16)
    bark[:, :3] = [attach(tree.branches[owner - 1], int(c[0]) / 255) if owner else trunk(p[2])
                   for owner, c, p in zip(wood['owner'], wood['colour'], wood['position'])]
    bark[:, 3] = 1
    card_bind = np.empty((len(crown['position']), 4), np.uint16)
    for card in tree.cards:
        first, owner = card['first'], card['branch']
        p = crown['position'][first]
        if owner:
            skin = attach(tree.branches[owner - 1], int(crown['colour'][first, 0]) / 255)
        else:
            skin = (4, 4, 255) if card['role'] == 'leader' else trunk(p[2])
        card_bind[first:first + card['count']] = (*skin, 0)
        card_bind[first, 3] = 1
    return bones, bark, card_bind


def card_signature(part, start, end):
    return tuple(part[name][start:end].tobytes() for name in ('uv', 'normal', 'colour'))


def source_cards(tree, source, level, starts):
    by_key = {card_signature(source, card['first'], card['first'] + card['count']): card
              for card in tree.cards}
    if len(by_key) != len(tree.cards):
        raise ValueError('The reference needs a unique attribute signature for every card')
    try:
        return [by_key[card_signature(level, start, end)]
                for start, end in zip(starts, np.r_[starts[1:], len(level['position'])])]
    except KeyError as error:
        raise ValueError('The far level contains a card with no source foliage binding') from error


def bind_card_level(tree, source, skin, level):
    """Recover exact source-card bindings from the attributes preserved by design_level."""
    source_bark, source_leaves = source
    bark_skin, leaf_skin = skin
    bark, foliage = level
    if (bark['name'], foliage['name']) != (source_bark['name'], source_leaves['name']):
        raise ValueError('The far level must use the accepted bark and foliage materials')
    wood_at = {tuple(point): joints for point, joints in zip(source_bark['position'], bark_skin)}
    starts, by_vertex, _ = tree_prefix.cards(foliage)
    cards = source_cards(tree, source_leaves, foliage, starts)
    bindings = np.empty((len(foliage['position']), 4), np.uint16)
    try:
        bark_bindings = np.stack([wood_at[tuple(point)] for point in bark['position']])
    except KeyError as error:
        raise ValueError('The far level contains a vertex with no source bark binding') from error
    source_starts = np.asarray([card['first'] for card in cards])
    bindings[:, :3] = leaf_skin[source_starts[by_vertex], :3]
    bindings[:, 3] = 0
    bindings[starts, 3] = 1
    return np.concatenate((bark_bindings, bindings))


def contours(alpha, step, coverage):
    """Extract opaque shoot silhouettes at the same integrated area as the source alpha."""
    import contourpy

    image = gaussian_filter(alpha, step * .45, mode='constant')
    h, w = alpha.shape
    y = np.r_[0, np.arange(step / 2, h - 1, step), h - 1]
    x = np.r_[0, np.arange(step / 2, w - 1, step), w - 1]
    samples = image[np.ix_(y.astype(int), x.astype(int))]
    samples[[0, -1], :] = 0.
    samples[:, [0, -1]] = 0.
    generator = contourpy.contour_generator(x=x + .5, y=y + .5, z=samples, fill_type='OuterOffset')

    def shape(threshold):
        curves, offsets = generator.filled(threshold, 2.)
        return [Polygon(curve[:ends[1]], [curve[a:b] for a, b in pairwise(ends[1:])])
                .simplify(step * .3, preserve_topology=True) for curve, ends in zip(curves, offsets)]

    target = alpha.sum() * coverage
    if not 0. < target <= sum(p.area for p in shape(0.)):
        raise ValueError('The sampled atlas silhouette cannot preserve the requested coverage')
    low, high = 0., 1.
    for _ in range(13):
        middle = (low + high) / 2
        if sum(p.area for p in shape(middle)) > target:
            low = middle
        else:
            high = middle
    return shapely.union_all(shape((low + high) / 2))


def stencil(uv, faces, silhouette, cell, size):
    """Interpolate on each original blade separately, sharing all vertices on its folds."""
    weights, triangles = [], []
    origin = np.array(cell['cell'][:2])
    pixels = uv.astype(np.float64) * size - origin
    # The low alpha of the original stem can fall below the contour threshold.
    # Keep an opaque connection to the original attachment at the base of the blade.
    stem = shapely.shortest_line(shapely.Point(pixels[0]), silhouette)
    if stem.length > 1e-6:
        start, end = np.asarray(stem.coords)
        direction = (end - start) / stem.length
        end += 1.8 * direction
        side = 1.8 * direction[::-1] * (-1, 1)
        silhouette = silhouette.union(Polygon((start, end + side, end - side)))
    count = 0
    for face in faces:
        corners = pixels[face]
        matrix = (corners[1:] - corners[:1]).T
        if abs(np.linalg.det(matrix)) < 1e-6:
            continue
        clipped = silhouette.intersection(Polygon(corners))
        patches = shapely.get_coordinates(shapely.constrained_delaunay_triangles(clipped)).reshape(-1, 4, 2)[:, :3]
        if not len(patches):
            continue
        uv_sign = np.linalg.det(matrix)
        patch_sign = np.linalg.det(patches[:, 1:] - patches[:, :1])
        flip = patch_sign * uv_sign < 0
        patches[flip] = patches[flip, ::-1]
        local = np.linalg.solve(matrix, (patches.reshape(-1, 2) - corners[0]).T).T
        bary = np.zeros((len(local), len(uv)), dtype=np.float64)
        bary[:, face] = np.column_stack((1 - local.sum(axis=1), local))
        weights.append(bary)
        triangles.append(np.arange(count, count + len(bary)).reshape(-1, 3))
        count += len(bary)
    if not weights:
        return None
    bary, indices = np.unique(np.round(np.concatenate(weights), 7), axis=0, return_inverse=True)
    index = indices[np.concatenate(triangles)]
    index = index[(index != np.roll(index, 1, axis=1)).all(axis=1)]
    # Keep the folds on the original shared edge. Raise the shoot surfaces by up to 6 mm between them.
    interior = (bary > 0.).sum(axis=1) == 3
    longitudinal = (bary @ pixels)[:, int(bool(cell.get('turn')))]
    near_stem = longitudinal < .07 * cell['cell'][3 if cell.get('turn') else 2]
    relief = .006 * (np.sin(longitudinal * .079) * .5 + .5) * interior * ~near_stem
    root = bary[:, 0] == 1.
    if np.count_nonzero(root) != 1:
        raise ValueError('Opaque foliage must have one pinned stem per blade template')
    return bary, index, root, relief


def opaque_crown(part, tree, card_bind, alpha, atlas, step, coverage):
    records = tree.cards
    fields = {name: [] for name in ('position', 'normal', 'uv', 'colour', 'index', 'skin')}
    owners = np.searchsorted([r['first'] for r in records], part['index'][:, 0], side='right') - 1
    face_start = np.r_[0, np.cumsum(np.bincount(owners, minlength=len(records)))]
    shapes, templates = {}, {}
    total, refined = 0, 0
    for card, begin, end in zip(records, face_start, face_start[1:]):
        first, kind = card['first'], card['kind']
        span = slice(first, first + card['count'])
        faces, uv = part['index'][begin:end] - first, part['uv'][span]
        detail = step
        if card['role'] == 'hang' and card['branch']:
            branch = tree.branches[card['branch'] - 1]
            # Keep smaller contours on the hanging shoots closing +Y's middle tiers in the accepted crown.
            reach = np.linalg.norm(part['position'][first, :2])
            if math.sin(branch.azimuth) > 0 and .27 < branch.height < .85 and reach > .45:
                detail = min(step, 20)
        refined += detail < step
        cell = atlas['cells'][kind]
        if (kind, detail) not in shapes:
            x, y, w, h = cell['cell']
            shapes[kind, detail] = contours(alpha[y:y + h, x:x + w], detail, coverage)
        key = (kind, detail, uv.tobytes(), faces.tobytes())
        if key not in templates:
            templates[key] = stencil(uv, faces, shapes[kind, detail], cell, atlas['size'])
        result = templates[key]
        if result is None:
            raise ValueError(f'No opaque foliage in atlas cell {kind}')
        bary, index, root, relief = result
        normal = bary @ part['normal'][span]
        normal /= np.linalg.norm(normal, axis=1, keepdims=True)
        position = bary @ part['position'][span] + relief[:, None] * normal
        skin = np.broadcast_to(card_bind[first], (len(bary), 4)).copy()
        skin[:, 3] = root
        fields['position'].append(position.astype(np.float32))
        fields['normal'].append(normal.astype(np.float32))
        fields['uv'].append((bary @ uv).astype(np.float32))
        fields['colour'].append(np.rint(bary @ part['colour'][span]).astype(np.uint8))
        fields['index'].append(index + total)
        fields['skin'].append(skin)
        total += len(bary)
    return {k: np.concatenate(v) for k, v in fields.items()}, refined


def write_mesh(path, parts):
    vertex_count = sum(len(p['position']) for p in parts)
    triangle_count = sum(len(p['index']) for p in parts)
    with path.open('wb') as file:
        file.write(struct.pack('<4sIII', b'FIRG', 1, vertex_count, triangle_count))
        for part in parts:
            vertices = np.zeros(len(part['position']), dtype=VERTEX)
            vertices['position'] = 100 * engine_axes(part['position'].astype(np.float64))
            normal = engine_axes(part['normal'].astype(np.float64))
            vertices['normal'] = normal / np.linalg.norm(normal, axis=1, keepdims=True)
            vertices['uv'], vertices['colour'] = part['uv'], part['colour']
            skin = part['skin']
            vertices['bones'], vertices['weight'], vertices['lock'] = skin[:, :2], skin[:, 2], skin[:, 3]
            vertices.tofile(file)
        offset = 0
        for material, part in enumerate(parts):
            triangles = np.full((len(part['index']), 4), material, dtype='<u4')
            triangles[:, :3] = part['index'] + offset
            triangles.tofile(file)
            offset += len(part['position'])
    return vertex_count, triangle_count


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('cards', type=Path)
    parser.add_argument('albedo', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--step', type=int, default=24, help='base atlas texel step for the opaque shoot contours')
    parser.add_argument('--coverage', type=float, default=1., help='fraction of the original integrated alpha to preserve')
    parser.add_argument('--card-level', type=Path, action='append', default=[],
                        help='write the shared wind rig for one accepted card LOD GLB')
    args = parser.parse_args()
    if args.step < 2 or not 0. < args.coverage < math.inf:
        parser.error('step must be at least 2 texels and coverage must be finite and positive')
    config = json.loads((HERE / 'fir.json').read_text())
    tree = fir_tree.build(config)
    parts = tree_glb.read(args.cards)
    if len(parts) != 2 or any(src['name'] != dst['name'] or any(not np.array_equal(src[key], dst[key])
                             for key in ('position', 'uv', 'colour', 'index')) for src, dst in zip(parts, tree.parts)):
        raise ValueError('The accepted spruce A (seed 7, unit scale) does not match the source topology')
    alpha = np.asarray(Image.open(args.albedo).convert('RGBA'), dtype=np.float32)[..., 3] / 255
    size = config['atlas']['size']
    if alpha.shape != (size, size):
        raise ValueError('Unexpected spruce atlas dimensions')
    bones, bark, cards = rig(tree)
    args.output.mkdir(parents=True, exist_ok=True)
    meta = {'version': 1, 'bones': bones}
    (args.output / 'geometry-rig.json').write_text(json.dumps(meta, separators=(',', ':')) + '\n')
    (args.output / 'cards-rig.json').write_text(json.dumps(dict(meta, vertices=np.concatenate((bark, cards)).tolist()),
                                                         separators=(',', ':')) + '\n')
    for path in args.card_level:
        weights = bind_card_level(tree, parts, (bark, cards), tree_glb.read(path))
        (args.output / f'{path.stem}-rig.json').write_text(
            json.dumps(dict(meta, vertices=weights.tolist()), separators=(',', ':')) + '\n')
    parts[0]['skin'] = bark
    crown, refined = opaque_crown(parts[1], tree, cards, alpha, config['atlas'], args.step, args.coverage)
    vertices, triangles = write_mesh(args.output / 'fir-a.firg', [parts[0], crown])
    print(json.dumps({'vertices': vertices, 'triangles': triangles, 'bones': len(bones),
                          'bark_triangles': len(parts[0]['index']), 'foliage_triangles': len(crown['index']),
                          'step': args.step, 'refined_cards': refined, 'coverage': args.coverage}))


if __name__ == '__main__':
    main()
