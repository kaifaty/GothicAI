"""The registry accepts reproducible drafts and rejects unsafe or lost recipes."""

from copy import deepcopy
import importlib.util
import hashlib
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


def test_builtin_imagegen_retains_unreported_seed_and_revision(tmp_path):
    entry = recipe()
    entry.update(pipeline="imagegen-filter", seed=None)
    entry["models"] = [{"id": "image_gen.imagegen", "revision": None,
                        "license": "OpenAI terms: output belongs to the user"}]
    assert registry.check_registry(write_registry(tmp_path, [entry])) == [entry]
    # The exception is tied to this pipeline, not to any hosted/unpinned model.
    entry["models"][0]["id"] = "black-forest-labs/FLUX.1-schnell"
    with pytest.raises(registry.RegistryError, match="40-character"):
        registry.validate_entry(entry)
    entry["models"][0]["id"] = "image_gen.imagegen"
    entry.update(pipeline="flux-filter", seed=1024)
    with pytest.raises(registry.RegistryError, match="owner/repository"):
        registry.validate_entry(entry)


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
    ("seed", None, "seed"),
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


def source_recipe(tmp_path):
    entry = recipe()
    source_root = tmp_path / 'raw-sources'
    source = source_root / 'city-3/wall-attempt-1.png'
    source.parent.mkdir(parents=True)
    source.write_bytes(b'original generator PNG bytes')
    entry['generation']['attempts'] = [dict(number=1,
        source_png=source.relative_to(source_root).as_posix(),
        source_sha256=hashlib.sha256(source.read_bytes()).hexdigest())]
    return entry, source_root, source


def test_recorded_sources_are_verified_by_default_and_cli_can_override(tmp_path, monkeypatch):
    entry, source_root, source = source_recipe(tmp_path)
    path = write_registry(tmp_path, [entry])
    monkeypatch.setattr(registry, 'DEFAULT_SOURCE_ROOT', source_root)
    assert registry.check_registry(path) == [entry]
    result = subprocess.run([sys.executable, str(SCRIPT), 'check', str(path),
        '--source-root', str(source_root)], capture_output=True, text=True)
    assert result.returncode == 0
    assert '1 source PNGs verified' in result.stdout
    source.write_bytes(b'changed after generation')
    with pytest.raises(registry.RegistryError, match='source_sha256: mismatch'):
        registry.check_registry(path)
    # Recipe construction can validate metadata before the raw PNG is retained.
    registry.validate_entry(entry)


def test_missing_source_and_escaping_source_symlink_fail(tmp_path):
    entry, source_root, source = source_recipe(tmp_path)
    source.unlink()
    with pytest.raises(registry.RegistryError, match='source_png: file does not exist'):
        registry.validate_entry(entry, source_root=source_root)
    outside = tmp_path / 'outside.png'
    outside.write_bytes(b'original generator PNG bytes')
    source.symlink_to(outside)
    with pytest.raises(registry.RegistryError, match='escapes source root'):
        registry.validate_entry(entry, source_root=source_root)


@pytest.mark.parametrize(('updates', 'message'), [
    ({'source_png': '../outside.png'}, 'source_png'),
    ({'source_png': '/absolute.png'}, 'source_png'),
    ({'source_png': 'raw/source.jpg'}, 'PNG source'),
    ({'source_sha256': None}, 'source_sha256'),
    ({'source_sha256': 'A' * 64}, 'source_sha256'),
    ({'source_sha256': '0' * 63}, 'source_sha256'),
])
def test_source_pairs_require_safe_paths_and_sha256(tmp_path, updates, message):
    entry, _, _ = source_recipe(tmp_path)
    entry['generation']['attempts'][0].update(updates)
    with pytest.raises(registry.RegistryError, match=message):
        registry.validate_entry(entry)


@pytest.mark.parametrize('missing', ['source_png', 'source_sha256'])
def test_source_path_and_digest_are_required_together(tmp_path, missing):
    entry, _, _ = source_recipe(tmp_path)
    del entry['generation']['attempts'][0][missing]
    with pytest.raises(registry.RegistryError, match=missing):
        registry.validate_entry(entry)


def test_all_attempts_are_checked_including_unselected_source(tmp_path):
    entry, source_root, source = source_recipe(tmp_path)
    attempt = deepcopy(entry['generation']['attempts'][0])
    attempt.update(number=2, source_png='city-3/missing-attempt-2.png', selected=False)
    entry['generation']['attempts'].append(attempt)
    with pytest.raises(registry.RegistryError, match='missing-attempt-2.png'):
        registry.check_registry(write_registry(tmp_path, [entry]), source_root=source_root)


def concept_recipe(tmp_path):
    entry, source_root, _ = source_recipe(tmp_path)
    attempt = entry['generation']['attempts'][0]
    entry.update(id='mesh3d-pilot-barrel-v1', kind='concept', pipeline='imagegen-concept', seed=None,
        replaces=['BARREL.MRM'], files={}, utc='2026-10-20T12:00:00Z',
        source_png=attempt['source_png'], source_sha256=attempt['source_sha256'],
        generation={'outcome': 'complete'},
        models=[dict(id='image_gen.imagegen', revision=None, license='OpenAI Terms of Use')])
    return entry, source_root


def test_concepts_and_materials_share_registry_and_source_verification(tmp_path):
    concept, source_root = concept_recipe(tmp_path)
    material = recipe()
    path = write_registry(tmp_path, [material, concept])
    assert registry.check_registry(path, source_root=source_root) == [material, concept]
    assert registry.source_references(concept) == [(concept['source_png'], concept['source_sha256'])]
    # Same standalone API used by backup-sources.sh on dev-ubuntu.
    registry.validate_entry(concept, source_root=source_root)
    assert registry.main(['check', str(path), '--source-root', str(source_root)]) == 0


@pytest.mark.parametrize(('updates', 'message'), [
    ({'pipeline': 'imagegen-filter'}, 'imagegen-concept pipeline'),
    ({'seed': 123}, 'null seed'),
    ({'generation': {}}, 'generation.outcome'),
    ({'generation': {'outcome': 'pending'}}, 'generation.outcome'),
    ({'replaces': []}, 'exactly one visual'),
    ({'utc': '2026-10-20T12:00:00'}, 'UTC timestamp'),
    ({'utc': '2026-10-20T12:00:00+03:00'}, 'UTC timestamp'),
    ({'utc': None}, 'UTC timestamp'),
    ({'files': {'albedo': 'source.png'}}, 'concept.files'),
    ({'models': [dict(id='owner/model', revision='a' * 40, license='MIT')]}, 'built-in'),
])
def test_concepts_require_completed_builtin_tool_and_raw_source(tmp_path, updates, message):
    entry, _ = concept_recipe(tmp_path)
    entry.update(updates)
    with pytest.raises(registry.RegistryError, match=message):
        registry.validate_entry(entry)


def test_concept_requires_top_level_raw_source_and_utc(tmp_path):
    entry, _ = concept_recipe(tmp_path)
    nested = dict(source_png=entry.pop('source_png'), source_sha256=entry.pop('source_sha256'))
    entry['generation']['attempts'] = [nested]
    with pytest.raises(registry.RegistryError, match='top-level source_png'):
        registry.validate_entry(entry)
    entry.update(nested)
    del entry['utc']
    with pytest.raises(registry.RegistryError, match='UTC timestamp'):
        registry.validate_entry(entry)


def world_chunk_entry(digest):
    return {
        "id": "harbour-stairs-newworld-1-1", "kind": "worldchunk", "replaces": ["NEWWORLD/1_-1"],
        "pipeline": "blender-world-edit", "models": [], "prompt": "", "negative": "", "seed": None,
        "parameters": {"steps": 12}, "status": "draft",
        "tool": {"script": "scripts/content/world/harbour_stairs.py", "commit": "a" * 40},
        "inputs": [{"input": "converted world mesh NEWWORLD/1_-1", "sha256": "b" * 64}],
        "files": {"glb": {"path": "stairs-3/worlds/NEWWORLD/chunks/1_-1.glb", "sha256": digest}},
    }


def test_world_chunk_edit(tmp_path):
    import hashlib
    glb = tmp_path / "stairs-3/worlds/NEWWORLD/chunks/1_-1.glb"
    glb.parent.mkdir(parents=True)
    glb.write_bytes(b"glb")
    entry = world_chunk_entry(hashlib.sha256(b"glb").hexdigest())
    registry.validate_entry(entry, content_root=tmp_path)
    with pytest.raises(registry.RegistryError, match="sha256 mismatch"):
        registry.validate_entry(world_chunk_entry("c" * 64), content_root=tmp_path)
    bad = world_chunk_entry("c" * 64)
    bad["inputs"][0]["input"] = "original mesh NEWWORLD/1_-1"
    with pytest.raises(registry.RegistryError, match="worldchunk.inputs"):
        registry.validate_entry(bad)
    bad = world_chunk_entry("c" * 64)
    bad["replaces"] = ["NEWWORLD.ZEN"]
    with pytest.raises(registry.RegistryError, match="worldchunk.replaces"):
        registry.validate_entry(bad)


def test_world_chunk_edits(tmp_path):
    entry = world_chunk_entry("c" * 64)
    entry["tool"]["script"] = "scripts/content/world/world_edit.py"
    entry["edits"] = [{"script": "scripts/content/world/harbour_stairs.py", "sha256": "d" * 64},
                      {"script": "scripts/content/world/roof_holes.py", "sha256": "e" * 64,
                       "inputs": [{"path": "data/content/world/roof01-holes.json", "sha256": "f" * 64}]}]
    registry.validate_entry(entry)
    bad = json.loads(json.dumps(entry))
    bad["edits"].append(dict(bad["edits"][0]))
    with pytest.raises(registry.RegistryError, match="appears twice"):
        registry.validate_entry(bad)
    bad = json.loads(json.dumps(entry))
    bad["tool"]["script"] = "scripts/content/world/harbour_stairs.py"
    with pytest.raises(registry.RegistryError, match="worldchunk.edits"):
        registry.validate_entry(bad)
    bad = json.loads(json.dumps(entry))
    bad["edits"][1]["inputs"][0]["sha256"] = "x"
    with pytest.raises(registry.RegistryError, match="inputs"):
        registry.validate_entry(bad)
