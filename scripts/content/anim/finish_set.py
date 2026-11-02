"""Finish a set of moves for every 1H talent level with finish.py, and pack them into a content folder.

    python3 scripts/content/anim/finish_set.py <set.json> <base>/models <sources dir> <out dir> [<content dir>]

set.json: {"levels": ["HUMANS", "HUMANS_1HST1", "HUMANS_1HST2"], "moves": [
    {"name": "T_1HBARGE", "src": "<path under the sources dir>", "warp": "0:0,0.9:0.5,...", "next": "",
     "sword_arm": "0.5,0.8,1,1", "free_arm": "0,0,0,0", "arm_window": "0.6,0.85", "free_window": "0.6,0.85",
     "fade": "0.12,0.2",
     "ref": "self", "start": "<an earlier move's name>", "end": "<name> | self", "lock_speed": 8,
     "lift_skims": true, "lock_turn": true, "blend_in": 0.1}]}

Each level finishes against its own stance <LEVEL>-S_1HRUN and takes the node set of its own jump-back
<LEVEL>-T_1HPARADEJUMPB (finish.py --fill). "ref": "self" uses the move's source as the stance (a hold loop
blends its arm to its own first frame); "start" / "end" land an end on an earlier move's finished record of the
same level, or on the move's own first frame ("self"). Moves are finished in the order given. Writes
<out>/<LEVEL>/<LEVEL>-<NAME>.MAN.json and, with a content dir, packs every level into <LEVEL>.MSB.

{"name": "T_1HCHARGEDEMO", "concat": ["T_1HCHARGE", "S_1HCHARGE:0-25", "T_1HCHARGEHIT"]} glues finished moves
(optionally a frame range) into one record for the bench: `ani` plays one clip and cannot run a hold loop, the
game's logic chains them. The root's way continues across the joins; the poses already meet there.
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import gothic_anim as ga  # noqa: E402


def concat(parts: list[tuple[ga.Animation, int, int]], name: str) -> ga.Animation:
    """Records one after another, each from its frame a to b; a join's first frame is dropped (it repeats the
    last one) and the root's way is carried over."""
    pos, rot = [], []
    for k, (anim, a, b) in enumerate(parts):
        p = anim.positions[a:b + 1].copy()
        r = anim.rotations[a:b + 1]
        if k:
            p[:, 0, [0, 2]] += pos[-1][-1, 0, [0, 2]] - p[0, 0, [0, 2]]
            p, r = p[1:], r[1:]
        pos.append(p)
        rot.append(r)
    first = parts[0][0]
    return ga.Animation(name, "", first.layer, first.fps, sum(len(p) for p in pos), list(first.node_indices),
                        np.concatenate(pos), np.concatenate(rot))


def main():
    spec_path, models, sources, out = (Path(a) for a in sys.argv[1:5])
    content = Path(sys.argv[5]) if len(sys.argv) > 5 else None
    spec = json.loads(spec_path.read_text())
    for level in spec["levels"]:
        ref = models / f"{level}-S_1HRUN.MAN.json"
        done: dict[str, Path] = {}
        for m in spec["moves"]:
            dst = out / level / f"{level}-{m['name']}.MAN.json"
            if "concat" in m:
                parts = []
                for item in m["concat"]:
                    name, _, span = item.partition(":")
                    anim = ga.read_animation(done[name])
                    a, b = (map(int, span.split("-")) if span else (0, anim.frame_count - 1))
                    parts.append((anim, a, b))
                ga.write_animation(dst, concat(parts, m["name"]), source="finish_set concat " + " + ".join(m["concat"]))
                print(level, m["name"], "glued from", " + ".join(m["concat"]))
                done[m["name"]] = dst
                continue
            src = sources / m["src"]
            pick = lambda key: src if m.get(key) == "self" else done[m[key]]  # noqa: E731
            args = [sys.executable, str(HERE / "finish.py"), str(src), str(dst),
                    "--ref", str(src if m.get("ref") == "self" else ref),
                    "--skeleton", str(models / "HUMANS.MDH.json"),
                    "--name", m["name"], "--next", m.get("next", ""), "--warp", m["warp"],
                    "--sword-arm", m.get("sword_arm", "0.5,0.8,1,1"), "--free-arm", m.get("free_arm", "0,0,0,0"),
                    "--fade", m.get("fade", "0.12,0.2"), "--fill", str(models / f"{level}-T_1HPARADEJUMPB.MAN.json")]
            for key, flag in (("start", "--start-ref"), ("end", "--end-ref")):
                if m.get(key):
                    args += [flag, str(pick(key))]
            if m.get("arm_window"):
                args += ["--arm-window", m["arm_window"]]
            if m.get("free_window"):
                args += ["--free-window", m["free_window"]]
            if m.get("lift_skims"):  # true, or the speed in cm a frame above which a low foot is lifted
                args += ["--lift-skims"] + ([] if m["lift_skims"] is True else [str(m["lift_skims"])])
            if m.get("lock_turn"):
                args.append("--lock-turn")
            if m.get("lock_speed"):
                args += ["--lock-speed", str(m["lock_speed"])]
            r = subprocess.run(args, capture_output=True, text=True)
            if r.returncode:
                raise SystemExit(f"{level} {m['name']}: {r.stderr.strip()}")
            print(level, r.stdout.strip())
            done[m["name"]] = dst
        if content:
            pack = {"model": level, "script": f"{level}.MSB",
                    "animations": [{"file": str(done[m["name"]]), "next": m.get("next", ""),
                                    "blend_in": m.get("blend_in", 0.0)} for m in spec["moves"]]}
            pack_path = out / level / "pack.json"
            pack_path.write_text(json.dumps(pack, indent=1))
            subprocess.run([sys.executable, str(HERE / "content_pack.py"), str(content), str(pack_path)], check=True)


if __name__ == "__main__":
    main()
