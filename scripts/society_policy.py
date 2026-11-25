#!/usr/bin/env python3
"""Record C++ society-teacher episodes, train a GRU actor, and export SmallNet weights."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import numpy as np


FORMAT = "gothic-society-net"
MODEL_VERSION = 1
TEACHER_FORMAT = "gothic-society-teacher"
ARCHITECTURE = "gru128-dot"
HIDDEN = 128
DAY_SECONDS = 86_400
ARRAYS = ("npc", "time", "own", "options", "mask", "choice")
SCORE_TIE_TOLERANCE = 1e-5


def layout(metadata):
    own = list(metadata["own_feature_names"])
    options = list(metadata["option_feature_names"])
    version, menu = metadata["feature_version"], metadata["menu_size"]
    if (type(version) is not int or version <= 0 or type(menu) is not int or menu < 1
            or any(not names or any(type(name) is not str or not name for name in names)
                   or len(names) != len(set(names)) for names in (own, options))):
        raise ValueError("invalid society feature layout")
    return dict(feature_version=version, menu_size=menu, own_feature_names=own, option_feature_names=options)


def record(worlds, config, output, *, seed, episodes, days, world_batch, threads):
    from society_sim import VecSociety

    if not 0 <= seed < 2**64 or episodes < 1 or seed + episodes > 2**64:
        raise ValueError("episode seeds must fit uint64")
    if min(days, world_batch, threads) < 1:
        raise ValueError("days, world batch size, and worker count must be positive")
    output.mkdir(parents=True)
    manifest = dict(format=TEACHER_FORMAT, version=1, episodes=[])
    seen = set()
    for world in worlds:
        for first in range(seed, seed + episodes, world_batch):
            seeds = list(range(first, min(first + world_batch, seed + episodes)))
            sim = VecSociety(str(world), str(config), seeds, threads=threads)
            meta = layout(dict(feature_version=sim.feature_version, menu_size=sim.stride,
                               own_feature_names=sim.own_feature_names,
                               option_feature_names=sim.option_feature_names))
            if manifest.setdefault("layout", meta) != meta:
                raise ValueError("all worlds must share the same feature layout")
            if any((sim.world_hash, value) in seen for value in seeds):
                raise ValueError("duplicate world and seed in dataset")
            chunks = [[] for _ in seeds]
            end = sim.start_time + days * DAY_SECONDS
            while True:
                batch = sim.advance(end)
                if len(batch["npc"]) == 0:
                    break
                times = np.asarray(sim.times, dtype=np.int64)[batch["world"]]
                choice = np.asarray(sim.act_teacher(), dtype=np.int64)
                for index in np.unique(batch["world"]):
                    selected = batch["world"] == index
                    chunks[index].append({key: batch[key][selected] for key in ("npc", "own", "options", "mask")}
                                         | {"time": times[selected], "choice": choice[selected]})
            for value, pieces in zip(seeds, chunks, strict=True):
                if not pieces:
                    raise ValueError(f"no teacher decisions for {world} with seed {value}")
                data = {key: np.concatenate([piece[key] for piece in pieces]) for key in ARRAYS}
                order = np.argsort(data["npc"], kind="stable")
                filename = f"episode-{len(manifest['episodes']):06d}.npz"
                np.savez_compressed(output / filename, **{key: data[key][order] for key in ARRAYS})
                manifest["episodes"].append(dict(world_hash=sim.world_hash, config_hash=sim.config_hash,
                                                 seed=value, rows=len(order)))
                seen.add((sim.world_hash, value))
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return dict(episodes=len(seen), decisions=sum(row["rows"] for row in manifest["episodes"]))


def dataset(paths):
    meta, files, groups = None, [], set()
    for path in paths:
        manifest = json.loads((path / "manifest.json").read_text(encoding="utf-8"))
        if (manifest.get("format"), manifest.get("version")) != (TEACHER_FORMAT, 1):
            raise ValueError(f"unsupported dataset format in {path}")
        current = layout(manifest["layout"])
        if meta is not None and current != meta:
            raise ValueError("datasets use different feature layouts")
        meta = current
        for index, episode in enumerate(manifest["episodes"]):
            key = episode["world_hash"], episode["seed"]
            if key in groups:
                raise ValueError("duplicate world and seed in dataset")
            groups.add(key)
            files.append((path / f"episode-{index:06d}.npz", episode["rows"]))
    if not files:
        raise ValueError("empty society dataset")
    return meta, files, groups


def read_episode(file, rows, meta):
    with np.load(file, allow_pickle=False) as data:
        result = {key: data[key] for key in ARRAYS}
    npc, time, own, options, mask, choice = (result[key] for key in ARRAYS)
    k = meta["menu_size"]
    if (rows < 1 or any(result[key].shape != (rows,) for key in ("npc", "time", "choice"))
            or own.shape != (rows, len(meta["own_feature_names"]))
            or options.shape != (rows, k, len(meta["option_feature_names"])) or mask.shape != (rows, k)
            or own.dtype != np.float32 or options.dtype != np.float32 or mask.dtype != np.uint8
            or any(result[key].dtype.kind not in "iu" for key in ("npc", "time", "choice"))
            or np.any(npc[1:] < npc[:-1]) or np.any((npc[1:] == npc[:-1]) & (time[1:] < time[:-1]))
            or not np.isfinite(own).all() or not np.isfinite(options).all()
            or not np.isin(mask, (0, 1)).all() or np.any(choice < 0) or np.any(choice >= k)
            or not mask[np.arange(rows), choice].all()):
        raise ValueError(f"invalid decision rows in {file}")
    return result


def model_for(torch, meta):
    class Actor(torch.nn.Module):
        def __init__(self):
            super().__init__()
            self.memory = torch.nn.GRU(len(meta["own_feature_names"]), HIDDEN, batch_first=True)
            self.option = torch.nn.Linear(len(meta["option_feature_names"]), HIDDEN)

        def forward(self, own, options, mask, valid):
            hidden, _ = self.memory(own)
            candidates = self.option(options).relu()
            logits = (candidates * hidden[valid].unsqueeze(1)).sum(-1) / math.sqrt(HIDDEN)
            return logits.masked_fill(~mask, float("-inf"))

    return Actor()


def evaluate_epoch(torch, model, files, meta, *, actors_per_batch, rng=None, optimizer=None):
    model.train(optimizer is not None)
    device = next(model.parameters()).device
    loss_sum = correct = total = 0
    with torch.set_grad_enabled(optimizer is not None):
        for index in (rng.permutation(len(files)) if rng is not None else range(len(files))):
            episode = read_episode(*files[index], meta)
            bounds = np.r_[0, np.flatnonzero(episode["npc"][1:] != episode["npc"][:-1]) + 1, len(episode["npc"])]
            actors = len(bounds) - 1
            order = rng.permutation(actors) if rng is not None else range(actors)
            for first in range(0, actors, actors_per_batch):
                group = [slice(bounds[i], bounds[i + 1]) for i in order[first:first + actors_per_batch]]
                sequences = [torch.as_tensor(episode["own"][rows], dtype=torch.float32) for rows in group]
                own = torch.nn.utils.rnn.pad_sequence(sequences, batch_first=True).to(device)
                valid = (torch.arange(own.size(1))[None, :] < torch.tensor([len(s) for s in sequences])[:, None]).to(device)

                def joined(key, dtype):
                    return torch.as_tensor(np.concatenate([episode[key][rows] for rows in group]),
                                           dtype=dtype, device=device)

                actions = joined("choice", torch.long)
                logits = model(own, joined("options", torch.float32), joined("mask", torch.bool), valid)
                loss = torch.nn.functional.cross_entropy(logits, actions)
                if optimizer is not None:
                    optimizer.zero_grad(set_to_none=True)
                    loss.backward()
                    torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0, error_if_nonfinite=True)
                    optimizer.step()
                loss_sum += float(loss.detach()) * len(actions)
                correct += int((logits.argmax(-1) == actions).sum())
                total += len(actions)
    return dict(decisions=total, cross_entropy=loss_sum / total, agreement=correct / total)


def train(training, validation, output, *, epochs, seed, actors_per_batch, device):
    import torch

    meta, train_files, train_groups = dataset(training)
    val_meta, val_files, val_groups = dataset(validation)
    if meta != val_meta:
        raise ValueError("training and validation feature layouts differ")
    if train_groups & val_groups:
        raise ValueError("training and validation share world and seed pairs")
    if min(epochs, actors_per_batch) < 1 or not 0 <= seed < 2**64:
        raise ValueError("epochs and actors per batch must be positive and the seed must fit uint64")
    if str(device).startswith("cuda") and not torch.cuda.is_available():
        raise ValueError("CUDA was requested but is unavailable")
    torch.manual_seed(seed)
    if str(device) == "cpu":
        torch.set_num_threads(1)
    model = model_for(torch, meta).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=0.001)
    rng = np.random.default_rng(seed)
    best = None
    for epoch in range(1, epochs + 1):
        training_stats = evaluate_epoch(torch, model, train_files, meta, actors_per_batch=actors_per_batch,
                                        rng=rng, optimizer=optimizer)
        validation_stats = evaluate_epoch(torch, model, val_files, meta, actors_per_batch=actors_per_batch)
        stats = dict(epoch=epoch, train=training_stats, validation=validation_stats)
        print(json.dumps(stats, allow_nan=False), flush=True)
        if best is None or validation_stats["cross_entropy"] < best["metrics"]["validation"]["cross_entropy"]:
            best = dict(format=FORMAT, version=MODEL_VERSION, architecture=ARCHITECTURE, layout=meta, metrics=stats,
                        seed=seed, train_groups=sorted(train_groups), validation_groups=sorted(val_groups),
                        state_dict={name: value.detach().cpu().clone() for name, value in model.state_dict().items()})
    with output.open("xb") as file:
        torch.save(best, file)
    return best["metrics"]


def load_checkpoint(torch, path):
    checkpoint = torch.load(path, weights_only=True, map_location="cpu")
    if (checkpoint.get("format"), checkpoint.get("version"), checkpoint.get("architecture")) != (FORMAT, MODEL_VERSION, ARCHITECTURE):
        raise ValueError("unsupported society checkpoint")
    meta = layout(checkpoint["layout"])
    model = model_for(torch, meta)
    model.load_state_dict(checkpoint["state_dict"])
    return meta, model.eval()


def export(checkpoint, output):
    import torch

    meta, model = load_checkpoint(torch, checkpoint)
    tensors, payload, offset = [], [], 0
    for name, value in model.state_dict().items():
        name = name.removesuffix("_l0")
        array = np.asarray(value.numpy(), dtype="<f4")
        if not np.isfinite(array).all():
            raise ValueError(f"nonfinite weights in {name}")
        data = array.tobytes(order="C")
        tensors.append(dict(name=name, shape=list(array.shape), dtype="float32", offset=offset, nbytes=len(data)))
        payload.append(data)
        offset += len(data)
    header = dict(format=FORMAT, version=MODEL_VERSION, architecture=ARCHITECTURE, byte_order="little",
                  offset_origin="first byte after JSON header newline", data_nbytes=offset,
                  checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(), tensors=tensors, **meta)
    with output.open("xb") as stream:
        stream.write(json.dumps(header, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n")
        stream.writelines(payload)
    return header


def check(checkpoint, weights, validation):
    import torch

    torch.set_num_threads(1)
    meta, model = load_checkpoint(torch, checkpoint)
    data_meta, files, _ = dataset(validation)
    if data_meta != meta:
        raise ValueError("checkpoint and validation feature layouts differ")
    with weights.open("rb") as stream:
        line = stream.readline(1 << 20)
        if not line.endswith(b"\n"):
            raise ValueError("socnet has no bounded JSON header")
        header = json.loads(line)
        payload = stream.read()
    if ((header.get("format"), header.get("version"), header.get("architecture")) != (FORMAT, MODEL_VERSION, ARCHITECTURE)
            or layout(header) != meta or header.get("byte_order") != "little"
            or header.get("offset_origin") != "first byte after JSON header newline"
            or header.get("data_nbytes") != len(payload)
            or header.get("checkpoint_sha256") != hashlib.sha256(checkpoint.read_bytes()).hexdigest()):
        raise ValueError("socnet metadata does not match checkpoint")
    expected = {key.removesuffix("_l0"): value for key, value in model.state_dict().items()}
    arrays, offset = {}, 0
    for entry in header["tensors"]:
        name = entry["name"]
        if (name in arrays or name not in expected or entry["dtype"] != "float32"
                or tuple(entry["shape"]) != tuple(expected[name].shape) or entry["offset"] != offset
                or entry["nbytes"] != 4 * expected[name].numel()):
            raise ValueError(f"invalid tensor header entry {name}")
        arrays[name] = np.frombuffer(payload, dtype="<f4", count=expected[name].numel(), offset=offset).reshape(entry["shape"])
        if not np.isfinite(arrays[name]).all():
            raise ValueError(f"nonfinite weights in {name}")
        offset += entry["nbytes"]
    if arrays.keys() != expected.keys() or offset != len(payload):
        raise ValueError("incomplete socnet payload")

    def sigmoid(value):
        return np.exp(np.minimum(value, 0)) / (1 + np.exp(-np.abs(value)))

    max_error = max_hidden_error = decisions = differing = near_ties = 0
    with torch.inference_mode():
        for file, rows in files:
            d = read_episode(file, rows, meta)
            prev_npc = None
            for npc, own, options, mask in zip(d["npc"], d["own"], d["options"], d["mask"], strict=True):
                mask = mask.astype(bool, copy=False)
                if npc != prev_npc:
                    hidden = np.zeros(HIDDEN, dtype=np.float32)
                    torch_hidden = None
                    prev_npc = npc
                torch_out, torch_hidden = model.memory(torch.as_tensor(own, dtype=torch.float32).reshape(1, 1, -1), torch_hidden)
                torch_scores = (model.option(torch.as_tensor(options, dtype=torch.float32)).relu()
                                @ torch_out.reshape(HIDDEN) / math.sqrt(HIDDEN)).numpy()
                ir, iz, inn = np.split(arrays["memory.weight_ih"] @ own + arrays["memory.bias_ih"], 3)
                hr, hz, hn = np.split(arrays["memory.weight_hh"] @ hidden + arrays["memory.bias_hh"], 3)
                update = sigmoid(iz + hz)
                candidate = np.tanh(inn + sigmoid(ir + hr) * hn)
                hidden = candidate + update * (hidden - candidate)
                scores = np.maximum(0, options @ arrays["option.weight"].T + arrays["option.bias"]) @ hidden / math.sqrt(HIDDEN)
                max_error = max(max_error, float(np.abs(scores[mask] - torch_scores[mask]).max()))
                max_hidden_error = max(max_hidden_error, float(np.abs(hidden - torch_out.numpy().reshape(HIDDEN)).max()))
                exported = np.argmax(np.where(mask, scores, -np.inf))
                reference = np.argmax(np.where(mask, torch_scores, -np.inf))
                if exported != reference:
                    differing += 1
                    if max(scores[exported] - scores[reference],
                           torch_scores[reference] - torch_scores[exported]) <= SCORE_TIE_TOLERANCE:
                        near_ties += 1
                decisions += 1
    if max_error > 1e-4 or max_hidden_error > 1e-4 or differing != near_ties:
        raise ValueError(f"socnet parity failed: logits={max_error}, hidden={max_hidden_error}, "
                         f"differing choices={differing}, near ties={near_ties}")
    return dict(decisions=decisions, max_logits_error=max_error, max_hidden_error=max_hidden_error,
                differing_choices=differing, near_tie_choices=near_ties)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    collect = sub.add_parser("record", help="record teacher episodes with one shard per world and seed")
    collect.add_argument("--world", type=Path, action="append", required=True)
    collect.add_argument("--config", type=Path, required=True)
    collect.add_argument("--out", type=Path, required=True)
    collect.add_argument("--seed", type=int, default=0)
    collect.add_argument("--episodes", type=int, default=1)
    collect.add_argument("--days", type=int, default=1)
    collect.add_argument("--world-batch", type=int, default=4)
    collect.add_argument("--threads", type=int, default=1)
    fit = sub.add_parser("train", help="imitate the teacher with validation from unseen episodes")
    fit.add_argument("--train", type=Path, action="append", required=True)
    fit.add_argument("--validation", type=Path, action="append", required=True)
    fit.add_argument("--out", type=Path, required=True)
    fit.add_argument("--epochs", type=int, default=5)
    fit.add_argument("--seed", type=int, default=0)
    fit.add_argument("--actors-per-batch", type=int, default=16)
    fit.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    dump = sub.add_parser("export", help="export a checkpoint as a little-endian SmallNet file")
    dump.add_argument("checkpoint", type=Path)
    dump.add_argument("--out", type=Path, required=True)
    verify = sub.add_parser("check", help="compare recurrent PyTorch and exported NumPy decisions on held-out data")
    verify.add_argument("checkpoint", type=Path)
    verify.add_argument("weights", type=Path)
    verify.add_argument("--validation", type=Path, action="append", required=True)
    args = parser.parse_args()
    try:
        if args.command == "record":
            result = record(args.world, args.config, args.out, seed=args.seed, episodes=args.episodes,
                            days=args.days, world_batch=args.world_batch, threads=args.threads)
        elif args.command == "train":
            result = train(args.train, args.validation, args.out, epochs=args.epochs, seed=args.seed,
                           actors_per_batch=args.actors_per_batch, device=args.device)
        elif args.command == "export":
            result = export(args.checkpoint, args.out)
        else:
            result = check(args.checkpoint, args.weights, args.validation)
    except (ImportError, KeyError, OSError, RuntimeError, ValueError) as exc:
        parser.exit(1, f"society policy: {exc}\n")
    print(json.dumps(result, allow_nan=False))


if __name__ == "__main__":
    main()
