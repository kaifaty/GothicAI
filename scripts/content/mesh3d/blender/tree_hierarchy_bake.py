"""Bake independent fir hierarchy cards and screen their budgets on held-out rays."""
from __future__ import annotations

import math
import multiprocessing
import struct
from importlib.util import module_from_spec, spec_from_file_location
from collections import defaultdict
from contextlib import nullcontext
from dataclasses import dataclass
from io import BytesIO
from pathlib import Path

import numpy as np
from PIL import Image

import fir_hierarchy
import tree_hierarchy_raster as measure
from tree_bough_lod import BACKGROUND, LUMA, QUAD, UV, direction, resize

TARGET = {'hang': 18., 'end': 18., 'bough': 30., 'tier': 22., 'sleeve': 18., 'leader': 18.}
LIMIT = {'hang': 4, 'end': 4, 'bough': 8, 'tier': 8, 'sleeve': 3, 'leader': 3}
SIZES = (.5, 1., 1.5, 2., 2.5, 3., 3.5, 4., 5., 6., 7., 8., 10., 12., 14., 16., 18., 20.,
         22., 24., 26., 28., 30., 32., 36., 40., 48., 64.)
BAKE = 48  # texels per bounding-box diagonal, before 2x sampling
SCREEN = 80  # reference samples per bounding-box diagonal
PAGE = 1024
PAD = 4


@dataclass
class Fit:
    cards: list[dict]
    max_px: float
    target_px: float
    metrics: dict
    holdouts: tuple[np.ndarray, ...]


def rgba_bytes(tile):
    rgba = tile.copy()
    rgb = rgba[..., :3]
    rgb[:] = np.where(rgb <= .0031308, rgb * 12.92, 1.055 * np.maximum(rgb, 0.) ** (1 / 2.4) - .055)
    return np.rint(np.clip(rgba, 0., 1.) * 255).astype(np.uint8)


def linear_rgba(pixels):
    rgba = pixels.astype(np.float32) / 255
    rgb = rgba[..., :3]
    rgb[:] = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    return rgba


def attribute_bytes(tile):
    values = tile.copy()
    values[..., :3] = .5 + .5 * values[..., :3]
    return np.rint(np.clip(values, 0., 1.) * 255).astype(np.uint8)


def linear_attributes(pixels):
    values = pixels.astype(np.float32) / 255
    values[..., :3] = 2 * values[..., :3] - 1
    return values


def source_part(foliage, nodes, leaves, texture):
    return dict(fir_hierarchy.leaf_part(foliage, nodes, leaves), rgba=texture)


def frame(foliage, nodes, leaves):
    """A signed longitudinal axis through the stems, or the first shoot's spine."""
    position = foliage['position']
    stems = np.array([nodes[leaf - 1].first for leaf in leaves])
    origins = position[stems]
    spine = position[stems[0] + min(3, nodes[leaves[0] - 1].count - 1)] - origins[0]
    if len(stems) >= 2 and np.linalg.norm(origins - origins.mean(axis=0)) > .05:
        _, _, axes = np.linalg.svd(origins - origins.mean(axis=0), full_matrices=False)
        spine = axes[0] * (-1 if np.dot(axes[0], spine) < 0 else 1)
    if np.linalg.norm(spine) < 1e-8:
        spine = np.array((1., 0., 0.))
    axis = direction(spine)
    vertical = np.array((0., 0., 1.)) if abs(axis[2]) < .95 else np.array((0., 1., 0.))
    up = direction(vertical - axis * np.dot(axis, vertical))
    return axis, up, direction(np.cross(up, axis))


def projection(axis, camera):
    right = direction(axis - camera * np.dot(axis, camera))
    return np.stack((right, np.cross(right, camera), -camera), axis=1)


def held_out(axis, up, side):
    # All four rays have a component along the branch. The baking rays only cross it.
    local = ((.66, .52, .54), (-.58, .74, -.34), (.42, -.48, .77), (-.41, -.85, -.33))
    return tuple(projection(axis, direction(x * axis + y * up + z * side)) for x, y, z in local)


def dilate(tile, covered):
    """Give transparent neighbours the nearest hit's material, leaving their coverage zero."""
    covered = covered.copy()
    for _ in range(PAD):
        occupied = covered.copy()
        for dst, src in (((slice(1, None), slice(None)), (slice(None, -1), slice(None))),
                         ((slice(None, -1), slice(None)), (slice(1, None), slice(None))),
                         ((slice(None), slice(1, None)), (slice(None), slice(None, -1))),
                         ((slice(None), slice(None, -1)), (slice(None), slice(1, None)))):
            moved = occupied[src] & ~covered[dst]
            tile[dst][moved] = tile[src][moved]
            covered[dst] |= moved


def anchored_colour(source, points):
    """Each cross-section takes the exact wind bytes of an original vertex of one branch."""
    anchors = np.array(((points[0] + points[3]) / 2, (points[1] + points[2]) / 2))
    vertices = source['position']
    distance = ((anchors[:, None] - vertices) ** 2).sum(axis=2)
    phases = source['colour'][:, 1]
    centre = np.argmin(((anchors.mean(axis=0) - vertices) ** 2).sum(axis=1))
    distance[:, phases != phases[centre]] = np.inf
    return source['colour'][np.argmin(distance, axis=1)][[0, 1, 1, 0]]


def bake_card(source, plane, ppm, low3, high3):
    coords = source['position'] @ plane[:, :2]
    low, high = coords.min(axis=0), coords.max(axis=0)
    if np.any(high - low < 1e-6):
        return None
    shape = tuple(np.maximum(1, np.ceil((high - low) * ppm).astype(int))[::-1])
    # Keep all UV texels inside the fitted rectangle. Each texel integrates four samples.
    aligned = (np.array(shape[::-1]) / ppm) - (high - low)
    low -= aligned / 2
    sampled, depth = measure.raster([source], plane, low, 2 * ppm, tuple(2 * n for n in shape))
    hits = depth[np.isfinite(depth)]
    if not len(hits):
        return None
    tile = sampled.reshape(shape[0], 2, shape[1], 2, 8).mean(axis=(1, 3))
    alpha = tile[..., 3].copy()
    material = tile[..., (0, 1, 2, 4, 5, 6, 7)] / np.maximum(alpha[..., None], 1e-8)
    normals = material[..., 3:6]
    normals /= np.maximum(np.linalg.norm(normals, axis=2, keepdims=True), 1e-8)
    dilate(material, alpha > 0.)
    rgba = linear_rgba(rgba_bytes(np.dstack((material[..., :3], alpha))))
    attributes = linear_attributes(attribute_bytes(material[..., 3:]))
    extent = np.array(shape[::-1]) / ppm
    points = np.column_stack((low + UV * extent, np.full(4, np.median(hits)))) @ plane.T
    points = np.clip(points, low3, high3).astype(np.float32)
    normal = tile[..., 4:7].sum(axis=(0, 1))
    normal = direction(normal) if np.linalg.norm(normal) > 1e-8 else -plane[:, 2]
    return dict(position=points, uv=UV.astype(np.float32), index=QUAD, cutout=True,
                colour=anchored_colour(source, points), normal=np.tile(normal, (4, 1)).astype(np.float32),
                rgba=rgba, attributes=attributes)


def split_groups(foliage, nodes, leaves, axis, tier):
    """Split the most populated along-axis support; prefer intact branches in a tier."""
    by_stem = {leaf: float(foliage['position'][nodes[leaf - 1].first] @ axis) for leaf in leaves}
    groups = [leaves]
    yield groups
    while max(map(len, groups)) > 1:
        number = max(range(len(groups)), key=lambda i: len(groups[i]))
        group = groups[number]
        if tier:
            branches = {}
            for leaf in group:
                branches.setdefault(nodes[leaf - 1].branch, []).append(leaf)
        else:
            branches = {}
        if len(branches) > 1:
            ordered = sorted(branches.values(), key=lambda members: np.mean([by_stem[i] for i in members]))
            cut = np.cumsum([len(members) for members in ordered])[:-1]
            middle = int(cut[np.argmin(np.abs(cut - len(group) / 2))])
            ordered = [i for members in ordered for i in members]
        else:
            ordered = sorted(group, key=by_stem.get)
            middle = len(group) // 2
        groups = [*groups[:number], tuple(ordered[:middle]), tuple(ordered[middle:]), *groups[number + 1:]]
        yield groups


def support_budgets(node, foliage, nodes, axis):
    tier = node.kind == 'tier' and len({nodes[i - 1].branch for i in node.leaves}) > 1
    max_budget = max(2 if tier else 1, min(LIMIT[node.kind], len(node.leaves) - 1))
    group_iter = split_groups(foliage, nodes, node.leaves, axis, tier)
    groups = next(group_iter)
    for budget in range(2 if tier else 1, max_budget + 1):
        supports = max(2 if tier and budget > 1 else 1, (budget + 1) // 2)
        if len(groups) < supports:
            groups = next(group_iter)
        crosses = set(sorted(range(supports), key=lambda i: -len(groups[i]))[:budget - supports])
        yield budget, tier, [(group, 1 + (i in crosses)) for i, group in enumerate(groups)]


def card_planes(foliage, nodes, group):
    axis, up, side = frame(foliage, nodes, group)
    return [projection(axis, direction(camera)) for camera in (.82 * up + .57 * side, -.57 * up + .82 * side)]


def trials(node, foliage, nodes, texture, axes, budget_limit):
    ppm = BAKE / node.diameter
    cache = {}
    for budget, tier, groups in support_budgets(node, foliage, nodes, axes[0]):
        cards = []
        for group, count in groups:
            if group not in cache:
                source = source_part(foliage, nodes, group, texture)
                cache[group] = (source, card_planes(foliage, nodes, group), [])
            source, planes, baked = cache[group]
            for j in range(count):
                if j == len(baked):
                    baked.append(bake_card(source, planes[j], ppm, node.rest_low, node.rest_high))
                if baked[j] is not None:
                    cards.append(baked[j])
        if len(cards) == budget and (not tier or len({card['colour'][0, 1] for card in cards}) >= 2):
            yield cards
            # If every cheaper bake was empty or lost a wind support, retain a diagnostic.
            if budget >= max(2 if tier else 1, budget_limit):
                return


def viewport(bounds, plane, ppm):
    low, high = bounds
    corners = np.array(np.meshgrid(*zip(low, high))).reshape(3, -1).T
    xy = corners @ plane[:, :2]
    start, end = xy.min(axis=0) - 2 / ppm, xy.max(axis=0) + 2 / ppm
    return plane, start, ppm, tuple(np.ceil((end - start) * ppm).astype(int)[::-1])


def dimensions(shape, px):
    size = tuple(np.maximum(1, np.ceil(np.array(shape) * px / SCREEN).astype(int)))
    return size, math.prod(shape) * (px / SCREEN) ** 2 / math.prod(size)


def screen(references, renders, px_grid):
    """Test a contiguous prefix. A pass at a later box alignment cannot hide an earlier failure."""
    errors = []
    for full, parent in zip(references, renders):
        difference = full - parent
        coverage = difference[..., 3]
        luma = difference[..., :3] @ LUMA - BACKGROUND * coverage
        errors.append(np.stack((coverage, luma), axis=2).astype(np.float32))
    max_px, failure_px, failure = 0., None, []
    for px in px_grid:
        values = []
        for error in errors:
            size, area = dimensions(error.shape[:2], px)
            values.append(np.abs(resize(error, size)).sum(axis=(0, 1)) * area)
        worst = np.max(values, axis=0)
        failure = [key for key, error, limit in zip(('coverage', 'luminance'), worst, (1., .10))
                   if error > limit + 1e-7]
        if failure:
            failure_px = px
            break
        max_px = px
    return max_px, failure_px, failure


def summarize(references, renders, px):
    if px <= 0.:
        return None
    values = []
    for full, parent in zip(references, renders):
        size, area = dimensions(full.shape[:2], px)
        values.append(measure.node_metrics(resize(full, size), resize(parent, size), area, lit=True))
    summary = {name: max(row[name] for row in values) for name in ('coverage_px2', 'luma_px2')}
    angles = [row['normal_deg'] for row in values if row['normal_deg'] is not None]
    opening = [row['openness_pp'] for row in values if row['openness_pp'] is not None]
    summary.update(normal_deg=float(np.mean(angles)) if angles else None,
                   openness_pp=max(map(abs, opening)) if opening else None, views=values)
    return summary


def mip_one(card):
    """Tile-local mip with alpha-weighted material averages and no neighbouring tiles."""
    alpha = card['rgba'][..., 3:]
    shape = tuple(max(1, math.ceil(n / 2)) for n in alpha.shape[:2])
    values = np.concatenate((card['rgba'][..., :3] * alpha, alpha, card['attributes'] * alpha), axis=2)
    tile = resize(values, shape)
    material = tile[..., (0, 1, 2, 4, 5, 6, 7)] / np.maximum(tile[..., 3:4], 1e-8)
    normals = material[..., 3:6]
    normals /= np.maximum(np.linalg.norm(normals, axis=2, keepdims=True), 1e-8)
    dilate(material, tile[..., 3] > 0.)
    return dict(card, rgba=linear_rgba(rgba_bytes(np.dstack((material[..., :3], tile[..., 3])))),
                attributes=linear_attributes(attribute_bytes(material[..., 3:])), alpha_test=True)


def fit_node(node, nodes, foliage, texture, budget_limit=math.inf):
    target = TARGET[node.kind]
    planned_high = round(target * 1.12 * 1.10, 4)
    sizes = sorted(set((*SIZES, planned_high)))
    axes = frame(foliage, nodes, node.leaves)
    holdouts = held_out(*axes)
    bounds = node.rest_low, node.rest_high
    views = [viewport(bounds, plane, SCREEN / node.diameter) for plane in holdouts]
    source = source_part(foliage, nodes, node.leaves, texture)
    references = [measure.raster([source], *view)[0] for view in views]
    all_trials, best = [], None
    for cards in trials(node, foliage, nodes, texture, axes, budget_limit):
        if best is not None and len(cards) > budget_limit:
            break
        renders = [measure.raster(cards, *view)[0] for view in views]
        largest, failure_px, failure = screen(references, renders, sizes)
        all_trials.append(dict(budget=len(cards), max_px=largest, first_failure_px=failure_px, failure=failure))
        if best is None or largest > best[0]:
            best = largest, cards, renders, failure_px, failure
        if largest >= planned_high:
            break
    if best is None:
        raise ValueError(f'Node {node.id} ({node.kind}) has no covered parent card')
    largest, cards, renders, failure_px, failure = best
    mip_cards = [mip_one(card) for card in cards] if largest > 0. else []
    mipped = [measure.raster(mip_cards, *view)[0] for view in views] if mip_cards else None
    metrics = dict(kind=node.kind, budget=len(cards), target_px=target, planned_high_px=planned_high, max_px=largest,
                   reason=('child cost' if len(cards) > budget_limit else ', '.join(failure)
                           if largest < planned_high else None),
                   first_failure_px=failure_px, at_max=summarize(references, renders, largest),
                   at_planned_high=summarize(references, renders, planned_high),
                   at_max_mip1=summarize(references, mipped, largest) if mipped is not None else None,
                   mip1_alpha_cutoff=.5, trials=all_trials)
    return Fit(cards, largest, target, metrics, holdouts)


_worker_input = None


def prepare_worker(nodes, foliage, texture):
    global _worker_input
    _worker_input = nodes, foliage, texture


def worker_fit(task):
    nodes, foliage, texture = _worker_input
    node_id, budget_limit = task
    return node_id, fit_node(nodes[node_id - 1], nodes, foliage, texture, budget_limit)


def fit_all(nodes, foliage, texture, *, jobs=1):
    """Fit each floor after the costs of its cheapest completed child cuts are known."""
    floors = defaultdict(list)
    depth, costs, fits = {}, {}, {}
    for node in nodes:
        if node.children:
            depth[node.id] = 1 + max(depth[i] for i in node.children)
            floors[depth[node.id]].append(node)
        else:
            depth[node.id] = 0
            costs[node.id] = (1, node.index_count // 3)
    workers = (multiprocessing.get_context('fork').Pool(jobs, prepare_worker, (nodes, foliage, texture))
               if jobs > 1 else nullcontext())
    with workers as pool:
        for _, floor in sorted(floors.items()):
            tasks = []
            for node in floor:
                children = [costs[i] for i in node.children]
                cards, triangles = map(sum, zip(*children))
                costs[node.id] = (cards, triangles)
                tasks.append((node.id, min(cards - 1, (triangles - 1) // 2)))
            outcome = (pool.imap(worker_fit, tasks, chunksize=4) if pool is not None else
                       ((i, fit_node(nodes[i - 1], nodes, foliage, texture, cap)) for i, cap in tasks))
            for node_id, fit in outcome:
                fits[node_id] = fit
                old_cards, old_triangles = costs[node_id]
                cards, triangles = len(fit.cards), sum(len(card['index']) for card in fit.cards)
                if fit.max_px and cards < old_cards and triangles < old_triangles:
                    costs[node_id] = (cards, triangles)
    return fits


def measure_transfer(nodes, foliage, texture, fits, groups):
    """Retest a typical node's atlas on a different shape of its type, retaining both own bakes."""
    results = []
    for label, ids in groups.items():
        if len(ids) < 2:
            continue
        spans = np.array([nodes[i - 1].rest_high - nodes[i - 1].rest_low for i in ids])
        relative = spans / np.maximum(np.median(spans, axis=0), 1e-5)
        order = np.argsort(np.linalg.norm(relative - 1., axis=1), kind='stable')
        source_id, target_id = ids[order[0]], ids[order[math.ceil(.9 * (len(order) - 1))]]
        src, dst = nodes[source_id - 1], nodes[target_id - 1]
        src_part = source_part(foliage, nodes, src.leaves, texture)
        dst_part = source_part(foliage, nodes, dst.leaves, texture)
        src_frame = np.stack(frame(foliage, nodes, src.leaves), axis=1)
        dst_frame = np.stack(frame(foliage, nodes, dst.leaves), axis=1)
        src_pos = src_part['position'] @ src_frame
        dst_pos = dst_part['position'] @ dst_frame
        scale = np.ptp(dst_pos, axis=0) / np.maximum(np.ptp(src_pos, axis=0), 1e-6)
        rotate = src_frame @ dst_frame.T
        transferred = []
        for card in fits[source_id].cards:
            position = ((card['position'] @ src_frame - src_pos.min(axis=0)) * scale
                        + dst_pos.min(axis=0)) @ dst_frame.T
            position = np.clip(position, dst.rest_low, dst.rest_high)
            attributes = card['attributes'].copy()
            attributes[..., :3] = attributes[..., :3] @ rotate
            transferred.append(dict(card, position=position, attributes=attributes,
                                    normal=card['normal'] @ rotate, colour=anchored_colour(dst_part, position)))
        views = [viewport((dst.rest_low, dst.rest_high), plane, SCREEN / dst.diameter)
                 for plane in fits[target_id].holdouts]
        refs = [measure.raster([dst_part], *view)[0] for view in views]
        images = [measure.raster(transferred, *view)[0] for view in views]
        own_px = fits[target_id].max_px
        transfer_px, failed_px, reason = screen(refs, images, sorted(set((*SIZES, own_px)) - {0.}))
        results.append(dict(kind=label, source=source_id, target=target_id, own_px=own_px, transfer_px=transfer_px,
                            first_failure_px=failed_px, reason=reason, at_own=summarize(refs, images, own_px)))
    return results


def pack(output: Path, fits: dict[int, Fit]):
    """Use a shelf atlas; each node's contiguous geometry uses a single atlas page."""
    groups = []
    for node_id, fit in fits.items():
        shapes = np.array([card['rgba'].shape[:2] for card in fit.cards]) + 2 * PAD
        width, height = int(shapes[:, 1].sum()), int(shapes[:, 0].max())
        if max(width, height) > PAGE:
            raise ValueError(f'Node {node_id} does not fit on one {PAGE}-texel page')
        groups.append((height, width, node_id))
    placements = []
    x = y = row = page = 0
    for height, width, node_id in sorted(groups, key=lambda item: (-item[0], -item[1], item[2])):
        if x + width > PAGE:
            x, y, row = 0, y + row, 0
        if y + height > PAGE:
            page += 1
            x = y = row = 0
        placements.append((page, x, y, node_id))
        x += width
        row = max(height, row)

    output.mkdir(parents=True, exist_ok=True)
    parents, pages = {}, []
    for page in range(placements[-1][0] + 1 if placements else 0):
        rgba = np.zeros((PAGE, PAGE, 4), np.uint8)
        attributes = np.zeros_like(rgba)
        for p, x, y, node_id in placements:
            if p != page:
                continue
            positions, indices, uvs, normals, colours = [], [], [], [], []
            vertex = 0
            for card in fits[node_id].cards:
                height, width = card['rgba'].shape[:2]
                region = np.s_[y:y + height + 2 * PAD, x:x + width + 2 * PAD]
                padding = ((PAD, PAD), (PAD, PAD), (0, 0))
                rgba[region] = np.pad(rgba_bytes(card['rgba']), padding, mode='edge')
                attributes[region] = np.pad(attribute_bytes(card['attributes']), padding, mode='edge')
                uvs.append((np.array((x + PAD, y + PAD)) + card['uv'] * (width, height)) / PAGE)
                positions.append(card['position'])
                indices.append(card['index'] + vertex)
                normals.append(card['normal'])
                colours.append(card['colour'])
                vertex += len(card['position'])
                x += width + 2 * PAD
            parents[node_id] = dict(page=page, position=np.concatenate(positions), normal=np.concatenate(normals),
                                    uv=np.concatenate(uvs).astype(np.float32), index=np.concatenate(indices),
                                    colour=np.concatenate(colours))
        files = dict(rgba=f'hierarchy-color-{page:03d}.png', attributes=f'hierarchy-normal-openness-{page:03d}.png')
        Image.fromarray(rgba).save(output / files['rgba'], compress_level=3)
        Image.fromarray(attributes).save(output / files['attributes'], compress_level=3)
        pages.append(dict(**files, width=PAGE, height=PAGE,
                          bytes=sum((output / name).stat().st_size for name in files.values())))
    atlas = dict(pages=pages, pad_px=PAD, tiles=sum(len(fit.cards) for fit in fits.values()),
                 bytes=sum(page['bytes'] for page in pages), raw_bytes=len(pages) * PAGE * PAGE * 8)
    return parents, atlas


def card_tangent_normals(card, normals):
    """Invert the engine's cotangent frame for each stored triangle."""
    height, width = normals.shape[:2]
    scaled_uv = dict(card, uv=card['uv'] * (width, height))
    u, v, face_normal = measure._cotangent_edges(scaled_uv)
    n = direction(card['normal'][0])
    tangent, bitangent = np.cross(u, n), np.cross(v, n)
    scale = np.maximum(np.linalg.norm(tangent, axis=1), np.linalg.norm(bitangent, axis=1))
    scale *= np.where(face_normal @ n < 0, -1., 1.)
    y, x = np.ogrid[:height, :width]
    u_above_v = (x + .5) / width >= (y + .5) / height
    result = np.empty_like(normals)
    for face, mask in enumerate((u_above_v, ~u_above_v)):
        basis = np.stack((tangent[face] / scale[face], bitangent[face] / scale[face], n))
        local = normals[mask] @ np.linalg.inv(basis)
        # DXT5nm reconstructs positive Z. The opposite hemisphere projects onto the tangent plane.
        local[:, 2] = np.maximum(local[:, 2], 0.)
        lengths = np.linalg.norm(local, axis=1, keepdims=True)
        result[mask] = np.where(lengths > 1e-8, local / np.maximum(lengths, 1e-8), (0., 0., 1.))
    return result


def eye_camera_frame(camera):
    aim, outward, root, centre, focal = camera
    forward = -np.asarray(outward)
    right = direction(np.cross(forward, (0., 1., 0.)))
    up = np.cross(right, forward)
    basis = np.stack((right, up, forward), axis=1)
    # tree_glb.to_gltf rotates to Y up; loadGltfMesh mirrors X.
    model = basis[[0, 2, 1]] * np.array((-1., -1., 1.))[:, None]
    origin = (np.asarray(root) - aim) @ basis
    return model, origin, centre @ model + origin, focal


def card_screen_span(card, camera, screen_ppm):
    """Upper bound of the length of each projected UV derivative at the stage's upper threshold."""
    model, origin, centre, focal = camera
    # MeshLevels divides by the radius to the full mesh's box centre. Solve
    # for the camera depth along the route at that radius.
    distance = focal / screen_ppm
    row_depth = math.sqrt(distance ** 2 - np.dot(centre[:2], centre[:2])) - centre[2]
    points = card['position'] @ model + origin
    points[:, 2] += row_depth
    faces = points[card['index']]
    a, b, c, d = points
    edges = np.array(((b - a, c - b), (c - d, d - a)))
    # For axis e, the projective derivative is f*(e.xy*p.z-p.xy*e.z)/p.z^2.
    # The numerator is affine over each triangle and its norm is bounded by the corners.
    numerator = edges[:, None, :, :2] * faces[:, :, None, 2:] - faces[:, :, None, :2] * edges[:, None, :, 2:]
    span = focal * np.linalg.norm(numerator, axis=-1).max(axis=1) / np.min(faces[..., 2], axis=1)[:, None] ** 2
    return span.max(axis=0)


def rebake_parent(task):
    node_id, stored, screen_ppm, camera, samples = task
    nodes, foliage, texture = _worker_input
    node = nodes[node_id - 1]
    budget = len(stored['position']) // 4
    axis = frame(foliage, nodes, node.leaves)[0]
    groups = next(groups for size, _, groups in support_budgets(node, foliage, nodes, axis) if size == budget)
    index = 0
    cards = []
    for group, count in groups:
        source = dict(source_part(foliage, nodes, group, texture), tangent_normal=foliage['tangent_normal'])
        for plane in card_planes(foliage, nodes, group)[:count]:
            vertices = slice(index * 4, (index + 1) * 4)
            card = {key: stored[key][vertices] for key in fir_hierarchy.VERTEX_FIELDS}
            card['index'] = stored['index'][index * 2:(index + 1) * 2] - index * 4
            if not np.array_equal(card['index'], QUAD) or not np.array_equal(card['uv'], UV):
                raise ValueError(f'Node {node_id}: expected stored quads in bake order')
            coords = source['position'] @ plane[:, :2]
            low, high = coords.min(axis=0), coords.max(axis=0)
            # Keep the previous UV rectangle, including the original bake's sub-texel margin.
            old_ppm = BAKE / node.diameter
            extent = np.ceil((high - low) * old_ppm) / old_ppm
            origin = (low + high - extent) / 2
            if not np.allclose(anchored_colour(source, card['position'])[:, :3], card['colour'][:, :3]):
                raise ValueError(f'Node {node_id}: stored wind anchor differs from its bake support')
            p = card['position']
            span = np.array((max(np.linalg.norm(p[1] - p[0]), np.linalg.norm(p[2] - p[3])),
                             max(np.linalg.norm(p[3] - p[0]), np.linalg.norm(p[2] - p[1]))))
            screen_span = np.maximum(extent, span) * screen_ppm
            if camera is not None:
                screen_span = np.maximum(screen_span, card_screen_span(card, camera, screen_ppm))
            width, height = np.ceil(1.5 * screen_span).astype(int)
            sampled, _ = measure.raster([source], plane, origin, samples * np.array((width, height)) / extent,
                                        (samples * height, samples * width))
            tile = sampled.reshape(height, samples, width, samples, 8).mean(axis=(1, 3))
            alpha = tile[..., 3]
            covered = alpha > 0
            if not covered.any():
                raise ValueError(f'Node {node_id}: the fixed card has no coverage')
            material = np.concatenate((tile[..., :3], tile[..., 4:7]), axis=2)
            material /= np.maximum(alpha[..., None], 1e-8)
            dilate(material, covered)
            colour = card['colour'].copy()
            colour[:, 3] = np.rint(255 * tile[..., 7].sum() / alpha.sum())
            cards.append(dict(card, colour=colour, rgba=np.dstack((material[..., :3], alpha)),
                              tangent_normal=card_tangent_normals(card, material[..., 3:])))
            index += 1
    return node_id, cards


def eye_layout(fits, side):
    tiles = [(card['rgba'].shape[0] + 2 * PAD, card['rgba'].shape[1] + 2 * PAD, node_id, i)
             for node_id, cards in fits.items() for i, card in enumerate(cards)]
    x = y = row = 0
    for height, width, node_id, i in sorted(tiles, key=lambda item: (-item[0], -item[1], item[2], item[3])):
        if x + width > side:
            x, y, row = 0, y + row, 0
        if width > side or y + height > side:
            raise ValueError(f'Node {node_id}: parent cards exceed one {side}-pixel atlas page')
        yield node_id, i, x, y
        x += width
        row = max(row, height)


def write_eye_atlas(output, source, page, kind, codec):
    """Keep the original and new pages in the top quadrants at each whole-block mip."""
    original = source.read_bytes()
    side, width = struct.unpack_from('<II', original, 12)
    if side != width or side != page.shape[0]:
        raise ValueError(f'{source}: expected the original square DDS')
    fourcc, block_bytes = (b'DXT1', 8) if kind == 'orm' else (b'DXT5', 16)
    levels = int(math.log2(side)) - 1
    if original[:4] != b'DDS ' or original[84:88] != fourcc or struct.unpack_from('<I', original, 28)[0] < levels:
        raise ValueError(f'{source}: expected a {fourcc.decode()} mip chain')
    header = bytearray(original[:128])
    struct.pack_into('<III', header, 12, 2 * side, 2 * side, 4 * (side // 4) ** 2 * block_bytes)
    struct.pack_into('<I', header, 28, levels)
    offset = len(header)
    with output.open('wb') as file:
        file.write(header)
        for level in range(levels):
            if kind == 'albedo':
                pixels = rgba_bytes(page)
            elif kind == 'normal':
                normal = page / np.maximum(np.linalg.norm(page, axis=2, keepdims=True), 1e-8)
                pixels = np.full((*page.shape[:2], 4), 255, np.uint8)
                pixels[..., [3, 1]] = np.rint(np.clip(normal[..., :2] * .5 + .5, 0., 1.) * 255).astype(np.uint8)
            else:
                pixels = np.rint(page * 255).astype(np.uint8)
            blocks = codec.blocks(pixels)
            encode = codec.bc1 if kind == 'orm' else codec.bc3
            encoded = np.concatenate([encode(blocks[i:i + 8192]) for i in range(0, len(blocks), 8192)])
            stride = side // 4 * block_bytes
            size = side // 4 * stride
            combined = np.zeros((side // 2, 2 * stride), np.uint8)
            combined[:side // 4, :stride] = np.frombuffer(original, np.uint8, size, offset).reshape(-1, stride)
            combined[:side // 4, stride:] = encoded.reshape(-1, stride)
            file.write(combined.tobytes())
            offset += size
            if level + 1 < levels:
                if kind == 'albedo':
                    alpha = page[..., 3:4]
                    premultiplied = codec.downsample(page[..., :3] * alpha)
                    alpha = codec.downsample(alpha)
                    page = np.concatenate((premultiplied / np.maximum(alpha, 1e-8), alpha), axis=2)
                    dilate(page[..., :3], page[..., 3] > 0)
                else:
                    page = codec.downsample(page)
                side //= 2


def rebake_cards(foliage, nodes, parents, px_per_m, *, camera=None, jobs=1, samples=2):
    arguments = (nodes, foliage, foliage['rgba'])
    workers = (multiprocessing.get_context('fork').Pool(jobs, prepare_worker, arguments)
               if jobs > 1 else nullcontext())
    frame = eye_camera_frame(camera) if camera is not None else None
    tasks = [(node_id, parents[node_id], ppm, frame, samples) for node_id, ppm in sorted(px_per_m.items())]
    with workers as pool:
        if pool is not None:
            return dict(pool.imap(rebake_parent, tasks, chunksize=4))
        prepare_worker(*arguments)
        return dict(map(rebake_parent, tasks))


def rebake_variant(output, foliage, nodes, parents, px_per_m, source_material, *, camera=None, jobs=1):
    """Reprint selected cards next to the unmodified boughs page in a square atlas."""
    fits = rebake_cards(foliage, nodes, parents, px_per_m, camera=camera, jobs=jobs)

    side = foliage['rgba'].shape[0]
    rgba = np.zeros((side, side, 4), np.float32)
    normal = np.zeros((side, side, 3), np.float32)
    normal[..., 2] = 1.
    pad = ((PAD, PAD), (PAD, PAD), (0, 0))
    for node_id, i, x, y in eye_layout(fits, side):
        card = fits[node_id][i]
        height, width = card['rgba'].shape[:2]
        region = np.s_[y:y + height + 2 * PAD, x:x + width + 2 * PAD]
        rgba[region] = np.pad(card['rgba'], pad, mode='edge')
        normal[region] = np.pad(card['tangent_normal'], pad, mode='edge')
        card['uv'] = ((np.array((side + x + PAD, y + PAD)) + card['uv'] * (width, height))
                      / (2 * side)).astype(np.float32)

    output.mkdir(parents=True, exist_ok=True)
    spec = spec_from_file_location('tree_atlas_dds', Path(__file__).parents[2] / 'test-material.py')
    codec = module_from_spec(spec)
    spec.loader.exec_module(codec)
    orm = np.empty((side // 2, side // 2, 3), np.float32)
    orm[:] = (1., .85, 0.)
    for kind, page in (('albedo', rgba), ('normal', normal), ('orm', orm)):
        write_eye_atlas(output / f'{kind}.dds', source_material / f'boughs-{kind}.dds', page, kind, codec)
    size = sum((output / f'{kind}.dds').stat().st_size for kind in ('albedo', 'normal', 'orm'))

    with Image.open(output / 'albedo.dds') as image:
        rgba = linear_rgba(np.asarray(image.convert('RGBA')))
    with Image.open(output / 'normal.dds') as image:
        xy = np.asarray(image.convert('RGBA'), dtype=np.float32)[..., [3, 1]] * (2 / 255) - 1
    normal = np.dstack((xy, np.sqrt(np.maximum(0, 1 - np.sum(xy ** 2, axis=-1)))))
    textures = dict(rgba=rgba, tangent_normal=normal)
    mapped = {}
    for node_id, cards in fits.items():
        part = dict(parents[node_id], **textures, coverage=True, cutout=True)
        for key in ('uv', 'colour'):
            part[key] = np.concatenate([card[key] for card in cards])
        mapped[node_id] = part
    return dict(foliage, **textures, uv=foliage['uv'] * .5), mapped, size


BOUGH_PAD = 8  # One texel of gutter remains in mip 3.
BOUGH_MIPS = 4


def _bough_alpha_samples(alpha, x, y, width, height, factor):
    """Measure filtered alpha over the card's actual UV rectangle, excluding its gutters."""
    coordinates = []
    for offset, extent in ((x, width), (y, height)):
        count = max(16, math.ceil(4 * extent / factor))
        coordinates.append((offset + BOUGH_PAD + (np.arange(count) + .5) * extent / count) / factor - .5)
    u, v = coordinates
    ix, iy = u.astype(np.intp), v.astype(np.intp)
    fx, fy = (u - ix)[None, :], (v - iy)[:, None]
    lower = alpha[iy[:, None], ix] * (1 - fx) + alpha[iy[:, None], ix + 1] * fx
    upper = alpha[iy[:, None] + 1, ix] * (1 - fx) + alpha[iy[:, None] + 1, ix + 1] * fx
    return lower * (1 - fy) + upper * fy


def _bough_cutoff(samples, target):
    """Find the nearest filtered coverage when a constant alpha plateau straddles the target."""
    samples = samples.ravel()
    rank = round((1 - target) * samples.size)
    if rank <= 0:
        return samples.min() - 1 / 255
    if rank >= samples.size:
        return samples.max() + 1 / 255
    lower, upper = np.partition(samples, [rank - 1, rank])[[rank - 1, rank]]
    if lower == upper:
        dropped = np.count_nonzero(samples < upper)
        lost = np.count_nonzero(samples <= upper)
        if rank - dropped > lost - rank:
            return (upper + samples[samples > upper].min(initial=1 + 1 / 255)) / 2
        lower = samples[samples < upper].max(initial=-1 / 255)
    # Keep truly empty parts of a tile on the transparent side of the cutoff.
    return max((lower + upper) / 2, 1 / 255)


def _bough_alpha_palette(blocks, *, reserve_extremes=False):
    hi, lo = blocks.max(axis=1), blocks.min(axis=1)
    palette = np.stack([hi, lo] + [((7 - i) * hi + i * lo) // 7 for i in range(1, 7)], axis=1)
    delta = np.abs(blocks[:, :, None] - palette[:, None, :])
    selectors = delta.argmin(axis=2)
    if reserve_extremes:
        # BC3's alternate mode stores 0 and 255 explicitly. In a block crossing
        # tile edges it can quantize the filtered centre far more finely.
        interior = (blocks > 0) & (blocks < 255)
        mid_lo = np.where(interior, blocks, 255).min(axis=1)
        mid_hi = np.where(interior, blocks, 0).max(axis=1)
        mid_lo = np.minimum(mid_lo, mid_hi)
        alt = [mid_lo, mid_hi] + [((5 - i) * mid_lo + i * mid_hi) // 5 for i in range(1, 5)]
        alt = np.stack(alt + [np.zeros_like(lo), np.full_like(hi, 255)], axis=1)
        alt_delta = np.abs(blocks[:, :, None] - alt[:, None, :])
        use_alt = (alt_delta.min(axis=2) ** 2).sum(axis=1) < (delta.min(axis=2) ** 2).sum(axis=1)
        palette[use_alt] = alt[use_alt]
        selectors[use_alt] = alt_delta[use_alt].argmin(axis=2)
    return palette, selectors


def _bough_dxt_alpha(alpha, codec):
    """Evaluate the standard BC3 alpha endpoints and selectors without encoding RGB."""
    pixels = np.rint(alpha * 255).astype(np.uint8)
    blocks = codec.blocks(pixels[..., None])[:, :, 0].astype(np.int16)
    decoded = np.empty_like(blocks, dtype=np.uint8)
    for start in range(0, len(blocks), 8192):
        block = blocks[start:start + 8192]
        palette, selectors = _bough_alpha_palette(block)
        decoded[start:start + len(block)] = np.take_along_axis(palette, selectors, axis=1)
    h, w = alpha.shape
    return decoded.reshape(h // 4, w // 4, 4, 4).transpose(0, 2, 1, 3).reshape(h, w) / 255


def _bough_color_bc3(blocks, codec):
    palette, selectors = _bough_alpha_palette(blocks[..., 3].astype(np.int16), reserve_extremes=True)
    packed = np.sum(selectors.astype(np.uint64) << (3 * np.arange(16, dtype=np.uint64)), axis=1)
    encoded = np.empty((len(blocks), 16), np.uint8)
    encoded[:, :2] = palette[:, :2]
    encoded[:, 2:8] = packed.astype('<u8').view(np.uint8).reshape(-1, 8)[:, :6]
    encoded[:, 8:] = codec.bc1(blocks)
    return encoded


def _write_bough_mip(file, pixels, codec, *, cutout=False):
    """Store one BC3 level, then decode it to compare the same texels the game receives."""
    side = len(pixels)
    blocks = codec.blocks(pixels)
    chunks = (blocks[start:start + 8192] for start in range(0, len(blocks), 8192))
    encoded = b''.join((_bough_color_bc3(block, codec) if cutout else codec.bc3(block)).tobytes()
                       for block in chunks)
    file.write(encoded)
    with Image.open(BytesIO(_bough_dds_header(side, 1) + encoded)) as image:
        return np.array(image.convert('RGBA'))


def _bough_dds_header(side, levels):
    header = [124, 0xA1007, side, side, side * side, 0, levels] + [0] * 11
    header += [32, 4, int.from_bytes(b'DXT5', 'little'), 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0]
    return b'DDS ' + struct.pack('<31I', *header)


def write_bough_atlas(output, side, placements, *, page=0):
    """Write aligned alpha-coverage-preserving parent tiles and their mip diagnostics."""
    spec = spec_from_file_location('tree_atlas_dds', Path(__file__).parents[2] / 'test-material.py')
    codec = module_from_spec(spec)
    spec.loader.exec_module(codec)
    source = np.zeros((side, side, 7), np.float32)
    tiles = []
    for card, x, y, outer_width, outer_height in placements:
        rgba = card['rgba']
        height, width = rgba.shape[:2]
        alpha = rgba[..., 3:]
        tile = np.concatenate((rgba[..., :3] * alpha, alpha, card['tangent_normal'] * alpha), axis=2)
        padding = ((BOUGH_PAD, outer_height - height - BOUGH_PAD),
                   (BOUGH_PAD, outer_width - width - BOUGH_PAD), (0, 0))
        source[y:y + outer_height, x:x + outer_width] = np.pad(tile, padding, mode='edge')
        tiles.append((x, y, width, height, outer_width, outer_height, float(alpha.mean())))

    textures, diagnostics = [], []
    output.mkdir(parents=True, exist_ok=True)
    with (output / f'hierarchy-color-{page:03d}.dds').open('wb') as color_file, (
            output / f'hierarchy-normal-{page:03d}.dds').open('wb') as normal_file:
        for file in (color_file, normal_file):
            file.write(_bough_dds_header(side, BOUGH_MIPS))
        for level in range(BOUGH_MIPS):
            factor = 2 ** level
            alpha = source[..., 3].copy()
            raw_dxt = _bough_dxt_alpha(alpha, codec)
            albedo = np.dstack((source[..., :3] / np.maximum(alpha[..., None], 1e-8), alpha))
            normals = source[..., 4:7].copy()
            row = dict.fromkeys(('children', 'raw', 'raw_cutoff', 'raw_dxt_cutoff',
                                 'corrected_cutoff', 'dxt_cutoff'), 0.)
            for x, y, width, height, outer_width, outer_height, target in tiles:
                area = width * height
                sample_args = (x, y, width, height, factor)
                raw = _bough_alpha_samples(alpha, *sample_args)
                raw_compressed = _bough_alpha_samples(raw_dxt, *sample_args)
                row['children'] += area * target
                row['raw'] += area * raw.mean()
                row['raw_cutoff'] += area * (raw >= .5).mean()
                row['raw_dxt_cutoff'] += area * (raw_compressed >= .5).mean()
                # Since filtering commutes with an affine transform, shifting the filtered
                # percentile to 0.5 restores thin projected area without changing RGB.
                region = np.s_[y // factor:(y + outer_height) // factor, x // factor:(x + outer_width) // factor]
                cutoff = _bough_cutoff(raw, target)
                gain = .5 / max(cutoff, 1 - cutoff)
                albedo[region][..., 3] = .5 + (alpha[region] - cutoff) * gain
                covered = alpha[region] > 0
                dilate(albedo[region][..., :3], covered)
                dilate(normals[region], covered)

            normals /= np.maximum(np.linalg.norm(normals, axis=2, keepdims=True), 1e-8)
            normal_pixels = np.full((*alpha.shape, 4), 255, np.uint8)
            normal_pixels[..., [3, 1]] = np.rint(np.clip(normals[..., :2] * .5 + .5, 0., 1.) * 255).astype(np.uint8)
            rgba = linear_rgba(_write_bough_mip(color_file, rgba_bytes(albedo), codec, cutout=True))
            xy = _write_bough_mip(normal_file, normal_pixels, codec)[..., [3, 1]].astype(np.float32) * (2 / 255) - 1
            normal = np.dstack((xy, np.sqrt(np.maximum(0, 1 - np.sum(xy ** 2, axis=-1)))))
            for x, y, width, height, _, _, _ in tiles:
                area = width * height
                sample_args = (x, y, width, height, factor)
                row['corrected_cutoff'] += area * (_bough_alpha_samples(albedo[..., 3], *sample_args) >= .5).mean()
                row['dxt_cutoff'] += area * (_bough_alpha_samples(rgba[..., 3], *sample_args) >= .5).mean()
            area = sum(width * height for _, _, width, height, _, _, _ in tiles)
            diagnostics.append({name: value / area for name, value in row.items()})
            textures.append(dict(rgba=rgba, tangent_normal=normal, coverage=True, alpha_test=True, cutout=True))
            if level + 1 < BOUGH_MIPS:
                source = codec.downsample(source)
    return diagnostics, textures


BOUGH_SIDE = 2048


def bough_layout(fits, side):
    """Place aligned tiles on a skyline, keeping all the cards of a node on one page."""
    def dimensions(card):
        h, w = card['rgba'].shape[:2]
        return (w + BOUGH_PAD - 1) // BOUGH_PAD + 2, (h + BOUGH_PAD - 1) // BOUGH_PAD + 2

    units = side // BOUGH_PAD
    sizes = {ident: [dimensions(card) for card in cards] for ident, cards in fits.items()}
    order = sorted(sizes, key=lambda ident: (-max(h for _, h in sizes[ident]),
                                             -sum(w * h for w, h in sizes[ident]), ident))

    def place(skyline, ident):
        skyline = skyline.copy()
        tiles = []
        for i in sorted(range(len(sizes[ident])), key=lambda i: sizes[ident][i][::-1], reverse=True):
            w, h = sizes[ident][i]
            if w > units or h > units:
                raise ValueError(f'Node {ident}: a card exceeds {side} texels with mip gutters')
            windows = np.lib.stride_tricks.sliding_window_view(skyline, w)
            tops = windows.max(axis=1)
            y = int(tops.min())
            if y + h > units:
                return None
            exposed = np.flatnonzero(tops == y)
            x = int(exposed[np.argmax(windows[exposed].sum(axis=1))])
            skyline[x:x + w] = y + h
            tiles.append((ident, i, x * BOUGH_PAD, y * BOUGH_PAD, w * BOUGH_PAD, h * BOUGH_PAD))
        return skyline, tiles

    pages = []
    for ident in order:
        for index, (skyline, placements) in enumerate(pages):
            placed = place(skyline, ident)
            if placed is not None:
                pages[index] = placed[0], placements + placed[1]
                break
        else:
            placed = place(np.zeros(units, np.int32), ident)
            if placed is None:
                raise ValueError(f'Node {ident}: all cards must fit on one atlas page')
            pages.append(placed)
    return [placements for _, placements in pages]


def pack_bough(output, fits, parents):
    """Keep the card geometry and wind bytes, replacing only tile UVs and openness."""
    for side in (BOUGH_SIDE, 2 * BOUGH_SIDE, 4 * BOUGH_SIDE):
        layout = bough_layout(fits, side)
        if len(layout) == 1:
            break
    else:
        raise ValueError(f'Parent tiles exceed one {side}-pixel atlas page')
    pages, textures, diagnostics = [], [], []
    mapped = {}
    area = 0
    for page, tiles in enumerate(layout):
        uv = defaultdict(dict)
        placements = []
        for ident, i, x, y, width, height in tiles:
            card = fits[ident][i]
            h, w = card['rgba'].shape[:2]
            uv[ident][i] = (np.array((x + BOUGH_PAD, y + BOUGH_PAD)) + card['uv'] * (w, h)) / side
            placements.append((card, x, y, width, height))
            area += width * height
        metrics, mips = write_bough_atlas(output, side, placements, page=page)
        textures.append(dict(mips[0], rgba_mips=tuple(mip['rgba'] for mip in mips),
                             normal_mips=tuple(mip['tangent_normal'] for mip in mips)))
        diagnostics.append(metrics)
        files = dict(rgba=f'hierarchy-color-{page:03d}.dds', attributes=f'hierarchy-normal-{page:03d}.dds')
        pages.append(dict(**files, width=side, height=side,
                          bytes=sum((output / name).stat().st_size for name in files.values())))
        for ident, coords in uv.items():
            cards = fits[ident]
            mapped[ident] = dict(parents[ident], page=page,
                                 uv=np.concatenate([coords[i] for i in range(len(cards))]).astype(np.float32),
                                 colour=np.concatenate([card['colour'] for card in cards]))
    atlas = dict(pages=pages, pad_px=BOUGH_PAD, tiles=sum(map(len, fits.values())),
                 bytes=sum(page['bytes'] for page in pages), raw_bytes=len(pages) * side ** 2 * 8)
    return mapped, atlas, textures, diagnostics, area
