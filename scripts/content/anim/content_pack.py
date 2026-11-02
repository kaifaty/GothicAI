"""Put finished MAN records into a content folder: the records, the model script overlay and the index.

    python3 scripts/content/anim/content_pack.py <content dir> <pack.json>

pack.json:
    {"model": "HUMANS", "script": "HUMANS.MSB",
     "animations": [{"file": "/abs/HUMANS-T_1HDASHB.MAN.json", "next": "", "flags": 1,
                     "blend_in": 0.0, "blend_out": 0.0, "events": [], "sfx": []}]}

The game reads <content>/models/models.json (docs/asset-migration.md, «Анимации из своего контента»):
records `<MODEL>-<ANI>.MAN.json` + `.bin` and a `script-overlay` that adds or replaces animation entries
of the base script by name. Entries already in the index or overlay stay unless the pack names them
again. Event frames count from 0, the first frame of the record.
"""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path

ENTRY = {"layer": 1, "next": "", "blend_in": 0.0, "blend_out": 0.0, "flags": 1, "model": "", "direction": 0,
         "speed": 0.0, "collision_volume_scale": 1.0, "events": [], "pfx": [], "pfx_stop": [], "sfx": [],
         "sfx_ground": [], "morph": [], "tremors": []}


def main():
    content, pack_path = Path(sys.argv[1]), Path(sys.argv[2])
    pack = json.loads(pack_path.read_text())
    models = content / "models"
    models.mkdir(parents=True, exist_ok=True)
    index_path = models / "models.json"
    index = json.loads(index_path.read_text()) if index_path.exists() else {"format": "gothic-models", "version": 1, "files": {}}
    script = pack["script"].upper()
    overlay_path = models / f"{script}.json"
    overlay = (json.loads(overlay_path.read_text()) if overlay_path.exists() else
               {"format": "gothic-model", "version": 1, "kind": "script-overlay", "buffer": f"{script}.bin",
                "byte_length": 0, "data": {"animations": [], "aliases": []}})
    (models / f"{script}.bin").write_bytes(b"")
    entries = {a["name"]: a for a in overlay["data"]["animations"]}
    for spec in pack["animations"]:
        src = Path(spec["file"])
        record = json.loads(src.read_text())
        data = record["data"]
        name = data["name"].upper()
        stem = f"{pack['model'].upper()}-{name}.MAN"
        record["buffer"] = f"{stem}.bin"
        shutil.copyfile(src.parent / json.loads(src.read_text())["buffer"], models / f"{stem}.bin")
        (models / f"{stem}.json").write_text(json.dumps(record, ensure_ascii=False, indent=1))
        index["files"][stem] = f"{stem}.json"
        entry = {**ENTRY, **{k: v for k, v in spec.items() if k != "file"}}
        entry.update(name=name, first_frame=0, last_frame=data["frame_count"] - 1, fps=data["fps"])
        entries[name] = entry
    overlay["data"]["animations"] = list(entries.values())
    overlay_path.write_text(json.dumps(overlay, ensure_ascii=False, indent=1))
    index["files"][script] = f"{script}.json"
    index_path.write_text(json.dumps(index, indent=1))
    print(f"{len(pack['animations'])} animations into {models}; overlay has {len(entries)}")


if __name__ == "__main__":
    main()
