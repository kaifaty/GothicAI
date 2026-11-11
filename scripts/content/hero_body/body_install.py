"""Put the hero's body (a root written by mpfb_write.py) into a content root that holds other content.

    python3 scripts/content/hero_body/body_install.py <staged root> <content root> [--dry]

mpfb_write.py writes a root of its own: models.json, materials.json and render-rigs.json list the body alone. A
root the game runs has other models, animations and materials, so the three indexes are merged, the staged entry
winning, and the files they name are copied:

- models/: every record of the staged models.json with its buffer, into the target's `files`;
- models/render-rigs.json: `rigs`, `meshes` and `bodies` of the staged table over the target's;
- materials/: every folder the staged materials.json names, its entries into the target's.

Nothing else of the target is touched. The lines printed are what changed; --dry prints them and writes nothing.
"""
from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path


def load(path: Path, empty: dict) -> dict:
    return json.loads(path.read_text()) if path.exists() else empty


def main():
    args = [a for a in sys.argv[1:] if a != "--dry"]
    dry = "--dry" in sys.argv[1:]
    staged, target = Path(args[0]), Path(args[1])

    def copy(src: Path, dst: Path):
        if not dry:
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)

    def write(path: Path, data: dict, indent: int):
        if not dry:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(json.dumps(data, indent=indent) + "\n")

    # models
    s_models = json.loads((staged / "models" / "models.json").read_text())
    t_models = load(target / "models" / "models.json", {"format": "gothic-models", "version": 1, "files": {}})
    for name, record in s_models["files"].items():
        src = staged / "models" / record
        copy(src, target / "models" / record)
        buffer = json.loads(src.read_text()).get("buffer")
        if buffer:
            copy(staged / "models" / buffer, target / "models" / buffer)
        print(("new  " if name not in t_models["files"] else "over ") + "model " + name)
        t_models["files"][name] = record
    write(target / "models" / "models.json", t_models, 1)

    # the render rigs' table
    s_rigs = staged / "models" / "render-rigs.json"
    if s_rigs.exists():
        s = json.loads(s_rigs.read_text())
        t = load(target / "models" / "render-rigs.json", {"format": s["format"], "version": s["version"], "rigs": {}})
        if (t["format"], t["version"]) != (s["format"], s["version"]):
            sys.exit(f"render-rigs.json: the target is {t['format']} {t['version']}, the staged {s['format']} {s['version']}")
        for key in ("rigs", "meshes", "bodies"):
            for name, value in s.get(key, {}).items():
                kind = {"rigs": "rig", "meshes": "mesh", "bodies": "body"}[key]
                print(("new  " if name not in t.get(key, {}) else "over ") + f"{kind} {name}")
                if key == "bodies":                      # a body's variants add up: {"8": mesh} beside another's
                    t.setdefault(key, {}).setdefault(name, {}).update(value)
                else:
                    t.setdefault(key, {})[name] = value
        write(target / "models" / "render-rigs.json", t, 1)

    # materials
    s_mats = load(staged / "materials" / "materials.json", {})
    t_mats = load(target / "materials" / "materials.json", {})
    folders = set()
    for tex, spec in s_mats.items():
        folders |= {str(Path(spec[k]).parent) for k in ("albedo", "normal", "orm") if spec.get(k)}
        print(("new  " if tex not in t_mats else "over ") + "material " + tex)
        t_mats[tex] = spec
    for folder in sorted(folders):
        if not dry:
            shutil.copytree(staged / "materials" / folder, target / "materials" / folder, dirs_exist_ok=True)
    if s_mats:
        write(target / "materials" / "materials.json", t_mats, 2)
    print("dry run: nothing written" if dry else f"installed into {target}")


if __name__ == "__main__":
    main()
