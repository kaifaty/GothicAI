"""How many clips of the base are keyed how often, and which ones are sparse (under 25 keys a second).

  python3 census.py <base> [model=HUMANS]

Reads only the JSON headers of models/*.MAN.json. Prints the table by key rate, the sparse clips by model, and
the names of the sparse clips of one model.
"""
from __future__ import annotations

import collections
import json
import sys
from pathlib import Path


def main():
    base = Path(sys.argv[1])
    model = sys.argv[2] if len(sys.argv) > 2 else "HUMANS"
    rows = []
    for path in sorted((base / "models").glob("*.MAN.json")):
        meta = json.loads(path.read_text())
        if meta.get("kind") != "animation":
            continue
        d = meta["data"]
        rows.append((path.name.split("-")[0], d["name"], d["fps"], d["frame_count"]))
    print(f"clips: {len(rows)}")
    print("| keys a second | clips | of them under 3 frames |\n|---|---|---|")
    by = collections.defaultdict(list)
    for r in rows:
        by[r[2]].append(r)
    for fps in sorted(by, reverse=True):
        print(f"| {fps:g} | {len(by[fps])} | {sum(r[3] < 3 for r in by[fps])} |")
    sparse = [r for r in rows if r[2] < 25 and r[3] >= 3]
    print(f"\nsparse clips with motion (under 25 keys a second, 3 frames or more): {len(sparse)}")
    models = collections.Counter(r[0] for r in sparse)
    print(", ".join(f"{m} {c}" for m, c in models.most_common()))
    print(f"\n{model}:")
    mine = collections.defaultdict(list)
    for r in sparse:
        if r[0] == model:
            mine[r[2]].append(f"{r[1]} ({r[3]})")
    for fps in sorted(mine, reverse=True):
        print(f"  {fps:g} keys a second, {len(mine[fps])}: " + ", ".join(sorted(mine[fps])))
    blows = [r for r in rows if "ATTACK" in r[1] and r[3] >= 3]
    print(f"\nblows (*ATTACK*, 3 frames or more): {len(blows)}, sparse among them: {sum(r[2] < 25 for r in blows)}")


if __name__ == "__main__":
    main()
