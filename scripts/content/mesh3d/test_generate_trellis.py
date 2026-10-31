"""Numerical/file contract tests. No model, network or GPU imports."""
import json
from pathlib import Path
import socket
import sys
import time
from types import SimpleNamespace

import numpy as np
from PIL import Image
import pytest

sys.path.insert(0, str(Path(__file__).parent))
import generate_trellis as generator
from generate_trellis import MemoryJournal, read_rgba, sample_voxel_attributes, save_raw, srgb_to_linear


@pytest.fixture(autouse=True)
def offline(monkeypatch):
    def blocked(*args, **kwargs):
        raise AssertionError("Network is forbidden in mesh3d CPU checks")
    monkeypatch.setattr(socket, "create_connection", blocked)
    monkeypatch.setattr(socket.socket, "connect", blocked)


def test_sparse_attributes_use_voxel_centers_and_world_origin():
    coords = [[5, 0, 0], [0, 0, 0], [2, 0, 0]]
    attrs = np.array([[1, 0, 0, 0.8, 0.2], [0, 1, 0, 0, 1], [0, 0, 1, 0.1, 0.9]])
    origin, size = np.array([-0.5, -1, 2]), 0.25
    query = np.array([[0.6, 0.5, 0.5], [2.7, 0.5, 0.5], [5.5, 0.5, 0.5]])
    result = sample_voxel_attributes(origin + size * query, coords, attrs, origin, size)
    np.testing.assert_allclose(result, attrs[[1, 2, 0]])
    assert result.dtype == np.float32
    # The decision boundary is between voxel centers, not between integers.
    adjacent = sample_voxel_attributes([[0.9, 0.5, 0.5], [1.1, 0.5, 0.5]],
                                       [[0, 0, 0], [1, 0, 0]], [[0], [1]], [0, 0, 0], 1)
    np.testing.assert_array_equal(adjacent, [[0], [1]])


def test_raw_formats_have_exact_fields_dtypes_and_roundtrip(tmp_path):
    vertices = [[0, 0, 0], [1, 0, 0], [0, 1, 0]]
    faces = [[0, 1, 2]]
    color = srgb_to_linear([[0, 0.5, 1]] * 3)
    for pbr in (False, True):
        path = tmp_path / f"{pbr}.npz"
        fields = {"metallic": [0, 0.5, 1], "roughness": [1, 0.5, 0]} if pbr else {}
        assert save_raw(path, vertices, faces, color, **fields) == {"vertices": 3, "faces": 1}
        with np.load(path, allow_pickle=False) as saved:
            assert set(saved.files) == {"vertices", "faces", "color", *fields}
            assert saved["vertices"].shape == saved["color"].shape == (3, 3)
            assert saved["faces"].shape == (1, 3)
            assert saved["faces"].dtype == np.int32
            for name in set(saved.files) - {"faces"}:
                assert saved[name].dtype == np.float32
            for name in fields:
                assert saved[name].shape == (3,)
            np.testing.assert_allclose(saved["color"][0], [0, 0.21404114, 1], atol=1e-7)


@pytest.mark.parametrize("field,value,match", [
    ("faces", [[0, 1, 9]], "index"),
    ("color", [[1.1, 0, 0]] * 3, "0..1"),
    ("color", [[np.nan, 0, 0]] * 3, "nonfinite"),
    ("metallic", [0, 0, 0], "together"),
])
def test_raw_rejects_invalid_contract(tmp_path, field, value, match):
    arrays = dict(vertices=[[0, 0, 0], [1, 0, 0], [0, 1, 0]], faces=[[0, 1, 2]],
                  color=[[0.5, 0.5, 0.5]] * 3)
    arrays[field] = value
    with pytest.raises(ValueError, match=match):
        save_raw(tmp_path / "raw.npz", **arrays)
    assert not (tmp_path / "raw.npz").exists()


def test_rgba_requires_transparency_and_foreground(tmp_path):
    path = tmp_path / "input.png"
    image = Image.new("RGBA", (16, 16), (0, 0, 0, 0))
    image.paste((100, 60, 20, 255), (4, 4, 12, 12))
    image.save(path)
    assert read_rgba(path).mode == "RGBA"
    for mode, color in (("RGB", (100, 60, 20)), ("RGBA", (100, 60, 20, 255)), ("RGBA", (0, 0, 0, 0))):
        Image.new(mode, (16, 16), color).save(path)
        with pytest.raises(ValueError):
            read_rgba(path)


def test_check_imports_no_gpu_dependency_in_cpu_module():
    assert "torch" not in sys.modules
    assert "transformers" not in sys.modules


def test_live_metadata_is_durable_before_model_returns(tmp_path):
    # No CUDA import: the counter is a stub, while writes/threading are real.
    peak = {"bytes": 2**20}
    cuda = SimpleNamespace(is_initialized=lambda: True)
    for name in ("memory_allocated", "memory_reserved", "max_memory_allocated", "max_memory_reserved"):
        setattr(cuda, name, lambda: peak["bytes"])
    meta = {"attempts": []}
    journal = MemoryJournal(tmp_path, meta, SimpleNamespace(cuda=cuda))
    try:
        journal.begin_attempt("512", 1)
        saved = json.loads((tmp_path / "meta.json").read_text())
        assert saved["active_attempt"] == 0
        assert saved["attempts"][0]["outcome"] == "running"
        assert saved["attempts"][0]["peak_allocated_mib"] == 1
        peak["bytes"] = 2 * 2**20
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            saved = json.loads((tmp_path / "meta.json").read_text())
            if saved["peak_allocated_mib"] == 2:
                break
            time.sleep(0.05)
        assert saved["peak_allocated_mib"] == 2
        assert saved["peak_rss_mib"] > 0
        records = [json.loads(line) for line in (tmp_path / "memory.jsonl").read_text().splitlines()]
        assert records[-1]["active_attempt"] == 0
        assert records[-1]["pipeline_type"] == "512"
        assert records[-1]["peak_allocated_mib"] == 2
        journal.update_attempt(outcome="ok", stage="finished")
        journal.finish_attempt()
    finally:
        journal.stop()
    saved = json.loads((tmp_path / "meta.json").read_text())
    assert saved["active_attempt"] is None
    assert saved["attempts"][0]["outcome"] == "ok"


@pytest.fixture
def batch(monkeypatch, tmp_path):
    """Exercise the real runner, mesh export and journal with tiny fake tensors."""
    inputs, output = tmp_path / "inputs", tmp_path / "output"
    inputs.mkdir()
    for name in ("a", "b"):
        image = Image.new("RGBA", (8, 8), (0, 0, 0, 0))
        image.paste((100, 60, 20, 255), (2, 2, 6, 6))
        image.save(inputs / f"{name}.png")
    state = SimpleNamespace(loads=0, cuda_calls=0, resets=0, calls=[], peak=2**20,
                            before_run=lambda image, seed: None, errors={})

    class Tensor:
        def __init__(self, array):
            self.array = np.asarray(array)

        def detach(self):
            return self

        def cpu(self):
            return self

        def numpy(self):
            return self.array

    class Pipeline:
        def cuda(self):
            state.cuda_calls += 1

        def run(self, image, *, seed, **kwargs):
            state.before_run(image, seed)
            key = (Path(image.filename).stem, seed)
            state.calls.append((key, kwargs))
            state.peak = (len(state.calls) + 1) * 2**20
            if key in state.errors:
                raise state.errors[key]
            mesh = SimpleNamespace(vertices=Tensor([[0, 0, 0], [1, 0, 0], [0, 1, 0]]),
                                   faces=Tensor([[0, 1, 2]]),
                                   vertex_attrs=Tensor([[0.5, 0.5, 0.5]] * 3),
                                   coords=Tensor([[0, 0, 0]]),
                                   attrs=Tensor([[0.5, 0.5, 0.5, 0.2, 0.8]]),
                                   origin=Tensor([0, 0, 0]), voxel_size=1)
            return [mesh] if "pipeline_type" in kwargs else {"mesh": [mesh]}

    def load_pipeline(root, model, **kwargs):
        state.loads += 1
        return Pipeline()

    def reset():
        state.resets += 1
        state.peak = 2**20

    cuda = SimpleNamespace(is_initialized=lambda: True, is_available=lambda: True,
                           empty_cache=lambda: None, synchronize=lambda: None,
                           reset_peak_memory_stats=reset)
    for name in ("memory_allocated", "memory_reserved", "max_memory_allocated", "max_memory_reserved"):
        setattr(cuda, name, lambda: state.peak)
    torch = SimpleNamespace(cuda=cuda, OutOfMemoryError=type("OutOfMemoryError", (Exception,), {}))
    monkeypatch.setitem(sys.modules, "torch", torch)
    monkeypatch.setitem(sys.modules, "adapter", SimpleNamespace(load_pipeline=load_pipeline,
                                                              assert_clean_imports=lambda: None))
    monkeypatch.setattr(generator, "_generation_metadata", lambda args, torch: (
        {"model": args.model, "attempts": [], "parameters": {}},
        ["512" if args.model == "trellis2" else None]))
    args = SimpleNamespace(root=tmp_path, model="trellis", images_dir=inputs, output=output,
                           seeds=[1, 2], pipeline_type="auto", deadline=None)
    return args, state


@pytest.mark.parametrize("model", ["trellis", "trellis2"])
def test_batch_loads_model_once_and_saves_before_next_variant(batch, model):
    args, state = batch
    args.model = model
    args.pipeline_type = "512" if model == "trellis2" else "auto"
    prior = []

    def before_run(image, seed):
        if prior:
            previous = prior[-1]
            meta = json.loads((previous / "meta.json").read_text())
            assert meta["outcome"] == "ok"
            assert meta["active_attempt"] is None
            assert (previous / "raw.npz").is_file()
            assert (previous / "memory.jsonl").stat().st_size > 0
            records = [json.loads(line) for line in (args.output / "batch.jsonl").read_text().splitlines()]
            assert any(row["event"] == "variant_finished" and row["output"] == str(previous)
                       for row in records)
        prior.append(args.output / Path(image.filename).stem / f"{model}-s{seed}")

    state.before_run = before_run
    assert generator.generate_batch(args) == 0
    assert state.loads == state.cuda_calls == 1
    assert state.resets == 4
    assert [key for key, _ in state.calls] == [("a", 1), ("a", 2), ("b", 1), ("b", 2)]
    for index, path in enumerate(prior):
        meta = json.loads((path / "meta.json").read_text())
        assert meta["mesh"] == {"vertices": 3, "faces": 1}
        assert meta["peak_allocated_mib"] == index + 2
        assert "lifetime" in meta["rss_scope"]
        with np.load(path / "raw.npz") as raw:
            assert ("metallic" in raw.files) == (model == "trellis2")


def test_batch_one_generator_error_does_not_drop_other_variants(batch):
    args, state = batch
    state.errors[("a", 2)] = RuntimeError("isolated bad sample")
    assert generator.generate_batch(args) == 1
    assert len(state.calls) == 4
    assert state.loads == 1
    failed = args.output / "a/trellis-s2"
    meta = json.loads((failed / "meta.json").read_text())
    assert meta["outcome"] == "error"
    assert meta["attempts"][0]["error"] == "RuntimeError: isolated bad sample"
    assert not (failed / "raw.npz").exists()
    assert len(list(args.output.glob("*/*/raw.npz"))) == 3


def test_batch_repeated_generator_error_stops_and_explains_unattempted(batch):
    args, state = batch
    state.errors = {("a", 1): RuntimeError("same generator error"),
                    ("a", 2): RuntimeError("same generator error")}
    assert generator.generate_batch(args) == 86
    assert len(state.calls) == 2
    records = [json.loads(line) for line in (args.output / "batch.jsonl").read_text().splitlines()]
    failures = [row for row in records if row["event"] == "variant_finished"]
    skipped = [row for row in records if row["event"] == "variant_skipped"]
    assert len(failures) == len(skipped) == 2
    assert all(row["reason"] == "repeated_generator_error" for row in skipped)
    assert all(row["error"] == "RuntimeError: same generator error" for row in failures)


def test_batch_second_oom_stops_even_with_different_allocation_sizes(batch):
    args, state = batch
    oom = sys.modules["torch"].OutOfMemoryError
    state.errors = {("a", 1): oom("cannot allocate 100 MiB"),
                    ("a", 2): oom("cannot allocate 200 MiB")}
    assert generator.generate_batch(args) == 86
    assert len(state.calls) == 2
    records = [json.loads(line) for line in (args.output / "batch.jsonl").read_text().splitlines()]
    failures = [row for row in records if row["event"] == "variant_finished"]
    assert all(row["outcome"] == "oom" and row["error_signature"] == "OutOfMemoryError"
               for row in failures)


def test_batch_deadline_prevents_next_variant(batch):
    args, state = batch
    args.deadline = time.time() + 1000

    def expire(image, seed):
        args.deadline = time.time() - 1

    state.before_run = expire
    assert generator.generate_batch(args) == 87
    assert len(state.calls) == state.loads == 1
    assert (args.output / "a/trellis-s1/raw.npz").is_file()
    records = [json.loads(line) for line in (args.output / "batch.jsonl").read_text().splitlines()]
    assert len([row for row in records if row.get("reason") == "deadline"]) == 3


def test_batch_already_expired_deadline_never_loads_model(batch):
    args, state = batch
    args.deadline = time.time() - 1
    assert generator.generate_batch(args) == 87
    assert not state.calls and state.loads == 0
    records = [json.loads(line) for line in (args.output / "batch.jsonl").read_text().splitlines()]
    assert len(records) == 4
    assert all(row["event"] == "variant_skipped" and row["reason"] == "deadline" for row in records)


def test_batch_trellis2_rejects_1024_without_loading(batch):
    args, state = batch
    args.model, args.pipeline_type = "trellis2", "1024_cascade"
    with pytest.raises(ValueError, match="requires --pipeline-type 512"):
        generator.generate_batch(args)
    assert not state.calls and state.loads == 0


def test_single_generation_still_exports_raw_and_live_metadata(batch, monkeypatch):
    args, state = batch
    args.image = args.images_dir / "a.png"
    args.seed = 1
    monkeypatch.setattr(generator, "_nvidia_smi", lambda: {})
    generator.generate(args)
    assert state.loads == 1
    meta = json.loads((args.output / "meta.json").read_text())
    assert meta["outcome"] == "ok"
    assert meta["mesh"] == {"vertices": 3, "faces": 1}
    assert (args.output / "raw.npz").is_file()
    assert meta["input"]["filename"] == "a.png"
