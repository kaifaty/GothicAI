#!/usr/bin/env python3
"""Retain built-in imagegen concepts unchanged and compose the pilot sheet.

Only the CLI import invokes backup-sources.sh; library calls are CPU-only and
offline. No original images or geometry are accepted by this module.
"""

from __future__ import annotations

import argparse
import hashlib
from io import BytesIO
import json
from pathlib import Path
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageOps

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import registry as registry_module


DEFAULT_TARGETS = Path(__file__).resolve().parents[3] / "data/content/mesh3d-pilot.json"
SOURCE_SET = "mesh3d-pilot"


def _png_metrics(raw: bytes, min_transparent: float = 0.30) -> dict:
    with Image.open(BytesIO(raw)) as image:
        if image.format != "PNG":
            raise ValueError("concept source must be a PNG")
        if "A" not in image.getbands() and "transparency" not in image.info:
            raise ValueError("concept PNG has no alpha transparency")
        alpha = image.convert("RGBA").getchannel("A")
        # Same visible-pixel cutoff as content-style.md. Tool PNGs can contain
        # alpha=1 background noise; preserve those source bytes unchanged.
        bbox = alpha.point(lambda value: 255 if value >= 128 else 0).getbbox()
        if bbox is None:
            raise ValueError("concept PNG has no visible object")
        width, height = image.size
        transparent = alpha.histogram()[0] / (width * height)
        if transparent <= min_transparent:
            raise ValueError(f"concept PNG needs more than {min_transparent:.0%} fully transparent pixels; "
                             f"got {transparent:.1%}")
        if bbox[0] == 0 or bbox[1] == 0 or bbox[2] == width or bbox[3] == height:
            raise ValueError("concept object touches the image edge")
        return dict(width=width, height=height, transparent_fraction=transparent, alpha_cutoff=128,
                    visible_bbox=list(bbox))


def validate_png(source: Path) -> dict:
    """Require real transparent pixels, a visible object and free outer edges."""
    return _png_metrics(Path(source).read_bytes())


def _write_immutable(path: Path, raw: bytes, root: Path) -> None:
    if not path.resolve().is_relative_to(root):
        raise ValueError("concept source path escapes the permanent store")
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("xb") as stream:
            stream.write(raw)
    except FileExistsError:
        if path.read_bytes() != raw:
            raise ValueError(f"refusing to overwrite retained PNG: {path}")


VIEWS = ("34", "front", "side", "top")


def import_concept(target: dict, source: Path, variant: int, prompt: str, utc: str,
                   registry_path: Path = registry_module.DEFAULT_REGISTRY,
                   source_root: Path = registry_module.DEFAULT_SOURCE_ROOT,
                   view: str | None = None, edit_of: str | None = None) -> dict:
    """Copy exact tool bytes once and append one recipe; equal retries reuse it.

    A modelling concept has views: ``34`` from text, then ``front``, ``side`` and ``top``
    as image_gen edits of our own ``34`` image (``edit_of``), never of the original.
    """
    if type(variant) is not int or not 1 <= variant <= 9:
        raise ValueError("variant must be an integer 1..9")
    if view is not None and view not in VIEWS:
        raise ValueError(f"view must be one of {', '.join(VIEWS)}")
    if edit_of is not None and (view in (None, "34") or not registry_module.SLUG.fullmatch(edit_of)):
        raise ValueError("edit_of names our own 34 concept and needs a front or top view")
    target_id = target["id"]
    if not isinstance(target_id, str) or not registry_module.SLUG.fullmatch(target_id):
        raise ValueError("target.id must be a lowercase resource slug")
    raw = Path(source).read_bytes()
    # A flat orthographic view of a face fills its canvas; it only needs a free border.
    metrics = _png_metrics(raw, 0.30 if view in (None, "34") else 0.10)
    suffix = f"-v{variant}" + (f"-{view}" if view else "")
    relative = f"{SOURCE_SET}/{target_id}{suffix}.png"
    parameters = dict(set=SOURCE_SET, target=target_id, variant=variant,
                      input="text only; original numbers only")
    if view:
        parameters["view"] = view
    if edit_of:
        parameters["edit_of"] = edit_of
        parameters["input"] = "text plus our own concept image; original numbers only"
    record = dict(
        id=f"{SOURCE_SET}-{target_id}{suffix}", kind="concept",
        replaces=[target["visual"]], pipeline="imagegen-concept",
        models=[dict(id="image_gen.imagegen", revision=None,
                     license="OpenAI Terms of Use; output owned by user")],
        prompt=prompt, negative="", seed=None, utc=utc, status="draft", files={},
        source_png=relative, source_sha256=hashlib.sha256(raw).hexdigest(),
        parameters=parameters,
        generation=dict(outcome="complete", alpha=metrics,
                        model_identity="Tool image_gen.imagegen; underlying model/version not exposed",
                        seed_policy="No seed exposed by the built-in tool"),
    )
    registry_module.validate_entry(record)
    registry_path = Path(registry_path)
    source_root = Path(source_root).expanduser().resolve()
    records = registry_module.check_registry(registry_path, source_root=source_root) if registry_path.exists() else []
    previous = next((entry for entry in records if entry["id"] == record["id"]), None)
    if previous is not None and previous != record:
        raise ValueError(f"concept id already has a different recipe: {record['id']}")
    _write_immutable(source_root / relative, raw, source_root)
    registry_module.validate_entry(record, source_root=source_root)
    if previous is not None:
        return previous
    registry_path.parent.mkdir(parents=True, exist_ok=True)
    existing = registry_path.read_bytes() if registry_path.exists() else b""
    if existing and not existing.endswith(b"\n"):
        existing += b"\n"
    serialized = existing + (json.dumps(record, ensure_ascii=False, sort_keys=True, allow_nan=False) + "\n").encode("utf-8")
    with tempfile.NamedTemporaryFile(dir=registry_path.parent, prefix=registry_path.name + ".",
                                     suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(serialized)
    try:
        registry_module.check_registry(temporary, source_root=source_root)
        temporary.replace(registry_path)
    finally:
        temporary.unlink(missing_ok=True)
    return record


def load_targets(path: Path = DEFAULT_TARGETS) -> list[dict]:
    targets = json.loads(Path(path).read_text(encoding="utf-8"))
    if not isinstance(targets, list) or len(targets) != 5 or len({target["id"] for target in targets}) != 5:
        raise ValueError("pilot requires exactly five distinct targets")
    return targets


def make_contact_sheet(targets: list[dict], registry_path: Path, source_root: Path,
                       output: Path) -> None:
    """Compose five target columns and two variant rows; leave source PNGs intact."""
    if len(targets) != 5 or len({target["id"] for target in targets}) != 5:
        raise ValueError("contact sheet requires exactly five distinct targets")
    records = registry_module.check_registry(registry_path, source_root=source_root)
    concepts = {entry["id"]: entry for entry in records if entry["kind"] == "concept"}
    cell_width, image_height, label_height = 320, 320, 32
    sheet = Image.new("RGB", (5 * cell_width, 2 * (image_height + label_height)), (220, 217, 211))
    draw = ImageDraw.Draw(sheet)
    for column, target in enumerate(targets):
        for variant in (1, 2):
            entry_id = f"{SOURCE_SET}-{target['id']}-v{variant}"
            if entry_id not in concepts:
                raise ValueError(f"missing contact sheet concept: {entry_id}")
            entry = concepts[entry_id]
            if entry["replaces"] != [target["visual"]]:
                raise ValueError(f"concept visual does not match target: {entry_id}")
            source = Path(source_root).expanduser() / entry["source_png"]
            validate_png(source)
            with Image.open(source) as image:
                thumb = ImageOps.contain(image.convert("RGBA"), (cell_width - 24, image_height - 24))
            left = column * cell_width + (cell_width - thumb.width) // 2
            top = (variant - 1) * (image_height + label_height) + (image_height - thumb.height) // 2
            sheet.paste(thumb, (left, top), thumb)
            label = f"{target['id']} / v{variant}"
            draw.text((column * cell_width + 12, (variant - 1) * (image_height + label_height) + image_height + 8),
                      label, fill=(40, 38, 35))
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(output, format="JPEG", quality=92)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    command = commands.add_parser("import", help="retain one unchanged PNG and back up sources")
    command.add_argument("--target", required=True)
    command.add_argument("--png", type=Path, required=True)
    command.add_argument("--variant", type=int, choices=range(1, 10), required=True)
    command.add_argument("--view", choices=VIEWS)
    command.add_argument("--edit-of", help="registry id of our own 34 concept this view was edited from")
    command.add_argument("--prompt-file", type=Path, required=True)
    command.add_argument("--utc", required=True)
    sheet = commands.add_parser("sheet", help="compose the 5 × 2 JPG contact sheet")
    sheet.add_argument("--output", type=Path, required=True)
    for command in (command, sheet):
        command.add_argument("--targets", type=Path, default=DEFAULT_TARGETS)
        command.add_argument("--registry", type=Path, default=registry_module.DEFAULT_REGISTRY)
        command.add_argument("--source-root", type=Path, default=registry_module.DEFAULT_SOURCE_ROOT)
    args = parser.parse_args(argv)
    try:
        targets = load_targets(args.targets)
        if args.command == "import":
            selected = [target for target in targets if target["id"] == args.target]
            if not selected:
                raise ValueError(f"unknown target: {args.target}")
            record = import_concept(selected[0], args.png, args.variant,
                args.prompt_file.read_text(encoding="utf-8").strip(), args.utc,
                args.registry, args.source_root, args.view, args.edit_of)
            subprocess.run([str(Path(__file__).resolve().parents[1] / "backup-sources.sh"),
                "--registry", str(args.registry), "--source-root", str(args.source_root)], check=True)
            print(f"Imported {record['id']}: {record['source_sha256']}")
        else:
            make_contact_sheet(targets, args.registry, args.source_root, args.output)
            print(f"Contact sheet: {args.output}")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Concept import failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
