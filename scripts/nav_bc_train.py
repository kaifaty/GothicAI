#!/usr/bin/env python3
"""Train/evaluate navigation imitation on cached, pair-disjoint demonstrations.

Run on Ryzen under body-simulation.lock. No simulator or server is involved.
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

from nav_bc_data import CACHE_VERSION, HEADS, CLASSES, PREPROCESSING, VECTOR_SIZE, load_cached
from nav_bc_model import NavPolicy, export_weights


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def read_index(cache):
    index = json.loads((cache / "index.json").read_text())
    if (index.get("cache_version") != CACHE_VERSION or index.get("vector_size") != VECTOR_SIZE or
            index.get("preprocessing", {}).get("vector_order") != PREPROCESSING["vector_order"]):
        raise ValueError("rebuild cache for sensor-only inputs without previous_keys")
    train = {tuple(ep["pair"]) for ep in index["episodes"] if ep["split"] == "train"}
    valid = {tuple(ep["pair"]) for ep in index["episodes"] if ep["split"] != "train"}
    if train & valid:
        raise ValueError("pairs leaked between training and validation")
    return index


def load(cache, ep):
    return load_cached(cache / ep["file"])


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


def batch_tensors(episodes, start, length, device):
    stop = start + length
    arrays = {}
    for name in ("near", "far", "vector"):
        shape = (len(episodes), length, *episodes[0][name].shape[1:])
        a = np.zeros(shape, np.float32)
        for row, ep in enumerate(episodes):
            n = max(0, min(stop, len(ep["labels"])) - start)
            a[row, :n] = ep[name][start:start+n]
        arrays[name] = torch.from_numpy(a).to(device)
    labels = np.zeros((len(episodes), length, len(HEADS)), np.int64)
    mask = np.zeros((len(episodes), length), bool)
    for row, ep in enumerate(episodes):
        n = max(0, min(stop, len(ep["labels"])) - start)
        labels[row, :n] = ep["labels"][start:start+n]
        mask[row, :n] = True
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
    index = read_index(cache)
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
               "loss_head_weights": [2, 1, 2, 1, 1, 1]})
    for epoch in range(len(history), args.epochs):
        started = time.perf_counter()
        model.train()
        order = list(train_eps)
        random.Random(args.seed + epoch).shuffle(order)
        total_loss = total_steps = 0
        for offset in range(0, len(order), args.batch):
            group = [load(cache, meta) for meta in order[offset:offset+args.batch]]
            hidden = None
            for start in range(0, max(len(ep["labels"]) for ep in group), args.segment):
                features, labels, mask = batch_tensors(group, start, args.segment, device)
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
    model, state = checkpoint_model(out / "best.pt", "cpu")
    model.eval()
    export_weights(model, out / "policy.navbc", preprocessing={**index["preprocessing"], "eye_layout": index["eye_layout"]})
    reference = NumpyPolicy(out / "policy.navbc")
    entries = [e for e in index["episodes"] if e["split"] != "train"]
    # Prefer one uninterrupted episode >=1000, otherwise reset only at episode boundaries.
    entries.sort(key=lambda e: (e["steps"] < args.steps, e["id"]))
    n, max_logit_error, max_hidden_error = 0, 0., 0.
    checked, golden = [], {k: [] for k in ("near", "far", "vector", "hidden", "reset", *HEADS)}
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
            max_logit_error = max(max_logit_error, *(float(np.max(np.abs(logits[h][0, 0].numpy()-nl[h]))) for h in HEADS))
            max_hidden_error = max(max_hidden_error, float(np.max(np.abs(th[0, 0].numpy()-nh))))
            for key, value in a.items():
                golden[key].append(value)
            golden["hidden"].append(th[0, 0].numpy().copy())
            golden["reset"].append(j == 0)
            for head in HEADS:
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
        if command == "verify":
            p.add_argument("--steps", type=int, default=1000)
            p.add_argument("--iterations", type=int, default=1000)
    args = parser.parse_args()
    if not Path(args.cache).is_absolute() or not Path(args.output).is_absolute():
        parser.error("cache and output must be absolute paths")
    {"train": train, "evaluate": final_evaluate, "verify": verify, "plot": plot}[args.command](args)


if __name__ == "__main__":
    main()
