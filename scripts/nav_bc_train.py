#!/usr/bin/env python3
"""Train navigation imitation on direct recordings or prepared tensor caches.

Run on Ryzen through gq (batch class). No simulator or server is involved.
Example: python scripts/nav_bc_train.py train --cache /abs/cache --output /abs/run
Direct: python scripts/nav_bc_train.py train --source /abs/recordings \
    --manifest /abs/manifest.json --input-variant hd-familiar --output /abs/run
Optional prefetch: --loader-workers 4 --loader-prefetch 1 (default: synchronous).
Torch is imported only by this training/validation driver, never by the numpy reader.
"""
from __future__ import annotations

import argparse
from collections import deque
from concurrent.futures import ProcessPoolExecutor
import json
import multiprocessing
from pathlib import Path
import random
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np
import torch
from torch import nn

from nav_bc_data import (CACHE_VERSION, BC3_CACHE_VERSION, HD_CACHE_VERSION, BC4_CACHE_VERSION, HEADS, CLASSES, BC3_HEADS,
                         BC3_CLASSES, PREPROCESSING, HD_PREPROCESSING, BC4_PREPROCESSING,
                         HD_NEAR_SHAPE, HD_FAR_SHAPE, BC4_NEAR_SHAPE, BC4_FAR_SHAPE,
                         VECTOR_SIZE, encode_episode, load_cached, training_index, unpack_cached)
from nav_bc_model import NavPolicy, export_weights
import nav_demos


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def read_index(cache):
    index = json.loads((cache / "index.json").read_text())
    familiar = index.get("cache_version") == BC4_CACHE_VERSION
    hd = index.get("cache_version") in (HD_CACHE_VERSION, BC4_CACHE_VERSION)
    preprocessing = BC4_PREPROCESSING if familiar else HD_PREPROCESSING if hd else PREPROCESSING
    if (index.get("cache_version") not in (CACHE_VERSION, BC3_CACHE_VERSION, HD_CACHE_VERSION, BC4_CACHE_VERSION)
            or index.get("vector_size") != VECTOR_SIZE or index.get("preprocessing") != preprocessing):
        raise ValueError("rebuild cache for NAVDEMO v2 carrot inputs without previous_keys")
    near_shape, far_shape = (BC4_NEAR_SHAPE, BC4_FAR_SHAPE) if familiar else (HD_NEAR_SHAPE, HD_FAR_SHAPE)
    if hd and (index.get("near_shape") != list(near_shape) or index.get("far_shape") != list(far_shape)):
        raise ValueError("rebuild HD cache for native near cells and the forward far-memory window")
    if familiar and (index.get("navdemo_version") != 3 or index.get("navigation_memory_version") != 4):
        raise ValueError("rebuild BC4 cache for NAVDEMO v3 memory-v4 recordings")
    train = {tuple(ep["pair"]) for ep in index["episodes"] if ep["split"] == "train"}
    valid = {tuple(ep["pair"]) for ep in index["episodes"] if ep["split"] != "train"}
    if train & valid:
        raise ValueError("pairs leaked between training and validation")
    return index


def load(cache, ep, *, packed=False):
    if ep.get("storage") == "navdemo":
        samples = cache / ep["file"]
        arrays = encode_episode(samples, nav_demos.ray_layout(samples), ep["teacher_version"], ep["input_variant"])
        if len(arrays["labels"]) != ep["steps"]:
            raise ValueError(f"record count differs from training index: {samples}")
        return arrays if packed else unpack_cached(arrays)
    return load_cached(cache / ep["file"], packed=packed)


def _loader_worker_init():
    # Workers encode numpy arrays; they never run a model or consume training RNG.
    torch.set_num_threads(1)


def _load_packed(cache, entry):
    return load(cache, entry, packed=True)


class BatchLoader:
    """Decode/encode direct episodes in worker processes, preserving batch order.

    Only packed arrays cross process boundaries. The current batch and at most
    ``prefetch`` following batches are retained; no episode survives across epochs.
    Spawn remains safe after the training process has initialized CUDA.
    """
    def __init__(self, cache, workers=0, prefetch=1):
        if not 0 <= workers <= 8 or not 1 <= prefetch <= 3:
            raise ValueError("loader requires 0-8 workers and 1-3 prefetched batches")
        self.cache = Path(cache)
        self.workers, self.prefetch = workers, prefetch
        self.executor = None
        self.batch_wait_seconds = []

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        if self.executor is not None:
            self.executor.shutdown(wait=True, cancel_futures=True)

    def wait_stats(self):
        """Parent batch loading/waiting time, excluding model work."""
        waits = self.batch_wait_seconds
        return dict(load_wait_seconds=sum(waits),
                    load_wait_first_batch_seconds=waits[0] if waits else 0.,
                    load_wait_after_first_seconds=sum(waits[1:]),
                    load_wait_max_batch_seconds=max(waits, default=0.),
                    load_wait_per_batch_seconds=waits.copy())

    def loaded_batches(self, batches, *, packed=False):
        self.batch_wait_seconds = []
        if not self.workers or not any(entry.get("storage") == "navdemo" for batch in batches for entry in batch):
            for entries in batches:
                started = time.perf_counter()
                group = [load(self.cache, entry, packed=packed) for entry in entries]
                self.batch_wait_seconds.append(time.perf_counter() - started)
                yield entries, group
                del group
            return
        if self.executor is None:
            self.executor = ProcessPoolExecutor(max_workers=self.workers,
                mp_context=multiprocessing.get_context("spawn"), initializer=_loader_worker_init)
        remaining = iter(batches)
        pending = deque()

        def submit():
            entries = next(remaining, None)
            if entries is not None:
                pending.append((entries, [self.executor.submit(_load_packed, self.cache, entry)
                                           for entry in entries]))

        # The current and configured following packed batches bound the memory.
        for _ in range(1 + self.prefetch):
            submit()
        while pending:
            entries, futures = pending.popleft()
            started = time.perf_counter()
            group = [future.result() for future in futures]
            self.batch_wait_seconds.append(time.perf_counter() - started)
            yield entries, group if packed else [unpack_cached(episode) for episode in group]
            del group, futures
            submit()


def metrics(truth, prediction):
    result = {}
    for j, (head, count) in enumerate(zip(HEADS, CLASSES)):
        cm = np.bincount(truth[:, j] * count + prediction[:, j], minlength=count**2).reshape(count, count)
        support = cm.sum(1)
        recall = np.divide(cm.diagonal(), support, out=np.zeros(count), where=support > 0)
        denom = cm.sum(0) + support
        f1 = np.divide(2 * cm.diagonal(), denom, out=np.zeros(count), where=denom > 0)
        result[head] = {"accuracy": float(cm.trace() / max(1, cm.sum())),
                        "balanced_accuracy": float(recall[support > 0].mean()) if support.any() else 0.,
                        "macro_f1": float(f1.mean()), "class_f1": f1.tolist(),
                        "support": support.tolist(), "confusion": cm.tolist()}
    return result


def training_batches(entries, batch_size, seed):
    """Keep teacher episodes in every mixed batch; never over half DAgger rows."""
    if batch_size < 1:
        raise ValueError("batch size must be positive")
    rng = random.Random(seed)
    teacher = [entry for entry in entries if entry.get("source_kind", "teacher") == "teacher"]
    dagger = [entry for entry in entries if entry.get("source_kind") == "dagger"]
    rng.shuffle(teacher)
    rng.shuffle(dagger)
    if not teacher:
        raise ValueError("training requires successful teacher episodes")
    if not dagger:
        return [teacher[offset:offset + batch_size] for offset in range(0, len(teacher), batch_size)]
    if batch_size < 2:
        raise ValueError("DAgger requires batch size at least two")
    dagger_slots = batch_size // 2
    teacher_slots = batch_size - dagger_slots
    count = max((len(teacher) + teacher_slots - 1) // teacher_slots,
                (len(dagger) + dagger_slots - 1) // dagger_slots)
    return [[teacher[(number * teacher_slots + slot) % len(teacher)] for slot in range(teacher_slots)] +
            dagger[number * dagger_slots:(number + 1) * dagger_slots]
            for number in range(count)]


def cap_dagger_steps(mask, dagger_rows, rng):
    """Cap valid labelled DAgger steps, including unequal episode lengths/padding."""
    dagger_rows = np.asarray(dagger_rows, dtype=bool)
    teacher_steps = int(mask[~dagger_rows].sum())
    selected = np.argwhere(mask & dagger_rows[:, None])
    excess = len(selected) - teacher_steps
    if excess > 0:
        omitted = selected[rng.choice(len(selected), excess, replace=False)]
        mask[omitted[:, 0], omitted[:, 1]] = False


def batch_tensors(episodes, start, length, device, dagger_rows=None, rng=None, heads=HEADS):
    stop = start + length
    arrays = {}
    for name in ("near", "far", "vector"):
        shape = (len(episodes), length, *episodes[0][name].shape[1:])
        a = np.zeros(shape, np.float32)
        for row, ep in enumerate(episodes):
            n = max(0, min(stop, len(ep["labels"])) - start)
            a[row, :n] = ep[name][start:start+n]
        arrays[name] = torch.from_numpy(a).to(device)
    labels = np.zeros((len(episodes), length, len(heads)), np.int64)
    mask = np.zeros((len(episodes), length), bool)
    for row, ep in enumerate(episodes):
        n = max(0, min(stop, len(ep["labels"])) - start)
        labels[row, :n] = ep["labels"][start:start+n]
        mask[row, :n] = True
    if dagger_rows is not None:
        cap_dagger_steps(mask, dagger_rows, rng)
    return arrays, torch.from_numpy(labels).to(device), torch.from_numpy(mask).to(device)


@torch.inference_mode()
def predict_episode(model, ep, device, segment=128):
    hidden = None
    predictions = []
    for start in range(0, len(ep["labels"]), segment):
        tensors = {k: torch.from_numpy(ep[k][None, start:start+segment]).to(device)
                   for k in ("near", "far", "vector")}
        logits, hidden = model(**tensors, hidden=hidden)
        predictions.append(np.stack([logits[h][0].argmax(-1).cpu().numpy() for h in HEADS], 1))
    return np.concatenate(predictions)


def evaluate(model, cache, entries, device, details=False):
    model.eval()
    truth, predictions, baselines, regions, situations, changes = [], [], [], [], [], []
    for meta in entries:
        ep = load(cache, meta)
        truth.append(ep["labels"])
        predictions.append(predict_episode(model, ep, device))
        baselines.append(ep["baseline"])
        regions.extend([meta["region"]] * len(ep["labels"]))
        situations.append(ep["situations"])
        changes.append(ep["action_change"])
    y, p, road = map(np.concatenate, (truth, predictions, baselines))
    always = np.tile(np.array([2, 1, 1, 1, 0, 0]), (len(y), 1))
    result = {"steps": len(y), "model": metrics(y, p), "always_forward": metrics(y, always),
              "road_forward": metrics(y, road)}
    change_mask = np.concatenate(changes).astype(bool)
    result["action_change"] = {
        "steps": int(change_mask.sum()),
        "all_heads_accuracy": float((y[change_mask] == p[change_mask]).all(axis=1).mean())
                              if change_mask.any() else None,
        "model": metrics(y[change_mask], p[change_mask]),
        "always_forward": metrics(y[change_mask], always[change_mask]),
        "road_forward": metrics(y[change_mask], road[change_mask]),
    }
    if details:
        r = np.array(regions)
        s = np.concatenate(situations)
        result["by_region"] = {name: {"steps": int((r == name).sum()), **metrics(y[r == name], p[r == name])}
                               for name in sorted(set(regions))}
        result["by_situation"] = {}
        for j, name in enumerate(("wall", "turn", "start")):
            mask = s[:, j].astype(bool)
            result["by_situation"][name] = {"steps": int(mask.sum()), **metrics(y[mask], p[mask])}
    return result


def checkpoint_model(path, device):
    state = torch.load(path, map_location="cpu", weights_only=False)
    model = NavPolicy(**state["config"]).to(device)
    model.load_state_dict(state["model"])
    return model, state


def train(args):
    source = getattr(args, "source", None)
    cache, out = Path(source or args.cache), Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    index = (training_index(cache, Path(args.manifest), args.input_variant)
             if source else read_index(cache))
    if index["cache_version"] in (BC3_CACHE_VERSION, HD_CACHE_VERSION, BC4_CACHE_VERSION):
        return train_bc3(args, index)
    train_eps = [e for e in index["episodes"] if e["split"] == "train"]
    val_eps = [e for e in index["episodes"] if e["split"] != "train"]
    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    torch.set_num_threads(args.threads)
    device = torch.device(args.device)
    config = {"vector_size": index["vector_size"], "hidden_size": 256}
    model = NavPolicy(**config).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    history, best = [], -1.
    if args.resume:
        state = torch.load(out / "last.pt", map_location="cpu", weights_only=False)
        model.load_state_dict(state["model"])
        optimizer.load_state_dict(state["optimizer"])
        history, best = state["history"], state["best"]
    counts = [np.array(index["class_counts"]["train"][h], dtype=np.int64) for h in HEADS]
    weights = []
    for count in counts:
        present = count > 0
        w = np.ones(len(count), np.float32)
        w[present] = np.clip(np.sqrt(count[present].sum() / (present.sum() * count[present])), .5, 3.)
        weights.append(torch.tensor(w, device=device))
    write_json(out / "training_config.json", {**vars(args), "architecture": model.architecture(),
               "class_counts": [c.tolist() for c in counts], "class_weights": [w.cpu().tolist() for w in weights],
               "loss_head_weights": [2, 1, 2, 1, 1, 1], "max_dagger_fraction": .5})
    with BatchLoader(cache, getattr(args, "loader_workers", 0), getattr(args, "loader_prefetch", 1)) as loader:
        for epoch in range(len(history), args.epochs):
            started = time.perf_counter()
            model.train()
            batches = training_batches(train_eps, args.batch, args.seed + epoch)
            sample_rng = np.random.default_rng(args.seed + epoch)
            total_loss = total_steps = 0
            for entries, group in loader.loaded_batches(batches):
                dagger_rows = [meta.get("source_kind") == "dagger" for meta in entries]
                hidden = None
                for start in range(0, max(len(ep["labels"]) for ep in group), args.segment):
                    features, labels, mask = batch_tensors(group, start, args.segment, device,
                                                           dagger_rows, sample_rng)
                    if not mask.any():
                        break
                    optimizer.zero_grad(set_to_none=True)
                    logits, hidden = model(**features, hidden=hidden)
                    hidden = hidden.detach()
                    loss = sum(scale * nn.functional.cross_entropy(logits[h][mask], labels[:, :, j][mask], weight=weights[j])
                               for j, (h, scale) in enumerate(zip(HEADS, (2, 1, 2, 1, 1, 1)))) / 8
                    loss.backward()
                    nn.utils.clip_grad_norm_(model.parameters(), 1.)
                    optimizer.step()
                    n = int(mask.sum())
                    total_loss += loss.item() * n
                    total_steps += n
            validation = evaluate(model, cache, val_eps, device)
            score = (validation["model"]["turn"]["macro_f1"] + validation["model"]["forward"]["balanced_accuracy"]) / 2
            record = {"epoch": epoch + 1, "loss": total_loss / total_steps,
                      "seconds": time.perf_counter()-started, "validation": validation, "score": score,
                      **loader.wait_stats()}
            history.append(record)
            is_best = score > best
            best = max(best, score)
            state = {"config": config, "model": model.state_dict(), "optimizer": optimizer.state_dict(),
                     "history": history, "best": best}
            torch.save(state, out / "last.pt")
            if is_best:
                torch.save(state, out / "best.pt")
            write_json(out / "history.json", history)
            print(json.dumps({k: v for k, v in record.items() if k != "validation"} |
                  {"turn_f1": validation["model"]["turn"]["macro_f1"],
                   "forward_accuracy": validation["model"]["forward"]["accuracy"],
                   "forward_balanced": validation["model"]["forward"]["balanced_accuracy"],
                   "action_change_steps": validation["action_change"]["steps"],
                   "action_change_all_heads": validation["action_change"]["all_heads_accuracy"],
                   "best": is_best}), flush=True)


def initialize_bc3(model, path, index):
    """Reuse compatible actor inputs/GRU; condition v1 maneuvers on exclusivity."""
    from nav_bc_numpy import NumpyPolicy
    source = NumpyPolicy(path)
    preprocessing = source.header.get("preprocessing", {})
    if source.vector_size != model.vector_size or source.hidden_size != model.hidden_size:
        raise ValueError("initial policy dimensions differ from the BC3 learner")
    for key, value in index["preprocessing"].items():
        if preprocessing.get(key) != value:
            raise ValueError(f"initial policy preprocessing differs: {key}")
    if not np.array_equal(preprocessing.get("eye_layout"), index["eye_layout"]):
        raise ValueError("initial policy eye layout differs from the demonstrations")
    state = model.state_dict()
    for name, values in source.weights.items():
        if name in state:
            state[name] = torch.from_numpy(values.copy())
        elif not name.startswith(("head_jump.", "head_climb.")):
            raise ValueError(f"unexpected initial policy tensor: {name}")
    if source.header["version"] == 1:
        for suffix in ("weight", "bias"):
            jump, climb = (torch.from_numpy(source.weights[f"head_{head}.{suffix}"].copy())
                           for head in ("jump", "climb"))
            state[f"head_maneuver.{suffix}"] = torch.stack(
                (jump[0] + climb[0], jump[1] + climb[0], jump[0] + climb[1]))
    model.load_state_dict(state)


def initialize_bc3_hd(model, path, index):
    """Initialize HD from BC3, or expand HD's first convolutions for BC4."""
    from nav_bc_numpy import NumpyPolicy
    path = Path(path)
    if path.suffix != ".pt":
        raise ValueError("HD initialization requires a last.pt training checkpoint")
    source = torch.load(path, map_location="cpu", weights_only=False)
    config = source["config"]
    familiar = model.format_version == 4
    source_version = 3 if familiar else 2
    if (config.get("format_version") != source_version or config.get("auxiliary") is not True
            or config.get("vector_size") != model.vector_size or config.get("hidden_size") != model.hidden_size):
        raise ValueError(f"initialization requires a compatible v{source_version} checkpoint with auxiliary heads")
    # Check the paired portable policy's sensor layout; the new map encoding is
    # deliberately different, while every transferred vector field stays exact.
    portable = NumpyPolicy(path.parent / "policy.navbc")
    preprocessing = portable.header.get("preprocessing", {})
    expected_preprocessing = HD_PREPROCESSING if familiar else PREPROCESSING
    if any(preprocessing.get(name) != value for name, value in expected_preprocessing.items()):
        raise ValueError("source checkpoint preprocessing differs from the compatible input layout")
    if not np.array_equal(preprocessing.get("eye_layout"), index["eye_layout"]):
        raise ValueError("HD source eye layout differs from the demonstrations")
    state, transferred, reset, expanded = model.state_dict(), [], [], []
    for name in state:
        if not familiar and name.startswith(("near_encoder.", "far_encoder.")):
            reset.append(name)
            continue
        if familiar and name in ("near_encoder.0.weight", "far_encoder.0.weight"):
            values = source["model"][name]
            if values.shape != (state[name].shape[0], 10, *state[name].shape[2:]):
                raise ValueError(f"HD source first convolution is incompatible: {name}")
            state[name].zero_()
            state[name][:, :10] = values
            expanded.append(name)
            continue
        if name not in source["model"] or source["model"][name].shape != state[name].shape:
            raise ValueError(f"HD source tensor is missing or incompatible: {name}")
        state[name] = source["model"][name]
        transferred.append(name)
    unexpected = set(source["model"]) - set(state)
    if unexpected:
        raise ValueError(f"unexpected BC3 source tensors: {sorted(unexpected)}")
    model.load_state_dict(state)
    return dict(source=str(path), transferred=transferred, reset=reset, expanded=expanded,
                maps=("HD tensors preserved; familiar input channel initialized to zero" if familiar else
                      "both CNNs initialized from scratch, including shape-compatible tensors"))


def bc3_targets(episodes, start, length, device):
    """Targets may use annotations; none are passed to the policy encoder."""
    result = {}
    for name in ("action_change", "maneuver_weight", "stuck", "stuck_valid",
                 "next_near_valid", "detour", "detour_valid"):
        shape = (len(episodes), length, *episodes[0][name].shape[1:])
        values = np.zeros(shape, np.float32)
        for row, episode in enumerate(episodes):
            count = max(0, min(length, len(episode["labels"]) - start))
            values[row, :count] = episode[name][start:start + count]
        result[name] = torch.from_numpy(values).to(device)
    target = np.zeros((len(episodes), length, 9, 32, 32), np.float32)
    for row, episode in enumerate(episodes):
        count = max(0, min(length, len(episode["labels"]) - start - 1))
        if count:
            target[row, :count] = episode.get("aux_near", episode["near"])[start + 1:start + count + 1]
    result["next_near"] = torch.from_numpy(target).to(device)
    return result


def bc3_loss(logits, auxiliary, labels, mask, targets, class_weights, change_weight):
    step_weights = torch.where(targets["action_change"] != 0, change_weight, 1.)[mask]
    terms = {}
    for column, head in enumerate(BC3_HEADS):
        values, truth = logits[head][mask], labels[:, :, column][mask]
        ce = nn.functional.cross_entropy(values, truth, weight=class_weights[column], reduction="none")
        weights = step_weights
        if head == "maneuver":
            probability = values.log_softmax(-1).gather(-1, truth[:, None]).squeeze(-1).exp()
            ce = ce * (1 - probability).square()
            weights = weights * targets["maneuver_weight"][mask]
        terms[head] = (ce * weights).sum() / step_weights.sum()
    for head in ("stuck", "next_near", "detour"):
        valid = mask & targets[head + "_valid"].bool()
        if valid.any():
            if head == "stuck":
                terms[head] = nn.functional.binary_cross_entropy_with_logits(
                    auxiliary[head][valid], targets[head][valid])
            else:
                terms[head] = nn.functional.smooth_l1_loss(auxiliary[head][valid], targets[head][valid])
        else:
            terms[head] = auxiliary[head].sum() * 0
    action = sum(scale * terms[head] for head, scale in zip(BC3_HEADS, (2, 1, 2, 1, 1))) / 7
    return action + .1 * terms["stuck"] + .1 * terms["next_near"] + .5 * terms["detour"], terms


def train_bc3(args, index):
    """Fixed-epoch BC on manifest train only; selection requires closed-loop validation."""
    cache, out = Path(getattr(args, "source", None) or args.cache), Path(args.output)
    if any(entry["split"] != "train" for entry in index["episodes"]):
        raise ValueError("BC3 cache must contain only manifest training episodes")
    random.seed(args.seed)
    np.random.seed(args.seed)
    torch.manual_seed(args.seed)
    torch.set_num_threads(args.threads)
    device = torch.device(args.device)
    familiar = index["cache_version"] == BC4_CACHE_VERSION
    hd = index["cache_version"] in (HD_CACHE_VERSION, BC4_CACHE_VERSION)
    config = dict(vector_size=index["vector_size"], hidden_size=256,
                  format_version=4 if familiar else 3 if hd else 2, auxiliary=True)
    model = NavPolicy(**config).to(device)
    initialization = None
    if args.init and not args.resume:
        if hd:
            initialization = initialize_bc3_hd(model, args.init, index)
        else:
            initialize_bc3(model, args.init, index)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, weight_decay=1e-4)
    history = []
    if args.resume:
        state = torch.load(out / "last.pt", map_location="cpu", weights_only=False)
        model.load_state_dict(state["model"])
        optimizer.load_state_dict(state["optimizer"])
        history = state["history"]
        if hd:
            initialization = json.loads((out / "training_config.json").read_text())["initialization"]
    counts = [np.asarray(index["class_counts"]["train"][head]) for head in BC3_HEADS]
    weights = []
    for count in counts:
        present = count > 0
        weight = np.ones(len(count), np.float32)
        weight[present] = np.clip(np.sqrt(count.sum() / (present.sum() * count[present])), .5, 3.)
        weights.append(torch.tensor(weight, device=device))
    change_weight = float(index["action_change_weight"])
    write_json(out / "training_config.json", dict(vars(args), architecture=model.architecture(),
        class_weights=[weight.cpu().tolist() for weight in weights],
        action_change_weight=change_weight, maneuver_window_weight=5, focal_gamma=2,
        auxiliary_weights=dict(stuck=.1, next_near=.1, detour=.5),
        auxiliary_targets=index["auxiliary"], initialization=initialization,
        selection="fixed final epoch; closed-loop validation pending"))
    with BatchLoader(cache, getattr(args, "loader_workers", 0), getattr(args, "loader_prefetch", 1)) as loader:
        for epoch in range(len(history), args.epochs):
            started = time.perf_counter()
            model.train()
            total, steps, losses = 0., 0, {}
            batches = training_batches(index["episodes"], args.batch, args.seed + epoch)
            for entries, group in loader.loaded_batches(batches, packed=hd):
                hidden = None
                for start in range(0, max(len(ep["labels"]) for ep in group), args.segment):
                    # Keep HD episodes packed and expand this truncated-BPTT segment
                    # plus one future decision for the unchanged next-near target.
                    segment_group = ([unpack_cached(ep, start, start + args.segment + 1) for ep in group]
                                     if hd else group)
                    segment_start = 0 if hd else start
                    # Legacy batching uses six labels; BC3 explicitly supplies its five heads.
                    features, labels, mask = batch_tensors(segment_group, segment_start, args.segment, device,
                                                          heads=BC3_HEADS)
                    targets = bc3_targets(segment_group, segment_start, args.segment, device)
                    optimizer.zero_grad(set_to_none=True)
                    logits, hidden, auxiliary = model(**features, hidden=hidden, return_auxiliary=True)
                    hidden = hidden.detach()
                    loss, terms = bc3_loss(logits, auxiliary, labels, mask, targets, weights, change_weight)
                    if not torch.isfinite(loss):
                        raise ValueError("nonfinite BC3 loss")
                    loss.backward()
                    nn.utils.clip_grad_norm_(model.parameters(), 1.)
                    optimizer.step()
                    count = int(mask.sum())
                    total += loss.item() * count
                    steps += count
                    for name, value in terms.items():
                        losses[name] = losses.get(name, 0.) + value.item() * count
                    del segment_group
                del group
            record = dict(epoch=epoch + 1, loss=total / steps, decisions=steps,
                          seconds=time.perf_counter() - started,
                          losses={name: value / steps for name, value in losses.items()},
                          **loader.wait_stats())
            history.append(record)
            torch.save(dict(config=config, model=model.state_dict(), optimizer=optimizer.state_dict(),
                            history=history, navigation_memory_version=index["navigation_memory_version"],
                            preprocessing={**index["preprocessing"], "eye_layout": index["eye_layout"]}), out / "last.pt")
            export_weights(model, out / "policy.navbc",
                           preprocessing={**index["preprocessing"], "eye_layout": index["eye_layout"]})
            write_json(out / "history.json", history)
            print(json.dumps(record), flush=True)


def timed_step(step, iterations):
    for _ in range(30):
        step()
    durations = []
    for _ in range(iterations):
        t = time.perf_counter_ns()
        step()
        durations.append((time.perf_counter_ns()-t)/1000)
    return {"median_us": float(np.median(durations)), "p95_us": float(np.percentile(durations, 95)),
            "mean_us": float(np.mean(durations)), "iterations": iterations}


@torch.inference_mode()
def verify(args):
    from nav_bc_numpy import NumpyPolicy
    torch.set_num_threads(1)
    torch.set_num_interop_threads(1)
    cache, out = Path(args.cache), Path(args.output)
    index = read_index(cache)
    bc3 = index["cache_version"] in (BC3_CACHE_VERSION, HD_CACHE_VERSION, BC4_CACHE_VERSION)
    model, state = checkpoint_model(out / ("last.pt" if bc3 else "best.pt"), "cpu")
    heads = tuple(model.head_sizes)
    model.eval()
    export_weights(model, out / "policy.navbc", preprocessing={**index["preprocessing"], "eye_layout": index["eye_layout"]})
    reference = NumpyPolicy(out / "policy.navbc")
    entries = [e for e in index["episodes"] if bc3 or e["split"] != "train"]
    # Prefer one uninterrupted episode >=1000, otherwise reset only at episode boundaries.
    entries.sort(key=lambda e: (e["steps"] < args.steps, e["id"]))
    n, max_logit_error, max_hidden_error = 0, 0., 0.
    checked, golden = [], {k: [] for k in ("near", "far", "vector", "hidden", "reset", *heads)}
    example = None
    for meta in entries:
        ep = load(cache, meta)
        th = nh = None
        length = min(len(ep["labels"]), args.steps-n)
        for j in range(length):
            a = {k: ep[k][j] for k in ("near", "far", "vector")}
            t = {k: torch.from_numpy(v[None, None]) for k, v in a.items()}
            logits, th = model(**t, hidden=th)
            nl, nh = reference.step(**a, hidden=nh)
            max_logit_error = max(max_logit_error, *(float(np.max(np.abs(logits[h][0, 0].numpy()-nl[h]))) for h in heads))
            max_hidden_error = max(max_hidden_error, float(np.max(np.abs(th[0, 0].numpy()-nh))))
            for key, value in a.items():
                golden[key].append(value)
            golden["hidden"].append(th[0, 0].numpy().copy())
            golden["reset"].append(j == 0)
            for head in heads:
                golden[head].append(logits[head][0, 0].numpy().copy())
            example = a
        checked.append({"id": meta["id"], "steps": length})
        n += length
        if n >= args.steps:
            break
    if n != args.steps or max(max_logit_error, max_hidden_error) > 1e-4:
        raise AssertionError(f"numpy parity failed: steps={n}, logits={max_logit_error}, hidden={max_hidden_error}")
    np.savez_compressed(out / "reference-1000.npz", **{k: np.stack(v) for k, v in golden.items()})
    nh = None
    def numpy_step():
        nonlocal nh
        _, nh = reference.step(**example, hidden=nh)
    th = None
    tensors = {k: torch.from_numpy(v[None, None]) for k, v in example.items()}
    def torch_step():
        nonlocal th
        _, th = model(**tensors, hidden=th)
    result = {"steps": n, "episodes": checked, "max_logit_error": max_logit_error,
              "max_hidden_error": max_hidden_error, "tolerance": 1e-4,
              "macs": model.mac_count(), "parameters": sum(p.numel() for p in model.parameters()),
              "numpy_cpu": timed_step(numpy_step, args.iterations),
              "torch_cpu": timed_step(torch_step, args.iterations),
              "threads": 1, "numpy": np.__version__, "torch": torch.__version__,
              "cpu": next((line.split(":",1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                            if line.startswith("model name")), "unknown"),
              "best_epoch": state["history"][-1]["epoch"]}
    write_json(out / "verification.json", result)
    print(json.dumps(result, indent=2))


def final_evaluate(args):
    torch.set_num_threads(args.threads)
    cache, out = Path(args.cache), Path(args.output)
    index = read_index(cache)
    if index["cache_version"] in (BC3_CACHE_VERSION, HD_CACHE_VERSION, BC4_CACHE_VERSION):
        raise ValueError("BC3 validation is unrecorded; use nav_rl.py drive --manifest in closed loop")
    model, _ = checkpoint_model(out / "best.pt", args.device)
    result = evaluate(model, cache, [e for e in index["episodes"] if e["split"] != "train"], args.device, True)
    result["acceptance"] = {"turn_macro_f1": all(result["model"]["turn"]["macro_f1"] > result[b]["turn"]["macro_f1"]
                            for b in ("always_forward", "road_forward")),
                            "forward_balanced_accuracy": all(result["model"]["forward"]["balanced_accuracy"] >
                            result[b]["forward"]["balanced_accuracy"] for b in ("always_forward", "road_forward")),
                            "forward_raw_accuracy": all(result["model"]["forward"]["accuracy"] > result[b]["forward"]["accuracy"]
                            for b in ("always_forward", "road_forward"))}
    write_json(out / "metrics.json", result)
    print(json.dumps(result, indent=2))


def plot(args):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    out = Path(args.output)
    history = json.loads((out / "history.json").read_text())
    epochs = [r["epoch"] for r in history]
    fig, axes = plt.subplots(1, 2, figsize=(11, 4), layout="constrained")
    axes[0].plot(epochs, [r["loss"] for r in history])
    axes[0].set(title="Training weighted cross entropy", xlabel="Epoch", ylabel="Loss")
    for head, metric, label in (("turn", "macro_f1", "Turn macro-F1"),
                                ("forward", "balanced_accuracy", "Forward balanced accuracy"),
                                ("forward", "accuracy", "Forward accuracy")):
        axes[1].plot(epochs, [r["validation"]["model"][head][metric] for r in history], label=label)
    axes[1].set(title="Held-out pairs", xlabel="Epoch", ylabel="Score", ylim=(0, 1))
    axes[1].legend()
    for axis in axes:
        axis.grid(alpha=.25)
    fig.savefig(out / "curves.png", dpi=150)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    for command in ("train", "evaluate", "verify", "plot"):
        p = sub.add_parser(command)
        if command == "train":
            recordings = p.add_mutually_exclusive_group(required=True)
            recordings.add_argument("--cache")
            recordings.add_argument("--source", help="read NAVDEMO recordings directly without a prepared cache")
            p.add_argument("--manifest", help="frozen training manifest for --source")
            p.add_argument("--input-variant", choices=("compact", "hd", "hd-familiar"), default="compact")
        else:
            p.add_argument("--cache", required=True)
        p.add_argument("--output", required=True)
        p.add_argument("--threads", type=int, default=6)
        p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
        if command == "train":
            p.add_argument("--epochs", type=int, default=20)
            p.add_argument("--batch", type=int, default=8)
            p.add_argument("--segment", type=int, default=64)
            p.add_argument("--loader-workers", type=int, choices=range(9), default=0,
                           help="optional direct-recording loader processes; default 0 loads synchronously")
            p.add_argument("--loader-prefetch", type=int, choices=(1, 2, 3), default=1,
                           help="following packed batches to prefetch")
            p.add_argument("--lr", type=float, default=3e-4)
            p.add_argument("--seed", type=int, default=17)
            p.add_argument("--resume", action="store_true")
            p.add_argument("--init", help="compatible .navbc for BC3; BC3 last.pt for HD; HD last.pt for BC4")
        if command == "verify":
            p.add_argument("--steps", type=int, default=1000)
            p.add_argument("--iterations", type=int, default=1000)
    args = parser.parse_args()
    source = getattr(args, "source", None)
    if not Path(source or args.cache).is_absolute() or not Path(args.output).is_absolute():
        parser.error("cache/source and output must be absolute paths")
    if source:
        if not args.manifest or not Path(args.manifest).is_absolute():
            parser.error("--source requires an absolute --manifest")
    elif getattr(args, "manifest", None):
        parser.error("--manifest requires --source")
    {"train": train, "evaluate": final_evaluate, "verify": verify, "plot": plot}[args.command](args)


if __name__ == "__main__":
    main()
