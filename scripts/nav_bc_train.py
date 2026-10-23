#!/usr/bin/env python3
"""Train/evaluate navigation imitation on cached, pair-disjoint demonstrations.

Run on Ryzen through gq (batch class). No simulator or server is involved.
Example: python scripts/nav_bc_train.py train --cache /abs/cache --output /abs/run
Torch is imported only by this training/validation driver, never by the numpy reader.
"""
from __future__ import annotations

import argparse
import json
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
                         VECTOR_SIZE, load_cached, unpack_cached)
from nav_bc_model import NavPolicy, export_weights


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def read_index(cache):
    index = json.loads((cache / "index.json").read_text())
    familiar = index.get("cache_version") == BC4_CACHE_VERSION
    hd = index.get("cache_version") in (HD_CACHE_VERSION, BC4_CACHE_VERSION)
    preprocessing = BC4_PREPROCESSING if familiar else HD_PREPROCESSING if hd else PREPROCESSING
    if hd and not familiar and index.get("navigation_memory_version") == 4:
        preprocessing = {**preprocessing, "navigation_memory_version": 4}
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


def aggregate_training_index(cache, extra_caches, balance_v4_cache):
    """Combine prepared HD train caches without copying maps or reopening corpora."""
    caches = [cache, *map(Path, extra_caches)]
    indices = [read_index(path) for path in caches]
    reference = indices[0]
    if any(index["cache_version"] != HD_CACHE_VERSION for index in indices):
        raise ValueError("DAgger aggregation requires HD caches without familiar")
    for index in indices:
        if any(index[name] != reference[name] for name in ("eye_layout", "heads", "classes", "near_shape", "far_shape")):
            raise ValueError("aggregate caches have incompatible policy inputs or action heads")
        if any(entry["split"] != "train" for entry in index["episodes"]):
            raise ValueError("aggregate caches must contain only frozen training episodes")
    index = {**reference, "episodes": [], "training_caches": [str(path) for path in caches]}
    index["navigation_memory_version"] = max(part["navigation_memory_version"] for part in indices)
    index["navdemo_version"] = max(part["navdemo_version"] for part in indices)
    if index["navigation_memory_version"] == 4:
        index["preprocessing"] = {**HD_PREPROCESSING, "navigation_memory_version": 4}
    for number, (path, part) in enumerate(zip(caches, indices)):
        for entry in part["episodes"]:
            index["episodes"].append({**entry, "id": f"cache-{number}-{entry['id']}",
                                      "family": "follow" if entry["family"] == "follow_below_gate" else entry["family"],
                                      "file": str(path / entry["file"])})
    index["class_counts"] = {"train": {head: np.sum([
        part["class_counts"]["train"][head] for part in indices], axis=0).tolist() for head in BC3_HEADS}}
    decisions = sum(part["action_change"]["N"] for part in indices)
    changes = sum(part["action_change"]["n_I"] for part in indices)
    index["action_change_weight"] = float(np.clip(decisions / changes, 1, 10)) if changes else 1.
    index["action_change"] = {**reference["action_change"], "N": decisions, "n_I": changes}
    index["auxiliary"] = {**reference["auxiliary"], "detour": {**reference["auxiliary"]["detour"],
        "available_steps": sum(part["auxiliary"]["detour"]["available_steps"] for part in indices)}}
    if balance_v4_cache:
        baseline = read_index(Path(balance_v4_cache))
        if (baseline.get("navigation_memory_version") != 3 or baseline.get("navdemo_version") != 2
                or any(entry.get("source_kind", "teacher") != "teacher" or entry["split"] != "train"
                       for entry in baseline["episodes"])):
            raise ValueError("v4 family balance reference must contain only frozen teacher train episodes")
        weights, balance = family_balance_weights(index["episodes"], baseline["episodes"])
        for entry in index["episodes"]:
            entry["family_weight"] = weights[entry["family"]]
        index["family_balance"] = {**balance, "v4_cache": str(balance_v4_cache)}
    return index


def family_balance_weights(entries, v4_entries):
    """Keep object/follow's effective decision shares at their v4 floors."""
    counts, reference = {}, {}
    for entries_, target in ((entries, counts), (v4_entries, reference)):
        for entry in entries_:
            family = "follow" if entry["family"] == "follow_below_gate" else entry["family"]
            target[family] = target.get(family, 0.) + entry.get("effective_steps", entry["steps"])
    floors = {family: reference.get(family, 0.) / sum(reference.values()) for family in ("object", "follow")}
    if any(floors[family] and not counts.get(family) for family in floors):
        raise ValueError("aggregate is missing an object/follow family present in v4")
    active = set()
    total = sum(counts.values())
    while True:
        missing = {family for family in floors if counts.get(family, 0.) < floors[family] * total - 1e-8}
        if missing <= active:
            break
        active |= missing
        total = sum(value for family, value in counts.items() if family not in active) / (1 - sum(floors[f] for f in active))
    weighted = {family: floors[family] * total if family in active else value for family, value in counts.items()}
    weights = {family: weighted[family] / value for family, value in counts.items()}
    return weights, dict(method="per-decision loss weights; teacher-controlled DAgger decisions count 0.5",
        minimum_shares=floors, v4_decisions=reference, effective_decisions=counts, weights=weights,
        weighted_decisions=weighted, weighted_shares={family: value / total for family, value in weighted.items()})


def load(cache, ep, *, packed=False):
    return load_cached(cache / ep["file"], packed=packed)


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


def epoch_family_balance(batches, cache, segment, seed, v4_decisions, dagger_weights):
    """Balance exactly the decisions retained by this epoch's deterministic cap."""
    rng = np.random.default_rng(seed)
    counts = {}
    for entries in batches:
        lengths = np.asarray([entry["steps"] for entry in entries])
        dagger_rows = [entry.get("source_kind") == "dagger" for entry in entries]
        for entry, dagger in zip(entries, dagger_rows):
            if dagger and entry["file"] not in dagger_weights:
                with np.load(cache / entry["file"], allow_pickle=False) as source:
                    dagger_weights[entry["file"]] = source["sample_weight"]
        for start in range(0, int(lengths.max()), segment):
            mask = np.arange(segment)[None] < np.clip(lengths - start, 0, segment)[:, None]
            cap_dagger_steps(mask, dagger_rows, rng)
            if not mask.any():
                break
            for row, (entry, dagger) in enumerate(zip(entries, dagger_rows)):
                if dagger:
                    weights = dagger_weights[entry["file"]][start:start + segment]
                    count = float(weights[mask[row, :len(weights)]].sum())
                else:
                    count = float(mask[row].sum())
                if count:
                    family = "follow" if entry["family"] == "follow_below_gate" else entry["family"]
                    counts[family] = counts.get(family, 0.) + count
    return family_balance_weights(
        [dict(family=family, steps=count) for family, count in counts.items()],
        [dict(family=family, steps=count) for family, count in v4_decisions.items()])


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
    cache, out = Path(args.cache), Path(args.output)
    out.mkdir(parents=True, exist_ok=True)
    index = (aggregate_training_index(cache, args.aggregate_cache, args.balance_v4_cache)
             if args.aggregate_cache or args.balance_v4_cache else read_index(cache))
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
    for epoch in range(len(history), args.epochs):
        started = time.perf_counter()
        model.train()
        batches = training_batches(train_eps, args.batch, args.seed + epoch)
        sample_rng = np.random.default_rng(args.seed + epoch)
        total_loss = total_steps = 0
        for entries in batches:
            group = [load(cache, meta) for meta in entries]
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
                  "seconds": time.perf_counter()-started, "validation": validation, "score": score}
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
    """Initialize HD from BC3, preserve HD for DAgger, or expand it for BC4."""
    from nav_bc_numpy import NumpyPolicy
    path = Path(path)
    if path.suffix != ".pt":
        raise ValueError("HD initialization requires a last.pt training checkpoint")
    source = torch.load(path, map_location="cpu", weights_only=False)
    config = source["config"]
    familiar = model.format_version == 4
    source_version = config.get("format_version")
    if (source_version not in ((3,) if familiar else (2, 3)) or config.get("auxiliary") is not True
            or config.get("vector_size") != model.vector_size or config.get("hidden_size") != model.hidden_size):
        raise ValueError("initialization requires a compatible BC3/HD checkpoint with auxiliary heads")
    # Check the paired portable policy's sensor layout; the new map encoding is
    # deliberately different, while every transferred vector field stays exact.
    portable = NumpyPolicy(path.parent / "policy.navbc")
    preprocessing = portable.header.get("preprocessing", {})
    expected_preprocessing = HD_PREPROCESSING if source_version == 3 else PREPROCESSING
    if any(preprocessing.get(name) != value for name, value in expected_preprocessing.items()):
        raise ValueError("source checkpoint preprocessing differs from the compatible input layout")
    if not np.array_equal(preprocessing.get("eye_layout"), index["eye_layout"]):
        raise ValueError("HD source eye layout differs from the demonstrations")
    state, transferred, reset, expanded = model.state_dict(), [], [], []
    for name in state:
        if not familiar and source_version == 2 and name.startswith(("near_encoder.", "far_encoder.")):
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
                      "all HD tensors preserved" if source_version == 3 else
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
    for name in ("sample_weight", "maneuver_valid"):
        values = np.zeros((len(episodes), length), np.float32)
        for row, episode in enumerate(episodes):
            count = max(0, min(length, len(episode["labels"]) - start))
            values[row, :count] = episode[name][start:start + count] if name in episode else 1.
        result[name] = torch.from_numpy(values).to(device)
    return result


def bc3_loss(logits, auxiliary, labels, mask, targets, class_weights, change_weight):
    change_weights = torch.where(targets["action_change"] != 0, change_weight, 1.)
    sample_weights = targets.get("sample_weight", torch.ones_like(change_weights))
    terms = {}
    for column, head in enumerate(BC3_HEADS):
        valid = mask
        if head == "maneuver" and "maneuver_valid" in targets:
            valid = valid & targets["maneuver_valid"].bool()
        if not valid.any():
            terms[head] = logits[head].sum() * 0
            continue
        values, truth = logits[head][valid], labels[:, :, column][valid]
        ce = nn.functional.cross_entropy(values, truth, weight=class_weights[column], reduction="none")
        weights = change_weights[valid] * sample_weights[valid]
        if head == "maneuver":
            probability = values.log_softmax(-1).gather(-1, truth[:, None]).squeeze(-1).exp()
            ce = ce * (1 - probability).square()
            weights = weights * targets["maneuver_weight"][valid]
        terms[head] = (ce * weights).sum() / change_weights[valid].sum()
    for head in ("stuck", "next_near", "detour"):
        valid = mask & targets[head + "_valid"].bool()
        if valid.any():
            if head == "stuck":
                errors = nn.functional.binary_cross_entropy_with_logits(
                    auxiliary[head][valid], targets[head][valid], reduction="none")
            else:
                errors = nn.functional.smooth_l1_loss(auxiliary[head][valid], targets[head][valid], reduction="none")
                errors = errors.reshape(len(errors), -1).mean(1)
            terms[head] = (errors * sample_weights[valid]).mean()
        else:
            terms[head] = auxiliary[head].sum() * 0
    action = sum(scale * terms[head] for head, scale in zip(BC3_HEADS, (2, 1, 2, 1, 1))) / 7
    return action + .1 * terms["stuck"] + .1 * terms["next_near"] + .5 * terms["detour"], terms


def train_bc3(args, index):
    """Fixed-epoch BC on manifest train only; selection requires closed-loop validation."""
    cache, out = Path(args.cache), Path(args.output)
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
        family_balance=index.get("family_balance"), training_caches=index.get("training_caches", [str(cache)]),
        max_dagger_fraction=.5, dagger_teacher_control_weight=.5,
        selection="fixed final epoch; closed-loop validation pending"))
    dagger_weights = {}
    for epoch in range(len(history), args.epochs):
        started = time.perf_counter()
        model.train()
        total, steps, losses = 0., 0, {}
        source_steps = dict(teacher=0, dagger=0)
        weighted_family_steps = {}
        epoch_batches = training_batches(index["episodes"], args.batch, args.seed + epoch)
        epoch_weights = None
        if "family_balance" in index:
            epoch_weights, _ = epoch_family_balance(epoch_batches, cache, args.segment, args.seed + epoch,
                index["family_balance"]["v4_decisions"], dagger_weights)
            for entry in index["episodes"]:
                entry["family_weight"] = epoch_weights.get(entry["family"], 1.)
        sample_rng = np.random.default_rng(args.seed + epoch)
        for entries in epoch_batches:
            group = [load(cache, entry, packed=hd) for entry in entries]
            dagger_rows = [entry.get("source_kind") == "dagger" for entry in entries]
            hidden = None
            for start in range(0, max(len(ep["labels"]) for ep in group), args.segment):
                # Keep HD episodes packed and expand this truncated-BPTT segment
                # plus one future decision for the unchanged next-near target.
                segment_group = ([unpack_cached(ep, start, start + args.segment + 1) for ep in group]
                                 if hd else group)
                segment_start = 0 if hd else start
                # Legacy batching uses six labels; BC3 explicitly supplies its five heads.
                features, labels, mask = batch_tensors(segment_group, segment_start, args.segment, device,
                                                      dagger_rows, sample_rng, heads=BC3_HEADS)
                if not mask.any():
                    break
                targets = bc3_targets(segment_group, segment_start, args.segment, device)
                for row, entry in enumerate(entries):
                    targets["sample_weight"][row] *= entry.get("family_weight", 1.)
                    count = int(mask[row].sum())
                    source_steps["dagger" if dagger_rows[row] else "teacher"] += count
                    if "family" in entry:
                        family = entry["family"]
                        weighted_family_steps[family] = weighted_family_steps.get(family, 0.) + float(
                            targets["sample_weight"][row][mask[row]].sum())
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
                      source_decisions=source_steps,
                      weighted_family_decisions=weighted_family_steps,
                      weighted_family_shares={family: count / sum(weighted_family_steps.values())
                                              for family, count in weighted_family_steps.items()},
                      losses={name: value / steps for name, value in losses.items()})
        if epoch_weights is not None:
            record["family_weights"] = epoch_weights
            for family, minimum in index["family_balance"]["minimum_shares"].items():
                if record["weighted_family_shares"].get(family, 0.) + 1e-6 < minimum:
                    raise ValueError(f"actual epoch {family} decision share is below the v4 floor")
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
    export_weights(model, out / "policy.navbc", preprocessing=state.get(
        "preprocessing", {**index["preprocessing"], "eye_layout": index["eye_layout"]}))
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
        p.add_argument("--cache", required=True)
        p.add_argument("--output", required=True)
        p.add_argument("--threads", type=int, default=6)
        p.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
        if command == "train":
            p.add_argument("--epochs", type=int, default=20)
            p.add_argument("--batch", type=int, default=8)
            p.add_argument("--segment", type=int, default=64)
            p.add_argument("--lr", type=float, default=3e-4)
            p.add_argument("--seed", type=int, default=17)
            p.add_argument("--resume", action="store_true")
            p.add_argument("--init", help="compatible .navbc for BC3; BC3/HD last.pt for HD; HD last.pt for BC4")
            p.add_argument("--aggregate-cache", action="append", default=[],
                           help="additional prepared HD train cache; repeat for frozen corpora/DAgger rounds")
            p.add_argument("--balance-v4-cache", help="prepared v4 train cache supplying object/follow decision-share floors")
        if command == "verify":
            p.add_argument("--steps", type=int, default=1000)
            p.add_argument("--iterations", type=int, default=1000)
    args = parser.parse_args()
    if any(not Path(value).is_absolute() for value in
           (args.cache, args.output, *getattr(args, "aggregate_cache", []),
            *([args.balance_v4_cache] if getattr(args, "balance_v4_cache", None) else []))):
        parser.error("cache and output must be absolute paths")
    {"train": train, "evaluate": final_evaluate, "verify": verify, "plot": plot}[args.command](args)


if __name__ == "__main__":
    main()
