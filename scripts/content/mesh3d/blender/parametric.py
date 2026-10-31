"""Board-and-iron controls for the mesh generator pilot (run with Blender -b -P)."""

import argparse
import json
from pathlib import Path
import sys

import numpy as np


class Mesh:
    def __init__(self):
        self.vertices, self.faces, self.color, self.metallic = [], [], [], []

    def part(self, vertices, faces, color, metallic=0.0):
        offset = len(self.vertices)
        self.vertices.extend(vertices)
        self.faces.extend(tuple(offset + i for i in face) for face in faces)
        self.color.extend([color] * len(vertices))
        self.metallic.extend([metallic] * len(vertices))

    def box(self, low, high, color, metallic=0.0):
        x0, y0, z0 = low
        x1, y1, z1 = high
        vertices = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
                    (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
        quads = [(0, 3, 2, 1), (4, 5, 6, 7), (0, 1, 5, 4),
                 (1, 2, 6, 5), (2, 3, 7, 6), (3, 0, 4, 7)]
        self.part(vertices, [tri for a, b, c, d in quads
                             for tri in [(a, b, c), (a, c, d)]], color, metallic)

    def arrays(self):
        return (np.asarray(self.vertices, dtype=np.float32),
                np.asarray(self.faces, dtype=np.int32),
                np.asarray(self.color, dtype=np.float32),
                np.asarray(self.metallic, dtype=np.float32))


WOOD = np.array([0.29, 0.13, 0.045])
IRON = [0.07, 0.075, 0.08]


def build_barrel():
    """Sixteen separate thick staves, bowed sides, lids, and three iron hoops."""
    mesh = Mesh()
    levels = np.linspace(0.0, 1.0, 9)

    def radius(y):
        return 0.41 + 0.085 * np.sin(np.pi * y)

    # Each stave is a closed solid with a narrow longitudinal seam.
    for stave in range(16):
        a0 = 2 * np.pi * (stave + 0.012) / 16
        a1 = 2 * np.pi * (stave + 0.988) / 16
        vertices = []
        for y in levels:
            outer = radius(y)
            for r, angle in [(outer, a0), (outer, a1),
                             (outer - 0.035, a0), (outer - 0.035, a1)]:
                vertices.append((r * np.cos(angle), y, r * np.sin(angle)))
        quads = []
        for row in range(len(levels) - 1):
            p, q = row * 4, (row + 1) * 4
            quads.extend([(p, q, q + 1, p + 1), (p + 2, p + 3, q + 3, q + 2),
                          (p, p + 2, q + 2, q), (p + 1, q + 1, q + 3, p + 3)])
        last = (len(levels) - 1) * 4
        quads.extend([(0, 1, 3, 2), (last, last + 2, last + 3, last + 1)])
        mesh.part(vertices, [tri for a, b, c, d in quads
                             for tri in [(a, b, c), (a, c, d)]],
                  WOOD * (0.88 + 0.04 * (stave % 6)))

    for center in [0.13, 0.50, 0.87]:
        vertices = []
        for y in [center - 0.025, center + 0.025]:
            for r_offset in [0.002, 0.017]:
                for segment in range(48):
                    angle = segment * 2 * np.pi / 48
                    r = radius(y) + r_offset
                    vertices.append((r * np.cos(angle), y, r * np.sin(angle)))
        quads = []
        for s in range(48):
            t = (s + 1) % 48
            quads.extend([(s, t, 96 + t, 96 + s),
                          (48 + s, 144 + s, 144 + t, 48 + t),
                          (s, 48 + s, 48 + t, t),
                          (96 + s, 96 + t, 144 + t, 144 + s)])
        mesh.part(vertices, [tri for a, b, c, d in quads
                             for tri in [(a, b, c), (a, c, d)]], IRON, 1.0)

    # Lids are parallel wooden planks clipped to the circular opening.
    for y0, y1 in [(0.018, 0.048), (0.952, 0.982)]:
        r = 0.38
        for board in range(8):
            x0 = -r + board * 2 * r / 8 + 0.001
            x1 = -r + (board + 1) * 2 * r / 8 - 0.001
            z = np.sqrt(max(0.0, r * r - max(abs(x0), abs(x1)) ** 2))
            mesh.box([x0, y0, -z], [x1, y1, z], WOOD * (0.93 + board * 0.02))
    return mesh.arrays()


def build_crate():
    """Six faces of planks, raised wooden edge ribs, and iron corner fittings."""
    mesh = Mesh()
    for normal in range(3):
        along, across = [i for i in range(3) if i != normal]
        for side in [-1, 1]:
            for board in range(5):
                low, high = np.full(3, -0.475), np.full(3, 0.475)
                low[normal], high[normal] = sorted([side * 0.445, side * 0.475])
                low[across] = -0.475 + board * 0.19 + 0.002
                high[across] = -0.475 + (board + 1) * 0.19 - 0.002
                mesh.box(low, high, WOOD * (0.89 + 0.045 * (board % 5)))
            for position in [-0.41, 0.41]:
                low, high = np.full(3, -0.48), np.full(3, 0.48)
                low[normal], high[normal] = sorted([side * 0.474, side * 0.50])
                low[along], high[along] = position - 0.035, position + 0.035
                mesh.box(low, high, WOOD * 0.78)

    # Three small plates meet at each corner, giving an actual L-shaped fitting.
    for sx in [-1, 1]:
        for sy in [-1, 1]:
            for sz in [-1, 1]:
                corner = np.array([sx, sy, sz])
                for normal in range(3):
                    low = np.minimum(corner * 0.36, corner * 0.505)
                    high = np.maximum(corner * 0.36, corner * 0.505)
                    low[normal], high[normal] = sorted(
                        [corner[normal] * 0.50, corner[normal] * 0.512])
                    mesh.box(low, high, IRON, 1.0)
    vertices, faces, color, metallic = mesh.arrays()
    vertices[:, 1] += 0.512
    return vertices, faces, color, metallic


def build(kind):
    if kind == "barrel":
        return build_barrel()
    if kind in {"crate", "box"}:
        return build_crate()
    raise ValueError(f"Parametric control only supports barrel or crate, got {kind!r}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument("--id")
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    import postprocess

    target = postprocess.load_target(args.target, args.id)
    vertices, faces, color, metallic = build(target["id"])
    # Construct directly in target proportions before the common orientation/fit.
    size = np.asarray(target["bbox_max"]) - np.asarray(target["bbox_min"])
    vertices *= size / np.ptp(vertices, axis=0)
    args.out.mkdir(parents=True, exist_ok=True)
    meta = {"up_axis": "+Y", "meters_per_unit": 1.0}
    np.savez_compressed(args.out / "raw.npz", vertices=vertices, faces=faces,
                        color=color, metallic=metallic)
    (args.out / "meta.json").write_text(json.dumps(meta, indent=2) + "\n")
    postprocess.process_arrays(vertices, faces, color, metallic, target, args.out,
                               meta=meta, cleanup=False)


if __name__ == "__main__":
    main()
