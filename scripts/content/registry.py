#!/usr/bin/env python3
"""Validate the text recipes for locally generated material and concept assets.

    python3 scripts/content/registry.py check
    python3 scripts/content/registry.py check --content-root /path/to/bakeoff

The registry belongs in git; file paths are relative to the external content
root. ``generation.outcome`` may be ``pending``, ``complete`` or ``skipped``.
Skipped entries require a reason, retain their recipe, and have no output files.
An inaccessible model may have a null revision only in a skipped recipe.
The built-in image_gen.imagegen tool does not expose a seed or model revision;
imagegen-filter and imagegen-concept records retain these values as null.
Raw ``source_png`` paths are relative to ~/.local/share/gothic-ai/content-sources,
with a ``source_sha256`` beside each path. ``check`` verifies their bytes by
default; ``--source-root`` selects an isolated store or the backup copy.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timedelta
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys
from typing import Any


DEFAULT_REGISTRY = Path(__file__).resolve().parents[2] / "data/content/registry.jsonl"
DEFAULT_SOURCE_ROOT = Path.home() / ".local/share/gothic-ai/content-sources"
MAP_NAMES = frozenset({"albedo", "normal", "roughness", "metallic", "height", "ao"})
REQUIRED_FIELDS = frozenset({
    "id", "kind", "replaces", "pipeline", "models", "prompt", "negative",
    "seed", "parameters", "status", "files",
})
SLUG = re.compile(r"[a-z0-9]+(?:[._-][a-z0-9]+)*\Z")
MODEL_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
REVISION = re.compile(r"[0-9a-f]{40}\Z")
SHA256 = re.compile(r"[0-9a-f]{64}\Z")
BUILTIN_IMAGEGEN_MODEL = "image_gen.imagegen"


class RegistryError(ValueError):
    """An invalid registry recipe or JSONL document."""


def _nonempty(value: Any) -> bool:
    return isinstance(value, str) and bool(value.strip())


def _finite_json(value: Any, field: str = "entry") -> None:
    if isinstance(value, float) and not math.isfinite(value):
        raise RegistryError(f"{field}: JSON numbers must be finite")
    if isinstance(value, dict):
        for key, item in value.items():
            if not isinstance(key, str):
                raise RegistryError(f"{field}: object keys must be strings")
            _finite_json(item, f"{field}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            _finite_json(item, f"{field}[{index}]")
    elif value is not None and not isinstance(value, (str, bool, int, float)):
        raise RegistryError(f"{field}: expected a JSON value")


def _safe_path(value: Any, field: str) -> PurePosixPath:
    if not _nonempty(value):
        raise RegistryError(f"{field}: expected a nonempty relative file path")
    if (
        value.startswith("/") or "\\" in value or ":" in value
        or any(ord(char) < 32 or ord(char) == 127 for char in value)
        or any(part in ("", ".", "..") for part in value.split("/"))
    ):
        raise RegistryError(f"{field}: expected a relative POSIX path without traversal")
    return PurePosixPath(value)


def source_references(value: Any, field: str = "entry") -> list[tuple[str, str]]:
    """Find raw-source pairs anywhere in a recipe, including every attempt.

    ``source_png`` is relative to the permanent source store, independently of
    the staging/output root. Its checksum describes the generator's PNG bytes,
    before resizing, seam repair, brightness correction or map filtering.
    """
    found: list[tuple[str, str]] = []
    if isinstance(value, dict):
        if "source_png" in value or "source_sha256" in value:
            relative = _safe_path(value.get("source_png"), f"{field}.source_png")
            if relative.suffix.lower() != ".png":
                raise RegistryError(f"{field}.source_png: expected a PNG source path")
            digest = value.get("source_sha256")
            if not isinstance(digest, str) or not SHA256.fullmatch(digest):
                raise RegistryError(f"{field}.source_sha256: expected a lowercase SHA-256 digest")
            found.append((relative.as_posix(), digest))
        for key, item in value.items():
            found.extend(source_references(item, f"{field}.{key}"))
    elif isinstance(value, list):
        for index, item in enumerate(value):
            found.extend(source_references(item, f"{field}[{index}]"))
    return found


def verify_sources(entry: Any, source_root: Path) -> None:
    """Require every recorded raw PNG to exist beneath the source root."""
    root = Path(source_root).expanduser().resolve()
    for relative, expected in source_references(entry):
        candidate = (root / relative).resolve()
        if not candidate.is_relative_to(root):
            raise RegistryError(f"source_png: resolved path escapes source root: {relative}")
        if not candidate.is_file():
            raise RegistryError(f"source_png: file does not exist: {relative}")
        try:
            with candidate.open("rb") as stream:
                digest = hashlib.sha256()
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
                actual = digest.hexdigest()
        except OSError as error:
            raise RegistryError(f"source_png: cannot read {relative}: {error}") from error
        if actual != expected:
            raise RegistryError(f"source_sha256: mismatch for {relative}: expected {expected}, got {actual}")


WORLD_CHUNK = re.compile(r"[A-Z0-9_]+/-?[0-9]+_-?[0-9]+\Z")
WORLD_EDIT_TOOL = "scripts/content/world/world_edit.py"


def _validate_world_chunk(entry: dict[str, Any], content_root: Path | None) -> None:
    """A chunk of the converted world mesh edited as a derivative (docs/asset-migration.md, П2).

    No generator is involved: the edit is a script run on the base chunk. ``replaces`` names the
    chunk as ``<WORLD>/<x>_<z>``, ``inputs`` records it as ``converted world mesh <WORLD>/<x>_<z>``
    with the base file's digest, ``files.glb`` is the edited chunk with its digest. A chunk built by
    ``scripts/content/world/world_edit.py`` lists its ``edits`` in order, each a script with its digest
    and the digests of the data files it read."""
    if entry["status"] != "draft":
        raise RegistryError("status: expected 'draft'")
    replaces = entry["replaces"]
    if not isinstance(replaces, list) or len(replaces) != 1 or not isinstance(replaces[0], str) \
            or not WORLD_CHUNK.fullmatch(replaces[0]):
        raise RegistryError("worldchunk.replaces: expected one '<WORLD>/<x>_<z>' chunk")
    if entry["pipeline"] != "blender-world-edit" or entry["models"] != [] or entry["seed"] is not None:
        raise RegistryError("worldchunk: expected blender-world-edit pipeline, no models and null seed")
    if not isinstance(entry["prompt"], str) or not isinstance(entry["negative"], str):
        raise RegistryError("worldchunk: prompt and negative must be strings")
    tool = entry.get("tool")
    if not isinstance(tool, dict) or not _nonempty(tool.get("script")) or \
            not isinstance(tool.get("commit"), str) or not REVISION.fullmatch(tool["commit"]):
        raise RegistryError("worldchunk.tool: expected script and a full commit hash")
    if not isinstance(entry["parameters"], dict):
        raise RegistryError("parameters: expected an object")
    if "edits" in entry:
        edits = entry["edits"]
        if tool["script"] != WORLD_EDIT_TOOL or not isinstance(edits, list) or not edits:
            raise RegistryError(f"worldchunk.edits: expected a nonempty list built by {WORLD_EDIT_TOOL}")
        seen = set()
        for index, edit in enumerate(edits):
            field = f"worldchunk.edits[{index}]"
            if not isinstance(edit, dict) or not _nonempty(edit.get("script")) or \
                    not SHA256.fullmatch(str(edit.get("sha256", ""))):
                raise RegistryError(f"{field}: expected script and sha256")
            if edit["script"] in seen:
                raise RegistryError(f"{field}: {edit['script']} appears twice")
            seen.add(edit["script"])
            _safe_path(edit["script"], f"{field}.script")
            for item in edit.get("inputs", []):
                if not isinstance(item, dict) or not _nonempty(item.get("path")) or \
                        not SHA256.fullmatch(str(item.get("sha256", ""))):
                    raise RegistryError(f"{field}.inputs: expected path and sha256")
    inputs = entry.get("inputs")
    expected = f"converted world mesh {replaces[0]}"
    if not isinstance(inputs, list) or len(inputs) != 1 or not isinstance(inputs[0], dict) or \
            inputs[0].get("input") != expected or not SHA256.fullmatch(str(inputs[0].get("sha256", ""))):
        raise RegistryError(f"worldchunk.inputs: expected one '{expected}' with its sha256")
    files = entry["files"]
    if not isinstance(files, dict) or files.keys() != {"glb"} or not isinstance(files["glb"], dict):
        raise RegistryError("worldchunk.files: expected only glb with path and sha256")
    relative = _safe_path(files["glb"].get("path"), "files.glb.path")
    digest = files["glb"].get("sha256")
    if relative.suffix != ".glb" or not isinstance(digest, str) or not SHA256.fullmatch(digest):
        raise RegistryError("worldchunk.files.glb: expected a .glb path and a sha256")
    if content_root is not None:
        root = Path(content_root).expanduser().resolve()
        candidate = (root / relative).resolve()
        if not candidate.is_relative_to(root) or not candidate.is_file():
            raise RegistryError(f"files.glb: file does not exist: {relative}")
        if hashlib.sha256(candidate.read_bytes()).hexdigest() != digest:
            raise RegistryError(f"files.glb: sha256 mismatch for {relative}")


def validate_entry(entry: Any, *, content_root: Path | None = None,
                   source_root: Path | None = None) -> None:
    """Raise RegistryError on invalid recipes; optionally verify output files.

    Metadata outside the required fields is retained for measurements and future
    recipes. It must remain ordinary, finite JSON. Completed materials contain
    all six maps; concepts retain a raw PNG with no map files.
    """
    if not isinstance(entry, dict):
        raise RegistryError("entry: expected a JSON object")
    _finite_json(entry)
    source_references(entry)
    missing = REQUIRED_FIELDS - entry.keys()
    if missing:
        raise RegistryError(f"entry: missing fields: {', '.join(sorted(missing))}")
    if not isinstance(entry["id"], str) or not SLUG.fullmatch(entry["id"]):
        raise RegistryError("id: expected a lowercase resource slug")
    if entry["kind"] == "worldchunk":
        _validate_world_chunk(entry, content_root)
        return
    if entry["kind"] not in ("material", "concept"):
        raise RegistryError("kind: expected 'material', 'concept' or 'worldchunk'")
    concept = entry["kind"] == "concept"
    if entry["status"] != "draft":
        raise RegistryError("status: expected 'draft'")
    if not isinstance(entry["replaces"], list) or any(not _nonempty(item) for item in entry["replaces"]):
        raise RegistryError("replaces: expected an array of nonempty strings")
    if len(set(entry["replaces"])) != len(entry["replaces"]):
        raise RegistryError("replaces: duplicate resource names")
    for field in ("pipeline", "prompt"):
        if not _nonempty(entry[field]):
            raise RegistryError(f"{field}: expected a nonempty string")
    if not isinstance(entry["negative"], str):
        raise RegistryError("negative: expected a string")
    imagegen = entry["pipeline"] == "imagegen-filter" or (concept and entry["pipeline"] == "imagegen-concept")
    if not (imagegen and entry["seed"] is None) and (
        type(entry["seed"]) is not int or not 0 <= entry["seed"] < 2**64
    ):
        raise RegistryError("seed: expected an integer from 0 through 2**64-1")
    if not isinstance(entry["parameters"], dict):
        raise RegistryError("parameters: expected an object")

    generation = entry.get("generation", {})
    if not isinstance(generation, dict):
        raise RegistryError("generation: expected an object")
    outcome = generation.get("outcome", "complete")
    if outcome not in ("pending", "complete", "skipped"):
        raise RegistryError("generation.outcome: expected 'pending', 'complete' or 'skipped'")
    skip_reason = entry.get("skip_reason", generation.get("reason"))
    if "skip_reason" in entry:
        if not _nonempty(entry["skip_reason"]):
            raise RegistryError("skip_reason: expected a nonempty string")
        if "outcome" in generation and outcome != "skipped":
            raise RegistryError("skip_reason: conflicts with generation.outcome")
        outcome = "skipped"
    if outcome == "skipped" and not _nonempty(skip_reason):
        raise RegistryError("skipped entry: expected a nonempty skip_reason or generation.reason")
    if concept:
        if entry["pipeline"] != "imagegen-concept" or entry["seed"] is not None:
            raise RegistryError("concept: expected imagegen-concept pipeline and null seed")
        if generation.get("outcome") != "complete" or outcome != "complete":
            raise RegistryError("concept: generation.outcome must be 'complete'")
        if len(entry["replaces"]) != 1:
            raise RegistryError("concept: replaces must name exactly one visual")
        if not all(field in entry for field in ("source_png", "source_sha256")):
            raise RegistryError("concept: expected top-level source_png and source_sha256")
        try:
            utc = datetime.fromisoformat(entry.get("utc", "").replace("Z", "+00:00"))
        except (ValueError, AttributeError, TypeError) as error:
            raise RegistryError("concept.utc: expected an ISO 8601 UTC timestamp") from error
        if utc.utcoffset() != timedelta(0):
            raise RegistryError("concept.utc: expected an ISO 8601 UTC timestamp")

    models = entry["models"]
    if not isinstance(models, list) or not models:
        raise RegistryError("models: expected a nonempty array")
    seen_models: set[str] = set()
    for index, model in enumerate(models):
        field = f"models[{index}]"
        if not isinstance(model, dict):
            raise RegistryError(f"{field}: expected an object")
        builtin_imagegen = imagegen and model.get("id") == BUILTIN_IMAGEGEN_MODEL
        if not builtin_imagegen and (not isinstance(model.get("id"), str) or not MODEL_ID.fullmatch(model["id"])):
            raise RegistryError(f"{field}.id: expected a Hugging Face owner/repository identifier")
        if model["id"] in seen_models:
            raise RegistryError(f"{field}.id: duplicate model {model['id']!r}")
        seen_models.add(model["id"])
        if "revision" not in model:
            raise RegistryError(f"{field}.revision: expected a full 40-character commit hash")
        revision = model["revision"]
        if not (revision is None and (outcome == "skipped" or builtin_imagegen)):
            if not isinstance(revision, str) or not REVISION.fullmatch(revision):
                raise RegistryError(f"{field}.revision: expected a full 40-character commit hash")
        if not _nonempty(model.get("license")):
            raise RegistryError(f"{field}.license: expected a nonempty license identifier")
    if concept and seen_models != {BUILTIN_IMAGEGEN_MODEL}:
        raise RegistryError("concept.models: expected only the built-in image_gen.imagegen tool")

    files = entry["files"]
    if not isinstance(files, dict):
        raise RegistryError("files: expected an object mapping map names to paths")
    unknown_maps = files.keys() - MAP_NAMES
    if unknown_maps:
        raise RegistryError(f"files: unknown map names: {', '.join(sorted(unknown_maps))}")
    if concept and files:
        raise RegistryError("concept.files: must be empty; raw PNG is named by source_png")
    if not concept and outcome == "complete" and files.keys() != MAP_NAMES:
        raise RegistryError(f"files: completed material requires {', '.join(sorted(MAP_NAMES))}")
    if outcome in ("pending", "skipped") and files:
        raise RegistryError(f"files: {outcome} material must have no output files")
    for name, value in files.items():
        relative = _safe_path(value, f"files.{name}")
        if content_root is not None:
            root = Path(content_root).expanduser().resolve()
            candidate = (root / relative).resolve()
            if not candidate.is_relative_to(root):
                raise RegistryError(f"files.{name}: resolved path escapes content root")
            if not candidate.is_file():
                raise RegistryError(f"files.{name}: file does not exist: {value}")
    if source_root is not None:
        verify_sources(entry, source_root)


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RegistryError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def check_registry(path: Path = DEFAULT_REGISTRY, *, content_root: Path | None = None,
                   source_root: Path | None = None) -> list[dict[str, Any]]:
    """Validate recipes, unique ids, and all recorded sources by default."""
    path = Path(path)
    source_root = DEFAULT_SOURCE_ROOT if source_root is None else Path(source_root)
    if content_root is not None and not Path(content_root).expanduser().is_dir():
        raise RegistryError(f"content root is not a directory: {content_root}")
    entries: list[dict[str, Any]] = []
    seen: set[str] = set()
    try:
        with path.open(encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    raise RegistryError(f"{path}:{line_number}: empty JSONL row")
                try:
                    entry = json.loads(line, object_pairs_hook=_unique_object)
                    validate_entry(entry, content_root=content_root, source_root=source_root)
                    if entry["id"] in seen:
                        raise RegistryError(f"duplicate id: {entry['id']!r}")
                except (RegistryError, json.JSONDecodeError) as error:
                    raise RegistryError(f"{path}:{line_number}: {error}") from error
                seen.add(entry["id"])
                entries.append(entry)
    except (OSError, UnicodeError) as error:
        raise RegistryError(f"{path}: {error}") from error
    if not entries:
        raise RegistryError(f"{path}: registry is empty")
    return entries


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="validate JSONL recipes and optional external output files")
    check.add_argument("registry", nargs="?", type=Path, default=DEFAULT_REGISTRY)
    check.add_argument("--content-root", type=Path, help="also require output files beneath this directory")
    check.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT,
                       help="raw PNG store (default: ~/.local/share/gothic-ai/content-sources)")
    args = parser.parse_args(argv)
    try:
        entries = check_registry(args.registry, content_root=args.content_root, source_root=args.source_root)
    except RegistryError as error:
        print(f"Registry check failed: {error}", file=sys.stderr)
        return 1
    suffix = " (output files verified)" if args.content_root is not None else ""
    source_count = len({pair for entry in entries for pair in source_references(entry)})
    print(f"Registry check passed: {len(entries)} recipes, {source_count} source PNGs verified{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
