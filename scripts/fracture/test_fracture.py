"""Small geometry and content-routing tests without game archives."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

import fracture


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


if __name__ == "__main__":
    unittest.main()
