"""Software cutout raster for fir hierarchy bakes and held-out comparisons."""

from __future__ import annotations

from collections import defaultdict
from io import BytesIO
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import tree_density
from tree_bough_lod import BACKGROUND, LUMA, basis, direction, resize


LIGHTS = tuple(direction(v).astype(np.float32) for v in ((1., .5, 1.), (-1., -.5, .55)))
LUMA = LUMA.astype(np.float32)
SHEET_VIEWS = {(0, 0): 'a', (135, 30): 'b'}


def _cutout(part):
    return part.get('cutout', part.get('attributes') is not None or part.get('material', {}).get('alphaMode') == 'MASK')


def _texel_indices(shape, u, v, *, repeat):
    h, w = shape[:2]
    if repeat:
        u, v = u % 1., v % 1.
    return np.clip((v * h).astype(np.intp), 0, h - 1), np.clip((u * w).astype(np.intp), 0, w - 1)


def _texture_sample(part, u, v):
    """Source mip 0 cutout or bilinearly filtered, premultiplied baked coverage and attributes."""
    rgba = part.get('rgba')
    attributes = part.get('attributes')
    if rgba is None:
        base = (.18, .18, .18, 1.) if _cutout(part) else (.09, .06, .038, 1.)
        return np.broadcast_to(np.float32(base), (len(u), 4)), None
    h, w = rgba.shape[:2]
    if attributes is None and not part.get('coverage'):
        texels = rgba[_texel_indices(rgba.shape, u, v, repeat=not _cutout(part))]
        texels[:, 3] = texels[:, 3] >= .5 if _cutout(part) else 1.
        return texels, None

    x = np.clip(u * w - .5, 0, w - 1)
    y = np.clip(v * h - .5, 0, h - 1)
    xi, yi = x.astype(np.intp), y.astype(np.intp)
    fx, fy = np.float32(x - xi), np.float32(y - yi)
    base = np.zeros((len(u), 4), np.float32)
    aux = np.zeros_like(base) if attributes is not None else None
    for dx, dy, weight in ((0, 0, (1 - fx) * (1 - fy)), (1, 0, fx * (1 - fy)),
                           (0, 1, (1 - fx) * fy), (1, 1, fx * fy)):
        at = np.minimum(yi + dy, h - 1), np.minimum(xi + dx, w - 1)
        colour = rgba[at]
        alpha = colour[:, 3] * weight
        base[:, :3] += colour[:, :3] * alpha[:, None]
        base[:, 3] += alpha
        if aux is not None:
            aux += attributes[at] * alpha[:, None]
    if part.get('alpha_test'):
        alpha = base[:, 3].copy()
        coverage = alpha >= .5
        weight = np.where(coverage, 1. / np.maximum(alpha, .5), 0.)
        base *= weight[:, None]
        if aux is not None:
            aux *= weight[:, None]
    return base, aux


def _cotangent_edges(part):
    p = part['position'][part['index']]
    uv = part['uv'][part['index']]
    p1, p2 = p[:, 1] - p[:, 0], p[:, 2] - p[:, 0]
    uv1, uv2 = uv[:, 1] - uv[:, 0], uv[:, 2] - uv[:, 0]
    grad = p2[:, :, None] * uv1[:, None, :] - p1[:, :, None] * uv2[:, None, :]
    return grad[:, :, 0], grad[:, :, 1], np.cross(p1, p2)


def _mapped_normal(normal, faces, edges, texels):
    u, v, face_normal = edges
    tangent = np.cross(u[faces], normal)
    bitangent = np.cross(v[faces], normal)
    # Match the authored side for the same two-sided normal in every camera.
    sign = np.where(np.einsum('ij,ij->i', face_normal[faces], normal) < 0, -1., 1.)
    scale2 = np.maximum(np.einsum('ij,ij->i', tangent, tangent), np.einsum('ij,ij->i', bitangent, bitangent))
    scale = sign / np.sqrt(np.maximum(scale2, 1e-20))
    mapped = normal * texels[:, 2:3] + (tangent * texels[:, 0:1] + bitangent * texels[:, 1:2]) * scale[:, None]
    return np.where(scale2[:, None] > 1e-20, mapped, normal)


def raster(parts, projection, low, ppm, shape, *, foliage=None):
    """Nearest covered ray, with premultiplied linear RGB, coverage, XYZ normal and openness.

    If provided, foliage is a caller-allocated array of shape shape for the winning foliage alpha.
    Partial baked texels contribute their filtered coverage at the front hit; no rear colour is composited.
    """
    depth = np.full(math.prod(shape), np.inf, np.float32)
    pixels_out = np.zeros((len(depth), 8), np.float32)
    if foliage is not None:
        foliage.fill(0)
        foliage = foliage.ravel()
    for part in parts:
        index = part['index']
        points = part['position'] @ projection
        points[:, :2] = (points[:, :2] - low) * ppm
        cutout = _cutout(part)
        tangent_normal = part.get('tangent_normal')
        edges = _cotangent_edges(part) if tangent_normal is not None else None
        for faces, pixels, z, u, v, weights in tree_density.fragments(
                points, index, shape, part['uv'], with_weights=True):
            nearer = z < depth[pixels]
            if not nearer.any():
                continue
            candidates = np.flatnonzero(nearer)
            rgba, aux = _texture_sample(part, u[candidates], v[candidates])
            covered = np.flatnonzero(rgba[:, 3] > 0.)
            if not len(covered):
                continue
            hits = candidates[covered]
            order = np.lexsort((z[hits], pixels[hits]))
            sorted_pixels = pixels[hits[order]]
            keep = np.r_[True, sorted_pixels[1:] != sorted_pixels[:-1]]
            selected = covered[order[keep]]
            dst, hits = sorted_pixels[keep], candidates[selected]
            depth[dst] = z[hits]
            pixels_out[dst, :4] = rgba[selected]
            if aux is None:
                vertices = index[faces[hits]]
                vertex_normals = part['normal'][vertices]
                vertex_openness = part['colour'][vertices, 3] / np.float32(255)
                barycentric = np.asarray([w[hits] for w in weights], np.float32).T
                normal = np.einsum('nk,nkc->nc', barycentric, vertex_normals)
                if edges is not None:
                    texels = tangent_normal[_texel_indices(tangent_normal.shape, u[hits], v[hits], repeat=not cutout)]
                    normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-8)
                    normal = _mapped_normal(normal, faces[hits], edges, texels)
                normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-8)
                alpha = rgba[selected, 3]
                pixels_out[dst, 4:7] = normal * alpha[:, None]
                pixels_out[dst, 7] = np.einsum('nk,nk->n', barycentric, vertex_openness) * alpha
            else:
                normal = aux[selected, :3]
                alpha = rgba[selected, 3]
                normal *= (alpha / np.maximum(np.linalg.norm(normal, axis=1), 1e-8))[:, None]
                pixels_out[dst, 4:7] = normal
                pixels_out[dst, 7] = aux[selected, 3]
            if foliage is not None:
                foliage[dst] = rgba[selected, 3] if cutout else 0.
    return pixels_out.reshape(*shape, 8), depth.reshape(shape)


def _normal_and_openness(full, proxy):
    shared = np.minimum(full[..., 3], proxy[..., 3])
    common = shared > 1e-8
    if not common.any():
        return None, None
    weights = shared[common]
    ref_n, cur_n = full[..., 4:7][common], proxy[..., 4:7][common]
    denom = np.linalg.norm(ref_n, axis=1) * np.linalg.norm(cur_n, axis=1)
    with_normal = denom > 1e-8
    angle = None
    if with_normal.any():
        dot = np.einsum('ij,ij->i', ref_n[with_normal], cur_n[with_normal]) / denom[with_normal]
        angle = float(np.average(np.degrees(np.arccos(np.clip(dot, -1, 1))), weights=weights[with_normal]))
    full_a = full[..., 7][common] / full[..., 3][common]
    proxy_a = proxy[..., 7][common] / proxy[..., 3][common]
    return angle, float(100 * np.average(proxy_a - full_a, weights=weights))


def _light(image, light):
    alpha = image[..., 3]
    normal = image[..., 4:7]
    diffuse = np.einsum('...c,c->...', normal, light)
    diffuse /= np.maximum(np.linalg.norm(normal, axis=-1), 1e-8)
    openness = image[..., 7] / np.maximum(alpha, 1e-8)
    return .55 + .17 * openness + .28 * np.maximum(diffuse, 0.)


def node_metrics(full, proxy, cell_area=1., *, lit=False):
    """Section 6 errors on one common, box-filtered frame; cell_area is in real screen px²."""
    delta = full[..., 3] - proxy[..., 3]
    luma_a = full[..., :3] @ LUMA
    luma_b = proxy[..., :3] @ LUMA
    normal, openness = _normal_and_openness(full, proxy)
    row = dict(coverage_px2=float(np.abs(delta).sum() * cell_area),
               luma_px2=float(np.abs(luma_a - luma_b - BACKGROUND * delta).sum() * cell_area),
               normal_deg=normal, openness_pp=openness)
    if lit:
        row['lit_luma_px2'] = [float(np.abs(luma_a * _light(full, light) - luma_b * _light(proxy, light)
                                          - BACKGROUND * delta).sum() * cell_area) for light in LIGHTS]
    return row


def _pack(parts):
    """Batch the triangles sharing a raster material, retaining their submission order."""
    groups = defaultdict(list)
    for part in parts:
        key = (id(part.get('rgba')), id(part.get('attributes')), id(part.get('tangent_normal')),
               _cutout(part), part.get('alpha_test', False), part.get('coverage', False))
        groups[key].append(part)
    packed = []
    for group in groups.values():
        if len(group) == 1:
            packed.append(group[0])
            continue
        offsets = np.cumsum([0] + [len(part['position']) for part in group[:-1]])
        combined = dict(group[0])
        for attr in ('position', 'normal', 'uv', 'colour'):
            combined[attr] = np.concatenate([part[attr] for part in group])
        combined['index'] = np.concatenate([part['index'] + offset for part, offset in zip(group, offsets)])
        packed.append(combined)
    return packed


def _views():
    for elevation in (0, 30, -30):
        for azimuth in range(0, 360, 45):
            e, a = np.radians((elevation, azimuth))
            camera = np.array((np.cos(e) * np.cos(a), np.cos(e) * np.sin(a), np.sin(e)))
            yield azimuth, elevation, basis(camera)


def _capture(parts, projection, low, ppm, shape):
    foliage = np.empty(shape, np.float32)
    image, _ = raster(parts, projection, low, ppm, shape, foliage=foliage)
    # Raster hits are premultiplied; keep only front foliage for tone statistics.
    luma = np.where(foliage > 0, image[..., :3] @ LUMA, 0.)
    tones = np.stack((foliage, luma, *(luma * _light(image, light) for light in LIGHTS)), axis=-1)
    return image, tones


def _tone_percentile(values, weights, share):
    order = np.argsort(values, kind='stable')
    total = np.cumsum(weights[order], dtype=np.float64)
    return float(values[order[np.searchsorted(total, share * total[-1])]])


def _tone_stats(tones, bright_at=None):
    mask = tones[..., 0] > 1e-8
    if not mask.any():
        return [None] * 3
    weights = tones[..., 0][mask]
    stats = []
    for channel, premult in enumerate(np.moveaxis(tones[..., 1:][mask], -1, 0)):
        values = premult / weights
        p90 = _tone_percentile(values, weights, .9)
        threshold = p90 if bright_at is None or bright_at[channel] is None else bright_at[channel]['p90']
        bright = float(weights[values >= threshold].sum() / weights.sum())
        stats.append(dict(mean=float(premult.sum() / weights.sum()), p90=p90, bright=bright))
    return stats


def _relative(full, test):
    return (test / full - 1) * 100 if full else None


def _compare(full, full_stats, outlines, proxy, proxy_tones):
    alpha = full[..., 3]
    change = proxy[..., 3] - alpha
    inside = max(outlines.sum(), 1)
    full_area = max(alpha.sum(), 1e-8)
    lost = 100 * -np.minimum(change, 0).sum() / full_area
    gained = 100 * np.maximum(change, 0).sum() / full_area
    normal, openness = _normal_and_openness(full, proxy)
    lights = []
    for ref, cur in zip(full_stats, _tone_stats(proxy_tones, full_stats)):
        if ref is None or cur is None:
            lights.append(dict(tone_pct=None, p90_pct=None, bright_pp=None))
            continue
        lights.append(dict(tone_pct=_relative(ref['mean'], cur['mean']),
                           p90_pct=_relative(ref['p90'], cur['p90']),
                           bright_pp=100 * (cur['bright'] - ref['bright'])))
    return dict(open_pp=float(-100 * change[outlines].sum() / inside),
                full_open_pct=float(100 * (1 - alpha[outlines].sum() / inside)),
                mask_residual_pp=float(lost + gained), mask_loss_pp=float(lost), mask_spill_pp=float(gained),
                normal_deg=normal, openness_pp=openness, unlit=lights[0], lights=lights[1:])


def _image(render):
    alpha = render[..., 3:4]
    linear = render[..., :3] * _light(render, LIGHTS[0])[..., None] + BACKGROUND * (1. - alpha)
    srgb = np.where(linear <= .0031308, 12.92 * linear, 1.055 * np.maximum(linear, 0.) ** (1 / 2.4) - .055)
    return Image.fromarray(np.uint8(np.rint(np.clip(srgb[::-1], 0., 1.) * 255)))


def _pair(full, proxy, *, crop=True):
    if crop:
        y, x = np.nonzero(np.maximum(full[..., 3], proxy[..., 3]) > 0.)
        if len(y):
            box = (slice(max(0, y.min() - 3), y.max() + 4), slice(max(0, x.min() - 3), x.max() + 4))
            full, proxy = full[box], proxy[box]
    return _image(full), _image(proxy)


def _save_jpeg(sheet, destination, cap):
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    for quality in range(88, 39, -4):
        encoded = BytesIO()
        sheet.save(encoded, format='JPEG', quality=quality, optimize=True)
        if encoded.tell() <= cap:
            destination.write_bytes(encoded.getvalue())
            return
    raise ValueError(f'Comparison sheet exceeds {cap} bytes: {destination.name}')


def _tree_sheet(rows, destination, azimuth, elevation):
    width = max(pair[0].width * (2 + 2 * zoom) + 56 for _, pair, zoom in rows)
    height = 43 + sum(pair[0].height * zoom + 29 for _, pair, zoom in rows)
    sheet = Image.new('RGB', (width, height), (31, 37, 39))
    draw = ImageDraw.Draw(sheet)
    draw.text((16, 12), f'Azimuth {azimuth} deg / elevation {elevation:+d} deg     Full | Hierarchy', fill='white')
    y = 42
    for px, pair, zoom in rows:
        draw.text((16, y), f'{px} px   1x and {zoom}x', fill='white')
        y += 18
        w, h = pair[0].size
        for i, image in enumerate(pair):
            sheet.paste(image, (16 + i * (w + 4), y))
            sheet.paste(image.resize((w * zoom, h * zoom), Image.Resampling.NEAREST),
                        (32 + 2 * w + i * (w * zoom + 4), y))
        y += h * zoom + 11
    _save_jpeg(sheet, destination, 220_000)


def node_sheet(rows, destination):
    """Rows of (caption, filtered children, filtered parent), enlarged to reveal screen samples."""
    pairs = [(name, _pair(full, proxy, crop=False)) for name, full, proxy in rows]
    if not pairs:
        return
    cell_w, cell_h = 240, 135
    sheet = Image.new('RGB', (3 * cell_w, 34 + math.ceil(len(pairs) / 3) * cell_h), (31, 37, 39))
    draw = ImageDraw.Draw(sheet)
    draw.text((10, 10), 'Children | Parent    nearest-neighbour enlargement', fill='white')
    for i, (name, pair) in enumerate(pairs):
        x, y = (i % 3) * cell_w + 8, (i // 3) * cell_h + 34
        draw.text((x, y), name, fill='white')
        w, h = pair[0].size
        zoom = min(104 / w, 104 / h)
        size = max(1, round(w * zoom)), max(1, round(h * zoom))
        for j, image in enumerate(pair):
            sheet.paste(image.resize(size, Image.Resampling.NEAREST), (x + j * 111, y + 19))
    _save_jpeg(sheet, destination, 240_000)


def cost_sheet(measured, model, destination):
    """Shared height sweep for the baked hierarchy, the initial model and whole-tree levels."""
    sheet = Image.new('RGB', (1024, 600), (247, 248, 247))
    draw = ImageDraw.Draw(sheet)
    draw.text((22, 14), 'Fir A / submitted geometry and weighted contribution', fill=(20, 35, 39))
    heights = np.asarray(measured['samples_px'])
    if not np.array_equal(heights, model['samples_px']):
        raise ValueError('Cost curves must use the same height samples')
    left, right = 68, sheet.width - 26
    xs = left + np.log(heights[0] / heights) / np.log(heights[0] / heights[-1]) * (right - left)
    curves = [
        ('Baked weighted', measured['effective'], (12, 112, 105), False),
        ('Baked submitted', measured['submitted'], (28, 74, 159), False),
        ('Model weighted', model['effective'], (165, 87, 43), True),
        ('Model submitted', model['submitted'], (146, 82, 145), True),
        ('GLB / impostor', measured['current'], (49, 58, 60), False),
    ]
    for col, (label, ticks) in enumerate((
            ('Cards', (1, 10, 100, 1000, 10_000)),
            ('Triangles', (1, 10, 100, 1000, 10_000, 100_000)))):
        top, bottom = 79 + col * 241, 273 + col * 241
        draw.text((22, top - 27), label, fill=(20, 35, 39))

        def y(count):
            return bottom - np.log(np.maximum(count, 1)) / np.log(ticks[-1]) * (bottom - top)

        for count in ticks:
            py = float(y(count))
            draw.text((14, py - 6), f'{count:,}', fill=(68, 75, 76))
            draw.line((left, py, right, py), fill=(209, 215, 215))
        for px in (800, 400, 200, 100, 50, 30):
            x = float(left + np.log(heights[0] / px) / np.log(heights[0] / heights[-1]) * (right - left))
            draw.line((x, top, x, bottom), fill=(209, 215, 215))
            if col == 1:
                draw.text((x, bottom + 11), str(px), anchor='mt', fill=(68, 75, 76))
        for title, values, colour, dashed in curves:
            path = tuple(zip(xs, y(np.asarray(values)[:, col])))
            if title == 'GLB / impostor':
                steps = [path[0]]
                for (x, ny), (_, py) in zip(path[1:], path):
                    steps.extend(((x, py), (x, ny)))
                draw.line(steps, fill=colour, width=2)
            elif dashed:
                for i in range(0, len(path) - 1, 7):
                    draw.line(path[i:i + 5], fill=colour, width=2)
            else:
                draw.line(path, fill=colour, width=3)
    for i, (title, _, colour, dashed) in enumerate(curves):
        x, y = 95 + i * 160, 567
        if dashed:
            draw.line((x, y, x + 8, y), fill=colour, width=2)
            draw.line((x + 12, y, x + 20, y), fill=colour, width=2)
        else:
            draw.line((x, y, x + 20, y), fill=colour, width=3)
        draw.text((x + 26, y - 7), title, fill=(20, 35, 39))
    draw.text((sheet.width // 2, 545), 'Projected tree height (px)', anchor='mt', fill=(68, 75, 76))
    _save_jpeg(sheet, destination, 160_000)


def tree_comparisons(full_parts, alternatives, *, jpeg_dir=None):
    """Compare alternatives grouped by representation and height in 24 matched orthographic frames.

    For a given view, full and variants use identical ray grids of projected height 1600, 800 or 400.
    Full rays are reused for sizes sharing a grid; box filtering does the pixel integration.
    """
    full = _pack(full_parts)
    heights = sorted({h for meshes in alternatives.values() for h in meshes}, reverse=True)
    packed = {}
    for name, meshes in alternatives.items():
        packed[name] = {h: full if len(parts) == len(full_parts) and all(
            a is b for a, b in zip(parts, full_parts)) else _pack(parts) for h, parts in meshes.items()}
    results = {name: {h: [] for h in meshes} for name, meshes in packed.items()}
    position = np.concatenate([part['position'] for part in full])
    for azimuth, elevation, projection in _views():
        xy = position @ projection[:, :2]
        low = xy.min(axis=0)
        span = xy.max(axis=0) - low
        sheet_rows = []
        references = {}
        for px in heights:
            ray_height = 1600 if px > 400 else 800 if px > 200 else 400
            ray_ppm = ray_height / span[1]
            if ray_height not in references:
                margin = .07 * span[1] + 2 / ray_ppm
                shape = tuple(np.ceil((span + 2 * margin)[::-1] * ray_ppm).astype(int))
                ref_image, ref_tones = _capture(full, projection, low - margin, ray_ppm, shape)
                references[ray_height] = ref_image, ref_tones, low - margin
            ref_image, ref_tones, origin = references[ray_height]
            screen = tuple(np.ceil(np.array(ref_image.shape[:2]) * px / ray_height).astype(int))
            ref_small = resize(ref_image, screen)
            ref_stats = _tone_stats(resize(ref_tones, screen))
            outline = tree_density.outline(ref_small[..., 3] >= .5, px / span[1], tree_density.CLOSE)
            for name, meshes in packed.items():
                if px not in meshes:
                    continue
                parts = meshes[px]
                if parts is full:
                    image, tones = ref_image, ref_tones
                else:
                    image, tones = _capture(parts, projection, origin, ray_ppm, ref_image.shape[:2])
                small = resize(image, screen)
                row = _compare(ref_small, ref_stats, outline, small, resize(tones, screen))
                results[name][px].append(dict(azimuth=azimuth, elevation=elevation, **row))
                if name == 'cut' and jpeg_dir is not None and (azimuth, elevation) in SHEET_VIEWS and px in (400, 200, 100, 50):
                    sheet_rows.append((px, _pair(ref_small, small), 2 if px == 400 else 3 if px == 200 else 4))
        if sheet_rows:
            file = Path(jpeg_dir) / f'tree-hierarchy-tree-{SHEET_VIEWS[azimuth, elevation]}.jpg'
            _tree_sheet(sheet_rows, file, azimuth, elevation)
    return results
