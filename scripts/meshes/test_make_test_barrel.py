"""Structural tests for the generated content; all files live in temporary dirs."""

import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).with_name("make_test_barrel.py").resolve()


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


class TestMakeTestBarrel(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="gothic-test-barrel-")
        cls.root = Path(cls.temp.name) / "content"
        result = subprocess.run([sys.executable, str(SCRIPT), str(cls.root)],
                                capture_output=True, text=True, check=False)
        if result.returncode != 0:
            cls.temp.cleanup()
            raise RuntimeError(result.stderr)
        cls.glb = (cls.root / "meshes/test/nw_harbour_barrel_01.glb").read_bytes()
        cls.dds = (cls.root / "materials/test/barrel_wood.dds").read_bytes()
        cls.chunks = []
        at = 12
        while at < len(cls.glb):
            length, kind = struct.unpack_from("<I4s", cls.glb, at)
            cls.chunks.append((at, length, kind, cls.glb[at + 8:at + 8 + length]))
            at += 8 + length
        cls.document = json.loads(cls.chunks[0][3])
        cls.binary = cls.chunks[1][3]

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def read_accessor(self, index, document=None, binary=None):
        document = self.document if document is None else document
        binary = self.binary if binary is None else binary
        accessor = document["accessors"][index]
        view = document["bufferViews"][accessor["bufferView"]]
        components = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[accessor["type"]]
        code = {5123: "H", 5126: "f"}[accessor["componentType"]]
        layout = "<" + code * components
        size = struct.calcsize(layout)
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        stride = view.get("byteStride", size)
        return [struct.unpack_from(layout, binary, start + i * stride)
                for i in range(accessor["count"])]

    def geometry(self):
        primitive = self.document["meshes"][0]["primitives"][0]
        attributes = primitive["attributes"]
        positions = self.read_accessor(attributes["POSITION"])
        normals = self.read_accessor(attributes["NORMAL"])
        uv = self.read_accessor(attributes["TEXCOORD_0"])
        indices = [value[0] for value in self.read_accessor(primitive["indices"])]
        return positions, normals, uv, indices

    def run_script(self, directory, home=None, radius_scale=None):
        env = os.environ.copy()
        if home is not None:
            env["HOME"] = str(home)
        command = [sys.executable, str(SCRIPT), str(directory)]
        if radius_scale is not None:
            command.append(f"--radius-scale={radius_scale}")
        return subprocess.run(command,
                              env=env, capture_output=True, text=True, check=False)

    def test_output_layout_and_indexes(self):
        files = {p.relative_to(self.root).as_posix()
                 for p in self.root.rglob("*") if p.is_file()}
        self.assertEqual(files, {
            "meshes/meshes.json", "meshes/test/nw_harbour_barrel_01.glb",
            "materials/materials.json", "materials/test/barrel_wood.dds"})
        meshes = json.loads((self.root / "meshes/meshes.json").read_text())
        self.assertEqual(meshes, {"NW_HARBOUR_BARREL_01": {
            "glb": "test/nw_harbour_barrel_01.glb"}})
        materials = json.loads((self.root / "materials/materials.json").read_text())
        self.assertEqual(materials, {"OWN_TEST_BARREL_WOOD.TGA": {
            "albedo": "test/barrel_wood.dds"}})
        for material in self.document["materials"]:
            self.assertEqual(material["name"] + ".TGA", next(iter(materials)))
            self.assertEqual(material["extras"]["group"], "WOOD")
            self.assertEqual(material["alphaMode"], "OPAQUE")

    def test_glb_header_chunks_and_scene(self):
        magic, version, length = struct.unpack_from("<4sII", self.glb)
        self.assertEqual((magic, version, length), (b"glTF", 2, len(self.glb)))
        self.assertEqual(len(self.glb) % 4, 0)
        self.assertEqual([chunk[2] for chunk in self.chunks], [b"JSON", b"BIN\0"])
        for at, size, _, payload in self.chunks:
            self.assertEqual(at % 4, 0)
            self.assertEqual(size % 4, 0)
            self.assertEqual(size, len(payload))
        at, size, _, _ = self.chunks[-1]
        self.assertEqual(at + 8 + size, len(self.glb))
        self.assertEqual(self.document["asset"]["version"], "2.0")
        self.assertEqual(self.document["buffers"], [{"byteLength": len(self.binary)}])
        self.assertEqual(self.document["scenes"][self.document["scene"]]["nodes"], [0])
        self.assertEqual(self.document["nodes"][0]["mesh"], 0)
        self.assertEqual(len(self.document["meshes"]), 1)
        self.assertEqual(len(self.document["meshes"][0]["primitives"]), 1)
        self.assertEqual(self.document["meshes"][0]["primitives"][0]["mode"], 4)
        for unsupported in ("skins", "animations", "images", "textures", "extensionsRequired"):
            self.assertNotIn(unsupported, self.document)

    def test_buffer_views_accessors_and_min_max(self):
        occupied = []
        for view in self.document["bufferViews"]:
            start = view.get("byteOffset", 0)
            self.assertEqual(view["buffer"], 0)
            self.assertEqual(start % 4, 0)
            self.assertGreater(view["byteLength"], 0)
            self.assertLessEqual(start + view["byteLength"], len(self.binary))
            self.assertIn(view["target"], (34962, 34963))
            occupied.append((start, start + view["byteLength"]))
        occupied.sort()
        for first, second in zip(occupied, occupied[1:]):
            self.assertLessEqual(first[1], second[0])
        self.assertEqual(len(self.document["accessors"]), 4)
        for index, accessor in enumerate(self.document["accessors"]):
            with self.subTest(accessor=index):
                self.assertNotIn("sparse", accessor)
                self.assertGreater(accessor["count"], 0)
                self.assertLessEqual(accessor["count"], 65535)
                components = {"SCALAR": 1, "VEC2": 2, "VEC3": 3}[accessor["type"]]
                component_size = {5123: 2, 5126: 4}[accessor["componentType"]]
                offset = accessor.get("byteOffset", 0)
                self.assertEqual(offset % component_size, 0)
                view = self.document["bufferViews"][accessor["bufferView"]]
                self.assertLessEqual(offset + accessor["count"] * components * component_size,
                                     view["byteLength"])
                values = self.read_accessor(index)
                for row in values:
                    self.assertTrue(all(math.isfinite(value) for value in row))
                self.assertEqual(accessor["min"], [min(row[i] for row in values)
                                                    for i in range(components)])
                self.assertEqual(accessor["max"], [max(row[i] for row in values)
                                                    for i in range(components)])

    def test_geometry_normals_uv_and_index_bounds(self):
        positions, normals, uv, indices = self.geometry()
        self.assertEqual(len(positions), 258)
        self.assertEqual(len(normals), len(positions))
        self.assertEqual(len(uv), len(positions))
        self.assertEqual(len(indices), 224 * 3)
        self.assertEqual(len(indices) % 3, 0)
        self.assertEqual(min(indices), 0)
        self.assertLess(max(indices), len(positions))
        self.assertEqual(set(indices), set(range(len(positions))))
        for normal in normals:
            self.assertAlmostEqual(sum(n * n for n in normal), 1, delta=1e-6)
        for point in uv:
            self.assertTrue(all(0 <= component <= 1 for component in point))
        side_count = 16 * 7 * 2
        # Seventeen boundary coordinates delimit exactly sixteen side staves.
        self.assertEqual({point[0] for point in uv[:side_count]},
                         {i / 16 for i in range(17)})
        self.assertEqual(sum(normal == (0, 1, 0) for normal in normals), 17)
        self.assertEqual(sum(normal == (0, -1, 0) for normal in normals), 17)
        # The end radius is smaller than the middle radius: it is a barrel,
        # rather than a cylinder with only a painted silhouette.
        first_stave = positions[:14]
        center_x = (min(p[0] for p in positions) + max(p[0] for p in positions)) / 2
        self.assertGreater(abs(first_stave[6][0] - center_x),
                           abs(first_stave[0][0] - center_x) * 1.1)

    def test_front_faces_before_and_after_gothic_conversion(self):
        positions, normals, _, indices = self.geometry()
        gltf_signs, gothic_signs = [], []
        for i in range(0, len(indices), 3):
            triangle = indices[i:i + 3]
            for transformed, signs in ((False, gltf_signs), (True, gothic_signs)):
                points = [positions[index] for index in triangle]
                if transformed:
                    points = [(-p[0] * 100, p[1] * 100, p[2] * 100) for p in points]
                a, b, c = points
                face = cross(tuple(b[k] - a[k] for k in range(3)),
                             tuple(c[k] - a[k] for k in range(3)))
                for index in triangle:
                    normal = normals[index]
                    if transformed:
                        normal = (-normal[0], normal[1], normal[2])
                    signs.append(sum(face[k] * normal[k] for k in range(3)))
        self.assertEqual(len(gltf_signs), 224 * 3)
        self.assertTrue(all(value > 0 for value in gltf_signs))
        # Direct measurement of the original MRM on 2026-10-20: all 180/180
        # wedge normals of 60 triangles have negative cross(b-a,c-a) dot normal.
        # X reflection with unchanged indices reproduces that Gothic front side.
        self.assertTrue(all(value < 0 for value in gothic_signs))

    def test_bbox_centimetres_and_original_origin(self):
        positions, _, _, _ = self.geometry()
        gothic = [(-p[0] * 100, p[1] * 100, p[2] * 100) for p in positions]
        expected_low = (-39.9873046875, -55.88946533203125, -40.013084411621094)
        expected_high = (39.85627746582031, 54.89350128173828, 39.83049774169922)
        for axis in range(3):
            self.assertAlmostEqual(min(p[axis] for p in gothic), expected_low[axis], delta=1e-4)
            self.assertAlmostEqual(max(p[axis] for p in gothic), expected_high[axis], delta=1e-4)
        self.assertLess(sum(p[1] for p in gothic) / len(gothic), -0.4)

    def test_explicit_unit_radius_scale_preserves_default_content(self):
        target = Path(self.temp.name) / "unit-radius"
        result = self.run_script(target, radius_scale=1)
        self.assertEqual(result.returncode, 0, result.stderr)
        for original in self.root.rglob("*"):
            if original.is_file():
                self.assertEqual((target / original.relative_to(self.root)).read_bytes(),
                                 original.read_bytes())

    def test_radius_scale_preserves_axis_height_origin_and_normals(self):
        positions, normals, _, _ = self.geometry()
        center = tuple((min(p[axis] for p in positions) + max(p[axis] for p in positions)) / 2
                       for axis in (0, 2))
        for scale in (0.5, 1.5):
            with self.subTest(scale=scale):
                target = Path(self.temp.name) / f"radius-{scale}"
                result = self.run_script(target, radius_scale=scale)
                self.assertEqual(result.returncode, 0, result.stderr)
                glb = (target / "meshes/test/nw_harbour_barrel_01.glb").read_bytes()
                json_length = struct.unpack_from("<I", glb, 12)[0]
                document = json.loads(glb[20:20 + json_length])
                binary = glb[28 + json_length:]
                attributes = document["meshes"][0]["primitives"][0]["attributes"]
                scaled_positions = self.read_accessor(attributes["POSITION"], document, binary)
                scaled_normals = self.read_accessor(attributes["NORMAL"], document, binary)
                self.assertEqual(len(scaled_positions), len(positions))
                for point, scaled in zip(positions, scaled_positions):
                    self.assertEqual(scaled[1], point[1])
                    for axis, middle in zip((0, 2), center):
                        self.assertAlmostEqual(scaled[axis], middle + scale * (point[axis] - middle),
                                               delta=1e-7)
                # Scaling geometry requires inverse-transpose normals; the Y
                # component changes through normalization even though height does not.
                for normal, scaled in zip(normals, scaled_normals):
                    expected = (normal[0] / scale, normal[1], normal[2] / scale)
                    length = math.sqrt(sum(value * value for value in expected))
                    for actual, value in zip(scaled, expected):
                        self.assertAlmostEqual(actual, value / length, delta=1e-7)
                self.assertNotIn("translation", document["nodes"][0])
                self.assertNotIn("scale", document["nodes"][0])
                self.assertEqual((target / "materials/test/barrel_wood.dds").read_bytes(), self.dds)

    def test_refuses_invalid_radius_scale_without_creating_output(self):
        for scale in ("0", "-1", "nan", "inf", "-inf"):
            with self.subTest(scale=scale):
                target = Path(self.temp.name) / f"invalid-radius-{scale}"
                result = self.run_script(target, radius_scale=scale)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("radius scale must be finite and greater than zero", result.stderr)
                self.assertFalse(target.exists())

    def dds_pixel(self, x, y):
        width = struct.unpack_from("<I", self.dds, 16)[0]
        offset = 128 + ((y // 4) * (width // 4) + x // 4) * 8
        a, b, bits = struct.unpack_from("<HHI", self.dds, offset)

        def unpack(value):
            return ((value >> 11) * 255 // 31, (value >> 5 & 63) * 255 // 63,
                    (value & 31) * 255 // 31)

        ca, cb = unpack(a), unpack(b)
        palette = (ca, cb, tuple((2 * ca[i] + cb[i]) // 3 for i in range(3)),
                   tuple((ca[i] + 2 * cb[i]) // 3 for i in range(3)))
        return palette[(bits >> (2 * ((y % 4) * 4 + x % 4))) & 3]

    def test_dds_header_payload_and_procedural_staves_hoops(self):
        self.assertEqual(self.dds[:4], b"DDS ")
        words = struct.unpack_from("<31I", self.dds, 4)
        self.assertEqual(words[0], 124)
        self.assertEqual(words[1], 0x81007)
        self.assertEqual(words[2:4], (256, 256))
        self.assertEqual(words[4], 256 * 256 // 2)
        self.assertEqual(words[5:7], (0, 0))
        self.assertEqual(words[18:21], (32, 4, int.from_bytes(b"DXT1", "little")))
        self.assertEqual(words[21:26], (0, 0, 0, 0, 0))
        self.assertEqual(words[26:31], (0x1000, 0, 0, 0, 0))
        self.assertEqual(len(self.dds), 128 + words[4])
        for offset in range(128, len(self.dds), 8):
            a, b = struct.unpack_from("<HH", self.dds, offset)
            self.assertGreater(a, b)  # Opaque four-colour DXT1 mode in every block.
        self.assertGreater(len(set(self.dds[offset:offset + 8]
                                   for offset in range(128, len(self.dds), 8))), 500)
        wood = sum(sum(self.dds_pixel(x, 12)) for x in range(256)) / (256 * 3)
        for hoop in (27, 66, 125, 165):
            metal = sum(sum(self.dds_pixel(x, hoop)) for x in range(256)) / (256 * 3)
            self.assertLess(metal, wood * 0.85)
        seams = sum(sum(self.dds_pixel(x, y)) for x in range(0, 256, 16)
                    for y in range(8, 20))
        centers = sum(sum(self.dds_pixel(x, y)) for x in range(8, 256, 16)
                      for y in range(8, 20))
        self.assertLess(seams, centers * 0.85)

    def test_accepts_existing_empty_directory(self):
        with tempfile.TemporaryDirectory(prefix="gothic-empty-content-") as directory:
            result = self.run_script(directory)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue((Path(directory) / "meshes/meshes.json").is_file())

    def test_refuses_nonempty_directory_without_changing_it(self):
        with tempfile.TemporaryDirectory(prefix="gothic-nonempty-content-") as directory:
            target = Path(directory)
            (target / "sentinel").write_bytes(b"do not replace")
            result = self.run_script(target)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("new or empty", result.stderr)
            self.assertEqual([p.name for p in target.iterdir()], ["sentinel"])
            self.assertEqual((target / "sentinel").read_bytes(), b"do not replace")

    def test_refuses_working_content_and_aliases(self):
        # A temporary HOME exercises the exact default path without touching
        # the real working content directory, even to create a test fixture.
        with tempfile.TemporaryDirectory(prefix="gothic-fake-home-") as directory:
            home = Path(directory)
            working = home / ".local/share/gothic-ai/content"
            working.mkdir(parents=True)
            alias = home / "alias"
            alias.symlink_to(working, target_is_directory=True)
            targets = [working, alias, working / "nested", alias / "nested"]
            case_alias = working.with_name("CONTENT")
            if case_alias.exists() and case_alias.samefile(working):
                targets.extend((case_alias, case_alias / "nested"))
            for target in targets:
                with self.subTest(path=target):
                    result = self.run_script(target, home=home)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertIn("working content directory", result.stderr)
                    self.assertEqual(list(working.iterdir()), [])

    def test_refuses_file_target(self):
        with tempfile.TemporaryDirectory(prefix="gothic-file-content-") as directory:
            target = Path(directory) / "existing-file"
            target.write_bytes(b"keep")
            result = self.run_script(target)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(target.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
