#!/usr/bin/env python3
"""Measure NAVDEMO storage and unchanged BC preparation on a bounded sample.

Run on Ryzen through gq. Outputs are numerical JSON/JSONL evidence; recordings
are read-only, and temporary copies/caches stay under --scratch and are removed.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
from contextlib import ExitStack
import faulthandler
import gzip
import hashlib
import importlib.util
import json
import lzma
import os
from pathlib import Path
import platform
import shutil
import sys
import threading
import time
from types import SimpleNamespace
from unittest.mock import patch
import zlib

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_bc_data as data


CHUNK_RECORDS = 32
PROBE_CHUNK_RECORDS = 128
TIMEOUT_SECONDS = 100 * 60


class Progress:
    def __init__(self, output):
        output.mkdir(parents=True, exist_ok=True)
        self.stream = (output / "events.jsonl").open("a", encoding="utf-8", buffering=1)
        self.stage, self.episode = "starting", None
        self.started = time.monotonic()
        self.stopped = threading.Event()
        self.lock = threading.Lock()
        self.thread = threading.Thread(target=self.heartbeat, daemon=True)

    def emit(self, event, **values):
        value = dict(event=event, elapsed_seconds=time.monotonic() - self.started,
                     episode=self.episode, stage=self.stage, **values)
        line = json.dumps(value, allow_nan=False, separators=(",", ":"))
        with self.lock:
            print(line, flush=True)
            self.stream.write(line + "\n")

    def begin(self, stage, episode=None):
        self.stage = stage
        if episode is not None:
            self.episode = episode
        self.emit("phase_begin")

    def heartbeat(self):
        while not self.stopped.wait(30):
            self.emit("heartbeat", timeout_seconds=TIMEOUT_SECONDS)

    def close(self):
        self.stopped.set()
        self.thread.join()
        self.stream.close()


class ProcessMemory:
    """Sample Linux process-tree RSS/PSS, separating loader children from parent."""
    def __init__(self):
        self.stopped = threading.Event()
        self.peak = dict(loader_rss_bytes=0, loader_pss_bytes=0,
                         process_tree_rss_bytes=0, process_tree_pss_bytes=0)
        self.samples = 0
        self.worker_pids = set()
        self.error = None
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        try:
            self.sample()
        except BaseException as error:
            self.error = error

    def sample(self):
        while not self.stopped.is_set():
            pending, children = [os.getpid()], set()
            while pending:
                pid = pending.pop()
                # Pool processes can be spawned by the executor's manager thread.
                for task in Path(f"/proc/{pid}/task").glob("*"):
                    try:
                        found = {int(value) for value in (task / "children").read_text().split()}
                    except (FileNotFoundError, ProcessLookupError):
                        continue
                    pending.extend(found - children)
                    children.update(found)
            current = dict.fromkeys(self.peak, 0)
            for pid in children | {os.getpid()}:
                try:
                    worker = b"multiprocessing.spawn" in Path(f"/proc/{pid}/cmdline").read_bytes()
                    fields = dict(line.split(":", 1) for line in
                                  Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
                                  if ":" in line)
                except (FileNotFoundError, ProcessLookupError):
                    continue
                for metric in ("rss", "pss"):
                    value = int(fields[metric.capitalize()].split()[0]) * 1024
                    current[f"process_tree_{metric}_bytes"] += value
                    if worker:
                        current[f"loader_{metric}_bytes"] += value
                        self.worker_pids.add(pid)
            for key, value in current.items():
                self.peak[key] = max(self.peak[key], value)
            self.samples += 1
            self.stopped.wait(.5)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *_args):
        self.stopped.set()
        self.thread.join()

    def result(self):
        if self.error is not None:
            raise RuntimeError("Process-tree memory sampling failed") from self.error
        if not self.samples:
            raise ValueError("No process-tree memory samples were collected")
        return dict(self.peak, samples=self.samples, sample_interval_seconds=.5,
                    loader_worker_pids=sorted(self.worker_pids),
                    loader_excludes_spawn_resource_tracker=True,
                    rss_sums_shared_pages_pss_apportions_them=True)


def training_entries(manifest_path, source, corpus):
    """Use the production validator with archived v4 paths rebased in memory."""
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if corpus == "v4":
        old_root = Path(manifest["root"])
        manifest["root"] = str(source)
        for entry in manifest["episodes"]:
            entry["episode"] = str(source / Path(entry["episode"]).relative_to(old_root))
        for value in manifest.get("sources", {}).values():
            old_path = Path(value["path"])
            if old_path.is_relative_to(old_root):
                value["path"] = str(source / old_path.relative_to(old_root))
    # load_training_manifest reads only read_text on this argument. A memory
    # reader preserves its validation and ordering without writing a manifest.
    reader = SimpleNamespace(read_text=lambda **_kwargs: json.dumps(manifest))
    return data.load_training_manifest(reader, source)


def leaves(values, prefix=""):
    if values.dtype.names:
        for name in values.dtype.names:
            child = values[name]
            field = f"{prefix}.{name}" if prefix else name
            if name == "channels" and prefix in ("memory_near", "memory_far"):
                names = data.nav_demos.CHANNELS_V3 if child.shape[-1] == 8 else data.nav_demos.CHANNELS
                for index, channel in enumerate(names):
                    yield f"{field}.{channel}", child[..., index]
            else:
                yield from leaves(child, field)
    else:
        yield prefix, values


def profile_recording(entry, scratch, progress):
    source = entry["directory"] / "samples.bin.gz"
    local, raw_path = scratch / "samples.bin.gz", scratch / "records.bin"
    timing = defaultdict(float)
    progress.begin("nas_compressed_read", entry["id"])
    started = time.perf_counter()
    compressed_bytes = 0
    with source.open("rb") as incoming, local.open("wb") as outgoing:
        while True:
            before = time.perf_counter()
            block = incoming.read(8 * 1024**2)
            timing["nas_read_seconds"] += time.perf_counter() - before
            if not block:
                break
            compressed_bytes += len(block)
            before = time.perf_counter()
            outgoing.write(block)
            timing["local_compressed_write_seconds"] += time.perf_counter() - before
    timing["nas_copy_wall_seconds"] = time.perf_counter() - started

    progress.begin("local_gzip_inflate")
    decisions = 0
    with gzip.open(local, "rb") as incoming, raw_path.open("wb") as outgoing:
        before = time.perf_counter()
        version = data.nav_demos._read_header(incoming)
        timing["inflate_seconds"] += time.perf_counter() - before
        dtype = data.nav_demos.RECORD_DTYPES[version]
        while True:
            before = time.perf_counter()
            block = incoming.read(CHUNK_RECORDS * dtype.itemsize)
            timing["inflate_seconds"] += time.perf_counter() - before
            if not block:
                break
            if len(block) % dtype.itemsize:
                raise ValueError(f"Truncated record: {entry['id']}")
            decisions += len(block) // dtype.itemsize
            before = time.perf_counter()
            outgoing.write(block)
            timing["local_raw_write_seconds"] += time.perf_counter() - before
    if decisions != entry["decisions"]:
        raise ValueError(f"Recording decision count differs: {entry['id']}: {decisions}")

    progress.begin("array_views_and_field_gzip1")
    compressors, fields = {}, {}
    familiar = dict(near_positive_decisions=0, far_positive_decisions=0,
                    policy_positive_decisions=0, near_positive_cells=0, far_positive_cells=0)
    with raw_path.open("rb") as stream:
        while block := stream.read(CHUNK_RECORDS * dtype.itemsize):
            before = time.perf_counter()
            records = np.frombuffer(block, dtype=dtype)
            timing["array_view_seconds"] += time.perf_counter() - before
            for name, values in leaves(records):
                payload = np.ascontiguousarray(values).tobytes()
                if name not in compressors:
                    compressors[name] = zlib.compressobj(1, wbits=31)
                    fields[name] = dict(raw_bytes=0, gzip1_bytes=0)
                fields[name]["raw_bytes"] += len(payload)
                fields[name]["gzip1_bytes"] += len(compressors[name].compress(payload))
            if version == 3:
                near = records["memory_near"]["channels"][..., 7] != 0
                far = records["memory_far"]["channels"][:, 50:100, :, 7] != 0
                near_any, far_any = near.any(axis=(1, 2)), far.any(axis=(1, 2))
                familiar["near_positive_decisions"] += int(near_any.sum())
                familiar["far_positive_decisions"] += int(far_any.sum())
                familiar["policy_positive_decisions"] += int((near_any | far_any).sum())
                familiar["near_positive_cells"] += int(near.sum())
                familiar["far_positive_cells"] += int(far.sum())
    for name, compressor in compressors.items():
        fields[name]["gzip1_bytes"] += len(compressor.flush())
        fields[name]["raw_bytes_per_decision"] = fields[name]["raw_bytes"] / decisions
        fields[name]["gzip1_bytes_per_decision"] = fields[name]["gzip1_bytes"] / decisions

    # Candidate payloads are measurements only, not a proposed disk format.
    # Every leaf's IEEE/integer bytes are retained, including debug fields.
    progress.begin("lossless_candidate_sizes")
    candidates = {name: dict(payload_bytes=0, seconds=0., blocks=0)
                  for name in ("field_byte_shuffle_zlib1", "field_byte_shuffle_zlib6",
                               "temporal_xor_field_byte_shuffle_zlib1", "temporal_xor_field_byte_shuffle_zlib6")}
    with raw_path.open("rb") as stream:
        while block := stream.read(CHUNK_RECORDS * dtype.itemsize):
            records = np.frombuffer(block, dtype=dtype)
            for temporal in (False, True):
                before = time.perf_counter()
                planes = []
                for _name, values in leaves(records):
                    raw = np.ascontiguousarray(values).view(np.uint8).reshape(len(records), -1)
                    if temporal:
                        delta = raw.copy()
                        delta[1:] = raw[1:] ^ raw[:-1]
                        raw = delta
                    planes.append(raw.reshape(-1, values.dtype.itemsize).T.tobytes())
                payload = b"".join(planes)
                transform_seconds = time.perf_counter() - before
                for level in (1, 6):
                    name = ("temporal_xor_" if temporal else "") + f"field_byte_shuffle_zlib{level}"
                    before = time.perf_counter()
                    size = len(zlib.compress(payload, level))
                    candidates[name]["payload_bytes"] += size
                    candidates[name]["seconds"] += transform_seconds + time.perf_counter() - before
                    candidates[name]["blocks"] += 1
    for candidate in candidates.values():
        candidate["payload_bytes_per_decision"] = candidate["payload_bytes"] / decisions
        candidate["source_bytes_over_candidate_payload"] = compressed_bytes / candidate["payload_bytes"]
    return dict(id=entry["id"], family=entry["family"], decisions=decisions,
                navdemo_version=version, source=str(source), source_compressed_bytes=compressed_bytes,
                source_compressed_bytes_per_decision=compressed_bytes / decisions,
                timing=dict(timing), fields=fields, familiar=familiar, candidates=candidates)


def probe_encode(records, transform):
    """Reorder exact scalar bits, optionally decorrelating memory neighbours."""
    if transform == "raw":
        fields = {name: dict(raw_bytes=values.nbytes, transformed_bytes=values.nbytes,
                             encode_seconds=0.) for name, values in leaves(records)}
        return records.tobytes(), fields
    working = records.copy() if transform.startswith("residual_spatial_") else records
    if transform.startswith("residual_spatial_"):
        for name in ("memory_near", "memory_far"):
            channels = working[name]["channels"]
            channels[..., 1] ^= channels[..., 0]
    parts, fields = [], {}
    for name, values in leaves(working):
        before = time.perf_counter()
        raw = np.ascontiguousarray(values).view(np.uint8).reshape(*values.shape, values.dtype.itemsize)
        if transform.startswith("residual_spatial_") and name.startswith("memory_"):
            spatial = raw.copy()
            spatial[:, :, 1:] ^= raw[:, :, :-1]
            raw = spatial
        planes = raw.reshape(-1, values.dtype.itemsize).T
        if transform.endswith("bit_shuffle"):
            bits = np.unpackbits(planes[..., None], axis=2).transpose(0, 2, 1)
            payload = np.packbits(bits, axis=2).tobytes()
        else:
            payload = planes.tobytes()
        parts.append(payload)
        fields[name] = dict(raw_bytes=values.nbytes, transformed_bytes=len(payload),
                            encode_seconds=time.perf_counter() - before)
    return b"".join(parts), fields


def probe_decode(payload, shape, dtype, transform):
    """Invert probe_encode using only the fixed record layout and block count."""
    if transform == "raw":
        return np.frombuffer(payload, dtype=dtype).reshape(shape)
    result = np.empty(shape, dtype=dtype)
    position = 0
    for name, values in leaves(result):
        scalar_bytes, count = values.dtype.itemsize, values.size
        if transform.endswith("bit_shuffle"):
            packed_count = (count + 7) // 8
            length = scalar_bytes * 8 * packed_count
            packed = np.frombuffer(payload, np.uint8, length, position).reshape(scalar_bytes, 8, packed_count)
            bits = np.unpackbits(packed, axis=2)[..., :count].transpose(0, 2, 1)
            planes = np.packbits(bits, axis=2)[..., 0]
        else:
            length = values.nbytes
            planes = np.frombuffer(payload, np.uint8, length, position).reshape(scalar_bytes, count)
        position += length
        raw = np.ascontiguousarray(planes.T).reshape(*values.shape, scalar_bytes)
        if transform.startswith("residual_spatial_") and name.startswith("memory_"):
            raw = np.bitwise_xor.accumulate(raw, axis=2)
        values[...] = raw.reshape(-1).view(values.dtype).reshape(values.shape)
    if position != len(payload):
        raise ValueError("Probe payload length differs from its record layout")
    if transform.startswith("residual_spatial_"):
        for name in ("memory_near", "memory_far"):
            channels = result[name]["channels"]
            channels[..., 1] ^= channels[..., 0]
    return result


def probe_recording(entry, scratch, progress):
    """Compare eight fixed lossless recipes; this does not write a new format."""
    import zstandard

    source = entry["directory"] / "samples.bin.gz"
    local = scratch / "samples.bin.gz"
    compressed_hash = hashlib.sha256()
    compressed_bytes, nas_read_seconds = 0, 0.
    progress.begin("probe.nas_compressed_read", entry["id"])
    with source.open("rb") as incoming, local.open("wb") as outgoing:
        while True:
            before = time.perf_counter()
            block = incoming.read(8 * 1024**2)
            nas_read_seconds += time.perf_counter() - before
            if not block:
                break
            compressed_hash.update(block)
            compressed_bytes += len(block)
            outgoing.write(block)
    if compressed_hash.hexdigest() != entry["samples_sha256"]:
        raise ValueError(f"Probe source checksum differs: {entry['id']}")

    transforms = ("raw", "field_byte_shuffle", "residual_spatial_byte_shuffle",
                  "residual_spatial_bit_shuffle")
    codecs = {"zstd9": (zstandard.ZstdCompressor(level=9).compress,
                        zstandard.ZstdDecompressor().decompress),
              "lzma6": (lambda value: lzma.compress(value, preset=6), lzma.decompress)}
    candidates = {f"{transform}_{codec}": dict(payload_bytes=0, encode_seconds=0.,
                  inverse_seconds=0., compress_seconds=0., decompress_seconds=0.,
                  roundtrip_blocks=0, roundtrip_record_bytes=0, fields={})
                  for transform in transforms for codec in codecs}
    restored_hashes = {name: hashlib.sha256() for name in candidates}
    original_hash = hashlib.sha256()
    decisions, raw_bytes, read_inflate_seconds = 0, 0, 0.
    version = data.nav_demos.episode_version(local)
    iterator = iter(data.nav_demos.iter_chunks(local, PROBE_CHUNK_RECORDS))
    while True:
        progress.begin("probe.local_read_inflate")
        before = time.perf_counter()
        try:
            records = next(iterator)
        except StopIteration:
            read_inflate_seconds += time.perf_counter() - before
            break
        read_inflate_seconds += time.perf_counter() - before
        original = records.tobytes()
        original_hash.update(original)
        decisions += len(records)
        raw_bytes += len(original)
        for transform in transforms:
            progress.begin("probe." + transform)
            before = time.perf_counter()
            payload, fields = probe_encode(records, transform)
            encode_seconds = time.perf_counter() - before
            before = time.perf_counter()
            restored = probe_decode(payload, records.shape, records.dtype, transform).tobytes()
            inverse_seconds = time.perf_counter() - before
            if restored != original:
                raise ValueError(f"Probe transform lost record bits: {entry['id']}: {transform}")
            for codec, (compress, decompress) in codecs.items():
                name = f"{transform}_{codec}"
                progress.begin("probe." + name)
                before = time.perf_counter()
                compressed = compress(payload)
                compress_seconds = time.perf_counter() - before
                before = time.perf_counter()
                expanded = decompress(compressed)
                decompress_seconds = time.perf_counter() - before
                if expanded != payload:
                    raise ValueError(f"Probe codec lost payload bits: {entry['id']}: {name}")
                candidate = candidates[name]
                candidate["payload_bytes"] += len(compressed)
                candidate["encode_seconds"] += encode_seconds
                candidate["inverse_seconds"] += inverse_seconds
                candidate["compress_seconds"] += compress_seconds
                candidate["decompress_seconds"] += decompress_seconds
                candidate["roundtrip_blocks"] += 1
                candidate["roundtrip_record_bytes"] += len(restored)
                restored_hashes[name].update(restored)
                for field, values in fields.items():
                    total = candidate["fields"].setdefault(field, dict(raw_bytes=0, transformed_bytes=0,
                                                                      encode_seconds=0.))
                    for key, value in values.items():
                        total[key] += value
        progress.emit("probe_block_complete", decisions=decisions)
    if decisions != entry["decisions"]:
        raise ValueError(f"Probe decision count differs: {entry['id']}: {decisions}")
    for name, candidate in candidates.items():
        candidate["payload_bytes_per_decision"] = candidate["payload_bytes"] / decisions
        candidate["source_bytes_over_candidate_payload"] = compressed_bytes / candidate["payload_bytes"]
        candidate["record_bytes_sha256"] = restored_hashes[name].hexdigest()
        if candidate["record_bytes_sha256"] != original_hash.hexdigest():
            raise ValueError(f"Probe record checksum differs: {entry['id']}: {name}")
    return dict(id=entry["id"], family=entry["family"], decisions=decisions, navdemo_version=version,
                source=str(source), source_compressed_bytes=compressed_bytes,
                source_compressed_sha256=compressed_hash.hexdigest(), record_bytes=raw_bytes,
                record_bytes_sha256=original_hash.hexdigest(), nas_read_seconds=nas_read_seconds,
                local_read_inflate_seconds=read_inflate_seconds, candidates=candidates,
                zstandard_version=zstandard.__version__)


def measured_prepare(args, manifest, entries, cache, progress):
    """Instrument the existing prepare without changing its inputs or math."""
    episode_results = []
    current = None
    counts = defaultdict(int)
    known = {str(entry["directory"] / "samples.bin.gz"): entry for entry in entries}
    started = time.perf_counter()

    def finish_episode():
        if current is not None:
            current["wall_seconds"] = time.perf_counter() - current.pop("started")
            current["timing"] = dict(current["timing"])
            current["calls"] = dict(current["calls"])
            episode_results.append(current.copy())
            progress.emit("prepare_episode_result", result=current)

    original_version = data.nav_demos.episode_version

    def episode_version(path):
        nonlocal current
        finish_episode()
        entry = known[str(path)]
        current = dict(id=entry["id"], family=entry["family"], decisions=entry["decisions"],
                       started=time.perf_counter(), timing=defaultdict(float), calls=defaultdict(int))
        counts.clear()
        progress.begin("prepare.episode_header", entry["id"])
        progress.emit("episode_begin", source=str(path))
        return timed("header", original_version)(path)

    def timed(name, function):
        def wrapped(*positional, **kwargs):
            previous = progress.stage
            progress.stage = "prepare." + name
            if current is not None and not current["calls"][name]:
                progress.emit("prepare_phase_begin", phase=name)
            before = time.perf_counter()
            try:
                return function(*positional, **kwargs)
            finally:
                if current is not None:
                    current["timing"][name] += time.perf_counter() - before
                    current["calls"][name] += 1
                progress.stage = previous
        return wrapped

    original_chunks = data.nav_demos.iter_chunks

    def iter_chunks(path, chunk_records):
        iterator = iter(original_chunks(path, chunk_records))
        while True:
            counts["chunks"] += 1
            progress.stage = f"prepare.read_inflate.chunk{counts['chunks']}"
            before = time.perf_counter()
            try:
                records = next(iterator)
            except StopIteration:
                if current is not None:
                    current["timing"]["read_inflate"] += time.perf_counter() - before
                return
            if current is not None:
                current["timing"]["read_inflate"] += time.perf_counter() - before
                current["calls"]["read_inflate"] += 1
            progress.stage = f"prepare.tensor_assembly.chunk{counts['chunks']}"
            yield records

    original_open = Path.open

    def path_open(path, *positional, **kwargs):
        if str(path) in known:
            progress.stage = "prepare.samples_open"
        return original_open(path, *positional, **kwargs)

    progress.begin("prepare", entries[0]["id"])
    with ExitStack() as stack:
        stack.enter_context(patch.object(data, "load_training_manifest", return_value=(manifest, entries)))
        stack.enter_context(patch.object(data.nav_demos, "episode_version", episode_version))
        stack.enter_context(patch.object(data.nav_demos, "iter_chunks", iter_chunks))
        stack.enter_context(patch.object(Path, "open", path_open))
        for owner, name in ((data, "encode_features"), (data, "_pack"),
                            (data, "future_stuck"), (data, "maneuver_weights"),
                            (data.np, "savez_compressed"), (data.np, "concatenate"),
                            (data.hashlib, "file_digest")):
            stack.enter_context(patch.object(owner, name, timed(name, getattr(owner, name))))
        index = data.prepare_manifest(args.source, cache, args.manifest, CHUNK_RECORDS,
                                      "hd-familiar" if args.corpus == "v5" else "hd")
    finish_episode()
    return dict(seconds=time.perf_counter() - started, episodes=episode_results,
                cache_bytes=sum(path.stat().st_size for path in cache.rglob("*") if path.is_file()),
                splits=index["splits"], timings_are_nested=True)


def check_direct_training(args, manifest, entries, scratch, progress):
    """Compare frozen prepare with direct loading and identical BC updates."""
    import copy
    import torch
    import nav_bc_train as train
    import nav_lean_data as convert

    gpu = args.mode == "loader"
    if gpu and not torch.cuda.is_available():
        raise RuntimeError("The loader benchmark requires the admitted Ryzen GPU")
    families = sorted({entry["family"] for entry in entries})
    selected_ids = set()
    for number, family in enumerate(families):
        count = 50 // len(families) + (number < 50 % len(families))
        selected_ids.update(entry["id"] for entry in
                            [value for value in entries if value["family"] == family][:count])
    selected = [entry for entry in entries if entry["id"] in selected_ids]
    if len(selected) != 50:
        raise ValueError(f"Expected 50 parity episodes, found {len(selected)}")
    variant = "hd-familiar" if args.corpus == "v5" else "hd"
    new_entries = copy.deepcopy(selected)
    for old, new in zip(selected, new_entries):
        new["directory"] = args.converted_source / old["directory"].relative_to(args.source)
        if args.corpus == "v4":
            progress.begin("parity.convert_v4", old["id"])
            convert.convert_episode(old, new["directory"])

    spec = importlib.util.spec_from_file_location("nav_bc_data_before_lean", args.reference_script)
    reference = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reference)
    cache = scratch / "reference-cache"
    progress.begin("parity.reference_prepare")
    started = time.perf_counter()
    with patch.object(reference, "load_training_manifest", return_value=(manifest, selected)):
        old_index = reference.prepare_manifest(args.source, cache, args.manifest,
                                               CHUNK_RECORDS, variant)
    prepare_seconds = time.perf_counter() - started
    progress.begin("parity.direct_index")
    started = time.perf_counter()
    with patch.object(data, "load_training_manifest", return_value=(manifest, new_entries)):
        new_index = data.training_index(args.converted_source, args.manifest, variant)
    index_seconds = time.perf_counter() - started
    for key in ("class_counts", "action_change", "action_change_weight", "auxiliary", "splits",
                "preprocessing", "eye_layout", "heads", "classes", "near_shape", "far_shape"):
        if old_index[key] != new_index[key]:
            raise ValueError(f"Direct index differs from frozen prepare: {key}")
    if [entry["id"] for entry in old_index["episodes"]] != [entry["id"] for entry in new_index["episodes"]]:
        raise ValueError("Direct index changed episode order")

    old_by_id = {entry["id"]: entry for entry in old_index["episodes"]}
    batches = train.training_batches(new_index["episodes"], 8 if gpu else 4, 17)
    with train.BatchLoader(args.converted_source) as loader:
        # Exercise the same multiprocessing/order/packing path used by training.
        checked = check_loaded_tensors(loader.loaded_batches(batches, packed=True),
                                       old_by_id, reference, cache, args.corpus,
                                       progress, 64 if gpu else CHUNK_RECORDS)
    checked_arrays, checked_tensors, decisions, familiar_decisions = checked
    if args.corpus == "v5" and not familiar_decisions:
        raise ValueError("V5 parity did not exercise a nonzero familiar input")

    torch.use_deterministic_algorithms(True)
    if gpu:
        torch.backends.cudnn.benchmark = False
        torch.backends.cudnn.deterministic = True
    histories, memory = {}, {}
    for label, index in (("old", old_index), ("direct", new_index)):
        progress.begin("parity.bc_epochs_" + label)
        destination = args.output / f"epoch-{label}"
        destination.mkdir()
        options = SimpleNamespace(cache=str(cache) if label == "old" else None,
            source=str(args.converted_source) if label == "direct" else None,
            manifest=str(args.manifest), input_variant=variant, output=str(destination),
            threads=6 if gpu else 1, device="cuda" if gpu else "cpu", epochs=2 if gpu else 1,
            batch=8 if gpu else 4, segment=64 if gpu else 32, lr=.0003,
            seed=17, resume=False, init=None)
        with ProcessMemory() as usage:
            train.train_bc3(options, index)
        memory[label] = usage.result()
        histories[label] = json.loads((destination / "history.json").read_text())
        progress.emit("training_complete", path=label, memory=memory[label], history=histories[label])
    old_state = torch.load(args.output / "epoch-old/last.pt", map_location="cpu", weights_only=False)
    new_state = torch.load(args.output / "epoch-direct/last.pt", map_location="cpu", weights_only=False)

    def equal_state(left, right):
        if isinstance(left, torch.Tensor):
            return left.dtype == right.dtype and left.shape == right.shape and torch.equal(left, right)
        if isinstance(left, dict):
            return left.keys() == right.keys() and all(equal_state(left[key], right[key]) for key in left)
        if isinstance(left, (tuple, list)):
            return len(left) == len(right) and all(equal_state(a, b) for a, b in zip(left, right))
        return left == right

    losses = {label: [row["loss"] for row in history] for label, history in histories.items()}
    if (any(any(left[key] != right[key] for key in ("loss", "losses", "decisions"))
            for left, right in zip(histories["old"], histories["direct"]))
            or not all(equal_state(old_state[key], new_state[key]) for key in ("model", "optimizer"))):
        raise ValueError(f"Same-seed BC epochs differ: {losses}")
    seconds = {label: [row["seconds"] for row in history] for label, history in histories.items()}
    mean = {label: sum(values) / len(values) for label, values in seconds.items()}
    totals = dict(old=prepare_seconds + 20 * mean["old"], direct=index_seconds + 20 * mean["direct"])
    if not memory["direct"]["loader_pss_bytes"] or len(memory["direct"]["loader_worker_pids"]) != 4:
        raise ValueError("Loader memory sampling did not observe all four workers")
    return dict(mode=args.mode, corpus=args.corpus, episodes=len(selected), decisions=decisions,
                episode_ids=[entry["id"] for entry in selected],
                packed_arrays_bitwise_equal=checked_arrays, training_tensors_bitwise_equal=checked_tensors,
                familiar_nonzero_decisions=familiar_decisions, reference_prepare_seconds=prepare_seconds,
                direct_index_seconds=index_seconds, separate_prepare_required=False, seed=17,
                threads=options.threads, device=options.device, epochs=options.epochs,
                batch=options.batch, segment=options.segment, lr=options.lr,
                losses=losses, model_and_optimizer_tensors_equal=True, epoch_seconds=seconds,
                mean_epoch_seconds=mean, projected_20_epoch_seconds=totals,
                xz_epoch_slowdown_percent=100 * (mean["direct"] / mean["old"] - 1),
                xz_epoch_within_20_percent=mean["direct"] <= 1.2 * mean["old"],
                projected_20_epochs_not_slower=totals["direct"] <= totals["old"],
                memory=memory, cuda=torch.version.cuda,
                gpu=torch.cuda.get_device_name() if gpu else None,
                npz_medium="local NVMe scratch", xz_medium="NAS",
                torch=torch.__version__, reference_script_sha256=hashlib.sha256(args.reference_script.read_bytes()).hexdigest())


def check_loaded_tensors(groups, old_by_id, reference, cache, corpus, progress, segment):
    """Check every ordered worker result before unpacking it for training."""
    checked_arrays, checked_tensors, decisions, familiar_decisions = 0, 0, 0, 0
    for entries, group in groups:
        for new, actual in zip(entries, group):
            old = old_by_id[new["id"]]
            progress.begin("parity.episode_tensors", old["id"])
            expected = reference.load_cached(cache / old["file"], packed=True)
            if set(expected) != set(actual):
                raise ValueError(f"Direct packed array keys differ: {old['id']}")
            for name, value in expected.items():
                other = actual[name]
                if value.dtype != other.dtype or value.shape != other.shape or value.tobytes() != other.tobytes():
                    raise ValueError(f"Direct packed array differs: {old['id']}: {name}")
                checked_arrays += 1
            steps = len(expected["labels"])
            for start in range(0, steps, segment):
                left = reference.unpack_cached(expected, start, start + segment + 1)
                right = data.unpack_cached(actual, start, start + segment + 1)
                for name, value in left.items():
                    other = right[name]
                    if value.dtype != other.dtype or value.shape != other.shape or value.tobytes() != other.tobytes():
                        raise ValueError(f"Direct training tensor differs: {old['id']}: {name}")
                    checked_tensors += 1
            if corpus == "v5":
                familiar_decisions += int((expected["near_familiar"].any(axis=(1, 2, 3)) |
                                           expected["far_familiar"].any(axis=(1, 2, 3))).sum())
            decisions += steps
            del expected
            progress.emit("parity_episode_complete", decisions=steps)
    return checked_arrays, checked_tensors, decisions, familiar_decisions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("profile", "probe", "replay", "parity", "loader"))
    parser.add_argument("--corpus", choices=("v4", "v5"), required=True)
    for name in ("source", "manifest", "output", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--converted-source", type=Path)
    parser.add_argument("--reference-script", type=Path)
    args = parser.parse_args()
    if any(not getattr(args, name).is_absolute() for name in ("source", "manifest", "output", "scratch")):
        parser.error("All paths must be absolute")
    if args.mode == "replay" and args.corpus != "v4":
        parser.error("Replay uses the historical v4 [200:300] training slice")
    if args.mode in ("parity", "loader") and any(path is None or not path.is_absolute()
                                     for path in (args.converted_source, args.reference_script)):
        parser.error("Parity requires absolute --converted-source and --reference-script")
    faulthandler.enable()
    faulthandler.dump_traceback_later(TIMEOUT_SECONDS, exit=True)
    progress = Progress(args.output)
    progress.thread.start()
    scratch = args.scratch / f"nav-data-bench-{args.mode}-{args.corpus}"
    scratch_created = False
    try:
        progress.begin("load_training_manifest")
        manifest, entries = training_entries(args.manifest, args.source, args.corpus)
        scratch.mkdir(parents=True, exist_ok=False)
        scratch_created = True
        progress.emit("population", corpus=args.corpus, eligible_episodes=len(entries),
                      eligible_decisions=sum(entry["decisions"] for entry in entries))
        if args.mode in ("profile", "probe"):
            selected = []
            for family in sorted({entry["family"] for entry in entries}):
                candidates = [entry for entry in entries if entry["family"] == family]
                selected.append(min(candidates, key=lambda entry: (
                    not entry["scenario"].get("local", entry.get("local", False)), entry["id"])))
            results = []
            pending = list(selected)
            while pending:
                entry = pending.pop(0)
                episode_scratch = scratch / entry["id"]
                episode_scratch.mkdir()
                try:
                    if args.mode == "profile":
                        result = profile_recording(entry, episode_scratch, progress)
                        result["prepare"] = measured_prepare(args, manifest, [entry], episode_scratch / "cache", progress)
                    else:
                        result = probe_recording(entry, episode_scratch, progress)
                    results.append(result)
                    progress.emit(f"{args.mode}_episode_result", result=result)
                finally:
                    shutil.rmtree(episode_scratch)
                if (args.mode == "profile" and args.corpus == "v5" and not pending
                        and not any(item["familiar"]["policy_positive_decisions"] for item in results)):
                    measured = {item["id"] for item in results}
                    remaining = sorted((item for item in entries if item["id"] not in measured
                        and item["scenario"].get("local", item.get("local", False))), key=lambda item: item["id"])
                    if remaining:
                        pending.append(remaining[0])
            summary = dict(mode=args.mode, corpus=args.corpus,
                           chunk_records=PROBE_CHUNK_RECORDS if args.mode == "probe" else CHUNK_RECORDS,
                           episodes=results, candidate_sizes_exclude_container_headers=True,
                           nas_reads_may_use_warm_cache=True)
            if args.mode == "profile":
                summary.update(standalone_field_gzip_streams_are_not_additive_to_source=True,
                               prepare_runs_after_source_copy_and_may_use_warm_nas_cache=True)
            else:
                total_source = sum(result["source_compressed_bytes"] for result in results)
                total_decisions = sum(result["decisions"] for result in results)
                totals = dict(episodes=len(results), decisions=total_decisions,
                              source_compressed_bytes=total_source,
                              source_compressed_bytes_per_decision=total_source / total_decisions,
                              candidates={})
                for name in results[0]["candidates"]:
                    candidate = {key: sum(result["candidates"][name][key] for result in results)
                                 for key in ("payload_bytes", "encode_seconds", "inverse_seconds",
                                             "compress_seconds", "decompress_seconds",
                                             "roundtrip_blocks", "roundtrip_record_bytes")}
                    candidate["payload_bytes_per_decision"] = candidate["payload_bytes"] / total_decisions
                    candidate["source_bytes_over_candidate_payload"] = total_source / candidate["payload_bytes"]
                    totals["candidates"][name] = candidate
                summary["totals"] = totals
        elif args.mode == "replay":
            selected = entries[200:300]
            if len(selected) != 100:
                raise ValueError(f"Expected 100 replay episodes, found {len(selected)}")
            progress.emit("replay_selection", start=200, stop=300,
                          episode_ids=[entry["id"] for entry in selected])
            summary = dict(mode=args.mode, corpus=args.corpus, start=200, stop=300,
                           prepare=measured_prepare(args, manifest, selected, scratch / "cache", progress))
        else:
            summary = check_direct_training(args, manifest, entries, scratch, progress)
        summary["environment"] = dict(python=platform.python_version(), numpy=np.__version__,
                                      zlib=zlib.ZLIB_RUNTIME_VERSION, platform=platform.platform())
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")
        progress.begin("complete")
        progress.emit("completed", summary=str(args.output / "summary.json"))
    except BaseException as error:
        progress.emit("failed", error=repr(error))
        raise
    finally:
        if scratch_created and scratch.exists():
            shutil.rmtree(scratch)
        faulthandler.cancel_dump_traceback_later()
        progress.close()


if __name__ == "__main__":
    main()
