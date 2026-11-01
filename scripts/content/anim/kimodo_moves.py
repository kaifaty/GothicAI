"""Prepare Kimodo generation folders (meta.json + constraints.json) for a list of moves.

    python3 scripts/content/anim/kimodo_moves.py <moves.json> <base>/models <soma-ref.npz> <out-dir> [<pose dir>]

A move is a prompt, a length in frames (Kimodo runs at 30 fps), a travel and a list of keys:

    {"id": "dash_back", "frames": 45, "travel": [0.0, -2.0], "text": "A person ...",
     "keys": [{"type": "fullbody", "frame": 0, "pose": "stance", "at": 0},
              {"type": "root2d", "points": [[4, 0.06], [8, 0.27], [12, 0.55]]},
              {"type": "right-hand", "frame": 15, "pose": "apex", "at": 0.5},
              {"type": "fullbody", "frame": "end", "pose": "stance", "at": 1}]}

- travel: metres in the character's frame at the start, x = its left, z = forward;
- frame: an index or "end" (the last frame);
- pose: "stance" or a name from the spec's "poses" ({"clip": "HUMANS_....MAN" from the base, or a
  .MAN.json record in <pose dir>, "frame": n}),
  turned into SOMA by `retarget_soma.gothic_to_soma`; its hips are put at `at` × travel
  (or at "pos": [x, z]);
- types: "fullbody" (joint positions, root, heading), "right-hand" / "left-hand" / "right-foot" /
  "left-foot" (adds the global rotation of the wrist or ankle: the only key that holds the sword
  hand's twist), "hips" (pelvis position and rotation; the model follows it, post-processing does
  not), "root2d" (path points [frame, share of travel], or "dense": the same points spread over
  every frame, held after the last one; facing +Z unless "heading": false).

A "cfg" list ([[text, constraint], ...]) in the spec or in a move writes one folder per setting. The old probe
form (a "duration" in seconds, stance keys at both ends, "end_free") still works. `run_kimodo.sh`
runs the folders.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import gothic_anim as ga  # noqa: E402
import retarget_soma as rs  # noqa: E402

KIMODO_FPS = 30
# Kimodo turns meta.json's duration back into frames with int(duration * 30); these counts lose a
# frame that way, and a key on the lost last frame is dropped without a warning.
BAD_FRAME_COUNTS = {123, 245, 246, 247}
EE_TYPES = {"right-hand", "left-hand", "right-foot", "left-foot"}


def old_move(move: dict) -> dict:
    """The first probe's form: stance at both ends, or a root point at the end when `end_free`."""
    frames = int(round(move["duration"] * KIMODO_FPS))
    end = ({"type": "root2d", "points": [[frames - 1, 1.0]]} if move.get("end_free")
           else {"type": "fullbody", "frame": "end", "pose": "stance", "at": 1})
    return {**move, "frames": frames, "keys": [{"type": "fullbody", "frame": 0, "pose": "stance", "at": 0}, end]}


class Poses:
    """Gothic poses as SOMA local axis-angle rotations and hips, cached by name."""

    def __init__(self, models: Path, spec: dict, soma_ref: Path, pose_dir: Path | None = None):
        self.models, self.pose_dir = models, pose_dir
        skel0 = ga.read_skeleton(models / "HUMANS.MDH.json")
        self.stance = ga.read_animation(models / (spec["stance"] + ".json"))
        self.skel = rs.with_offsets(skel0, self.stance)
        pos, rot, self.names, _ = rs.load_soma(soma_ref)
        self.rig = rs.soma_rig(self.skel, pos[0], rot[0], self.names, self.stance)
        self.defs = {"stance": {"clip": spec["stance"], "frame": 0}, **spec.get("poses", {})}
        self.cache: dict[str, tuple[np.ndarray, np.ndarray]] = {}

    def get(self, name: str) -> tuple[np.ndarray, np.ndarray]:
        if name not in self.cache:
            d = self.defs[name]
            path = (self.pose_dir or self.models) / d["clip"] if d["clip"].endswith(".json") \
                else self.models / (d["clip"] + ".json")
            clip = self.stance if d["clip"] == self.defs["stance"]["clip"] else ga.read_animation(path)
            rots, hips = rs.gothic_to_soma(self.skel, clip, int(d.get("frame", 0)), self.rig, self.names)
            self.cache[name] = (rots, hips)
        return self.cache[name]


def place(key: dict, travel: np.ndarray) -> np.ndarray:
    """Hips xz of a key: an explicit [x, z] or a share of the move's travel."""
    return np.array(key["pos"], float) if "pos" in key else travel * float(key.get("at", 0.0))


def root2d(key: dict, frames: int, travel: np.ndarray) -> dict:
    points = sorted((frames - 1 if f == "end" else int(f), float(s)) for f, s in key["points"])
    if key.get("dense"):
        xs = [0] + [f for f, _ in points]
        ys = [0.0] + [s for _, s in points]
        idx = list(range(frames))
        share = np.interp(idx, xs, ys)  # held at the last value after the last point
    else:
        idx = [f for f, _ in points]
        share = np.array([s for _, s in points])
    xz = np.outer(share, travel)
    out = {"type": "root2d", "frame_indices": idx, "smooth_root_2d": xz.round(5).tolist()}
    if key.get("heading", True):  # facing +Z, as the stance does; off where the body turns (rolls)
        out["global_root_heading"] = [[1.0, 0.0]] * len(idx)
    return out


def build(move: dict, poses: Poses) -> list[dict]:
    frames = int(move["frames"])
    if frames in BAD_FRAME_COUNTS:
        raise SystemExit(f"{move['id']}: {frames} frames lose the last frame in Kimodo, pick another count")
    travel = np.array(move.get("travel", [0.0, 0.0]), float)
    grouped: dict[str, dict] = {}
    out = []
    for key in move["keys"]:
        kind = key["type"]
        if kind == "root2d":
            out.append(root2d(key, frames, travel))
            continue
        frame = frames - 1 if key["frame"] == "end" else int(key["frame"])
        if not 0 <= frame < frames:
            raise SystemExit(f"{move['id']}: key frame {frame} outside 0..{frames - 1}")
        rots, hips = poses.get(key["pose"])
        hips = hips.copy()
        hips[[0, 2]] = place(key, travel)
        name = "end-effector" if kind == "hips" else kind
        if name not in EE_TYPES | {"fullbody", "end-effector"}:
            raise SystemExit(f"{move['id']}: unknown key type {kind}")
        entry = grouped.setdefault(kind, {"type": name, "frame_indices": [], "local_joints_rot": [],
                                          "root_positions": [], "smooth_root_2d": []})
        if kind == "hips":
            entry["joint_names"] = ["Hips"]
        entry["frame_indices"].append(frame)
        entry["local_joints_rot"].append(rots.round(6).tolist())
        entry["root_positions"].append(hips.round(5).tolist())
        entry["smooth_root_2d"].append([float(hips[0]), float(hips[2])])
    for entry in grouped.values():
        if len(entry["frame_indices"]) >= 20:
            raise SystemExit(f"{move['id']}: {len(entry['frame_indices'])} {entry['type']} keys, Kimodo wants fewer than 20")
        if entry["type"] != "fullbody":
            entry.pop("smooth_root_2d")
        out.append(entry)
    return out


def main():
    spec_path, models, soma_ref, out = (Path(a) for a in sys.argv[1:5])
    spec = json.loads(spec_path.read_text())
    poses = Poses(models, spec, soma_ref, Path(sys.argv[5]) if len(sys.argv) > 5 else None)
    count = 0
    for i, move in enumerate(spec["moves"]):
        if "keys" not in move:
            move = old_move(move)
        cons = build(move, poses)
        cfgs = move.get("cfg", spec.get("cfg", [[2.0, 2.0]]))
        for text_w, cons_w in cfgs:
            name = move["id"] if len(cfgs) == 1 else f"{move['id']}_c{text_w:g}-{cons_w:g}"
            d = out / name
            d.mkdir(parents=True, exist_ok=True)
            (d / "constraints.json").write_text(json.dumps(cons))
            (d / "meta.json").write_text(json.dumps({
                "text": move["text"], "duration": move["frames"] / KIMODO_FPS,
                "num_samples": move.get("samples", spec["samples"]), "seed": spec["seed"] + i,
                "diffusion_steps": spec.get("steps", 100),
                "cfg": {"enabled": True, "text_weight": text_w, "constraint_weight": cons_w},
            }, indent=1))
            count += 1
    rots, hips = poses.get("stance")
    (out / "stance.json").write_text(json.dumps({"stance": spec["stance"], "hips_m": hips.tolist()}, indent=1))
    print("prepared", count, "folders in", out)


if __name__ == "__main__":
    main()
