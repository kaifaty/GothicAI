"""The registry accepts reproducible drafts and rejects unsafe or lost recipes."""

from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest


SCRIPT = Path(__file__).with_name("registry.py")
SPEC = importlib.util.spec_from_file_location("content_registry", SCRIPT)
registry = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(registry)


def recipe():
    return {
        "id": "bakeoff-cobblestone-flux-filter",
        "kind": "material", "replaces": [], "pipeline": "flux-filter",
        "models": [{"id": "black-forest-labs/FLUX.1-schnell", "revision": "a" * 40, "license": "Apache-2.0"}],
        "prompt": "Worn medieval cobblestones, seamless texture.", "negative": "text, logos",
        "seed": 1024, "parameters": {"width": 1024, "height": 1024, "tileable": True},
        "status": "draft",
        "files": {name: f"cobblestone/flux-filter/{name}.png" for name in registry.MAP_NAMES},
        "generation": {"outcome": "complete", "elapsed_seconds": 1.25, "peak_vram_mib": 2048},
    }


def write_registry(tmp_path, entries):
    path = tmp_path / "registry.jsonl"
    path.write_text("".join(json.dumps(entry) + "\n" for entry in entries))
    return path


def test_complete_recipe_and_external_outputs(tmp_path):
    entry = recipe()
    content_root = tmp_path / "external"
    for path in entry["files"].values():
        output = content_root / path
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(b"map")
    path = write_registry(tmp_path, [entry])
    assert registry.check_registry(path, content_root=content_root) == [entry]
    assert registry.main(["check", str(path), "--content-root", str(content_root)]) == 0


@pytest.mark.parametrize("outcome", ["pending", "skipped"])
def test_unfinished_recipes_do_not_claim_output_files(tmp_path, outcome):
    entry = recipe()
    entry["files"] = {}
    entry["generation"] = {"outcome": outcome}
    if outcome == "skipped":
        entry["generation"]["reason"] = "Gated model: agreement was not accepted."
        entry["models"][0]["revision"] = None
    assert registry.check_registry(write_registry(tmp_path, [entry]), content_root=tmp_path) == [entry]


def test_explicit_skip_reason_can_stand_alone(tmp_path):
    entry = recipe()
    entry["files"] = {}
    del entry["generation"]
    entry["skip_reason"] = "The model is gated."
    registry.check_registry(write_registry(tmp_path, [entry]))


@pytest.mark.parametrize("field,value,message", [
    ("id", "../stone", "resource slug"),
    ("kind", "texture", "kind"),
    ("status", "accepted", "status"),
    ("replaces", "", "replaces"),
    ("replaces", [1], "replaces"),
    ("replaces", ["WALL.TGA", "WALL.TGA"], "duplicate resource"),
    ("pipeline", " ", "pipeline"),
    ("prompt", "", "prompt"),
    ("negative", None, "negative"),
    ("seed", True, "seed"),
    ("seed", 1.0, "seed"),
    ("seed", -1, "seed"),
    ("seed", 2**64, "seed"),
    ("parameters", [], "parameters"),
    ("models", [], "models"),
    ("files", [], "files"),
    ("files", {}, "completed material requires"),
    ("generation", [], "generation"),
    ("generation", {"outcome": "failed"}, "generation.outcome"),
    ("generation", {"outcome": "skipped"}, "nonempty skip_reason"),
    ("generation", {"outcome": "pending"}, "must have no output"),
    ("skip_reason", "gated", "conflicts"),
    ("skip_reason", "", "skip_reason"),
])
def test_invalid_recipe_fields(tmp_path, field, value, message):
    entry = recipe()
    entry[field] = value
    with pytest.raises(registry.RegistryError, match=message):
        registry.check_registry(write_registry(tmp_path, [entry]))


@pytest.mark.parametrize("field,value,message", [
    ("id", "local-model", "owner/repository"),
    ("revision", "main", "40-character"),
    ("revision", None, "40-character"),
    ("revision", "a" * 39, "40-character"),
    ("license", "", "license"),
    ("license", False, "license"),
])
def test_models_require_pinned_revision_and_license(field, value, message):
    entry = recipe()
    entry["models"][0][field] = value
    with pytest.raises(registry.RegistryError, match=message):
        registry.validate_entry(entry)


def test_duplicate_models_and_missing_required_fields():
    entry = recipe()
    entry["models"] *= 2
    with pytest.raises(registry.RegistryError, match="duplicate model"):
        registry.validate_entry(entry)
    entry = recipe()
    del entry["models"][0]["revision"]
    with pytest.raises(registry.RegistryError, match="revision"):
        registry.validate_entry(entry)
    del entry["models"]
    with pytest.raises(registry.RegistryError, match="missing fields: models"):
        registry.validate_entry(entry)


@pytest.mark.parametrize("path", [
    "/tmp/material.png", "../material.png", "map/../../material.png",
    "map/./material.png", "map//material.png", "C:/material.png",
    r"map\material.png", "map/", "", "map/normal\x00.png",
])
def test_external_paths_cannot_escape_or_depend_on_platform(path):
    entry = recipe()
    entry["files"]["normal"] = path
    with pytest.raises(registry.RegistryError, match="files.normal"):
        registry.validate_entry(entry)


def test_missing_files_unknown_maps_and_symlinks_fail(tmp_path):
    entry = recipe()
    with pytest.raises(registry.RegistryError, match="file does not exist"):
        registry.validate_entry(entry, content_root=tmp_path)
    entry["files"]["original"] = "original.png"
    with pytest.raises(registry.RegistryError, match="unknown map"):
        registry.validate_entry(entry)
    del entry["files"]["original"]
    root = tmp_path / "content"
    root.mkdir()
    outside = tmp_path / "outside.png"
    outside.write_bytes(b"outside")
    (root / "escape.png").symlink_to(outside)
    for name in entry["files"]:
        entry["files"][name] = "escape.png"
    with pytest.raises(registry.RegistryError, match="escapes content root"):
        registry.validate_entry(entry, content_root=root)


def test_duplicate_ids_are_located_by_line(tmp_path):
    entry = recipe()
    with pytest.raises(registry.RegistryError, match=r"registry.jsonl:2: duplicate id"):
        registry.check_registry(write_registry(tmp_path, [entry, deepcopy(entry)]))


@pytest.mark.parametrize("text,message", [
    ("", "registry is empty"),
    ("\n", "empty JSONL row"),
    ("not json\n", "registry.jsonl:1:"),
    ("[]\n", "expected a JSON object"),
    ('{"id":"one","id":"two"}\n', "duplicate JSON key"),
])
def test_bad_jsonl_has_actionable_errors(tmp_path, text, message):
    path = tmp_path / "registry.jsonl"
    path.write_text(text)
    with pytest.raises(registry.RegistryError, match=message):
        registry.check_registry(path)


@pytest.mark.parametrize("number", [float("nan"), float("inf"), float("-inf")])
def test_nested_nonfinite_measurements_are_rejected(tmp_path, number):
    entry = recipe()
    entry["generation"]["samples"] = [{"seconds": number}]
    with pytest.raises(registry.RegistryError, match="JSON numbers must be finite"):
        registry.check_registry(write_registry(tmp_path, [entry]))


def test_cli_returns_nonzero_without_traceback(tmp_path):
    path = tmp_path / "missing.jsonl"
    result = subprocess.run([sys.executable, str(SCRIPT), "check", str(path)], capture_output=True, text=True)
    assert result.returncode == 1
    assert "Registry check failed:" in result.stderr
    assert "Traceback" not in result.stderr
