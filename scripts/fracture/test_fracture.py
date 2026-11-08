"""Small geometry and content-routing tests without game archives.

Version 3 (debris.py) is cut from synthetic debris here; the crate's split and broken boards come from
scripts/content/mesh3d/crate_debris.py, whose board geometry is tested here too.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

import numpy as np
from scipy.optimize import linprog
from scipy.spatial import ConvexHull

import debris
import fracture

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "content/mesh3d"))
import crate_debris  # noqa: E402


def tetrahedra():
    """Eight closed tetrahedra with explicit outward faces and nonzero UVs."""
    points = ((0, 0, 0), (1, 0, 0), (0, 1, 0), (0, 0, 1))
    faces = ((0, 2, 1), (0, 1, 3), (0, 3, 2), (1, 2, 3))
    uv = ((0, 0), (1, 0), (0, 1))
    lines = ["GOTHIC_FRACTURE 1", "pieces 8"]
    source = []
    for piece in range(8):
        lines.append(f"piece {1 / 6:.15g} {2 * piece + .25} .25 .25 4")
        for face in faces:
            triangle = ["tri OWN_WOOD.TGA"]
            for index, texcoord in zip(face, uv):
                x, y, z = points[index]
                triangle.append(f"{x + 2 * piece} {y} {z} 0 0 1 {texcoord[0]} {texcoord[1]}")
            lines.extend(triangle)
            source.extend(triangle)
    return ("\n".join(lines) + "\n").encode("ascii"), "\n".join(source) + "\n"


class TestFracture(unittest.TestCase):
    def setUp(self):
        self.v1, self.source = tetrahedra()
        self.glb = b"our embedded GLB bytes"

    def test_v2_source_row_and_serialized_geometry(self):
        v2 = fracture.bind_source(self.v1, self.glb)
        self.assertEqual(v2.splitlines()[:3], [
            b"GOTHIC_FRACTURE 2",
            f"source {len(self.glb)} {zlib.crc32(self.glb):08x}".encode("ascii"),
            b"pieces 8",
        ])
        self.assertEqual(v2.split(b"\n", 2)[2], self.v1.split(b"\n", 1)[1])
        count, volume = fracture.validate(v2, self.glb)
        self.assertEqual(count, 8)
        self.assertAlmostEqual(volume, 8 / 6)
        self.assertEqual(fracture.validate(self.v1), (count, volume))

    def test_v2_refuses_foreign_glb_and_v1_refuses_replacement(self):
        v2 = fracture.bind_source(self.v1, self.glb)
        for foreign in (b"different size", b"X" + self.glb[1:]):
            with self.subTest(foreign=foreign):
                with self.assertRaisesRegex(ValueError, "source does not match"):
                    fracture.validate(v2, foreign)
        with self.assertRaisesRegex(ValueError, "source does not match"):
            fracture.validate(v2)
        with self.assertRaisesRegex(ValueError, "invalid header"):
            fracture.validate(self.v1, self.glb)

    def test_content_uses_only_own_glb_and_adjacent_sidecar(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            glb = root / "content/meshes/test/barrel.glb"
            glb.parent.mkdir(parents=True)
            glb.write_bytes(self.glb)
            index = root / "content/meshes/meshes.json"
            index.write_text(json.dumps({
                "nw_harbour_barrel_01": {"glb": "test/barrel.glb"},
                "UNRELATED_VISUAL": {"glb": "not-present.glb"},
            }))
            args = argparse.Namespace(cache=root / "content", extractor=root / "fracture-gltf-extract",
                                      seed=601, pieces=12, verify=False)
            original_open = Path.open
            opened = []

            def own_open(path, *args, **kwargs):
                self.assertNotIn(path.suffix.upper(), (".MRM", ".VDF"))
                self.assertNotIn(root / "stock-fracture", path.parents)
                self.assertNotIn(root / "game", path.parents)
                opened.append(path)
                return original_open(path, *args, **kwargs)

            def extractor(command, *, check):
                self.assertTrue(check)
                self.assertEqual(command[0], str(args.extractor))
                self.assertEqual(len(command), 3)
                self.assertEqual(Path(command[1]).suffix, ".glb")
                self.assertEqual(Path(command[1]).read_bytes(), self.glb)
                Path(command[2]).write_text(self.source)

            with (patch.dict(os.environ, {"GOTHIC_DIR": str(root / "game"),
                                          "OPENGOTHIC_FRACTURE": str(root / "stock-fracture")}),
                  patch.object(Path, "open", own_open),
                  patch.object(fracture.subprocess, "run", side_effect=extractor) as convert,
                  patch.object(fracture, "fracture", return_value=(self.v1, 8 / 6, 1)) as cut,
                  contextlib.redirect_stdout(io.StringIO())):
                fracture.cut_content(args)
                sidecar = glb.with_suffix(".fracture")
                saved = sidecar.read_bytes()
                self.assertEqual(fracture.validate(saved, self.glb)[0], 8)
                self.assertEqual(convert.call_count, 1)
                self.assertEqual(cut.call_args.args[2:], (601, 12))
                args.verify = True
                fracture.cut_content(args)
                self.assertEqual(convert.call_count, 2)
                self.assertEqual(sidecar.read_bytes(), saved)
                # Equal size still rejects different CRC, before any conversion.
                glb.write_bytes(b"X" + self.glb[1:])
                with self.assertRaisesRegex(ValueError, "source does not match"):
                    fracture.cut_content(args)
                self.assertEqual(convert.call_count, 2)
                self.assertEqual(sidecar.read_bytes(), saved)
                sidecar.unlink()
                with self.assertRaises(FileNotFoundError):
                    fracture.cut_content(args)
            self.assertIn(index, opened)
            self.assertFalse((root / "stock-fracture").exists())
            self.assertFalse((root / "game").exists())

    def test_parallel_content_publishes_each_runs_own_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            glb = root / "content/meshes/test/barrel.glb"
            glb.parent.mkdir(parents=True)
            glb.write_bytes(self.glb)
            (root / "content/meshes/meshes.json").write_text(json.dumps({
                "NW_HARBOUR_BARREL_01": {"glb": "test/barrel.glb"},
            }))
            # One closed tetrahedron is enough to exercise real cutting and validation.
            source_text = "\n".join(self.source.splitlines()[:16]) + "\n"
            source_path = root / "source.mesh"
            source_path.write_text(source_text)
            source, textures = fracture.read_source(source_path)
            expected = {
                seed: fracture.bind_source(fracture.fracture(source, textures, seed, 8)[0], self.glb)
                for seed in (601, 602)
            }
            self.assertNotEqual(expected[601], expected[602])
            target = glb.with_suffix(".fracture")
            barrier = threading.Barrier(2)
            local = threading.local()
            publications = {}
            original_replace = Path.replace

            def extractor(command, *, check):
                Path(command[2]).write_text(source_text)

            def publish(temporary, destination):
                self.assertEqual(destination, target)
                # Both runs must finish writing before either is allowed to publish.
                barrier.wait(timeout=30)
                data = temporary.read_bytes()
                result = original_replace(temporary, destination)
                publications[local.seed] = (temporary, data)
                return result

            def cut(seed):
                local.seed = seed
                fracture.cut_content(argparse.Namespace(
                    cache=root / "content", extractor=root / "fracture-gltf-extract",
                    seed=seed, pieces=8, verify=False))

            with (patch.object(fracture.subprocess, "run", side_effect=extractor),
                  patch.object(Path, "replace", publish),
                  contextlib.redirect_stdout(io.StringIO()),
                  ThreadPoolExecutor(max_workers=2) as workers):
                futures = [workers.submit(cut, seed) for seed in expected]
                for future in futures:
                    future.result(timeout=60)

            self.assertEqual(set(publications), set(expected))
            for seed, (temporary, data) in publications.items():
                self.assertIn(target.parent, temporary.parents)
                self.assertEqual(data, expected[seed])
            self.assertEqual(len({path for path, _ in publications.values()}), 2)
            self.assertIn(target.read_bytes(), expected.values())
            self.assertEqual(set(glb.parent.iterdir()), {glb, target})


def solid(triangles):
    """Rows (T, 3, 8) of outward-wound triangles: position, face normal, uv from x and z."""
    triangles = np.asarray(triangles, dtype=np.float64)
    a, b, c = triangles[:, 0], triangles[:, 1], triangles[:, 2]
    if np.einsum("ij,ij->i", a, np.cross(b, c)).sum() < 0:
        triangles = triangles[:, [0, 2, 1]]
    normal = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    normal /= np.linalg.norm(normal, axis=1, keepdims=True)
    uv = triangles[:, :, [0, 2]] / 100
    return np.concatenate((triangles, np.repeat(normal[:, None], 3, axis=1), uv), axis=2)


def bar(low, high):
    """A box between two corners."""
    x = [low, high]
    corner = lambda i, j, k: (x[i][0], x[j][1], x[k][2])
    quads = [(corner(0, 0, 0), corner(0, 1, 0), corner(1, 1, 0), corner(1, 0, 0)),
             (corner(0, 0, 1), corner(1, 0, 1), corner(1, 1, 1), corner(0, 1, 1)),
             (corner(0, 0, 0), corner(1, 0, 0), corner(1, 0, 1), corner(0, 0, 1)),
             (corner(0, 1, 0), corner(0, 1, 1), corner(1, 1, 1), corner(1, 1, 0)),
             (corner(0, 0, 0), corner(0, 0, 1), corner(0, 1, 1), corner(0, 1, 0)),
             (corner(1, 0, 0), corner(1, 1, 0), corner(1, 1, 1), corner(1, 0, 1))]
    return solid([t for q in quads for t in ((q[0], q[1], q[2]), (q[0], q[2], q[3]))])


def band(radius, thick, height, start, end, segments, centre=(0, 0, 0)):
    """A flat band bent around the y axis from angle start to end; a full turn is a ring."""
    ring = np.isclose(end - start, 2 * np.pi)
    angles = np.linspace(start, end, segments, endpoint=not ring)
    inner = [(radius * np.cos(t), radius * np.sin(t)) for t in angles]
    outer = [((radius + thick) * np.cos(t), (radius + thick) * np.sin(t)) for t in angles]
    at = lambda p, y: (centre[0] + p[0], centre[1] + y, centre[2] + p[1])
    out = []

    def quad(a, b, c, d):
        out.extend(((a, b, c), (a, c, d)))

    for i in range(segments if ring else segments - 1):
        j = (i + 1) % segments
        quad(at(inner[i], height), at(outer[i], height), at(outer[j], height), at(inner[j], height))
        quad(at(inner[j], 0), at(outer[j], 0), at(outer[i], 0), at(inner[i], 0))
        quad(at(outer[i], 0), at(outer[j], 0), at(outer[j], height), at(outer[i], height))
        quad(at(inner[j], 0), at(inner[i], 0), at(inner[i], height), at(inner[j], height))
    if not ring:
        quad(at(inner[0], 0), at(outer[0], 0), at(outer[0], height), at(inner[0], height))
        quad(at(outer[-1], 0), at(inner[-1], 0), at(inner[-1], height), at(outer[-1], height))
    return solid(out)


def node(name, rows, texture="OWN_WOOD.TGA", group="WOOD"):
    return (name, rows, [texture] * len(rows), [group] * len(rows))


def written(data):
    """The hulls of each piece of version 3 bytes, as point arrays."""
    pieces, lines = [], data.decode("ascii").splitlines()
    for i, line in enumerate(lines):
        if line.startswith("piece "):
            pieces.append([])
        elif line.startswith("hull "):
            count = int(line.split()[1])
            pieces[-1].append(np.array([[float(x) for x in row.split()] for row in lines[i + 1:i + 1 + count]]))
    return pieces


def hull_counts(data):
    return [int(line.split()[6]) for line in data.decode("ascii").splitlines() if line.startswith("piece ")]


class TestDebris(unittest.TestCase):
    glb = b"our model GLB bytes"

    def test_v3_bars_curved_strip_and_ring(self):
        nail = solid([((0, 10, 0), (1, 10, 0), (0.5, 10.5, 0.5))])[:, [0, 2, 1]]   # an open cap: drawn only
        nodes = [node("board-a", np.concatenate((bar((0, 0, 0), (40, 2, 10)), nail))),
                 node("board-b", bar((0, 2, 0), (40, 4, 10))),          # lies on board-a face to face
                 node("stave", band(30, 2.5, 8, 0.2, 1.2, 10, (0, 0, 40))),
                 node("hoop", band(10, 6, 4, 0, 2 * np.pi, 48, (0, 0, -40)), "OWN_IRON.TGA", "METAL")]
        nodes[0][2][-1], nodes[0][3][-1] = "OWN_IRON.TGA", "METAL"
        data = debris.cut(nodes, self.glb)
        self.assertEqual(debris.cut(nodes, self.glb), data)
        lines = data.decode("ascii").splitlines()
        self.assertEqual(lines[:3], ["GOTHIC_FRACTURE 3", f"source {len(self.glb)} {zlib.crc32(self.glb):08x}", "pieces 4"])
        stats = debris.validate(data, self.glb)
        self.assertEqual(stats["pieces"], 4)
        counts = hull_counts(data)
        self.assertEqual(counts[:2], [1, 1])          # a straight board is one hull
        self.assertGreater(counts[2], 1)              # a bent stave is cut along its length
        self.assertGreaterEqual(counts[3], 8)         # a ring falls into arcs
        self.assertLessEqual(max(counts), debris.MAX_HULLS)
        self.assertLessEqual(stats["overlap"], debris.OVERLAP)
        pieces = [line.split() for line in lines if line.startswith("piece ")]
        self.assertAlmostEqual(float(pieces[0][1]), 800, places=3)   # the wood only, the nail adds no volume
        self.assertEqual([p[5] for p in pieces], ["WOOD", "WOOD", "WOOD", "METAL"])
        self.assertEqual([int(p[7]) for p in pieces[:2]], [13, 12])
        # every hull is shrunk 1.5 mm inside its detail: the board's hull is 39.7 x 1.7 x 9.7 cm
        first = lines.index("hull 8")
        points = np.array([[float(x) for x in line.split()] for line in lines[first + 1:first + 9]])
        np.testing.assert_allclose(points.min(0), (0.15, 0.15, 0.15), atol=1e-6)
        np.testing.assert_allclose(points.max(0), (39.85, 1.85, 9.85), atol=1e-6)

    def test_v3_cuts_a_hull_out_of_another_piece(self):
        # a 4 cm notch, 1 cm deep, barely adds hull volume (2.5%), but a block sits in it
        nodes = [node("notched", notched_bar()), node("block", bar((18.5, 3, 0.5), (21.5, 5, 3.5)))]
        data = debris.cut(nodes, self.glb)
        counts = hull_counts(data)
        self.assertGreaterEqual(counts[0], 3)
        self.assertEqual(counts[1], 1)
        self.assertLessEqual(debris.validate(data, self.glb)["overlap"], debris.OVERLAP)

    def test_v3_refuses_pieces_inside_each_other(self):
        nodes = [node("plank-1", bar((0, 0, 0), (40, 2, 10))), node("plank-2", bar((0, 1, 0), (40, 3, 10)))]
        with self.assertRaisesRegex(ValueError, "plank-2 and plank-1 overlap by 7.0 mm|plank-1 and plank-2 overlap"):
            debris.cut(nodes, self.glb)
        with self.assertRaisesRegex(ValueError, "no closed detail"):
            debris.cut([node("open", bar((0, 0, 0), (1, 1, 1))[:-1]), node("b", bar((5, 0, 0), (6, 1, 1)))], self.glb)

    def test_v3_ring_in_16_prisms_within_2_mm_of_its_inner_side(self):
        # a hoop: 8 mm thick, 6 cm wide, on the polygon of 20 staves with an inner radius of 32 cm
        nodes = [node("hoop", band(32, 0.8, 6, 0, 2 * np.pi, 20), "OWN_IRON.TGA", "METAL"),
                 node("board", bar((60, 0, 0), (70, 2, 10)))]
        data = debris.cut(nodes, self.glb)
        self.assertEqual(hull_counts(data), [16, 1])     # one prism per arc: 12 single facets, 4 over a corner
        ring = debris.validate(data, self.glb)["classes"]["ring"]
        self.assertLessEqual(ring["inward"], debris.INWARD)
        self.assertLessEqual(ring["outside"], debris.OUTSIDE)
        self.assertTrue(debris.RING_COVER <= ring["cover"] < debris.COVER, ring["cover"])   # 0.75
        # no point of a hull lies deeper inside the staves' polygon than 2 mm: the deepest point of each hull
        # below every side of the polygon (linprog over the point and its depth)
        middle = np.arange(20) * 2 * np.pi / 20 + np.pi / 20
        sides = np.c_[np.cos(middle), np.zeros(20), np.sin(middle), np.ones(20)]
        depths = []
        for points in written(data)[0]:
            hull = ConvexHull(points).equations
            deepest = linprog([0, 0, 0, -1], A_ub=np.r_[np.c_[hull[:, :3], np.zeros(len(hull))], sides],
                              b_ub=np.r_[-hull[:, 3], np.full(20, 32 * np.cos(np.pi / 20))],
                              bounds=[(None, None)] * 3 + [(None, 10)])
            depths.append(-deepest.fun)
        self.assertLessEqual(max(depths), debris.INWARD)
        self.assertEqual(sum(d > 0 for d in depths), 4)  # the four prisms over a corner reach into the staves
        # on 24 staves eight arcs run over a corner: 16 prisms cover 0.696, below the ring's 0.70
        nodes[0] = node("hoop", band(32, 0.8, 6, 0, 2 * np.pi, 24), "OWN_IRON.TGA", "METAL")
        with self.assertRaisesRegex(ValueError, r"hoop: hulls cover 0\.69\d of a detail \(least 0\.7\)"):
            debris.cut(nodes, self.glb)

    def test_v3_torn_edge_teeth_only_draw(self):
        board, _ = uv_board()
        halves = crate_debris.broken("plank", board, np.random.default_rng(3), [])
        nodes = [node(half.name, solid(100 * np.asarray([w for w, _ in half.triangles()]))) for half in halves]
        data = debris.cut(nodes, self.glb)
        self.assertEqual(hull_counts(data), [1, 1])      # each half one hull of its body behind the valleys
        torn = debris.validate(data, self.glb)["classes"]["torn"]
        self.assertGreaterEqual(torn["cover"], debris.COVER)
        self.assertLessEqual(torn["outside"], debris.OUTSIDE)
        for half, hulls in zip(halves, written(data)):
            # the tear: the outline points off the board's corners, in cm along the board
            tear = np.array([p for p in half.top if abs(abs(p[0]) - 1) > 1e-9]) * board.size[:2] / 2 * 100
            teeth = -1 if half.name.endswith("-2") else 1   # the side the teeth point to
            valleys = (teeth * tear[:, 0]).min()
            peaks = tear[teeth * tear[:, 0] > valleys + 1.0]
            self.assertGreater(len(peaks), 3)
            equations = ConvexHull(hulls[0]).equations
            for l, w in peaks:                               # every peak 1 cm beyond the valleys lies outside
                point = board.centre * 100 + np.array([l, w, 0]) @ (board.matrix * 100 / (board.size * 50)).T
                self.assertGreater((equations[:, :3] @ point + equations[:, 3]).max(), 0, (half.name, l, w))

    def test_v3_refuses_a_hull_beyond_8_mm(self):
        nodes = [node("board", bar((0, 0, 0), (40, 2, 10))), node("stave", band(30, 2.5, 8, 0.2, 1.2, 10, (0, 0, 40)))]
        with patch.object(debris, "MAX_HULLS", 1):         # the bent stave cannot keep within 8 mm in one hull
            with self.assertRaisesRegex(ValueError, r"stave: hulls stand \d+\.\d mm outside the detail "
                                                    r"\(limit 8 mm\) within 1 hulls"):
                debris.cut(nodes, self.glb)
        lines = debris.cut(nodes, self.glb).decode("ascii").splitlines()
        first = lines.index("hull 8")                        # the board's hull: its floor moved 2 cm down
        for k in range(first + 1, first + 9):
            x, y, z = map(float, lines[k].split())
            lines[k] = f"{x} {-2.0 if y < 1 else y} {z}"
        with self.assertRaisesRegex(ValueError, r"piece 0: hull 0 stands 20\.0 mm outside its details"):
            debris.validate(("\n".join(lines) + "\n").encode("ascii"), self.glb)

    def test_content_writes_v3_beside_debris_and_v2_without(self):
        boards = [node("a", bar((0, 0, 0), (40, 2, 10))), node("b", bar((0, 2, 0), (40, 4, 10)))]
        text = "".join(f"node {name}\n" + "".join(
            f"tri {texture} {group}\n" + "".join(" ".join(map(str, v)) + "\n" for v in tri[[0, 2, 1]])
            for tri, texture, group in zip(rows, textures, groups)) for name, rows, textures, groups in boards)
        source = "\n".join(tetrahedra()[1].splitlines()[:16]) + "\n"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "content"
            for name in ("crate", "barrel"):
                (root / "meshes" / name).mkdir(parents=True)
                (root / "meshes" / name / "model.glb").write_bytes(f"glb {name}".encode())
            (root / "meshes/crate/model.debris.glb").write_bytes(b"debris")
            (root / "meshes/meshes.json").write_text(json.dumps({
                "NW_HARBOUR_CRATE_01": {"glb": "crate/model.glb"}, "NW_HARBOUR_BARREL_01": {"glb": "barrel/model.glb"}}))

            def extractor(command, *, check):
                nodes = command[1] == "--nodes"
                self.assertEqual(Path(command[-2]).read_bytes(), b"debris" if nodes else b"glb barrel")
                Path(command[-1]).write_text(text if nodes else source)

            args = argparse.Namespace(cache=root, extractor=root / "x", seed=601, pieces=8, verify=False)
            with (patch.object(fracture.subprocess, "run", side_effect=extractor),
                  contextlib.redirect_stdout(io.StringIO())):
                fracture.cut_content(args)
                crate = (root / "meshes/crate/model.fracture").read_bytes()
                barrel = (root / "meshes/barrel/model.fracture").read_bytes()
                self.assertTrue(crate.startswith(b"GOTHIC_FRACTURE 3\nsource 9 "))   # bound to model.glb
                self.assertEqual(fracture.validate(crate, b"glb crate"), (2, 1600.0))
                self.assertTrue(barrel.startswith(b"GOTHIC_FRACTURE 2\n"))
                self.assertEqual(fracture.validate(barrel, b"glb barrel")[0], 8)
                args.verify = True
                fracture.cut_content(args)


def notched_bar():
    """A 40 x 4 x 4 cm bar with a 4 cm wide, 1 cm deep notch on top: one closed detail."""
    # an outline in x-y extruded along z: the notched bar
    outline = [(0, 0), (40, 0), (40, 4), (22, 4), (22, 3), (18, 3), (18, 4), (0, 4)]
    triangles = []
    flat = crate_debris.earclip(outline)
    for z, flip in ((4, False), (0, True)):
        for tri in flat:
            tri = tri[::-1] if flip else tri
            triangles.append([(*outline[i], z) for i in tri])
    for i, a in enumerate(outline):
        b = outline[(i + 1) % len(outline)]
        triangles += [[(*a, 0), (*b, 0), (*b, 4)], [(*a, 0), (*b, 4), (*a, 4)]]
    return solid(triangles)


def uv_board(dark=()):
    """A 100 x 20 x 2 cm crate board along x (in metres, as the baked crate), each face its own uv island at
    1 uv per metre; faces in `dark` sample black. Returns the Board and the islands {(axis, sign): u0, u1, v0, v1}."""
    size = np.array([1.0, 0.2, 0.02])
    corners = {(i, j, k): (np.array([i, j, k]) - 0.5) * size for i in (0, 1) for j in (0, 1) for k in (0, 1)}
    islands = {}
    positions, uvs = [], []
    origin = 0.0
    for axis in range(3):
        for sign in (0, 1):
            quad = [key for key in corners if key[axis] == sign]
            others = [k for k in range(3) if k != axis]
            ring = sorted(quad, key=lambda key: np.arctan2(key[others[1]] - 0.5, key[others[0]] - 0.5))
            if sign == 0:
                ring = ring[::-1]
            uv = [(origin + corners[key][others[0]] + size[others[0]] / 2,
                   corners[key][others[1]] + size[others[1]] / 2) for key in ring]
            islands[axis, 1 if sign else -1] = (origin, origin + size[others[0]], 0, size[others[1]])
            origin += size[others[0]] + 0.1
            for t in ((0, 1, 2), (0, 2, 3)):
                positions.append([corners[ring[i]] for i in t])
                uvs.append([uv[i] for i in t])
    positions = np.asarray(positions).reshape(-1, 3)
    uvs = np.asarray(uvs).reshape(-1, 2)
    faces = np.arange(len(positions)).reshape(-1, 3)

    def lum(uv):
        for face, (u0, u1, v0, v1) in islands.items():
            if u0 - 1e-6 <= uv[:, 0].min() and uv[:, 0].max() <= u1 + 1e-6:
                return 0.0 if face in dark else 0.05
        raise AssertionError("uv outside every island")

    return crate_debris.Board(positions, uvs, faces, lum), islands


def island(uv, islands):
    """The face whose uv island holds all of `uv`."""
    return next((face for face, (u0, u1, v0, v1) in islands.items()
                 if u0 - 1e-6 <= uv[:, 0].min() and uv[:, 0].max() <= u1 + 1e-6 and
                 v0 - 1e-6 <= uv[:, 1].min() and uv[:, 1].max() <= v1 + 1e-6), None)


def bonds_of(data):
    """{(a name, b name): KIND} of version 3 bytes, by piece index."""
    lines = data.decode("ascii").splitlines()
    rows = [line.split() for line in lines if line.startswith("bond ")]
    return {(int(r[1]), int(r[2])): r[3] for r in rows}


class TestBonds(unittest.TestCase):
    glb = b"our model GLB bytes"

    def test_crate_nails_cross_members_not_boards_edge_to_edge(self):
        # a wall of two boards edge to edge, a post across both behind them, one board split into two strips,
        # a lid board on top of the wall touching the post's top end
        nodes = [node("post-al", bar((0, -5, 0), (3, 0, 50))),
                 node("plank-side-a-1-strip-1", bar((0, 0, 0), (60, 2, 10))),
                 node("plank-side-a-1-strip-2", bar((0, 0, 10.1), (60, 2, 20))),
                 node("plank-side-a-2", bar((0, 0, 20.1), (60, 2, 30))),
                 node("lid-1", bar((0, -4, 50), (60, 2, 52)))]
        data = debris.cut(nodes, self.glb)
        self.assertEqual(debris.cut(nodes, self.glb), data)
        self.assertEqual(bonds_of(data), {(0, 1): "NAIL", (0, 2): "NAIL", (0, 3): "NAIL", (0, 4): "NAIL",
                                          (1, 2): "ALONG"})
        stats = debris.validate(data, self.glb)
        self.assertEqual(stats["bonds"], {"NAIL": 4, "ALONG": 1})
        self.assertEqual(stats["islands"], 1)
        point = next(line.split()[4:] for line in data.decode("ascii").splitlines() if line.startswith("bond 0 4 "))
        self.assertAlmostEqual(float(point[2]), 50, delta=0.3)      # where the lid sits on the post's top end

    def test_barrel_hoops_grooves_dowels_and_a_broken_stave(self):
        # two staves side by side (no bond), one broken across, a hoop around both, two head boards on them
        nodes = [node("stave-01", bar((30, 0, -5), (32.5, 40, 5))),
                 node("stave-01-top", bar((30, 40, -5), (32.5, 80, 5))),
                 node("stave-02", bar((30, 0, 5.05), (32.5, 80, 15))),
                 node("hoop-1", bar((32.5, 10, -6), (33.3, 16, 16)), "OWN_IRON.TGA", "METAL"),
                 node("lid-1", bar((20, 70, -5), (30, 72.5, 2))),
                 node("lid-2", bar((20, 70, 2.1), (30, 72.5, 15)))]
        data = debris.cut(nodes, self.glb)
        self.assertEqual(bonds_of(data), {(0, 1): "ACROSS", (0, 3): "HOOP", (2, 3): "HOOP", (1, 4): "GROOVE",
                                          (1, 5): "GROOVE", (2, 5): "GROOVE", (4, 5): "DOWEL"})

    def test_debris_without_a_known_generator_has_no_bonds(self):
        data = debris.cut([node("board-a", bar((0, 0, 0), (40, 2, 10))), node("board-b", bar((0, 2, 0), (40, 4, 10)))],
                          self.glb)
        self.assertNotIn(b"bonds", data)
        self.assertEqual(debris.validate(data, self.glb)["bonds"], {})

    def test_validate_refuses_broken_bond_blocks(self):
        nodes = [node("post-al", bar((0, -5, 0), (3, 0, 50))), node("plank-side-a-1", bar((0, 0, 0), (60, 2, 10)))]
        data = debris.cut(nodes, self.glb).decode("ascii")
        line = next(row for row in data.splitlines() if row.startswith("bond 0 1 NAIL "))
        for bad, message in ((data.replace("bond 0 1 NAIL", "bond 0 1 GLUE"), "invalid bond"),
                             (data.replace("bond 0 1 NAIL", "bond 1 0 NAIL"), "invalid bond"),
                             (data.replace("bond 0 1 NAIL", "bond 0 2 NAIL"), "invalid bond"),
                             (data.replace("bonds 1\n", "bonds 2\n") + line + "\n", "repeated"),
                             (data.replace("bonds 1\n", "bonds 2\n"), "invalid bond")):
            with self.subTest(bad=bad[-60:]), self.assertRaisesRegex(ValueError, message):
                debris.validate(bad.encode("ascii"), self.glb)


class TestCrateBoards(unittest.TestCase):
    """The crate's split and broken boards keep their uv on their own board's islands, unstretched."""

    def check(self, pieces, islands, board):
        for piece in pieces:
            for world, uv in piece.triangles():
                self.assertTrue(any(u0 - 1e-6 <= uv[:, 0].min() and uv[:, 0].max() <= u1 + 1e-6 and
                                    v0 - 1e-6 <= uv[:, 1].min() and uv[:, 1].max() <= v1 + 1e-6
                                    for u0, u1, v0, v1 in islands.values()), (piece.name, uv))
                area = np.linalg.norm(np.cross(world[1] - world[0], world[2] - world[0])) / 2
                d = uv[1:] - uv[0]
                uv_area = abs(d[0, 0] * d[1, 1] - d[0, 1] * d[1, 0]) / 2
                if area > 1e-9:   # 1 uv per metre: no stretch beyond the slant of a split or a tear (<= 3 mm in 2 cm)
                    self.assertLess(abs(uv_area / area - 1), 0.015, piece.name)

    def test_split_and_break_faces_take_their_boards_islands(self):
        for dark in ((), ((1, 1), (1, -1), (0, 1), (0, -1), (2, -1))):
            with self.subTest(dark=dark):
                board, islands = uv_board(dark)
                rng = np.random.default_rng(3)
                strips = crate_debris.split("plank", board, rng, 3, [])
                halves = crate_debris.broken("plank", board, rng, [])
                self.assertEqual([p.name for p in strips + halves],
                                 ["plank-strip-1", "plank-strip-2", "plank-strip-3", "plank-half-1", "plank-half-2"])
                self.check(strips + halves, islands, board)
                # the new faces: a split shows the long edge (or the front when it is dark), a break the end
                kinds = {kind[0] for piece in strips + halves for kind in piece.sides}
                self.assertEqual(kinds, {"face", "split", "break"})
                expected = {"split": {(1, 1), (1, -1)}, "break": {(0, 1), (0, -1)}}
                for piece in strips + halves:
                    for i, kind in enumerate(piece.sides):
                        if kind[0] not in expected:
                            continue
                        j = (i + 1) % len(piece.top)
                        quad = np.array([[*piece.top[i], 1], [*piece.bottom[i], -1],
                                         [*piece.bottom[j], -1], [*piece.top[j], 1]])
                        self.assertIn(island(piece.side_uv(kind, quad), islands),
                                      {board.front} if dark else expected[kind[0]], (piece.name, kind))
                for pieces in (strips, halves):   # each piece closed; nothing lost, nothing doubled
                    volume = 0.0
                    for piece in pieces:
                        (V, F, _), = debris.details(np.asarray([w for w, _ in piece.triangles()]))[0]
                        volume += debris.volume(V, F)[0]
                    self.assertAlmostEqual(volume, 1.0 * 0.2 * 0.02, places=9)


if __name__ == "__main__":
    unittest.main()
