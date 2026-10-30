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
import json
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("profile", "replay"))
    parser.add_argument("--corpus", choices=("v4", "v5"), required=True)
    for name in ("source", "manifest", "output", "scratch"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if any(not getattr(args, name).is_absolute() for name in ("source", "manifest", "output", "scratch")):
        parser.error("All paths must be absolute")
    if args.mode == "replay" and args.corpus != "v4":
        parser.error("Replay uses the historical v4 [200:300] training slice")
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
        if args.mode == "profile":
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
                    result = profile_recording(entry, episode_scratch, progress)
                    result["prepare"] = measured_prepare(args, manifest, [entry], episode_scratch / "cache", progress)
                    results.append(result)
                    progress.emit("profile_episode_result", result=result)
                finally:
                    shutil.rmtree(episode_scratch)
                if (args.corpus == "v5" and not pending
                        and not any(item["familiar"]["policy_positive_decisions"] for item in results)):
                    measured = {item["id"] for item in results}
                    remaining = sorted((item for item in entries if item["id"] not in measured
                        and item["scenario"].get("local", item.get("local", False))), key=lambda item: item["id"])
                    if remaining:
                        pending.append(remaining[0])
            summary = dict(mode=args.mode, corpus=args.corpus, chunk_records=CHUNK_RECORDS,
                           episodes=results, standalone_field_gzip_streams_are_not_additive_to_source=True,
                           candidate_sizes_exclude_container_headers=True,
                           prepare_runs_after_source_copy_and_may_use_warm_nas_cache=True)
        else:
            selected = entries[200:300]
            if len(selected) != 100:
                raise ValueError(f"Expected 100 replay episodes, found {len(selected)}")
            progress.emit("replay_selection", start=200, stop=300,
                          episode_ids=[entry["id"] for entry in selected])
            summary = dict(mode=args.mode, corpus=args.corpus, start=200, stop=300,
                           prepare=measured_prepare(args, manifest, selected, scratch / "cache", progress))
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
