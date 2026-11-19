"""Meshes of flat cards with authored vertex normals, written straight to a GLB, and their cut-out colour map.

A card is a strip of quads in the mesh's own space as the game has it: Gothic axes, centimetres, +Y up. The GLB is
written in glTF's metres with X mirrored, as the loader expects (docs/own-content-meshes.md), and every triangle is
wound so that the side it is added for is the front. A card seen from both sides, `both=True`, is added once
and the mesh is `two_sided`: its material's record of materials.json has to say `"two_sided": "same"`, both sides
lit by the card's normals (reports/engine/2026-11-16-modern-trees/two-sided.md). Without that field the engine
draws one side of a triangle (reports/engine/2026-11-13-grass/README.md, section 4); `Mesh(pairs=True)` is the
way before it: the card is added twice, the second copy wound the other way with the same normals.
"""
from __future__ import annotations

import json
import struct
from pathlib import Path

import numpy as np


class Mesh:
    def __init__(self, pairs: bool = False):
        self.position, self.normal, self.uv, self.index = [], [], [], []
        self.open = []              # per vertex, 0..1: COLOR_0 alpha, the openness the crown's light reads
        self.pairs = pairs          # a card of two sides is two copies under a one-sided material
        self.two_sided = False      # a card of two sides was added once: the material has to be two-sided

    def triangle(self, corners, front) -> None:
        """corners: three (position, normal, uv) or (position, normal, uv, openness); front: the direction the
        visible side faces, Gothic axes."""
        a, b, c = (np.asarray(corner[0], np.float64) for corner in corners)
        if np.dot(np.cross(b - a, c - a), front) > 0:   # X is mirrored on the way to glTF: clockwise here is its front
            corners = (corners[0], corners[2], corners[1])
        for position, normal, uv, *more in corners:
            self.open.append(float(more[0]) if more else 1.)
            self.index.append(len(self.position))
            self.position.append(tuple(float(v) for v in position))
            self.normal.append(tuple(float(v) for v in normal))
            self.uv.append(tuple(float(v) for v in uv))

    def quad(self, corners, front, both=False) -> None:
        """corners: four (position, normal, uv) round the quad; both: it is seen from the other side as well."""
        self.two_sided |= both and not self.pairs
        for side in ((1, -1) if both and self.pairs else (1,)):
            direction = np.asarray(front, np.float64) * side
            self.triangle((corners[0], corners[1], corners[2]), direction)
            self.triangle((corners[0], corners[2], corners[3]), direction)

    @property
    def triangles(self) -> int:
        return len(self.index) // 3

    def write(self, path: Path, material: str, group: str = 'UNDEF') -> None:
        position = np.asarray(self.position, np.float32) * np.array((-.01, .01, .01), np.float32)
        normal = np.asarray(self.normal, np.float32) * np.array((-1, 1, 1), np.float32)
        normal /= np.linalg.norm(normal, axis=1, keepdims=True)
        uv = np.asarray(self.uv, np.float32)
        index = np.asarray(self.index, np.uint16)
        parts, views, offset = [], [], 0
        coloured = any(o < 1 for o in self.open)
        colour = np.full((len(position), 4), 255, np.uint8)
        colour[:, 3] = np.rint(np.clip(self.open, 0, 1) * 255)
        arrays = [(position, 34962), (normal, 34962), (uv, 34962), (index, 34963)] + ([(colour, 34962)] if coloured else [])
        for array, target in arrays:
            data = array.tobytes()
            data += b'\0' * (-len(data) % 4)
            views.append(dict(buffer=0, byteOffset=offset, byteLength=array.nbytes, target=target))
            parts.append(data)
            offset += len(data)
        document = dict(
            asset=dict(version='2.0', generator='glb_cards.py'), scene=0, scenes=[dict(nodes=[0])],
            nodes=[dict(mesh=0, name=path.stem)],
            meshes=[dict(name=path.stem, primitives=[dict(attributes=dict(POSITION=0, NORMAL=1, TEXCOORD_0=2)
                                                         | (dict(COLOR_0=4) if coloured else {}), indices=3, material=0)])],
            materials=[dict(name=material, alphaMode='MASK', alphaCutoff=.5, doubleSided=self.two_sided,
                            extras=dict(group=group))],
            accessors=[dict(bufferView=0, componentType=5126, count=len(position), type='VEC3',
                            min=position.min(axis=0).tolist(), max=position.max(axis=0).tolist()),
                       dict(bufferView=1, componentType=5126, count=len(normal), type='VEC3'),
                       dict(bufferView=2, componentType=5126, count=len(uv), type='VEC2'),
                       dict(bufferView=3, componentType=5123, count=len(index), type='SCALAR')]
            + ([dict(bufferView=4, componentType=5121, normalized=True, count=len(colour), type='VEC4')] if coloured else []),
            bufferViews=views, buffers=[dict(byteLength=offset)])
        text = json.dumps(document, separators=(',', ':')).encode()
        text += b' ' * (-len(text) % 4)
        blob = b''.join(parts)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(b'glTF' + struct.pack('<II', 2, 28 + len(text) + len(blob))
                         + struct.pack('<I', len(text)) + b'JSON' + text
                         + struct.pack('<I', len(blob)) + b'BIN\0' + blob)


def spread_colour(colour: np.ndarray, alpha: np.ndarray) -> np.ndarray:
    """The colour of the nearest covered texel in every empty one: the filter then mixes a leaf's edge with the
    leaf's colour, not with black."""
    from scipy.ndimage import distance_transform_edt
    covered = alpha > 1e-3
    if covered.all() or not covered.any():
        return colour
    nearest = distance_transform_edt(~covered, return_distances=False, return_indices=True)
    return colour[nearest[0], nearest[1]]


def write_cutout(dds, encode, path: Path, colour: np.ndarray, alpha: np.ndarray) -> list[float]:
    """DXT5 colour with the cut-out in alpha. Every mip is the average in linear light of the covered texels, and
    its alpha the average of alpha: under the engine's hashed alpha a texel's alpha is the share of the pixels it
    covers, so the average keeps the coverage of mip 0 at every distance (a hard mask per mip would thin single
    leaves out). Returns the coverage per mip."""
    size = colour.shape[0]
    levels = int(np.log2(size)) + 1
    header = [124, 0xA1007, size, size, (size // 4) ** 2 * 16, 0, levels] + [0] * 11
    header += [32, 4, int.from_bytes(b'DXT5', 'little'), 0, 0, 0, 0, 0, 0x401008, 0, 0, 0, 0]
    coverage = []
    with path.open('wb') as output:
        output.write(b'DDS ' + struct.pack('<31I', *header))
        for level in range(levels):
            coverage.append(float(alpha.mean()))
            texels = np.concatenate([encode(spread_colour(colour, alpha)),
                                     np.rint(np.clip(alpha, 0, 1) * 255).astype(np.uint8)[..., None]], axis=2)
            output.write(dds.bc3(dds.blocks(texels)).tobytes())
            if level + 1 < levels:
                half = colour.shape[0] // 2
                weight = alpha.reshape(half, 2, half, 2)
                total = weight.sum(axis=(1, 3))
                colour = (colour.reshape(half, 2, half, 2, 3) * weight[..., None]).sum(axis=(1, 3)) \
                    / np.maximum(total, 1e-6)[..., None]
                alpha = total / 4
    return coverage
