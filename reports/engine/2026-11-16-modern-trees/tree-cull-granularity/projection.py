"""Camera and VOB transforms for the spruce capture fixtures, in row order."""

import csv
import hashlib
import json
import math

import numpy as np


VOB_SHA256 = "26f79531c4d3a7cb67d9441b25318cf8bfb65beb3a2cc8eef0e623c9e6f8f7f5"
NEAR = 10.0
FAR = 100_000.0
FOV_Y = 67.5


def _halton(index, base):
    factor = np.float32(1)
    value = np.float32(0)
    while index:
        factor /= np.float32(base)
        value += factor * np.float32(index % base)
        index //= base
    return value


def _perspective(width, height):
    # Camera::setViewport and Matrix4x4::perspective, using Gothic centimetres.
    focus = 1 / math.tan(math.radians(FOV_Y / 2))
    ratio = np.float32(width) / np.float32(height)
    depth = np.float32(FAR / (FAR - NEAR))
    return np.array([
        [np.float32(focus / float(ratio)), 0, 0, 0],
        [0, np.float32(focus), 0, 0],
        [0, 0, depth, np.float32(-NEAR * depth)],
        [0, 0, 1, 0],
    ], dtype=np.float64)


def _census_view(archive):
    bench = (archive / "capture/harness/fir-view1-pixels.bench").read_text()
    commands = [line.split()[2:] for line in bench.splitlines() if line.startswith("cam at ")]
    position, target = np.array(commands[-1], dtype=np.float64).reshape(2, 3)
    forward = target - position
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, [0, 1, 0])
    right /= np.linalg.norm(right)
    axes = np.stack((-right, np.cross(forward, right), forward))
    view = np.eye(4)
    view[:3, :3] = axes
    view[:3, 3] = -(axes @ position)
    return view


def load_camera(archive, label=None):
    """Return (view, perspective, near, jittered VP matrices) for one capture.

    The census did not record the VP, so rebuild the fixed camera and cover
    all 32 TAAU phases. Paired captures carry their exact jittered matrices.
    Their clip-W row is also the exact view-Z row used for cut selection.
    """
    if label is not None:
        with (archive / f"{label}.csv").open(newline="") as stream:
            frame = next(csv.DictReader(stream))
        width, height = (int(frame[key]) for key in ("width", "height"))
        perspective = _perspective(width, height)
        # The diagnostic writes one column per line at full float precision.
        vp = np.loadtxt(archive / f"{label}-vp.txt", dtype=np.float32).T.astype(np.float64)
        forward = vp[3, :3]
        jitter = (vp[:2, :3] @ forward) / (forward @ forward)
        view = np.eye(4)
        view[:2] = (vp[:2] - jitter[:, None] * vp[3]) / perspective.diagonal()[:2, None]
        view[2] = vp[3]
        return view, perspective, NEAR, vp[None]

    frame = (archive / "capture/snapshot/frame.txt").read_text().split()
    width, height = map(int, frame[4:6])
    perspective = _perspective(width, height)
    view = _census_view(archive)
    unjittered = perspective @ view
    phases = np.repeat(unjittered[None], 32, axis=0)
    for index, vp in enumerate(phases, 1):
        for axis, base, size in ((0, 2, width), (1, 3, height)):
            pixels = _halton(index, base) - np.float32(0.5)
            vp[axis] += float(np.float32(2) * pixels / np.float32(size)) * vp[3]
    return view, perspective, NEAR, phases


def load_objects(placements_csv, vobs_json):
    """Map the one-based census tree IDs to world-from-object matrices."""
    data = vobs_json.read_bytes()
    if hashlib.sha256(data).hexdigest() != VOB_SHA256:
        raise ValueError(f"VOB hash does not match the capture fixtures: {vobs_json}")
    records = {record["id"]: record for record in json.loads(data)["vobs"]
               if record.get("vob_name", "").startswith("tree-cost-")}
    objects = {}
    with placements_csv.open(newline="") as stream:
        for index, row in enumerate(csv.DictReader(stream), 1):
            record = records.pop(int(row["vob_id"]))
            position = [float(row[f"mixed_{axis}_cm"]) for axis in "xyz"]
            if record["vob_name"] != row["key"] or not np.allclose(record["position"], position, atol=0.001, rtol=0):
                raise ValueError(f"Placement {index} does not match VOB {record['id']}")
            # WorldData JSON stores matrix rows. Round source data as the C++ loader does.
            matrix = np.eye(4)
            matrix[:3, :3] = np.asarray(record["rotation"], dtype=np.float32)
            matrix[:3, 3] = np.asarray(record["position"], dtype=np.float32)
            objects[index] = matrix
    if len(objects) != 1024 or records:
        raise ValueError("The placements and VOBs must describe the same 1,024 spruces")
    return objects
