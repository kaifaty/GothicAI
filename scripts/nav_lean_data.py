#!/usr/bin/env python3
"""Convert frozen NAVDEMO episodes to lossless standard XZ without cache tensors.

Run on Ryzen through gq. The source corpus is read-only; each completed output
episode contains samples.bin.xz and episode.json. Results are numerical evidence,
not a replacement training manifest.
"""
from __future__ import annotations

import argparse
from concurrent.futures import as_completed, ThreadPoolExecutor
import gzip
import hashlib
import io
import json
import lzma
from pathlib import Path
import sys
import threading
import time

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nav_bc_data as data


CHUNK_RECORDS = 128
EMIT_LOCK = threading.Lock()


def emit(event, **values):
    with EMIT_LOCK:
        print(json.dumps(dict(event=event, **values), allow_nan=False), flush=True)


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def raw_digest(path):
    checksum, size = hashlib.sha256(), 0
    with lzma.open(path, "rb") as stream:
        while block := stream.read(8 * 1024**2):
            checksum.update(block)
            size += len(block)
    return checksum.hexdigest(), size


def convert_episode(entry, destination):
    """Publish one verified episode directory, or verify and reuse its output."""
    directory, destination = Path(entry["directory"]), Path(destination)
    source = directory / "samples.bin.gz"
    metadata_file = directory / "episode.json"
    metadata = json.loads(metadata_file.read_text(encoding="utf-8"))
    if metadata.get("decisions") != entry["decisions"]:
        raise ValueError(f"Source metadata decision count differs: {entry['id']}")
    started = time.perf_counter()
    emit("nav_convert_episode", id=entry["id"], stage="source_checksum", source=str(source))
    source_checksum = digest(source)
    if source_checksum != entry["samples_sha256"]:
        raise ValueError(f"Source sample checksum differs: {entry['id']}")

    samples = destination / "samples.bin.xz"
    if destination.exists():
        saved = json.loads((destination / "episode.json").read_text(encoding="utf-8"))
        storage = saved["navdemo_storage"]
        if (storage["source_samples_sha256"] != source_checksum
                or saved["nav_bc_stats"]["decisions"] != entry["decisions"]
                or {key: value for key, value in saved.items()
                    if key not in ("navdemo_storage", "nav_bc_stats")} != metadata
                or digest(samples) != storage["samples_sha256"]):
            raise ValueError(f"Existing converted episode differs: {entry['id']}")
        raw_checksum, raw_bytes = raw_digest(samples)
        if raw_checksum != storage["record_bytes_sha256"]:
            raise ValueError(f"Existing converted record checksum differs: {entry['id']}")
        samples_checksum = storage["samples_sha256"]
        stats = saved["nav_bc_stats"]
        reused = True
    else:
        # Publish the pair together. A killed writer cannot leave a complete
        # samples file next to absent or unrelated training statistics.
        temporary = destination.with_name(destination.name + ".tmp")
        temporary.mkdir(parents=True, exist_ok=False)
        temporary_samples = temporary / "samples.bin.xz"
        temporary_metadata = temporary / "episode.json"
        counts = {head: np.zeros(count, np.int64)
                  for head, count in zip(data.BC3_HEADS, data.BC3_CLASSES)}
        decisions, changes, detour_steps, previous = 0, 0, 0, None
        raw_checksum = hashlib.sha256()
        raw_bytes = 0
        teacher_version = metadata.get("teacher_version", entry.get("scenario", {}).get("teacher_version"))
        try:
            emit("nav_convert_episode", id=entry["id"], stage="compress")
            with gzip.open(source, "rb") as incoming, lzma.open(temporary_samples, "wb", preset=6) as outgoing:
                header = incoming.read(data.nav_demos.HEADER.size)
                # Validate the original header, then retain its exact bytes.
                version = data.nav_demos._read_header(io.BytesIO(header))
                dtype = data.nav_demos.RECORD_DTYPES[version]
                outgoing.write(header)
                raw_checksum.update(header)
                raw_bytes += len(header)
                while block := incoming.read(CHUNK_RECORDS * dtype.itemsize):
                    if len(block) % dtype.itemsize:
                        raise ValueError(f"Truncated source record: {entry['id']}")
                    records = np.frombuffer(block, dtype=dtype)
                    outgoing.write(block)
                    raw_checksum.update(block)
                    raw_bytes += len(block)
                    labels = data.encode_bc3_labels(records["keys"])
                    for number, (head, count) in enumerate(zip(data.BC3_HEADS, data.BC3_CLASSES)):
                        counts[head] += np.bincount(labels[:, number], minlength=count)
                    changes += int(data.label_action_changes(labels).sum())
                    if previous is not None:
                        changes += int(np.any(labels[0] != previous))
                    previous = labels[-1].copy()
                    detour_steps += int(data.detour_targets(records, teacher_version)[1].sum())
                    decisions += len(records)
            if decisions != entry["decisions"]:
                raise ValueError(f"Source record count differs: {entry['id']}: {decisions}")
            raw_checksum = raw_checksum.hexdigest()
            emit("nav_convert_episode", id=entry["id"], stage="verify_xz")
            restored_checksum, restored_bytes = raw_digest(temporary_samples)
            if (restored_checksum, restored_bytes) != (raw_checksum, raw_bytes):
                raise ValueError(f"Converted record bytes differ: {entry['id']}")
            stats = dict(decisions=decisions, class_counts={head: count.tolist() for head, count in counts.items()},
                         action_changes=changes, detour_available_steps=detour_steps)
            samples_checksum = digest(temporary_samples)
            metadata["navdemo_storage"] = dict(source_samples_sha256=source_checksum,
                samples_sha256=samples_checksum, record_bytes_sha256=raw_checksum)
            metadata["nav_bc_stats"] = stats
            temporary_metadata.write_text(json.dumps(metadata, indent=2, allow_nan=False) + "\n", encoding="utf-8")
            temporary.rename(destination)
        finally:
            if temporary.exists():
                temporary_samples.unlink(missing_ok=True)
                temporary_metadata.unlink(missing_ok=True)
                temporary.rmdir()
        reused = False
    result = dict(id=entry["id"], family=entry["family"], decisions=stats["decisions"],
                  source=str(source), samples=str(samples), source_samples_bytes=source.stat().st_size,
                  samples_bytes=samples.stat().st_size, source_metadata_bytes=metadata_file.stat().st_size,
                  metadata_bytes=(destination / "episode.json").stat().st_size,
                  source_samples_sha256=source_checksum, record_bytes_sha256=raw_checksum,
                  record_bytes=raw_bytes, samples_sha256=samples_checksum, seconds=time.perf_counter() - started,
                  reused=reused)
    emit("nav_convert_episode_done", **result)
    return result


def convert(source, manifest_path, output, results_path, workers=6):
    source, output = Path(source).resolve(), Path(output).resolve()
    if source == output or source in output.parents or output in source.parents:
        raise ValueError("Output must be separate from the source corpus")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if (manifest.get("schema_version") != 2 or manifest.get("dataset") != "nav-demos-v5"
            or manifest.get("complete") is not True):
        raise ValueError("Conversion requires the complete compact v5 manifest")
    entries, identities, destinations = [], set(), set()
    for entry in manifest["episodes"]:
        relative = Path(entry["episode"])
        directory = (source / relative).resolve()
        destination = (output / relative).resolve()
        if (relative.is_absolute() or not directory.is_relative_to(source)
                or not destination.is_relative_to(output)):
            raise ValueError(f"Source episode is outside the corpus: {entry['id']}")
        if entry["id"] in identities or destination in destinations:
            raise ValueError(f"Duplicate source episode: {entry['id']}")
        identities.add(entry["id"])
        destinations.add(destination)
        entries.append(({**entry, "directory": directory}, destination))
    totals = manifest["totals"]
    if (len(entries) != totals["episodes"]
            or sum(entry["decisions"] for entry, _destination in entries) != totals["decisions"]):
        raise ValueError("Corpus totals differ from the frozen manifest")
    started = time.perf_counter()
    executor = ThreadPoolExecutor(max_workers=workers, thread_name_prefix="nav-xz")
    futures = {executor.submit(convert_episode, entry, destination): index
               for index, (entry, destination) in enumerate(entries)}
    results = [None] * len(entries)
    try:
        for future in as_completed(futures):
            results[futures[future]] = future.result()
    finally:
        executor.shutdown(wait=True, cancel_futures=True)
    summed = {key: sum(result[key] for result in results) for key in
              ("decisions", "source_samples_bytes", "samples_bytes", "source_metadata_bytes",
               "metadata_bytes", "record_bytes")}
    summed.update(episodes=len(results), reused_episodes=sum(result["reused"] for result in results),
                  seconds=time.perf_counter() - started,
                  source_bytes_per_decision=summed["source_samples_bytes"] / summed["decisions"],
                  samples_bytes_per_decision=summed["samples_bytes"] / summed["decisions"],
                  source_over_xz=summed["source_samples_bytes"] / summed["samples_bytes"],
                  records_and_metadata_ratio=(summed["source_samples_bytes"] + summed["source_metadata_bytes"])
                      / (summed["samples_bytes"] + summed["metadata_bytes"]))
    summary = dict(source=str(source), output=str(output), codec="XZ/LZMA2 preset 6", chunk_records=CHUNK_RECORDS,
                   workers=workers, totals=summed, episodes=results)
    results_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_results = results_path.with_suffix(results_path.suffix + ".tmp")
    temporary_results.write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary_results.replace(results_path)
    emit("nav_convert_complete", **summed)
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("convert")
    for name in ("source", "manifest", "output", "results"):
        command.add_argument("--" + name, type=Path, required=True)
    command.add_argument("--workers", type=int, default=6)
    args = parser.parse_args()
    if any(not getattr(args, name).is_absolute() for name in ("source", "manifest", "output", "results")):
        parser.error("All paths must be absolute")
    if args.workers < 1:
        parser.error("--workers must be positive")
    convert(args.source, args.manifest, args.output, args.results, args.workers)


if __name__ == "__main__":
    main()
