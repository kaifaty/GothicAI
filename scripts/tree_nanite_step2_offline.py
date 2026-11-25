"""Compare opaque fir geometry and cooked cuts with the accepted crown, offscreen.

    uv run --no-project --with numpy --with pillow --with scipy python \
      scripts/tree_nanite_step2_offline.py FIR.glb FIR.tndg DDS_DIR OUTPUT_DIR

The two azimuths and two heights of the crown-fix comparison use one frame per
pair, stored DDS mips and expected cutout coverage under TAA. Five additional
views compare successive exact DAG cuts at their display sizes, with three
samples per display-pixel axis. A .firg or GLB candidate can be inspected
before cooking with --crown-only.
"""
from __future__ import annotations

import argparse
import json
import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage

CONTENT = Path(__file__).resolve().parent / 'content'
sys.path[:0] = [str(CONTENT), str(CONTENT / 'mesh3d/blender')]

import fir_tree
import tree_density
import tree_glb
import tree_hierarchy_raster as raster
from impostor_bake import dds_mips
from tree_bough_lod import basis

BARK = 'OWN_TREE_FIR_BARK'
BOUGHS = 'OWN_TREE_FIR_BOUGHS'
NO_GROUP = 0xffff_ffff
PROJECTION = 1.4966058
NEAR, FAR = 50., 24000.
VIEWPORT_HEIGHT = 720
CROWN_VIEWS = [(az, 0, height) for az in (0, 180) for height in (950, 633)]
LOD_VIEWS = [(90, 0), (270, 0), (0, 0), (180, 0), (90, 30)]
LOD_CUTS = ['400=11', '200=11', '100=11', '50=11']
MAX_GAP_PP, MAX_TONE_PCT = 1., .5
NEEDLE_GAIN = 1.021


def to_z_up(points):
    # Inverse of loadGltfMesh's (-x,z,-y) from the tree generator's Z-up metres.
    return points[:, [0, 2, 1]] * (-1., -1., 1.)


def to_engine(points):
    return np.asarray(points)[..., [0, 2, 1]] * (-100., 100., -100.)


def parts(vertices, indices):
    position = to_z_up(vertices['position']) / 100.
    return [{'name': name, 'position': position, 'uv': vertices['uv'], 'index': indices[material]}
            for material, name in enumerate((BARK, BOUGHS)) if len(indices[material])]


VERTEX_FIELDS = [('position', '<f4', 3), ('normal', '<f4', 3), ('uv', '<f4', 2), ('colour', '<u4')]
VERTEX = np.dtype(VERTEX_FIELDS)
SOURCE_VERTEX = np.dtype(VERTEX_FIELDS + [('bones', '<u2', 2), ('weight', 'u1'), ('lock', 'u1'), ('pad', 'V2')])


def firg(path):
    data = path.read_bytes()
    magic, version, vertex_count, triangle_count = struct.unpack_from('<4sIII', data)
    triangle_offset = 16 + vertex_count * SOURCE_VERTEX.itemsize
    if magic != b'FIRG' or version != 1 or len(data) != triangle_offset + 16 * triangle_count:
        raise ValueError(f'{path}: unsupported FIRG source')
    vertices = np.frombuffer(data, SOURCE_VERTEX, vertex_count, 16)
    triangles = np.frombuffer(data, '<u4', 4 * triangle_count, triangle_offset).reshape(-1, 4)
    if np.any(triangles[:, 3] >= 2) or np.any(triangles[:, :3] >= len(vertices)):
        raise ValueError(f'{path}: invalid FIRG triangles')
    return parts(vertices, [triangles[triangles[:, 3] == material, :3] for material in range(2)])


class CookedFir:
    def __init__(self, path):
        data = path.read_bytes()
        magic, version = struct.unpack_from('<4sI', data)
        if magic != b'TNDG' or version not in (1, 2):
            raise ValueError(f'{path}: unsupported tree DAG')
        counts = struct.unpack_from('<6I', data, 72)
        bone_count = struct.unpack_from('<I', data, 96)[0] if version == 2 else 0
        offset = 100 if version == 2 else 96
        types = (VERTEX, np.dtype(('<f4', 5)), np.dtype(('<u4', 5)),
                 np.dtype([('bounds', '<f4', 6), ('ranges', '<u4', 4)]), np.dtype('<u4'), np.dtype('<u4'))
        arrays = []
        for count, dtype in zip(counts, types):
            arrays.append(np.frombuffer(data, dtype, count, offset))
            offset += dtype.itemsize * count
        bone_bytes = bone_count * 32
        skin_bytes = counts[0] * 8 if bone_count else 0
        if len(data) != offset + bone_bytes + skin_bytes:
            raise ValueError(f'{path}: invalid DAG section sizes')
        self.vertices, self.groups, self.clusters, self.meshlets, refs, primitives = arrays
        self.triangles = []
        for first, start, vertex_count, triangle_count in self.meshlets['ranges']:
            local = (primitives[start:start + triangle_count, None] >> np.uint32([0, 8, 16])) & 255
            self.triangles.append(refs[first:first + vertex_count][local])
        self.stats = {
            'version': version, 'bytes': len(data), 'vertices': counts[0], 'groups': counts[1],
            'clusters': counts[2], 'meshlets': counts[3], 'stored_triangles': counts[5],
            'bones': bone_count, 'bone_bytes': bone_bytes, 'skin_bytes': skin_bytes,
        }

    def projected_error(self, camera, looking):
        # Same near/far guard and sphere surface distance as TreeNanite::clusterSelected.
        center, radius, error = self.groups[:, :3], self.groups[:, 3], self.groups[:, 4]
        delta = center - to_engine(camera)
        depth = delta @ (to_engine(looking) / 100.)
        guard = 8. * np.finfo(np.float32).eps * np.maximum(1., np.abs(depth) + radius)
        crosses = (np.abs(depth - NEAR) <= radius + guard) | (np.abs(depth - FAR) <= radius + guard)
        distance = np.maximum(np.linalg.norm(delta, axis=1) - radius, NEAR)
        return np.where(crosses, np.inf, error * (.5 * PROJECTION * VIEWPORT_HEIGHT / distance))

    def cut(self, threshold=0., camera=None, looking=None):
        fine, parent, materials, starts, counts = self.clusters.T
        if threshold == 'terminal':
            selected = np.isinf(self.groups[parent, 4])
        elif threshold == 0.:
            selected = fine == NO_GROUP
        else:
            error = self.projected_error(camera, looking)
            fine_error = np.r_[error, 0.]
            fine_ids = np.where(fine == NO_GROUP, len(error), fine)
            selected = (fine_error[fine_ids] <= threshold) & (error[parent] > threshold)
        ids = np.flatnonzero(selected)
        triangles = [[], []]
        meshlet_count = vertex_ref_count = 0
        for cluster in ids:
            start, count = int(starts[cluster]), int(counts[cluster])
            triangles[materials[cluster]].extend(self.triangles[start:start + count])
            meshlet_count += count
            vertex_ref_count += int(self.meshlets['ranges'][start:start + count, 2].sum())
        indices = [np.concatenate(group) if group else np.empty((0, 3), np.int32) for group in triangles]
        active_vertices = np.zeros(len(self.vertices), dtype=bool)
        for group in indices:
            active_vertices[group] = True
        vertex_count = int(active_vertices.sum())
        triangle_count = sum(map(len, indices))
        # Slice of the resident TNDG accessed by the cut, independent of allocator padding.
        bytes_read = (len(ids) * 20 + meshlet_count * 40 + (vertex_ref_count + triangle_count) * 4
                      + vertex_count * (VERTEX.itemsize + (8 if self.stats['bones'] else 0)))
        stats = {
            'clusters': len(ids), 'meshlets': meshlet_count, 'triangles': triangle_count, 'vertices': vertex_count,
            'active_geometry_bytes': bytes_read, 'bark_triangles': len(indices[0]), 'needle_triangles': len(indices[1]),
        }
        return parts(self.vertices, indices), stats


def textures(directory):
    out = {}
    for name, filename in ((BARK, 'bark-albedo.dds'), (BOUGHS, 'boughs-albedo.dds')):
        mips = [level.astype(np.float32) / 255. for level in dds_mips(directory / filename)]
        for level in mips:
            rgb = level[..., :3]
            rgb[:] = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
        out[name] = mips
    return out


def frame(parts, azimuth, elevation, height):
    az, el = map(math.radians, (azimuth, elevation))
    view = np.array((math.cos(az) * math.cos(el), math.sin(az) * math.cos(el), math.sin(el)))
    projection = basis(view)
    bounds = np.concatenate([part['position'] for part in parts]) @ projection[:, :2]
    low, high = bounds.min(axis=0), bounds.max(axis=0)
    span = high - low
    ppm = height / span[1]
    margin = .07 * span[1] + 2 / ppm
    shape = tuple(np.ceil((span + 2 * margin)[::-1] * ppm).astype(int))
    return projection, low - margin, ppm, shape


def capture(parts, maps, screen, samples, gain=1.):
    """Return premultiplied linear RGB and expected foreground coverage on one orthographic frame."""
    projection, low, ppm, size = screen
    shape = tuple(axis * samples for axis in size)
    count = math.prod(shape)
    opaque_z = np.full(count, np.inf, np.float32)
    opaque = np.zeros((count, 4), np.float32)
    needle = np.zeros(count, bool) if gain != 1. else None
    soft = []
    for part in parts:
        name = part['name']
        cutout = part.get('material', {}).get('alphaMode') == 'MASK'
        mips = maps[name]
        points = part['position'] @ projection
        points[:, :2] = (points[:, :2] - low) * ppm * samples
        lod = raster._triangle_mips(dict(part, rgba_mips=mips), points, 1. / samples) if name == BOUGHS else None
        for face, pixel, z, u, v in tree_density.fragments(points, part['index'], shape, part['uv']):
            if cutout:
                rgb = raster._sample_mips(mips, u, v, lod[face])
                keep = (rgb[:, 3] > 0.) & (z < opaque_z[pixel])
                soft.append((pixel[keep], z[keep], rgb[keep]))
                continue
            order = np.lexsort((z, pixel))
            sorted_pixels = pixel[order]
            at = order[np.r_[True, sorted_pixels[1:] != sorted_pixels[:-1]]] if len(order) else order
            at = at[z[at] < opaque_z[pixel[at]]]
            pixel = pixel[at]
            if lod is None:
                # Bark repeats, and crown-fix used the mip-zero nearest sample for wood.
                y, x = raster._texel_indices(mips[0].shape, u[at], v[at], repeat=True)
                rgb = mips[0][y, x]
            else:
                rgb = raster._sample_mips(mips, u[at], v[at], lod[face[at]])
            opaque[pixel, :3] = rgb[:, :3]
            opaque[pixel, 3] = 1.
            opaque_z[pixel] = z[at]
            if needle is not None:
                needle[pixel] = name == BOUGHS
    gain_luma = None
    if needle is not None:
        old_rgb = opaque[needle, :3]
        new_rgb = (old_rgb * gain).clip(0., 1.)
        gain_luma = (new_rgb - old_rgb) @ raster.LUMA
        opaque[needle, :3] = new_rgb
    if soft:
        pixel, z, texels = (np.concatenate(columns) for columns in zip(*soft))
        keep = z < opaque_z[pixel]
        pixel, z, texels = pixel[keep], z[keep], texels[keep]
        if len(pixel):
            order = np.lexsort((z, pixel))
            pixel, texels = pixel[order], texels[order]
            # Segment the cumulative log transmittance by pixel. Clamping protects log(0);
            # an exactly opaque layer leaves at most one part per million residual light.
            log = np.log1p(-texels[:, 3].astype(np.float64).clip(0., 1. - 1e-6))
            cumulative = np.cumsum(log)
            starts = np.r_[0, np.flatnonzero(pixel[1:] != pixel[:-1]) + 1]
            base = np.r_[0., cumulative[starts[1:] - 1]]
            first = np.repeat(base, np.diff(np.r_[starts, len(pixel)]))
            weight = np.exp(cumulative - log - first) * texels[:, 3]
            alpha = np.bincount(pixel, weights=weight, minlength=count).clip(0., 1.)
            opaque *= (1. - alpha[:, None]).astype(np.float32)
            if gain_luma is not None:
                gain_luma *= 1. - alpha[needle]
            opaque[:, 3] += alpha
            for channel in range(3):
                opaque[:, channel] += np.bincount(pixel, weights=weight * texels[:, channel], minlength=count)
    image = opaque.reshape(size[0], samples, size[1], samples, 4).mean(axis=(1, 3))
    gain_per_pixel = gain_luma.sum(dtype=np.float64) / opaque[:, 3].sum(dtype=np.float64) if gain_luma is not None else 0.
    return image, gain_per_pixel


def contour(image, ppm):
    solid = image[..., 3] >= .5
    radius = round(.4 * ppm)
    if radius and solid.any():
        grown = ndimage.distance_transform_edt(~solid) <= radius
        solid = solid | (ndimage.distance_transform_edt(grown) > radius)
    return ndimage.binary_fill_holes(solid)


def windows(alpha, region, height):
    labels, count = ndimage.label(region & (alpha < .5))
    weights = np.bincount(labels.ravel(), weights=(1. - alpha).ravel(), minlength=count + 1)
    large = np.flatnonzero(weights[1:] >= 150 * (height / 950.) ** 2) + 1
    if not len(large):
        return {'share_pct': 0., 'largest_px2': 0., 'largest_box': None, 'count': 0}
    largest = int(large[np.argmax(weights[large])])
    y, x = ndimage.find_objects(labels)[largest - 1]
    return {
        'share_pct': 100 * float(weights[large].sum()) / float((1. - alpha)[region].sum()),
        'largest_px2': float(weights[largest]), 'largest_box': [x.start, y.start, x.stop, y.stop], 'count': len(large),
    }


def gaps(image, common, height, tiers):
    alpha = image[..., 3]
    y, x = np.nonzero(common)
    mid = (y.min() + .30 * np.ptp(y), y.min() + .70 * np.ptp(y))
    rows, columns = np.indices(common.shape, sparse=True)
    band = common & (rows >= mid[0]) & (rows <= mid[1])
    left = columns < (x.min() + x.max()) // 2
    regions = {'whole': common, 'mid_left': band & left, 'mid_right': band & ~left}
    out = {name: float(100 * (1. - alpha[region]).mean()) for name, region in regions.items()}
    out['windows'] = {name: windows(alpha, regions[name], height) for name in ('mid_left', 'mid_right')}
    luma = image[..., :3] @ raster.LUMA
    out['tone'] = float(luma.sum(dtype=np.float64) / alpha.sum(dtype=np.float64))
    out['tier_gaps_pp'] = {}
    for label, y0, y1 in tiers:
        tier = common & (rows >= y0) & (rows < y1)
        for side, half in (('left', left), ('right', ~left)):
            region = tier & half
            if region.any():
                out['tier_gaps_pp'][f'{label}_{side}'] = float(100 * (1. - alpha[region]).mean())
    return out


def compare(old, new, common, height, tiers, ppm):
    old, old_gain = old
    new, new_gain = new
    old_stats, new_stats = [gaps(image, common, height, tiers) for image in (old, new)]
    change = {key: new_stats[key] - old_stats[key] for key in ('whole', 'mid_left', 'mid_right')}
    max_gap = max(map(abs, change.values()))
    tone = 100. * (new_stats['tone'] / old_stats['tone'] - 1.)
    raw_tone = 100. * ((new_stats['tone'] - new_gain) / (old_stats['tone'] - old_gain) - 1.)
    old_outline, new_outline = contour(old, ppm), contour(new, ppm)
    iou = float((old_outline & new_outline).sum() / (old_outline | new_outline).sum())
    return {
        'before': old_stats, 'after': new_stats, 'delta_gap_pp': change, 'max_gap_pp': max_gap,
        'tone_delta_pct': tone, 'uncalibrated_tone_delta_pct': raw_tone, 'silhouette_iou': iou,
        'passes': max_gap <= MAX_GAP_PP and abs(tone) <= MAX_TONE_PCT,
    }


def sheet(path, title, images, labels, common):
    panels = []
    for image in images:
        alpha = image[..., 3]
        linear = image[..., :3] + (1. - alpha[..., None]) * (.55, .62, .70)
        srgb = np.where(linear <= .0031308, 12.92 * linear, 1.055 * linear ** (1. / 2.4) - .055)
        pixels = np.uint8(np.rint(srgb.clip(0., 1.) * 255))
        openings = pixels.copy()
        openings[common & (alpha < .5)] = (217, 31, 26)
        panels.append((Image.fromarray(pixels[::-1]), Image.fromarray(openings[::-1])))
    width, height = panels[0][0].size
    result = Image.new('RGB', (width * len(panels) + 12 * (len(panels) + 1), 2 * height + 52), (247, 248, 247))
    draw = ImageDraw.Draw(result)
    draw.text((12, 8), title, fill=(20, 35, 39))
    for i, ((tree, openings), label) in enumerate(zip(panels, labels)):
        x = 12 + i * (width + 12)
        result.paste(tree, (x, 26))
        draw.text((x, 28 + height), label, fill=(20, 35, 39))
        result.paste(openings, (x, 44 + height))
    result.save(path, quality=91, optimize=True)


def parse_levels(values):
    levels = []
    for item in values:
        height, separator, error = item.partition('=')
        if not separator:
            raise ValueError('expected --lod HEIGHT=ERROR or HEIGHT=terminal')
        height = int(height)
        error = error if error == 'terminal' else float(error)
        if height <= 0 or (levels and height >= levels[-1][0]) or (error != 'terminal' and not 0 < error < math.inf):
            raise ValueError('LOD heights must decrease and numeric errors must be positive and finite')
        levels.append((height, error))
    return levels


def eye_at(screen, source):
    projection, _, ppm, _ = screen
    camera_direction = -projection[:, 2]
    center = (source.min(axis=0) + source.max(axis=0)) / 2.
    distance = .5 * PROJECTION * VIEWPORT_HEIGHT / ppm
    return center + camera_direction * distance, -camera_direction


def whorl_tiers(screen, whorls):
    projection, low, ppm, _ = screen
    # At level azimuths these rows bound individual whorls in world height. At
    # elevation they follow each whorl's projection on the central trunk axis.
    centers = (whorls * projection[2, 1] - low[1]) * ppm
    edges = (centers[1:] + centers[:-1]) * .5
    edges = np.r_[centers[0] - (edges[0] - centers[0]), edges,
                  centers[-1] + (centers[-1] - edges[-1])]
    return [(f'W{i:02d}', edges[i], edges[i + 1]) for i in range(len(whorls))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('textures', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--crown-only', action='store_true')
    parser.add_argument('--needle-gain', type=float, default=NEEDLE_GAIN, help='linear albedo gain on opaque needles')
    parser.add_argument('--samples', type=int, choices=(1, 2, 3), help='override samples per display-pixel axis')
    parser.add_argument('--lod', action='append', help='HEIGHT=ERROR or HEIGHT=terminal, in descending heights')
    args = parser.parse_args()
    if not 0 < args.needle_gain < math.inf:
        parser.error('needle gain must be positive and finite')
    crown_samples, lod_samples = (args.samples or 1, args.samples or tree_density.RAYS)
    args.output.mkdir(parents=True, exist_ok=True)
    maps = textures(args.textures)
    baseline = tree_glb.read(args.baseline)
    candidate = None
    if args.candidate.suffix == '.tndg':
        candidate = CookedFir(args.candidate)
        near, near_stats = candidate.cut()
    elif args.crown_only and args.candidate.suffix in ('.firg', '.glb'):
        near = firg(args.candidate) if args.candidate.suffix == '.firg' else tree_glb.read(args.candidate)
        near_stats = {'triangles': sum(len(part['index']) for part in near)}
    else:
        parser.error('expected a cooked .tndg, or use --crown-only with a .firg or .glb')
    cfg = json.loads(Path(fir_tree.__file__).with_name('fir.json').read_text())
    fir = fir_tree.build(cfg)
    whorl_bounds = {}
    for branch in fir.branches[fir.stubs:]:
        whorl_bounds.setdefault(branch.whorl, []).append(branch.points[0, 2])
    whorls = np.array([np.median(whorl_bounds[k]) for k in sorted(whorl_bounds)])
    source = np.concatenate([part['position'] for part in baseline])
    levels = parse_levels(args.lod or LOD_CUTS)
    report = {
        'card_source': str(args.baseline.resolve()), 'geometry_source': str(args.candidate.resolve()),
        'textures': str(args.textures.resolve()), 'samples_per_axis': {'crown': crown_samples, 'lod': lod_samples},
        'baseline_triangles': sum(len(part['index']) for part in baseline), 'needle_albedo_gain': args.needle_gain,
        'limits': {'max_gap_pp': MAX_GAP_PP, 'max_tone_pct': MAX_TONE_PCT},
        'near': near_stats, 'cooked': candidate.stats if candidate else None, 'crown': [], 'lod': [],
    }
    for azimuth, elevation, height in CROWN_VIEWS:
        screen = frame(baseline, azimuth, elevation, height)
        old = capture(baseline, maps, screen, crown_samples)
        new = capture(near, maps, screen, crown_samples, args.needle_gain)
        ppm = screen[2]
        common = contour(old[0], ppm)
        result = compare(old, new, common, height, whorl_tiers(screen, whorls), ppm)
        key = f'az{azimuth:03d}-{height}'
        report['crown'].append(dict(key=key, plus_y_side='left' if azimuth == 0 else 'right', **result))
        sheet(args.output / f'{key}.jpg', f'Crown view {azimuth} degrees, {height} px', (old[0], new[0]),
              ('Accepted cards', 'Opaque geometry'), common)
        print(f'{key}: gap {result["max_gap_pp"]:.3f} pp, tone {result["tone_delta_pct"]:+.3f}%, '
              f'IoU {result["silhouette_iou"]:.5f}', flush=True)
    if not args.crown_only:
        for azimuth, elevation in LOD_VIEWS:
            previous, previous_stats = near, near_stats
            for height, error in levels:
                screen = frame(baseline, azimuth, elevation, height)
                camera, looking = eye_at(screen, source)
                coarse, stats = candidate.cut(error, camera, looking)
                old = capture(baseline, maps, screen, lod_samples)
                fine, new = [capture(mesh, maps, screen, lod_samples, args.needle_gain) for mesh in (previous, coarse)]
                ppm = screen[2]
                common = contour(old[0], ppm)
                tiers = whorl_tiers(screen, whorls)
                key = f'lod-az{azimuth:03d}-el{elevation:02d}-{height}'
                result = compare(fine, new, common, height, tiers, ppm)
                to_cards = compare(old, new, common, height, tiers, ppm)
                projected = candidate.projected_error(camera, looking)[np.isfinite(candidate.groups[:, 4])]
                terminal_error = float(projected.max()) if len(projected) else 0.
                report['lod'].append({
                    'key': key, 'height': height, 'threshold': error, 'from_cut': previous_stats, 'to_cut': stats,
                    'terminal_threshold': terminal_error if math.isfinite(terminal_error) else None,
                    'adjacent': result, 'against_cards': to_cards,
                })
                sheet(args.output / f'{key}.jpg', f'LOD {height} px, azimuth {azimuth}, elevation {elevation}',
                      (old[0], fine[0], new[0]), ('Accepted cards', 'Previous cut', 'Current cut'), common)
                print(f'{key}: {previous_stats["triangles"]:,} -> {stats["triangles"]:,} tris, '
                      f'gap {result["max_gap_pp"]:.3f} pp, tone {result["tone_delta_pct"]:+.3f}%', flush=True)
                previous, previous_stats = coarse, stats
    report['passed'] = all(row['passes'] for row in report['crown']) and all(row['adjacent']['passes'] for row in report['lod'])
    (args.output / 'metrics.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
