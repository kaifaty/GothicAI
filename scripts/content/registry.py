#!/usr/bin/env python3
"""Validate the text recipes for locally generated material assets.

    python3 scripts/content/registry.py check
    python3 scripts/content/registry.py check --content-root /path/to/bakeoff

The registry belongs in git; file paths are relative to the external content
root. ``generation.outcome`` may be ``pending``, ``complete`` or ``skipped``.
Skipped entries require a reason, retain their recipe, and have no output files.
An inaccessible model may have a null revision only in a skipped recipe.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys
from typing import Any


DEFAULT_REGISTRY = Path(__file__).resolve().parents[2] / "data/content/registry.jsonl"
MAP_NAMES = frozenset({"albedo", "normal", "roughness", "metallic", "height", "ao"})
REQUIRED_FIELDS = frozenset({
    "id", "kind", "replaces", "pipeline", "models", "prompt", "negative",
    "seed", "parameters", "status", "files",
})
SLUG = re.compile(r"[a-z0-9]+(?:[._-][a-z0-9]+)*\Z")
MODEL_ID = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]*/[A-Za-z0-9][A-Za-z0-9_.-]*\Z")
REVISION = re.compile(r"[0-9a-f]{40}\Z")


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


def validate_entry(entry: Any, *, content_root: Path | None = None) -> None:
    """Raise RegistryError on invalid recipes; optionally verify output files.

    Metadata outside the required fields is retained for measurements and future
    recipes. It must remain ordinary, finite JSON. Completed drafts contain all
    six maps; pending and skipped drafts contain no output paths.
    """
    if not isinstance(entry, dict):
        raise RegistryError("entry: expected a JSON object")
    _finite_json(entry)
    missing = REQUIRED_FIELDS - entry.keys()
    if missing:
        raise RegistryError(f"entry: missing fields: {', '.join(sorted(missing))}")
    if not isinstance(entry["id"], str) or not SLUG.fullmatch(entry["id"]):
        raise RegistryError("id: expected a lowercase resource slug")
    if entry["kind"] != "material":
        raise RegistryError("kind: expected 'material'")
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
    if type(entry["seed"]) is not int or not 0 <= entry["seed"] < 2**64:
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

    models = entry["models"]
    if not isinstance(models, list) or not models:
        raise RegistryError("models: expected a nonempty array")
    seen_models: set[str] = set()
    for index, model in enumerate(models):
        field = f"models[{index}]"
        if not isinstance(model, dict):
            raise RegistryError(f"{field}: expected an object")
        if not isinstance(model.get("id"), str) or not MODEL_ID.fullmatch(model["id"]):
            raise RegistryError(f"{field}.id: expected a Hugging Face owner/repository identifier")
        if model["id"] in seen_models:
            raise RegistryError(f"{field}.id: duplicate model {model['id']!r}")
        seen_models.add(model["id"])
        if "revision" not in model:
            raise RegistryError(f"{field}.revision: expected a full 40-character commit hash")
        revision = model["revision"]
        if not (revision is None and outcome == "skipped"):
            if not isinstance(revision, str) or not REVISION.fullmatch(revision):
                raise RegistryError(f"{field}.revision: expected a full 40-character commit hash")
        if not _nonempty(model.get("license")):
            raise RegistryError(f"{field}.license: expected a nonempty license identifier")

    files = entry["files"]
    if not isinstance(files, dict):
        raise RegistryError("files: expected an object mapping map names to paths")
    unknown_maps = files.keys() - MAP_NAMES
    if unknown_maps:
        raise RegistryError(f"files: unknown map names: {', '.join(sorted(unknown_maps))}")
    if outcome == "complete" and files.keys() != MAP_NAMES:
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


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise RegistryError(f"duplicate JSON key: {key!r}")
        result[key] = value
    return result


def check_registry(path: Path = DEFAULT_REGISTRY, *, content_root: Path | None = None) -> list[dict[str, Any]]:
    """Read and validate every JSONL row, including registry-wide unique ids."""
    path = Path(path)
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
                    validate_entry(entry, content_root=content_root)
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
    args = parser.parse_args(argv)
    try:
        entries = check_registry(args.registry, content_root=args.content_root)
    except RegistryError as error:
        print(f"Registry check failed: {error}", file=sys.stderr)
        return 1
    suffix = " (output files verified)" if args.content_root is not None else ""
    print(f"Registry check passed: {len(entries)} material recipes{suffix}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
