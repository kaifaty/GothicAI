"""Compare opaque fir geometry and cooked cuts with the accepted crown, offscreen.

    uv run --no-project --with numpy --with pillow --with scipy python \
      scripts/tree_nanite_step2_offline.py FIR.glb FIR.tndg DDS_DIR OUTPUT_DIR

The two azimuths and two heights of the crown-fix comparison use one frame per
pair, stored DDS mips and expected cutout coverage under TAA. Five additional
views compare successive exact DAG cuts at their display sizes, with three
samples per display-pixel axis. A .firg or GLB candidate can be inspected
before cooking with --crown-only.

With --card-set, compare the fitted levels with the authored card ladder and
the baked impostor. A contour cache in --far-geometry reconstructs the pilot's
quantized meshlets from its four cooked card operands. Perspective readouts
use the pilot's 0.5 alpha test in the bind pose and the expectation over its
complementary per-pixel level selection.
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
from impostor_bake import FRAMES, PADDING, TILE, dds_mips, oct_direction, oct_encode
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
CARD_LOD_HEIGHTS = (400, 201, 200, 101, 100, 51, 50, 49, 48)
CARD_MATCH_HEIGHTS = (200, 100, 50, 48)
ROUTE_CHECKPOINTS = (0, 108, 168, 192, 240, 270, 291, 304, 310, 311, 348)
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
        out[name] = texture_mips(directory / filename, srgb=True)
    return out


def texture_mips(path, *, srgb=False):
    mips = [level.astype(np.float32) / 255. for level in dds_mips(path)]
    if srgb:
        for level in mips:
            rgb = level[..., :3]
            rgb[:] = np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)
    return mips


def expand_ranges(ranges):
    starts, counts = ranges.T.astype(np.int64)
    sources = np.repeat(np.arange(len(ranges)), counts)
    offsets = np.arange(len(sources)) - np.repeat(np.cumsum(counts) - counts, counts)
    return sources, starts[sources] + offsets


class ContourCache:
    """The FCTL face-to-meshlet tables consumed by the adaptive pilot."""

    def __init__(self, directory):
        path = directory / 'contours.bin'
        data = path.read_bytes()
        header = struct.Struct('<4s11I')
        magic, version, self.mip_base, self.mip_count, charts, meshlets, vertices, primitives, *faces = header.unpack_from(data)
        if (magic, version, self.mip_base, self.mip_count) != (b'FCTL', 1, 96, 257):
            raise ValueError(f'{path}: unsupported contour cache')
        words = np.frombuffer(data, '<u4', offset=header.size)
        expected = 4 * sum(faces) + 2 * charts * self.mip_count + 4 * meshlets + vertices + primitives
        if len(words) != expected:
            raise ValueError(f'{path}: invalid contour section sizes')
        offset = 0

        def section(count, stride):
            nonlocal offset
            result = words[offset:offset + count * stride].reshape(count, stride)
            offset += count * stride
            return result

        self.faces = {level: section(count, 4) for level, count in enumerate(faces, 1)}
        self.ranges = section(charts * self.mip_count, 2).reshape(charts, self.mip_count, 2)
        self.meshlets = section(meshlets, 4)
        self.bary = section(vertices, 1).ravel()
        self.prims = section(primitives, 1).ravel()
        self.sources = {level: CookedFir(directory / f'cards-l{level}.tndg') for level in self.faces}
        self.wood = {}
        for level, source in self.sources.items():
            self.wood[level] = next(part for part in source.cut()[0] if part['name'] == BARK)
            if np.any(self.faces[level][:, :3] >= len(source.vertices)) or np.any(self.faces[level][:, 3] >= charts):
                raise ValueError(f'{path}: face stream {level} is outside the source vertex or chart table')
        if (np.any(self.ranges[..., 0].astype(np.uint64) + self.ranges[..., 1] > meshlets) or
                np.any(self.meshlets[:, :2].astype(np.uint64) + self.meshlets[:, 2:] > (vertices, primitives)) or
                np.any((self.bary & 65535) + (self.bary >> 16) > 65535)):
            raise ValueError(f'{path}: invalid contour range or barycentric encoding')
        self.stats = {'file': str(path.resolve()), 'bytes': len(data), 'version': version, 'mip_base': self.mip_base,
                      'mip_bins': self.mip_count, 'charts': charts, 'meshlets': meshlets, 'vertices': vertices,
                      'triangles': primitives, 'faces_by_level': faces}

    def meshes(self, level, screen, eye, texture_extent):
        original = self.sources[level].vertices
        faces = self.faces[level]
        original_xyz, original_uv = original['position'], original['uv']
        indices = faces[:, :3]
        projection = project(to_z_up(original_xyz) / 100., screen, 1, eye)[indices]
        uv_faces = original_uv[indices]
        varying, dx, dy, area = perspective_coefficients(projection, uv_faces, 1)
        middle = varying.mean(axis=1)
        uv = middle[:, :2] / middle[:, 2, None]
        du, dv = [(derivative[:, :2] - uv * derivative[:, 2, None]) / middle[:, 2, None] for derivative in (dx, dy)]
        rho = np.maximum(np.linalg.norm(du * texture_extent, axis=1), np.linalg.norm(dv * texture_extent, axis=1))
        active = np.flatnonzero(np.abs(area) > 1e-12)
        top_bin = self.mip_base + self.mip_count - 1
        levels = np.floor(32 * np.log2(np.maximum(rho[active], 1.)) + .5)
        levels = np.clip(levels, self.mip_base, top_bin).astype(np.int64)
        ranges = self.ranges[faces[active, 3], levels - self.mip_base]
        face_meshlet, meshlet_ids = expand_ranges(ranges)
        templates = self.meshlets[meshlet_ids]
        meshlet_vert, vertex_ids = expand_ranges(templates[:, (0, 2)])
        meshlet_tri, primitive_ids = expand_ranges(templates[:, (1, 3)])
        face_by_meshlet = active[face_meshlet]
        face_by_vertex = face_by_meshlet[meshlet_vert]
        source_indices = indices[face_by_vertex]
        encoded = self.bary[vertex_ids]
        u = (encoded & 65535).astype(np.float32) / 65535
        v = (encoded >> 16).astype(np.float32) / 65535
        weights = np.stack((np.float32(1.) - u - v, u, v), axis=1)
        packed = self.prims[primitive_ids, None]
        triangles = (packed >> np.array((0, 8, 16), dtype=np.uint32)) & 255
        if np.any(triangles >= templates[meshlet_tri, 2, None]):
            raise ValueError('Contour triangle is outside its meshlet')
        first_vertex = np.cumsum(templates[:, 2], dtype=np.int64) - templates[:, 2]
        triangles = triangles + first_vertex[meshlet_tri, None]
        xyz = np.einsum('vi,vic->vc', weights, original_xyz[source_indices])
        uv = np.einsum('vi,vic->vc', weights, original_uv[source_indices])
        needles = {'name': BOUGHS, 'position': to_z_up(xyz) / 100., 'uv': uv, 'index': triangles,
                   'gradient_source': (dx, dy, face_by_meshlet[meshlet_tri])}
        wood = self.wood[level]
        stats = {'triangles': len(wood['index']) + len(triangles), 'crown_triangles': len(triangles),
                 'adaptive_meshlets': len(meshlet_ids), 'source_faces': len(faces), 'generated_vertices': len(xyz)}
        return [wood, needles], stats


class CardLevels:
    """The authored drawable levels and the captured views of one accepted tree."""

    def __init__(self, directory, baseline):
        meshes = directory / 'meshes'
        entry = json.loads((meshes / 'meshes.json').read_text())[baseline.stem]
        levels = entry['levels']
        self.paths = [meshes / entry['glb'], *(meshes / step['glb'] for step in levels[:-1])]
        self.thresholds = tuple(float(step['px']) for step in levels)
        self.geometry = [tree_glb.read(path) for path in self.paths]
        self.impostor = {name: texture_mips(meshes / levels[-1]['impostor'][name], srgb=(name == 'albedo'))
                         for name in ('albedo', 'surface')}
        bounds = np.concatenate([part['position'] for part in self.geometry[0]])
        low, high = bounds.min(axis=0), bounds.max(axis=0)
        self.center = (low + high) * .5
        self.radius = np.linalg.norm(high - low) * .5
        self.height = float(high[2] - low[2])

    def select(self, height):
        return sum(1 for threshold in self.thresholds if height < threshold)

    def weights(self, height):
        threshold = np.array(self.thresholds)
        amount = np.clip((56. - 49. * height / threshold) / 10., 0., 1.)
        coarse = amount * amount * (3. - 2. * amount)
        return -np.diff(np.r_[1., coarse, 0.])

    def image(self, level, maps, screen, samples, camera, *, alpha_cutoff=None, eye=None):
        if level < len(self.geometry):
            return capture(self.geometry[level], maps, screen, samples, alpha_cutoff=alpha_cutoff, eye=eye)
        return capture_impostor(self, screen, samples, camera, alpha_cutoff=alpha_cutoff, eye=eye)


def far_geometry(directory, cards):
    """Load the cooked far meshes used by the route or raw FIRG for fitting."""
    if (directory / 'contours.bin').exists():
        return ContourCache(directory)
    gains_path = directory / 'opaque-lod-gain.tsv'
    gains = [float(n) for n in gains_path.read_text().split()] if gains_path.exists() else [1.] * 4
    if len(gains) != 4 or not all(0. < gain < math.inf for gain in gains):
        raise ValueError(f'{gains_path}: expected four finite positive gains')
    result = {}
    for level, path in enumerate(cards.paths[1:], 1):
        cooked = directory / f'opaque-l{level}.tndg'
        if cooked.exists():
            meshes, stats = CookedFir(cooked).cut()
        else:
            meshes = firg(directory / path.with_suffix('.firg').name)
            stats = {'triangles': sum(len(part['index']) for part in meshes)}
        result[level] = meshes, stats, gains[level - 1]
    return result


def route_screen(row, source):
    eye = to_z_up(np.asarray([row[:3]], dtype=float))[0] / 100.
    look = to_z_up(np.asarray([row[3:6]], dtype=float))[0] / 100.
    forward = look - eye
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, (0., 0., 1.))
    right /= np.linalg.norm(right)
    projection = np.stack((right, np.cross(right, forward), forward), axis=1)
    k = .5 * PROJECTION * VIEWPORT_HEIGHT
    coords = (source - eye) @ projection
    pixels = coords[:, :2] * k / coords[:, 2, None]
    low = np.floor(pixels.min(axis=0) - 5)
    high = np.ceil(pixels.max(axis=0) + 5)
    shape = tuple(map(int, (high - low)[::-1]))
    return (projection, low / k, k, shape), eye, forward


def card_grid_screen(cards, source, azimuth, elevation, height):
    azimuth, elevation = map(math.radians, (azimuth, elevation))
    direction = np.array((math.cos(azimuth) * math.cos(elevation), math.sin(azimuth) * math.cos(elevation),
                          math.sin(elevation)))
    distance = .5 * PROJECTION * VIEWPORT_HEIGHT * cards.height / height
    eye = cards.center + direction * distance
    return route_screen(np.concatenate((to_engine(eye), to_engine(cards.center))), source)


def impostor_basis(direction):
    side = np.cross((0., 1., 0.), direction)
    right = side / np.linalg.norm(side) if side @ side > 1e-12 else np.array((1., 0., 0.))
    return np.stack((right, np.cross(direction, right)), axis=1)


def capture_impostor(cards, screen, samples, camera, *, alpha_cutoff=None, eye=None):
    """Replay the stock three-view octahedral blend using the actual captured DDS."""
    projection, low, ppm, (height, width) = screen
    y, x = np.indices((height * samples, width * samples))
    xy = (np.stack((x.ravel(), y.ravel()), axis=-1) + .5) / (ppm * samples) + low
    eye_local = to_engine(camera - cards.center) / (100 * cards.radius)
    view = eye_local / np.linalg.norm(eye_local)
    view_basis = impostor_basis(view)

    def ray_at(coords):
        if eye is None:
            xy_world = (coords - cards.center @ projection[:, :2]) @ projection[:, :2].T
            ray = np.broadcast_to(-view, xy_world.shape)
            plane = to_engine(xy_world) / (100 * cards.radius)
        else:
            direction = coords @ projection[:, :2].T + projection[:, 2]
            ray = to_engine(direction) / 100
            plane = eye_local - ray * ((eye_local @ view) / (ray @ view))[:, None]
            ray /= np.linalg.norm(ray, axis=1, keepdims=True)
        return plane, ray

    position, ray = ray_at(xy)
    main_uv = position @ view_basis
    visible = (np.abs(main_uv) <= 1.).all(axis=1)
    dx, dy = [ray_at(xy + delta)[0] - position for delta in np.eye(2) / ppm]
    grid = oct_encode(view) * (FRAMES - 1)
    cell = np.minimum(grid.astype(int), FRAMES - 2)
    blend = grid - cell
    middle = np.array((1, 0) if blend[0] >= blend[1] else (0, 1))
    angles = (1. - max(blend), abs(blend[0] - blend[1]), min(blend))
    frame_pixels = TILE - 2 * PADDING
    atlas_side = FRAMES * TILE
    albedo = cards.impostor['albedo']
    surface = cards.impostor['surface']
    premultiplied = np.zeros((len(xy), 4), np.float32)
    for tile, weight in zip((cell, cell + middle, cell + 1), angles):
        if weight == 0.:
            continue
        direction = oct_direction(*(tile * (2. / (FRAMES - 1)) - 1.))
        axes = impostor_basis(direction) * (1., -1.)
        projected_dx, projected_dy = (gradient @ axes * (.5 * frame_pixels) for gradient in (dx, dy))
        spread = np.maximum(np.linalg.norm(projected_dx, axis=1), np.linalg.norm(projected_dy, axis=1))
        mip = np.clip(np.log2(np.maximum(spread, 1.)), 0., min(4, len(surface) - 1))
        ray_dot = ray @ direction
        relevant = np.flatnonzero(visible & (ray_dot < -.1))
        ray_dot, origin, look, mip = ray_dot[relevant], position[relevant], ray[relevant], mip[relevant]
        plane_dot = origin @ direction
        flat = origin - look * (plane_dot / ray_dot)[:, None]
        padding = ((PADDING - .5 * np.exp2(np.ceil(mip))) / frame_pixels)[:, None]
        initial = np.clip(flat @ axes * .5 + .5, -padding, 1. + padding)
        uv0 = (tile * TILE + PADDING + initial * frame_pixels) / atlas_side
        depth = raster._sample_mips(surface, uv0[:, 0], uv0[:, 1], mip)[:, 3] * 2. - 1.
        adjusted = origin + look * ((depth - plane_dot) / ray_dot)[:, None]
        corrected = adjusted @ axes * .5 + .5
        inside = ((corrected >= 0.) & (corrected <= 1.)).all(axis=1)
        uv = (tile * TILE + PADDING + corrected[inside] * frame_pixels) / atlas_side
        rgba = raster._sample_mips(albedo, uv[:, 0], uv[:, 1], mip[inside])
        rgba[:, :3] *= rgba[:, 3, None]
        premultiplied[relevant[inside]] += weight * rgba
    if alpha_cutoff is not None:
        alpha = premultiplied[:, 3].copy()
        covered = alpha >= alpha_cutoff
        premultiplied[covered, :3] /= alpha[covered, None]
        premultiplied[~covered, :3] = 0.
        premultiplied[:, 3] = covered
    return premultiplied.reshape(height, samples, width, samples, 4).mean(axis=(1, 3)), 0.


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


def project(points, screen, samples, eye=None):
    projection, low, ppm, _ = screen
    pixels = (points if eye is None else points - eye) @ projection
    if eye is not None:
        if np.any(pixels[:, 2] <= NEAR / 100):
            raise ValueError('A route camera intersects the near plane of the tree')
        pixels[:, :2] /= pixels[:, 2, None]
    pixels[:, :2] = (pixels[:, :2] - low) * ppm * samples
    return pixels


def perspective_coefficients(vertices, uv, samples):
    reciprocal = 1. / vertices[:, :, 2]
    varying = np.concatenate((uv * reciprocal[..., None], reciprocal[..., None]), axis=-1)
    a, b = np.moveaxis((vertices[:, 1:, :2] - vertices[:, :1, :2]) / samples, 1, 0)
    s, t = np.moveaxis(varying[:, 1:] - varying[:, :1], 1, 0)
    area = a[:, 0] * b[:, 1] - a[:, 1] * b[:, 0]
    safe_area = np.where(abs(area) < 1e-20, 1., area)[:, None]
    dx = (s * b[:, 1:2] - t * a[:, 1:2]) / safe_area
    dy = (t * a[:, 0:1] - s * b[:, 0:1]) / safe_area
    return varying, dx, dy, area


def perspective_fragments(points, part, shape, samples, mips):
    """Perspective UVs and texture gradients at each route ray hit."""
    ids = part['index']
    vertices, uv = points[ids], part['uv'][ids]
    varying, dx, dy, _ = perspective_coefficients(vertices, uv, samples)
    parent = part.get('gradient_source')
    extent = mips[0].shape[1::-1]
    for face, pixel, _, _, _, weights in tree_density.fragments(points, ids, shape, with_weights=True):
        hit = np.einsum('fn,fnc->fc', np.stack(weights, axis=1), varying[face])
        z = 1. / hit[:, 2]
        at = hit[:, :2] * z[:, None]
        if parent is None:
            sample_dx, sample_dy = dx[face], dy[face]
        else:
            parent_dx, parent_dy, parent_face = parent
            sample_dx, sample_dy = parent_dx[parent_face[face]], parent_dy[parent_face[face]]
        grad_x, grad_y = [(d[:, :2] - at * d[:, 2, None]) * z[:, None] * extent for d in (sample_dx, sample_dy)]
        spread = np.maximum(np.linalg.norm(grad_x, axis=1), np.linalg.norm(grad_y, axis=1))
        lod = np.clip(np.log2(np.maximum(spread, 1.)), 0., len(mips) - 1)
        yield face, pixel, z, at[:, 0], at[:, 1], lod


def capture(parts, maps, screen, samples, gain=1., *, alpha_cutoff=None, eye=None):
    """Return premultiplied linear RGB and expected foreground coverage on one orthographic frame."""
    size = screen[3]
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
        points = project(part['position'], screen, samples, eye)
        if eye is None:
            face_lod = raster._triangle_mips(dict(part, rgba_mips=mips), points, 1. / samples) if name == BOUGHS else None
            fragments = ((face, pixel, z, u, v, None if face_lod is None else face_lod[face])
                         for face, pixel, z, u, v in tree_density.fragments(points, part['index'], shape, part['uv']))
        else:
            fragments = perspective_fragments(points, part, shape, samples, mips)
        for _, pixel, z, u, v, lod in fragments:
            if cutout and alpha_cutoff is None:
                rgb = raster._sample_mips(mips, u, v, lod)
                keep = (rgb[:, 3] > 0.) & (z < opaque_z[pixel])
                soft.append((pixel[keep], z[keep], rgb[keep]))
                continue
            if cutout:
                texels = raster._sample_mips(mips, u, v, lod)
                keep = texels[:, 3] >= alpha_cutoff
                pixel, z, texels = pixel[keep], z[keep], texels[keep]
            order = np.lexsort((z, pixel))
            sorted_pixels = pixel[order]
            at = order[np.r_[True, sorted_pixels[1:] != sorted_pixels[:-1]]] if len(order) else order
            at = at[z[at] < opaque_z[pixel[at]]]
            pixel = pixel[at]
            if cutout:
                rgb = texels[at]
            elif lod is None:
                # Bark repeats, and crown-fix used the mip-zero nearest sample for wood.
                y, x = raster._texel_indices(mips[0].shape, u[at], v[at], repeat=True)
                rgb = mips[0][y, x]
            else:
                rgb = raster._sample_mips(mips, u[at], v[at], lod[at])
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
    return tier_edges(centers)


def tier_edges(centers):
    edges = (centers[1:] + centers[:-1]) * .5
    edges = np.r_[centers[0] - (edges[0] - centers[0]), edges,
                  centers[-1] + (centers[-1] - edges[-1])]
    return [(f'W{i:02d}', edges[i], edges[i + 1]) for i in range(len(centers))]


def fitted_level(cards, variants, candidate, maps, screen, camera, looking, level, error, gain):
    if isinstance(variants, ContourCache):
        if level in variants.faces:
            meshes, stats = variants.meshes(level, screen, camera, maps[BOUGHS][0].shape[1::-1])
            return meshes, stats, 1.
    elif level in variants:
        return variants[level]
    if level == len(cards.geometry) and variants:
        return None, {'triangles': 2}, 1.
    meshes, stats = candidate.cut(error, camera, looking)
    return meshes, stats, gain


def blend_levels(cards, variants, candidate, maps, screen, samples, camera, looking, weights, error, gain, *, eye=None):
    control, fitted = (np.zeros((*screen[3], 4), np.float32) for _ in range(2))
    gain_luma = 0.
    for level in np.flatnonzero(weights > 0):
        image = cards.image(level, maps, screen, samples, camera, alpha_cutoff=.5, eye=eye)
        meshes, _, level_gain = fitted_level(cards, variants, candidate, maps, screen, camera, looking,
                                            level, error, gain)
        opaque = capture(meshes, maps, screen, samples, level_gain, eye=eye) if meshes is not None else image
        control += weights[level] * image[0]
        fitted += weights[level] * opaque[0]
        gain_luma += weights[level] * opaque[1] * opaque[0][..., 3].sum(dtype=np.float64)
    return (control, 0.), (fitted, gain_luma / fitted[..., 3].sum(dtype=np.float64))


def compare_card_grid(args, cards, variants, candidate, maps, source, whorls, samples):
    rows = []
    adaptive = isinstance(variants, ContourCache)
    tier_points = np.stack((np.zeros(len(whorls)), np.zeros(len(whorls)), whorls), axis=1)
    for azimuth, elevation in LOD_VIEWS:
        for height in CARD_LOD_HEIGHTS:
            if adaptive:
                screen, camera, looking = card_grid_screen(cards, source, azimuth, elevation, height)
                eye = camera
                ppm = height / cards.height
                tiers = tier_edges(project(tier_points, screen, 1, eye)[:, 1])
            else:
                screen = frame(cards.geometry[0], azimuth, elevation, height)
                camera, looking = eye_at(screen, source)
                eye, ppm = None, screen[2]
                tiers = whorl_tiers(screen, whorls)
            # Sample both sides of thresholds with named authored levels. At the
            # boundary itself use the arriving level (except L4 at 49 px).
            level = cards.select(np.nextafter(float(height), -np.inf) if height != 49 else height)
            meshes, stats, gain = fitted_level(cards, variants, candidate, maps, screen, camera, looking,
                                               level, 11., args.needle_gain)
            images = [cards.image(level, maps, screen, samples, camera, alpha_cutoff=cutoff, eye=eye)
                      for cutoff in (None, .5)]
            opaque = capture(meshes, maps, screen, samples, gain, eye=eye) if meshes is not None else None
            key = f'cards-l{level}-az{azimuth:03d}-el{elevation:02d}-{height}'
            row = {'key': key, 'height': height, 'level': level, 'candidate': stats, 'gain': gain}
            for label, control in zip(('taa_expected', 'alpha_0_5'), images):
                fitted = control if opaque is None else opaque
                common = contour(control[0], ppm) | contour(fitted[0], ppm)
                row[label] = compare(control, fitted, common, height, tiers, ppm)
            if height in CARD_MATCH_HEIGHTS:
                weights = cards.weights(height)
                control, fitted = blend_levels(cards, variants, candidate, maps, screen, samples, camera, looking,
                                               weights, 11., args.needle_gain, eye=eye)
                common = contour(control[0], ppm) | contour(fitted[0], ppm)
                row['blended_alpha_0_5'] = compare(control, fitted, common, height, tiers, ppm)
                row['blend_weights'] = weights.tolist()
            rows.append(row)
            if height in (400, 200, 100, 50, 48):
                control, fitted = images[-1], images[-1] if opaque is None else opaque
                common = contour(control[0], ppm) | contour(fitted[0], ppm)
                sheet(args.output / f'{key}.jpg', f'Card level {level} at {height} px',
                      (control[0], fitted[0]), ('Accepted card path', 'Fitted geometry'), common)
                exact = row['alpha_0_5']
                print(f'{key}: alpha 0.5 gap {exact["max_gap_pp"]:.3f} pp, '
                      f'tone {exact["tone_delta_pct"]:+.3f}%', flush=True)
    return rows


def compare_card_route(args, cards, variants, candidate, maps, source, whorls, samples):
    result = []
    rows = [tuple(map(float, line.split())) for line in args.route.read_text().splitlines()]
    adaptive = isinstance(variants, ContourCache)
    if len(rows) != 384 or any(len(row) not in (14, 15) or not all(map(math.isfinite, row))
                               or len(row) == 15 and row[14] not in (0., 1.) for row in rows):
        raise ValueError(f'{args.route}: expected 384 complete route records')
    if adaptive and (any(len(row) != 15 for row in rows) or any(row[14] != float(i >= 192) for i, row in enumerate(rows))):
        raise ValueError(f'{args.route}: expected 192 preserved frames followed by 192 adaptive frames')
    tier_points = np.stack((np.zeros(len(whorls)), np.zeros(len(whorls)), whorls), axis=1)
    for index, row in enumerate(rows):
        fitted_ladder = row[14:] == (1.,) or (len(row) == 14 and index >= 192)
        if index not in ROUTE_CHECKPOINTS and not (adaptive and fitted_ladder):
            continue
        screen, eye, forward = route_screen(row, source)
        height = cards.height * screen[2] / np.linalg.norm(eye - cards.center)
        weights = cards.weights(height) if fitted_ladder else np.r_[1., np.zeros(5)]
        control, fitted = blend_levels(cards, variants, candidate, maps, screen, samples, eye, forward, weights,
                                       row[13], args.needle_gain, eye=eye)
        ppm = height / cards.height
        common = contour(control[0], ppm) | contour(fitted[0], ppm)
        tiers = tier_edges(project(tier_points, screen, 1, eye)[:, 1])
        stats = compare(control, fitted, common, height, tiers, ppm)
        result.append({'frame': index, 'height': height, 'fitted_ladder': fitted_ladder,
                       'weights': weights.tolist(), 'against_cards': stats})
        if index in ROUTE_CHECKPOINTS:
            sheet(args.output / f'route-{index:03d}.jpg', f'Frame {index}, {height:.1f} px, bind pose',
                  (control[0], fitted[0]), ('Card route', 'Geometry route'), common)
            print(f'route-{index:03d}: {height:.1f}px, alpha 0.5 gap {stats["max_gap_pp"]:.3f} pp, '
                  f'tone {stats["tone_delta_pct"]:+.3f}%', flush=True)
    return result


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
    parser.add_argument('--card-set', type=Path, help='accepted set with meshes/meshes.json and DDS impostor')
    parser.add_argument('--far-geometry', type=Path,
                        help='directory of contours.bin with cards-l1–l4.tndg, or the static prototype levels')
    parser.add_argument('--route', type=Path, help='route frame TSV for matched perspective comparisons')
    args = parser.parse_args()
    if not 0 < args.needle_gain < math.inf:
        parser.error('needle gain must be positive and finite')
    if (args.far_geometry or args.route) and not args.card_set:
        parser.error('far geometry and route comparisons require --card-set')
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
                projected = candidate.projected_error(camera, looking)[np.isfinite(candidate.groups[:, 4])]
                terminal_error = float(projected.max()) if len(projected) else 0.
                reading = {
                    'key': key, 'height': height, 'threshold': error, 'from_cut': previous_stats, 'to_cut': stats,
                    'terminal_threshold': terminal_error if math.isfinite(terminal_error) else None,
                    'adjacent': result,
                }
                if args.card_set:
                    sheet(args.output / f'{key}.jpg', f'DAG {height} px, azimuth {azimuth}, elevation {elevation}',
                          (fine[0], new[0]), ('Previous cut', 'Current cut'), common)
                else:
                    reading['against_cards'] = compare(old, new, common, height, tiers, ppm)
                    sheet(args.output / f'{key}.jpg', f'LOD {height} px, azimuth {azimuth}, elevation {elevation}',
                          (old[0], fine[0], new[0]), ('Accepted cards', 'Previous cut', 'Current cut'), common)
                report['lod'].append(reading)
                print(f'{key}: {previous_stats["triangles"]:,} -> {stats["triangles"]:,} tris, '
                      f'gap {result["max_gap_pp"]:.3f} pp, tone {result["tone_delta_pct"]:+.3f}%', flush=True)
                previous, previous_stats = coarse, stats
    report['passed'] = all(row['passes'] for row in report['crown']) and all(row['adjacent']['passes'] for row in report['lod'])
    if args.card_set:
        if candidate is None:
            parser.error('card-level comparisons require a cooked candidate')
        cards = CardLevels(args.card_set, args.baseline)
        variants = far_geometry(args.far_geometry, cards) if args.far_geometry else {}
        if isinstance(variants, ContourCache):
            report['adaptive_contours'] = variants.stats
        report['card_lod'] = compare_card_grid(args, cards, variants, candidate, maps, source, whorls, lod_samples)
        report['matched_card_lod_passed'] = all(row['blended_alpha_0_5']['passes'] for row in report['card_lod']
                                                if 'blended_alpha_0_5' in row)
        report['passed'] &= report['matched_card_lod_passed']
        if args.route:
            report['card_route'] = compare_card_route(args, cards, variants, candidate, maps, source, whorls, lod_samples)
            report['matched_route_passed'] = all(row['against_cards']['passes'] for row in report['card_route']
                                                  if row['fitted_ladder'])
            report['passed'] &= report['matched_route_passed']
    (args.output / 'metrics.json').write_text(json.dumps(report, indent=2, allow_nan=False) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
