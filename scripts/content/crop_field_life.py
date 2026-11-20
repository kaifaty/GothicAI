"""What makes a field of crop_field_set.py grown and worked, not laid out: the parts of step A of
reports/content/2026-11-19-crop-fields/alive.md that are numbers and maps.

- `Patches`: slow noises over a field. The rye's height and tone go by patches of 5 to 10 m, a few spots are lodged
  (the rows lie over one way, as after rain) and a few are bare;
- `strip_ground`: the ground of a strip as a mesh of its own, laid over the field's triangles a few centimetres up:
  ridges of plough as geometry, a flat sheet under the stubble and under the rye;
- `ground_maps`: the two materials of that ground. The earth is the meadow's own earth layer drawn darker and
  browner (its normal map as it is); the litter is that earth with cut straw strewn over it, drawn here from
  numbers, with its height for the normal map;
- `tones`: the atlas of the cards in three tones of one family, ripe, pale and with green in the stems.

No pixel of the original goes into a map.
"""
from __future__ import annotations

import math
from pathlib import Path

import numpy as np

LIFT = 4.                   # cm of the ground's mesh over the field's triangles
RIDGE_STEP = 31.            # cm between the ridges of plough
RIDGE_HEIGHT = 12.
TILE = 200.                 # cm a repeat of the ground's maps
EARTH = '#4a3b27'           # the ploughed earth's mean, between the concept's #55442b on the ridge and #221a12 in the furrow
LITTER_EARTH = '#57462e'    # the dry earth between the stubble
STRAW = ('#b29c62', '#9c8650', '#c2ae78', '#8a7446')
STRAWS = 950                # cut straws a tile of 2 m
LODGED, BARE = .04, .008    # shares of the field's area
PIECE = 1700.               # cm of a strip a mesh of its ground covers


def linear(code: str) -> np.ndarray:
    c = np.array([int(code[i:i + 2], 16) for i in (1, 3, 5)]) / 255
    return np.where(c <= .04045, c / 12.92, ((c + .055) / 1.055) ** 2.4)


class Patches:
    """Noises over the rectangle low..high of the world's x and z, cm."""

    def __init__(self, rng, low, high, area_cm2: float, cell: float = 50.):
        from scipy.ndimage import gaussian_filter
        self.low, self.cell = np.asarray(low, np.float64) - cell, cell
        shape = np.ceil((np.asarray(high) - self.low) / cell).astype(int) + 3

        def field(sigma):
            f = gaussian_filter(rng.normal(size=shape), sigma / cell, mode='reflect')
            return (f - f.mean()) / f.std()

        self.height, self.tone, self.fine = field(300.), field(380.), field(110.)
        self.lodged, self.bare = [], []
        covered = 0.
        while covered < LODGED * area_cm2:
            radius = rng.uniform(150, 330)
            way = rng.uniform(0, 2 * math.pi)
            self.lodged.append((rng.uniform(low, high), radius, np.array((math.cos(way), math.sin(way))),
                                math.radians(rng.uniform(30, 55))))
            covered += math.pi * radius ** 2
        covered = 0.
        while covered < BARE * area_cm2:
            radius = rng.uniform(60, 130)
            self.bare.append((rng.uniform(low, high), radius))
            covered += math.pi * radius ** 2

    def _at(self, grid, p):
        x, z = (np.asarray(p, np.float64) - self.low) / self.cell
        i, j = int(np.clip(x, 0, grid.shape[0] - 2)), int(np.clip(z, 0, grid.shape[1] - 2))
        a, b = np.clip(x - i, 0, 1), np.clip(z - j, 0, 1)
        return float(grid[i, j] * (1 - a) * (1 - b) + grid[i + 1, j] * a * (1 - b)
                     + grid[i, j + 1] * (1 - a) * b + grid[i + 1, j + 1] * a * b)

    def sink(self, p) -> float:
        """cm a row stands lower than the tallest: 0 to 30."""
        return 15. * (1 + float(np.clip(self._at(self.height, p) / 1.6, -1, 1)))

    def tone_of(self, p, rng) -> str:
        t = self._at(self.tone, p) + .4 * self._at(self.fine, p) + rng.normal(0, .18)
        return 'PALE' if t > .62 else ('GREEN' if t < -1.02 else '')

    def lodge(self, p):
        """(the way the rye lies, radians from upright) in a lodged spot, None outside."""
        best = None
        for centre, radius, way, angle in self.lodged:
            d = float(np.hypot(*(np.asarray(p) - centre))) / radius
            if d < 1 and (best is None or d < best[0]):
                best = (d, way, angle * (1 - max(d - .55, 0) / .45 * .6))
        return best and best[1:]

    def is_bare(self, p) -> bool:
        return any(np.hypot(*(np.asarray(p) - centre)) < radius * (1 + .25 * math.sin(p[0] / 37 + p[1] / 23))
                   for centre, radius in self.bare)


def strip_ground(cards, ground, to_world, u0: float, u1: float, low, high, ridged: bool, rng):
    """The ground of a strip between the edges low(u) and high(u) (cm across the field) as meshes of PIECE cm.
    Yields (mesh, origin): vertices in the world's axes about the origin."""
    du, dw = (33., 10.) if ridged else (66., 60.)
    us = np.arange(u0, u1 + du, du)
    width = max(max(high(u) - low(u) for u in us), dw)
    shares = np.linspace(0, 1, int(math.ceil(width / dw)) + 1)
    position = np.full((len(us), len(shares), 3), np.nan)
    phase = rng.uniform(0, 2 * math.pi, 4)
    clods = rng.normal(0, 1, position.shape[:2])
    for i, u in enumerate(us):
        a, b = low(u), high(u)
        for j, s in enumerate(shares):
            x, z = to_world(u, a + s * (b - a))
            hit = ground.at(x, z)
            if hit is None:
                continue
            y = hit[0] + LIFT
            if ridged:
                across = s * (b - a)
                edge = min(across, (b - a) - across)
                window = float(np.clip(edge / 45., 0, 1)) ** 2 * (3 - 2 * float(np.clip(edge / 45., 0, 1)))
                wobble = 7 * math.sin(u / 260 + phase[0]) + 3 * math.sin(u / 71 + phase[1])
                wave = .5 + .5 * math.cos(2 * math.pi * (across + wobble) / RIDGE_STEP)
                tall = RIDGE_HEIGHT * (1 + .22 * math.sin(u / 140 + across / 90 + phase[2]))
                # the turned slice lies over to one side: the crest is sharper than the furrow
                y += window * (tall * wave ** 1.5 + 1.6 * clods[i, j] * (.4 + wave)) - 2.
            position[i, j] = (x, y, z)
    ok = ~np.isnan(position[..., 1])
    filled = np.where(ok[..., None], position, 0.)
    along, across = np.gradient(filled, axis=0), np.gradient(filled, axis=1)
    normal = np.cross(across, along)
    normal *= np.sign(normal[..., 1:2] + 1e-9)
    normal /= np.maximum(np.linalg.norm(normal, axis=2, keepdims=True), 1e-9)
    normal[~ok] = (0, 1, 0)
    rows = max(int(round(PIECE / du)), 2)
    for first in range(0, len(us) - 1, rows):
        last = min(first + rows, len(us) - 1)
        block = ok[first:last + 1]
        if not block.any():
            continue
        origin = np.round(position[first:last + 1][block].mean(axis=0), 1)
        mesh = cards.Mesh()
        for i in range(first, last):
            for j in range(len(shares) - 1):
                corners = ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))
                if not all(ok[c] for c in corners):
                    continue
                mesh.quad([(position[c] - origin, normal[c], (position[c][0] / TILE, position[c][2] / TILE))
                           for c in corners], (0., 1., 0.))
        if mesh.triangles:
            yield mesh, origin


def read_dds(path: Path) -> np.ndarray:
    from PIL import Image
    return np.asarray(Image.open(path).convert('RGBA'), np.float64) / 255


def decode(srgb: np.ndarray) -> np.ndarray:
    return np.where(srgb <= .04045, srgb / 12.92, ((srgb + .055) / 1.055) ** 2.4)


def halve(image: np.ndarray, to: int) -> np.ndarray:
    while image.shape[0] > to:
        h, w = image.shape[0] // 2, image.shape[1] // 2
        image = image.reshape(h, 2, w, 2, -1).mean(axis=(1, 3))
    return image


def to_mean(colour: np.ndarray, code: str, contrast: float = 1.) -> np.ndarray:
    """The map drawn to a mean colour in linear light, its spread about the mean times `contrast`."""
    mean = colour.mean(axis=(0, 1))
    grey = colour @ np.array((.2126, .7152, .0722))
    ratio = (grey / grey.mean())[..., None] ** contrast
    tint = (colour / np.maximum(grey[..., None], 1e-5)) / (mean / grey.mean())
    return np.clip(linear(code) * ratio * (.5 + .5 * tint), 0, 1)


def ground_maps(dds, encode, source: Path, directory: Path, rng, size: int = 1024) -> dict:
    """Write the maps of the two grounds from the meadow's earth layer at `source`; returns the records of
    materials.json by kind."""
    from PIL import Image, ImageDraw
    from scipy.ndimage import gaussian_filter
    directory.mkdir(parents=True, exist_ok=True)
    name = directory.name
    earth = halve(decode(read_dds(source / 'albedo.dds')[..., :3]), size)
    orm = halve(read_dds(source / 'orm.dds')[..., :3], size)
    height = orm[..., 2]                                 # the layer's height, ranked: 0 the hollows, 1 the clods
    plough = to_mean(earth, EARTH, 1.25) * (.72 + .5 * height[..., None])
    dds.write_dds(directory / 'earth_albedo.dds', (encode(plough) / 255).astype(np.float32))
    dds.write_dds(directory / 'earth_orm.dds',
                  np.stack([orm[..., 0] * (.7 + .3 * height), np.full_like(height, .95), np.zeros_like(height)],
                           axis=2).astype(np.float32))

    # the litter: cut straw over the dry earth. Painted back to front at twice the size, each straw at the tile's
    # nine places, so the tile repeats without a seam
    big = size * 2
    px = big / TILE                                      # pixels a centimetre
    colour = Image.new('RGB', (big, big))
    order = Image.new('L', (big, big), 0)
    paint, rank = ImageDraw.Draw(colour), ImageDraw.Draw(order)
    main = rng.uniform(0, math.pi)                       # the reaper lays the handfuls one way
    straws = [linear(code) for code in STRAW]
    for k in range(STRAWS):
        length = rng.uniform(6, 32) * px
        angle = main + rng.normal(0, .55) if rng.random() < .6 else rng.uniform(0, math.pi)
        centre = rng.uniform(0, big, 2)
        half = np.array((math.cos(angle), math.sin(angle))) * length / 2
        shade = straws[rng.integers(len(straws))] * rng.uniform(.8, 1.1) * (.62 + .38 * k / STRAWS)
        fill = tuple(int(v) for v in np.rint(shade * 255))          # linear, 8 bits: decoded below
        thick = int(round(rng.uniform(.5, .95) * px))
        for dx in (-big, 0, big):
            for dy in (-big, 0, big):
                a, b = centre - half + (dx, dy), centre + half + (dx, dy)
                if max(a[0], b[0]) < 0 or min(a[0], b[0]) > big or max(a[1], b[1]) < 0 or min(a[1], b[1]) > big:
                    continue
                paint.line([tuple(a), tuple(b)], fill=fill, width=thick)
                rank.line([tuple(a), tuple(b)], fill=1 + (k * 254) // STRAWS, width=thick)
    straw = halve(np.asarray(colour, np.float64) / 255, size)
    level = halve(np.asarray(order, np.float64)[..., None] / 255, size)[..., 0]
    cover = halve((np.asarray(order) > 0).astype(np.float64)[..., None], size)[..., 0]
    under = to_mean(earth, LITTER_EARTH, 1.1) * (.8 + .35 * height[..., None])
    shadow = 1 - .45 * np.clip(gaussian_filter(cover, 2.5, mode='wrap') * 1.6, 0, 1)      # straw shades the earth
    litter = under * shadow[..., None] * (1 - cover[..., None]) + straw
    dds.write_dds(directory / 'litter_albedo.dds', (encode(np.clip(litter, 0, 1)) / 255).astype(np.float32))
    relief = height * 1.4 * (1 - cover) + cover * (gaussian_filter(height, 6, mode='wrap') * 1.4 + .5 + 1.2 * level)   # cm
    texel = TILE / size
    slope_x = (np.roll(relief, -1, axis=1) - np.roll(relief, 1, axis=1)) / (2 * texel)
    slope_y = (np.roll(relief, -1, axis=0) - np.roll(relief, 1, axis=0)) / (2 * texel)
    normal = np.stack([-slope_x, -slope_y, np.ones_like(relief)], axis=2)
    dds.write_dds(directory / 'litter_normal.dds', normal.astype(np.float32), normal=True)
    ao = np.clip(orm[..., 0] * (.72 + .28 * height) * (1 - cover) * shadow + cover * (.8 + .2 * level), 0, 1)
    rough = .95 * (1 - cover) + .78 * cover
    dds.write_dds(directory / 'litter_orm.dds', np.stack([ao, rough, np.zeros_like(ao)], axis=2).astype(np.float32))
    common = dict(scale=1., antitile='rotate', antitile_cell=1)
    return dict(
        earth=dict(albedo=f'{name}/earth_albedo.dds', normal=f'{source.name}/normal.dds', orm=f'{name}/earth_orm.dds') | common,
        litter=dict(albedo=f'{name}/litter_albedo.dds', normal=f'{name}/litter_normal.dds', orm=f'{name}/litter_orm.dds') | common,
        means=dict(earth=plough.mean(axis=(0, 1)).round(4).tolist(), litter=litter.mean(axis=(0, 1)).round(4).tolist(),
                   straw_cover=round(float(cover.mean()), 3)))


STAND_UNDER = '#4a3a1c'     # between the stalks, from above: the stand's own shade
STAND_STALK = ('#8f7a45', '#7d6a3a', '#9c8850')
STAND_EAR = ('#a08348', '#b0934f', '#93783f', '#b89c5c')
STAND_MEAN = (.2, .14, .058)    # linear: a stalk of the grass system takes it and lightens it to the ear's #9a7a45


def stand_maps(dds, encode, directory: Path, rng, size: int = 1024) -> dict:
    """Write the maps of the ground under rye that stands as stalks (stalk.md): the rye from above, ears and stalks
    over their own shade. A stalk reads its colour from it, the eye sees it between the stalks and beyond their ring.
    Drawn from numbers, each stroke at the tile's nine places."""
    from PIL import Image, ImageDraw
    from scipy.ndimage import gaussian_filter
    directory.mkdir(parents=True, exist_ok=True)
    name = directory.name
    big = size * 2
    px = big / TILE
    colour = Image.new('RGB', (big, big), tuple(int(v) for v in np.rint(linear(STAND_UNDER) * 255)))
    order = Image.new('L', (big, big), 0)
    paint, rank = ImageDraw.Draw(colour), ImageDraw.Draw(order)
    lean = rng.uniform(0, 2 * math.pi)                   # the ears nod one way more than the others
    stalks, ears = [linear(c) for c in STAND_STALK], [linear(c) for c in STAND_EAR]
    count = 7000
    for k in range(count):
        ear = k > count * .35 and rng.random() < .75     # the ears lie over the stalks
        length = (rng.uniform(7, 12) if ear else rng.uniform(4, 16)) * px
        angle = lean + rng.normal(0, .9) if rng.random() < .55 else rng.uniform(0, 2 * math.pi)
        centre = rng.uniform(0, big, 2)
        half = np.array((math.cos(angle), math.sin(angle))) * length / 2
        tones = ears if ear else stalks
        shade = tones[rng.integers(len(tones))] * rng.uniform(.82, 1.12) * (.62 + .38 * k / count)
        fill = tuple(int(v) for v in np.rint(np.clip(shade, 0, 1) * 255))
        thick = max(int(round((rng.uniform(1.1, 1.6) if ear else rng.uniform(.4, .7)) * px)), 1)
        for dx in (-big, 0, big):
            for dy in (-big, 0, big):
                a, b = centre - half + (dx, dy), centre + half + (dx, dy)
                if max(a[0], b[0]) < 0 or min(a[0], b[0]) > big or max(a[1], b[1]) < 0 or min(a[1], b[1]) > big:
                    continue
                paint.line([tuple(a), tuple(b)], fill=fill, width=thick)
                rank.line([tuple(a), tuple(b)], fill=1 + (k * 254) // count, width=thick)
    top = halve(np.asarray(colour, np.float64) / 255, size)
    level = halve(np.asarray(order, np.float64)[..., None] / 255, size)[..., 0]
    # slow tones over the tile, a quarter and a half of it: patches of metres where the stalks read it wide
    slow = sum(gaussian_filter(rng.normal(size=(size, size)), size / n, mode='wrap') for n in (3, 6))
    slow = (slow - slow.mean()) / slow.std()
    warm = gaussian_filter(rng.normal(size=(size, size)), size / 4, mode='wrap')
    warm = (warm - warm.mean()) / warm.std()
    top = top * (1 + .1 * slow[..., None]) * (1 + np.array((.03, 0., -.08)) * warm[..., None])
    top = np.clip(top * (np.array(STAND_MEAN) / top.mean(axis=(0, 1))), 0, 1)
    dds.write_dds(directory / 'stand_albedo.dds', (encode(top) / 255).astype(np.float32))
    relief = 6. * level                                  # cm: flattened, the stand is no relief of the ground
    texel = TILE / size
    slope_x = (np.roll(relief, -1, axis=1) - np.roll(relief, 1, axis=1)) / (2 * texel)
    slope_y = (np.roll(relief, -1, axis=0) - np.roll(relief, 1, axis=0)) / (2 * texel)
    dds.write_dds(directory / 'stand_normal.dds', np.stack([-slope_x, -slope_y, np.ones_like(relief)], axis=2).astype(np.float32),
                  normal=True)
    ao = .45 + .55 * level
    # no antitile: the engine drops a record with both `grass` and `antitile`
    dds.write_dds(directory / 'stand_orm.dds', np.stack([ao, np.full_like(ao, .9), np.zeros_like(ao)], axis=2).astype(np.float32))
    return dict(record=dict(albedo=f'{name}/stand_albedo.dds', normal=f'{name}/stand_normal.dds', orm=f'{name}/stand_orm.dds',
                            scale=1.),
                mean=top.mean(axis=(0, 1)).round(4).tolist())


def saturate(colour: np.ndarray, amount: float) -> np.ndarray:
    grey = (colour @ np.array((.2126, .7152, .0722)))[..., None]
    return grey + (colour - grey) * amount


# The cards' tones, all of one family. Ripe: the rusty ears and the stems turned towards straw yellow (in the game
# the top of the field came out orange against the concept's #9a7a45; less saturation alone made it pink). Pale: a drier, lighter patch. Green: stems not yet
# ripe, the ears as they are.
TONES = {
    '': lambda c, rise: saturate(c, .92) * (.95, 1., .8),
    'PALE': lambda c, rise: saturate(c, .72) * (1.1, 1.14, .98),
    'GREEN': lambda c, rise: saturate(c, .92) * (.95, 1., .8) * (1 - (1 - np.array((.78, 1.06, .66))) * (1 - rise[..., None] * .75)),
}
# and by the kind of card, before the tone: the stubble and the sheaves are the same straw as the stems
KIND_TINT = dict(stubble=(1.1, 1., .8), sheaf=(1.35, 1.22, .92))
