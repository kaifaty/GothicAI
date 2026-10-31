#!/usr/bin/env python3
"""Materials made of pieces (shingle, slate, boards, beams), built as a 3D model in Blender and baked
into textures: the part every such material shares.

A material script (shingle_bake.py) lays out its pieces and writes a layout. The layout holds the
texture period in metres, the camera tilt, the surfaces and every piece:
- an outline;
- a bottom plane and a top plane;
- terms that shape its face: bow, ripples, twist, a rounded edge.
A grid piece is a face given by a grid of heights instead (grid_piece), for hewn timber.

Mesh points inside the pieces go to a .npz next to the layout. The scene
scripts/content/mesh3d/blender/piece_scene.py builds the pieces and their copies across the period's
edges, so the render tiles, and writes the passes. The material script then places weathering by
those passes and writes the albedo, normal and roughness maps with write_maps().

Rules of this path (docs/remaster-pipeline.md, «Материал из модели»): no light with a direction goes
into the albedo, only occlusion, dirt, moss and wet; the normal map and the roughness come from the
model.
"""
from __future__ import annotations

import json
import math
import subprocess
from pathlib import Path

import numpy as np

BLENDER = '/Applications/Blender.app/Contents/MacOS/Blender'
SCENE = Path(__file__).with_name('mesh3d') / 'blender' / 'piece_scene.py'


# ---------------------------------------------------------------- layout

def copies(bounds, period, margin: float) -> list[list[float]]:
    """Offsets of the copies of a piece that reach into the period square widened by margin."""
    minx, miny, maxx, maxy = bounds
    px, py = period
    return [[dx, dy] for dx in (-px, 0., px) for dy in (-py, 0., py)
            if maxx + dx > -margin and minx + dx < px + margin and maxy + dy > -margin and miny + dy < py + margin]


def ragged(polygon, rng, amount: float, wavelengths=(.05, .11, .23), step: float = .008):
    """Rough edge: every boundary point moves along its normal by a smooth random amount."""
    import shapely
    from shapely.geometry import Polygon
    ring = np.asarray(shapely.segmentize(polygon, step).exterior.coords)[:-1]
    seg = np.linalg.norm(np.roll(ring, -1, axis=0) - ring, axis=1)
    s = np.concatenate([[0], np.cumsum(seg)[:-1]])
    offset = np.zeros(len(ring))
    for wavelength in wavelengths:
        offset += rng.uniform(.3, 1) * np.sin(2 * np.pi * s / wavelength + rng.uniform(0, 2 * np.pi))
    offset = amount * offset / 2 + rng.normal(0, amount / 4, len(ring))
    tangent = np.roll(ring, -1, axis=0) - np.roll(ring, 1, axis=0)
    normal = np.stack([tangent[:, 1], -tangent[:, 0]], axis=1)
    normal /= np.linalg.norm(normal, axis=1, keepdims=True) + 1e-12
    return largest(Polygon(ring + normal * offset[:, None]).buffer(0))


def largest(geometry):
    if geometry.geom_type in ('MultiPolygon', 'GeometryCollection'):
        polygons = [g for g in geometry.geoms if g.geom_type == 'Polygon']
        return max(polygons, key=lambda g: g.area)
    return geometry


def mesh_points(shape, visible, across: float, along: float, angle: float = 0., rings=(.004, .01, .018)):
    """Points inside a piece for its shaped face: rings that follow the edge (for the rounded edge)
    and a grid, only where the piece can be seen. The grid is finer across the grain than along it;
    angle is the grain's lean from +y."""
    import shapely
    region = shape.intersection(visible) if visible is not None else shape
    points = []
    for depth in rings:
        inner = shape.buffer(-depth).intersection(region)
        for part in getattr(inner, 'geoms', [inner]):
            if part.geom_type == 'Polygon' and not part.is_empty:
                points += list(shapely.segmentize(part.exterior, min(across, along) * 1.5).coords)[:-1]
    core = shape.buffer(-max(rings) - .006).intersection(region)
    if not core.is_empty:
        c, s = math.cos(angle), math.sin(angle)
        corners = np.asarray(core.envelope.exterior.coords)
        # grid in the grain frame: a runs across the grain, b along it
        a = corners[:, 0] * c + corners[:, 1] * s
        b = -corners[:, 0] * s + corners[:, 1] * c
        ga, gb = np.meshgrid(np.arange(a.min(), a.max(), across), np.arange(b.min(), b.max(), along))
        gx, gy = ga * c - gb * s, ga * s + gb * c
        inside = shapely.contains_xy(core, gx.ravel(), gy.ravel())
        points += list(zip(gx.ravel()[inside], gy.ravel()[inside]))
    return np.round(np.asarray(points, dtype=np.float64).reshape(-1, 2), 4)


def piece(index: int, shape, bottom, top, surface: int, uv, terms=(), copy_offsets=(), kind: str = '',
          **extra) -> dict:
    """One piece of the layout. bottom and top are planes [a, bx, by]: z = a + bx·x + by·y; terms shape
    the top face (see piece_scene.py); uv is [u offset, v offset, grain lean in radians]."""
    from shapely.geometry import polygon as shapely_polygon
    if shape.interiors:
        raise ValueError(f'piece {index}: a hole inside a piece is not supported; cut it open to an edge')
    shape = shapely_polygon.orient(shape, 1.0)
    outline = []
    for x, y in list(shape.exterior.coords)[:-1]:
        point = [round(x, 5), round(y, 5)]
        if not outline or abs(point[0] - outline[-1][0]) + abs(point[1] - outline[-1][1]) > 2e-5:
            outline.append(point)
    while len(outline) > 3 and abs(outline[0][0] - outline[-1][0]) + abs(outline[0][1] - outline[-1][1]) <= 2e-5:
        outline.pop()
    return dict(id=index, kind=kind, outline=outline,
                bottom=[float(v) for v in bottom], top=[float(v) for v in top], surface=surface,
                uv=[round(float(v), 5) for v in uv], terms=list(terms), copies=[list(c) for c in copy_offsets],
                **extra)


def plane_through(z0: float, x0: float, y0: float, gx: float, gy: float) -> list[float]:
    """Plane [a, bx, by] through height z0 at (x0, y0) with slopes gx, gy."""
    return [z0 - gx * x0 - gy * y0, gx, gy]


def grid_piece(index: int, origin, step, top, surface: int, uv, copy_offsets=(), kind: str = '', **extra) -> dict:
    """A piece whose top face is a grid of heights over the top plane, for a face too fine for terms
    (hewn timber and its cracks). The heights go to write_layout(grids=...) under the same id; rows run
    along y from origin y, columns along x, step [dx, dy] metres apart. It has no side walls."""
    return dict(id=index, kind=kind, grid=[float(origin[0]), float(origin[1]), float(step[0]), float(step[1])],
                top=[float(v) for v in top], surface=surface, uv=[round(float(v), 5) for v in uv],
                copies=[list(c) for c in copy_offsets], **extra)


def write_layout(path: Path, layout: dict, points: dict[int, np.ndarray],
                 grids: dict[int, np.ndarray] | None = None) -> None:
    """The layout as JSON, its mesh points (key p<piece id>) and the heights of its grid pieces
    (key h<piece id>) as PATH.npz."""
    path.write_text(json.dumps(layout) + '\n')
    arrays = {f'p{k}': v.astype(np.float32) for k, v in points.items()}
    arrays.update({f'h{k}': v.astype(np.float32) for k, v in (grids or {}).items()})
    np.savez_compressed(path.with_suffix('.npz'), **arrays)


# ---------------------------------------------------------------- render

def render(layout_path: Path, out_dir: Path, surfaces: dict[str, Path], resolution: int, samples: int,
           options: dict[str, str] | None = None) -> Path:
    """Render the passes of a layout to OUT_DIR/passes.exr. surfaces maps a surface name of the
    layout to its image; a surface without an image renders in its plain colour."""
    out_dir.mkdir(parents=True, exist_ok=True)
    extra = [f'{k}={v}' for k, v in (options or {}).items()]
    subprocess.run([BLENDER, '-b', '--factory-startup', '--python-exit-code', '1', '-P', str(SCENE), '--',
                    str(layout_path), str(out_dir), str(resolution), str(samples)]
                   + [f'{name}={Path(path).resolve()}' for name, path in surfaces.items()] + extra, check=True)
    return out_dir / 'passes.exr'


def read_passes(path: Path, period) -> dict:
    """Passes of a render, stretched to the period's proportions: the camera looks up the slope, so
    the render is shorter than the square it covers."""
    import OpenEXR
    from PIL import Image
    raw = {}
    with OpenEXR.File(str(path), separate_channels=True) as exr:
        for part in exr.parts:
            for name, channel in part.channels.items():
                raw[name.removeprefix('ViewLayer.')] = channel.pixels.astype(np.float64)
    width = raw['Object Index.X'].shape[1]
    height = round(width * period[1] / period[0])

    def stretch(field, nearest=False):
        if field.shape[0] == height:
            return field
        method = Image.Resampling.NEAREST if nearest else Image.Resampling.BILINEAR
        return np.asarray(Image.fromarray(field.astype(np.float32), mode='F').resize((width, height), method),
                          dtype=np.float64)
    raw = {k: stretch(v, nearest=k.startswith('Object Index')) for k, v in raw.items()}

    def rgb(name):
        return np.stack([raw[f'{name}.{c}'] for c in 'RGB'], axis=-1)
    return dict(colour=rgb('Diffuse Color'), ao=raw['Ambient Occlusion.R'], crevice=raw['crevice.X'],
                sky=(rgb('Diffuse Direct') + rgb('Diffuse Indirect')).mean(axis=-1), combined=rgb('Combined'),
                x=raw['Position.X'], y=raw['Position.Y'], height=raw['Position.Z'],
                normal=np.stack([raw[f'Normal.{c}'] for c in 'XYZ'], axis=-1),
                piece=np.rint(raw['Object Index.X']).astype(int) - 1)


def seamless_along(source: Path, out: Path, band: float = .2) -> Path:
    """Copy of a surface image that wraps top to bottom: the rows near the top and bottom edges
    cross-fade into the image shifted by half its height. A generated surface rarely wraps; along the
    grain the fade only adds a few lines of grain."""
    from PIL import Image
    image = np.asarray(Image.open(source).convert('RGB'), dtype=np.float64)
    n = image.shape[0]
    i = np.arange(n)
    weight = np.clip(np.minimum(i, n - 1 - i) / (band * n), 0, 1)[:, None, None]
    shifted = np.roll(image, n // 2, axis=0)
    Image.fromarray((image * weight + shifted * (1 - weight) + .5).astype(np.uint8)).save(out)
    return out


# ---------------------------------------------------------------- compose

def periodic_noise(rng, shape, fx: float, fy: float | None = None) -> np.ndarray:
    """Smooth noise that wraps in both axes, unit spread; fx, fy are the highest frequencies in
    cycles per period across and down."""
    h, w = shape
    fy = fx if fy is None else fy
    ky = np.fft.fftfreq(h)[:, None] * h
    kx = np.fft.fftfreq(w)[None, :] * w
    spectrum = np.exp(-(kx / fx) ** 2 - (ky / fy) ** 2)
    field = np.fft.ifft2(np.fft.fft2(rng.standard_normal((h, w))) * spectrum).real
    return (field - field.mean()) / (field.std() + 1e-9)


def noise_metres(rng, shape, period, size: float) -> np.ndarray:
    """periodic_noise with round features of about SIZE metres in a period of PERIOD metres."""
    return periodic_noise(rng, shape, period[0] / size, period[1] / size)


def smooth(field: np.ndarray, passes: int = 2) -> np.ndarray:
    """3 × 3 box blur, repeated, wrapping at the edges: removes the sampling speckle of a pass."""
    for _ in range(passes):
        field = sum(np.roll(np.roll(field, dy, axis=0), dx, axis=1) for dy in (-1, 0, 1) for dx in (-1, 0, 1)) / 9
    return field


def overhang_shade(height: np.ndarray, metres_per_px: float, elevations=(17., 22., 29.), reach: float = .08,
                   softness: float = .006) -> np.ndarray:
    """0..1: how much of the light from the ridge side is blocked by an edge above a pixel. The roof's
    own sky comes mostly from the ridge (the zenith leans that way by the pitch). Pixel rows run down
    the slope."""
    shade = np.zeros_like(height)
    for elevation in elevations:
        rise = math.tan(math.radians(elevation)) * metres_per_px
        above = np.zeros_like(height)
        for d in range(1, int(reach / rise) + 2):
            above = np.maximum(above, np.roll(height, d, axis=0) - height - d * rise)
        shade += np.clip(above / softness, 0, 1)
    return shade / len(elevations)


def srgb(linear):
    linear = np.clip(linear, 0, 1)
    return np.where(linear <= .0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - .055)


def downsample(image: np.ndarray, size) -> np.ndarray:
    """Lanczos resize of a float image to (width, height), channel by channel."""
    from PIL import Image
    channels = image[..., None] if image.ndim == 2 else image
    out = np.stack([np.asarray(Image.fromarray(channels[..., c].astype(np.float32), mode='F')
                               .resize(tuple(size), Image.Resampling.LANCZOS)) for c in range(channels.shape[-1])],
                   axis=-1)
    return out[..., 0] if image.ndim == 2 else out


def write_maps(out: Path, size, albedo: np.ndarray, normal: np.ndarray, masks: dict[str, np.ndarray]) -> None:
    """OUT.png (sRGB albedo from linear), OUT.normal.png (tangent space, OpenGL style: X right, Y up
    the slope) and OUT.<mask>.png for every mask (roughness among them), all at SIZE."""
    from PIL import Image
    small = downsample(albedo, size)
    Image.fromarray((srgb(small) * 255 + .5).astype(np.uint8)).save(out)
    for name, mask in masks.items():
        small = downsample(np.clip(mask, 0, 1), size)
        Image.fromarray((np.clip(small, 0, 1) * 255 + .5).astype(np.uint8)).save(out.with_suffix(f'.{name}.png'))
    n = downsample(normal / (np.linalg.norm(normal, axis=-1, keepdims=True) + 1e-9), size)
    n /= np.linalg.norm(n, axis=-1, keepdims=True) + 1e-9
    Image.fromarray((n * 127.5 + 128).clip(0, 255).astype(np.uint8)).save(out.with_suffix('.normal.png'))


def write_preview(passes: dict, out: Path, size) -> None:
    """The clay render of the block-out: the lit model in its plain colours (Combined), for the
    concept of the layout."""
    from PIL import Image
    image = downsample(passes['combined'], size)
    exposure = .3 / np.median(image.mean(axis=-1))      # the typical piece mid-grey
    Image.fromarray((srgb(image * exposure) * 255 + .5).astype(np.uint8)).save(out)
