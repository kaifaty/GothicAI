#!/usr/bin/env python3
"""Stage 9.3: recurrent PPO on lockstep rollouts of body_rl_collect.py.

update  one PPO step from the episodes the current policy collected at temperature 1. Rewards come
        per decision and add up to the 9.2 episode score: half the change of the health margin
        between decisions, then the outcome (+1 win, -1 loss) and the win's speed term at the
        end. Every episode ends in a terminal of the task (down, time limit, visibility cut), so
        nothing is bootstrapped past it. GAE runs over decisions with per-second discounting. The
        actor replays every periodic row as in the rollout (memory), loss sits on decision rows:
        clipped ratio against the stored behaviour log-probabilities, an entropy bonus and a KL
        penalty to a frozen reference (BC). The critic is an MLP over the decision row's features
        and the actor's GRU state (detached). Episodes with a fallback carry no update.
        Before any step the parent's recomputed log-probabilities must equal the stored ones
        (same masks, same memory): the no-update ratio is 1. The new actor keeps the parent's
        normalizer and provenance; the critic is saved beside it (critic.pt).
check   that ratio test alone, without writing anything.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import random
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))

import numpy  # noqa: E402

TERMINAL_OUTCOME = {"opponent_down": 1.0, "primary_down": -1.0}


def carried(values) -> numpy.ndarray:
    """Health fractions with missing readings (-1) carried forward from the last valid one (start: full)."""
    out, last = numpy.empty(len(values), numpy.float64), 1.0
    for index, value in enumerate(values):
        last = float(value) if value >= 0 else last
        out[index] = last
    return out


def episode_rewards(arrays, summary: dict, timeout_ms: int):
    """Decision row indices, reward per decision, seconds to the next decision.

    Sum of rewards = score() of body_rl_collect when both actors are at full health at the
    first decision: 0.5 x the change of (own - opponent) health, +-1 on a down, and the win's
    speed term -0.2 x elapsed / timeout.
    """
    labels = numpy.asarray(arrays["labels"])
    decisions = numpy.flatnonzero(labels >= 1)
    if len(decisions) == 0:
        return decisions, numpy.zeros(0), numpy.zeros(0)
    own, other = carried(arrays["hp"]), carried(arrays["target_hp"])
    margin = own[decisions]-other[decisions]
    final = summary["primary_hp"]/summary["primary_hp_max"]-summary["opponent_hp"]/summary["opponent_hp_max"]
    after = numpy.append(margin[1:], final)
    rewards = 0.5*(after-margin)
    outcome = TERMINAL_OUTCOME.get(summary["reason"], 0.0)
    rewards[-1] += outcome+(-0.2*summary["elapsed_game_ms"]/timeout_ms if outcome > 0 else 0.0)
    times = numpy.asarray(arrays["t_ms"], dtype=numpy.float64)[decisions]
    dts = numpy.append(numpy.diff(times)/1000.0, 0.0)
    return decisions, rewards, dts


def gae(rewards, values, dts, *, gamma_per_s: float, lam: float):
    """Advantages and lambda-returns of one terminal episode; discount gamma^dt per transition."""
    advantages = numpy.zeros(len(rewards))
    running = 0.0
    for index in reversed(range(len(rewards))):
        last = index == len(rewards)-1
        discount = 0.0 if last else gamma_per_s**dts[index]
        delta = rewards[index]+discount*(0.0 if last else values[index+1])-values[index]
        running = delta+discount*lam*running
        advantages[index] = running
    return advantages, advantages+numpy.asarray(values, dtype=numpy.float64)


def load_episodes(collect_dirs, timeout_ms: int):
    """Usable episodes: no fallback of either actor, at least one decision, PPO arrays present."""
    episodes, skipped = [], {"fallback": 0, "no_decision": 0}
    for directory in collect_dirs:
        # Envs share one episode queue: order by episode, not by the env that happened to play it.
        rows = sorted(((path, json.loads(line)) for path in Path(directory).glob("worker-*/episodes.jsonl")
                       for line in path.read_text().splitlines()), key=lambda row: (row[1]["seed"], row[1]["name"]))
        for path, summary in rows:
            if summary.get("fallbacks") or summary.get("opponent_fallbacks"):
                skipped["fallback"] += 1
                continue
            npz = path.parent/f"{summary['name']}.npz"
            if not npz.exists():
                skipped["no_decision"] += 1
                continue
            arrays = dict(numpy.load(npz))
            if "logp" not in arrays:
                raise ValueError(f"{npz} has no behaviour log-probabilities: collected before stage 9.3")
            decisions, rewards, dts = episode_rewards(arrays, summary, timeout_ms)
            if len(decisions) == 0:
                skipped["no_decision"] += 1
                continue
            if summary.get("temperature") != 1.0:
                raise ValueError("PPO needs rollouts sampled at temperature 1")
            episodes.append({"summary": summary, "arrays": arrays, "decisions": decisions,
                             "rewards": rewards, "dts": dts})
    return episodes, skipped


def buckets(episodes, rows_per_batch: int, rng: random.Random):
    """Minibatches of similar length (little padding) under a row budget, in random order."""
    order = sorted(range(len(episodes)), key=lambda index: (len(episodes[index]["arrays"]["labels"]), rng.random()))
    batches, current, longest = [], [], 0
    for index in order:
        length = len(episodes[index]["arrays"]["labels"])
        if current and max(longest, length)*(len(current)+1) > rows_per_batch:
            batches.append(current)
            current, longest = [], 0
        current.append(index)
        longest = max(longest, length)
    if current:
        batches.append(current)
    rng.shuffle(batches)
    return batches


def make_critic(feature_count: int, hidden_size: int):
    import torch
    from torch import nn
    return nn.Sequential(nn.Linear(feature_count+hidden_size, 256), nn.ReLU(), nn.Linear(256, 256), nn.ReLU(),
                         nn.Linear(256, 1)).to(torch.float32)


def tensors(episodes, indices, device):
    import torch
    chosen = [episodes[index] for index in indices]
    length = max(len(item["arrays"]["labels"]) for item in chosen)
    features = numpy.zeros((len(chosen), length, chosen[0]["arrays"]["features"].shape[1]), numpy.float32)
    masks = numpy.zeros((len(chosen), length, 12), bool)
    masks[..., 0] = True  # padding rows: a valid mask, no loss
    resets = numpy.zeros((len(chosen), length), bool)
    for row, item in enumerate(chosen):
        count = len(item["arrays"]["labels"])
        features[row, :count] = item["arrays"]["features"]
        masks[row, :count] = item["arrays"]["masks"]
        resets[row, :count] = item["arrays"]["resets"]
    rows = numpy.concatenate([numpy.full(len(item["decisions"]), row) for row, item in enumerate(chosen)])
    steps = numpy.concatenate([item["decisions"] for item in chosen])
    return (torch.from_numpy(features).to(device), torch.from_numpy(masks).to(device), torch.from_numpy(resets).to(device),
            torch.from_numpy(rows).to(device), torch.from_numpy(steps).to(device))


def unroll(actor, features, masks, resets):
    """Logits and GRU state after every row, exactly as the runtime steps one row at a time."""
    import torch
    state, logits, states = None, [], []
    for index in range(features.shape[1]):
        out, state = actor.step(features[:, index], masks[:, index], state, resets[:, index])
        logits.append(out)
        states.append(state)
    return torch.stack(logits, dim=1), torch.stack(states, dim=1)


def decision_view(logits, states, features, masks, rows, steps):
    import torch
    picked = logits[rows, steps]
    mask = masks[rows, steps]
    logp = torch.log_softmax(picked.masked_fill(~mask, float("-inf")), dim=-1)
    return logp, mask, states[rows, steps], features[rows, steps]


def prepass(actor, reference, critic, episodes, args, device):
    """Behaviour check, reference log-probabilities and values for every decision (no gradients)."""
    import torch
    stats = {"decisions": 0, "max_abs_logp_diff": 0.0, "sum_abs_logp_diff": 0.0}
    with torch.no_grad():
        for start in range(0, len(episodes), args.prepass_batch):
            indices = list(range(start, min(len(episodes), start+args.prepass_batch)))
            features, masks, resets, rows, steps = tensors(episodes, indices, device)
            logits, states = unroll(actor, features, masks, resets)
            logp, mask, hidden, decision_features = decision_view(logits, states, features, masks, rows, steps)
            ref_logits, _ = unroll(reference, features, masks, resets)
            ref_logp = torch.log_softmax(ref_logits[rows, steps].masked_fill(~mask, float("-inf")), dim=-1)
            values = critic(torch.cat((decision_features, hidden), dim=-1)).squeeze(-1)
            offset = 0
            for index in indices:
                item = episodes[index]
                count = len(item["decisions"])
                labels = torch.as_tensor(item["arrays"]["labels"][item["decisions"]].astype(numpy.int64), device=device)
                own = logp[offset:offset+count].gather(-1, labels[:, None]).squeeze(-1).double().cpu().numpy()
                stored = item["arrays"]["logp"][item["decisions"]]
                diff = numpy.abs(own-stored)
                stats["max_abs_logp_diff"] = max(stats["max_abs_logp_diff"], float(diff.max()))
                stats["sum_abs_logp_diff"] += float(diff.sum())
                stats["decisions"] += count
                item["old_logp"] = stored.astype(numpy.float32)
                item["ref_logp"] = ref_logp[offset:offset+count].cpu()
                item["values"] = values[offset:offset+count].double().cpu().numpy()
                item["labels"] = labels.cpu()
                offset += count
    stats["mean_abs_logp_diff"] = stats.pop("sum_abs_logp_diff")/max(1, stats["decisions"])
    return stats


def explained_variance(values, returns) -> float:
    variance = float(numpy.var(returns))
    return float("nan") if variance == 0 else 1.0-float(numpy.var(returns-values))/variance


def update(args):
    import torch
    from gothic_body.checkpoint import load_checkpoint, save_checkpoint
    torch.manual_seed(args.seed)
    torch.set_num_threads(args.threads)
    device = args.device
    started = time.monotonic()
    parent = load_checkpoint(args.policy, device=device)
    actor = parent["model"]
    reference = load_checkpoint(args.reference, device=device)["model"].eval()
    for parameter in reference.parameters():
        parameter.requires_grad_(False)
    config = actor.config
    critic = make_critic(config.feature_count, config.hidden_size).to(device)
    if args.critic is not None:
        critic.load_state_dict(torch.load(args.critic, map_location=device, weights_only=True)["state_dict"])
    episodes, skipped = load_episodes(args.collect, args.timeout_ms)
    if not episodes:
        raise ValueError("No usable episode")
    actor.eval()
    check = prepass(actor, reference, critic, episodes, args, device)
    print(json.dumps({"episodes": len(episodes), "skipped": skipped, "ratio_check": check}), flush=True)
    if check["max_abs_logp_diff"] > args.max_logp_diff:
        raise ValueError(f"behaviour log-probabilities differ from the parent's: {check}")
    if args.command == "check":
        return
    for item in episodes:
        item["advantages"], item["returns"] = gae(item["rewards"], item["values"], item["dts"],
                                                  gamma_per_s=args.gamma_per_s, lam=args.lam)
    all_adv = numpy.concatenate([item["advantages"] for item in episodes])
    mean, std = float(all_adv.mean()), float(all_adv.std())+1e-8
    values_before = numpy.concatenate([item["values"] for item in episodes])
    returns_all = numpy.concatenate([item["returns"] for item in episodes])
    actor_optimizer = torch.optim.Adam(actor.parameters(), lr=args.learning_rate, eps=1e-5)
    critic_optimizer = torch.optim.Adam(critic.parameters(), lr=args.critic_learning_rate, eps=1e-5)
    rng = random.Random(args.seed)
    history, stopped = [], None
    epochs = max(args.policy_epochs, args.critic_epochs)
    for epoch in range(1, epochs+1):
        train_policy = epoch <= args.policy_epochs and stopped is None
        sums = {key: 0.0 for key in ("policy_loss", "value_loss", "entropy", "kl_ref", "approx_kl", "clip_fraction")}
        count = 0
        actor.train(train_policy)
        for batch in buckets(episodes, args.rows_per_batch, rng):
            features, masks, resets, rows, steps = tensors(episodes, batch, device)
            with torch.set_grad_enabled(train_policy):
                logits, states = unroll(actor, features, masks, resets)
                logp_all, mask, hidden, decision_features = decision_view(logits, states, features, masks, rows, steps)
            labels = torch.cat([episodes[index]["labels"] for index in batch]).to(device)
            old = torch.from_numpy(numpy.concatenate([episodes[index]["old_logp"] for index in batch])).to(device)
            advantages = torch.from_numpy(((numpy.concatenate([episodes[index]["advantages"] for index in batch])-mean)/std)
                                          .astype(numpy.float32)).to(device)
            returns = torch.from_numpy(numpy.concatenate([episodes[index]["returns"] for index in batch]).astype(numpy.float32)).to(device)
            ref = torch.cat([episodes[index]["ref_logp"] for index in batch]).to(device)
            values = critic(torch.cat((decision_features, hidden.detach()), dim=-1)).squeeze(-1)
            value_loss = (values-returns).pow(2).mean()
            critic_optimizer.zero_grad(set_to_none=True)
            if epoch <= args.critic_epochs:
                value_loss.backward()
                torch.nn.utils.clip_grad_norm_(critic.parameters(), args.max_grad_norm)
                critic_optimizer.step()
            new = logp_all.gather(-1, labels[:, None]).squeeze(-1)
            ratio = torch.exp(new-old)
            probabilities = logp_all.exp()
            safe = torch.where(mask, logp_all, torch.zeros_like(logp_all))
            entropy = -(probabilities*safe).sum(-1).mean()
            kl_ref = (probabilities*(safe-torch.where(mask, ref, torch.zeros_like(ref)))).sum(-1).mean()
            policy_loss = -torch.min(ratio*advantages, ratio.clamp(1-args.clip, 1+args.clip)*advantages).mean()
            if train_policy:
                loss = policy_loss-args.entropy_coef*entropy+args.kl_coef*kl_ref
                actor_optimizer.zero_grad(set_to_none=True)
                loss.backward()
                torch.nn.utils.clip_grad_norm_(actor.parameters(), args.max_grad_norm)
                actor_optimizer.step()
            with torch.no_grad():
                log_ratio = new-old
                sums["approx_kl"] += float(((ratio-1)-log_ratio).mean())*len(batch)
                sums["clip_fraction"] += float(((ratio-1).abs() > args.clip).float().mean())*len(batch)
            sums["policy_loss"] += float(policy_loss)*len(batch)
            sums["value_loss"] += float(value_loss)*len(batch)
            sums["entropy"] += float(entropy)*len(batch)
            sums["kl_ref"] += float(kl_ref)*len(batch)
            count += len(batch)
        row = {"epoch": epoch, "policy_updated": train_policy, **{key: value/count for key, value in sums.items()}}
        history.append(row)
        print(json.dumps(row), flush=True)
        if train_policy and row["approx_kl"] > args.target_kl:
            stopped = epoch  # the next epochs fit only the critic
    wins = sum(item["summary"]["reason"] == "opponent_down" for item in episodes)
    record = {"kind": "recurrent_ppo", "parent_checkpoint_sha256": parent["checkpoint_sha256"],
              "parent_training_kind": parent["metadata"]["training"].get("kind"),
              "reference_checkpoint": str(args.reference), "collect": [str(path) for path in args.collect],
              "episodes": len(episodes), "skipped": skipped, "decisions": int(len(all_adv)), "wins": wins,
              "mean_episode_return": float(numpy.mean([item["rewards"].sum() for item in episodes])),
              "ratio_check": check, "explained_variance_before": explained_variance(values_before, returns_all),
              "hyperparameters": {key: getattr(args, key) for key in (
                  "learning_rate", "critic_learning_rate", "policy_epochs", "critic_epochs", "clip", "entropy_coef",
                  "kl_coef", "target_kl", "gamma_per_s", "lam", "rows_per_batch", "max_grad_norm", "seed", "timeout_ms")},
              "stopped_policy_after_epoch": stopped, "history": history}
    args.output.mkdir(parents=True, exist_ok=False)
    actor.eval()
    checkpoint = save_checkpoint(args.output/"best.pt", actor.cpu(), normalizer=parent["normalizer"],
                                 provenance=parent["metadata"]["dataset"], training=record)
    torch.save({"state_dict": {key: value.cpu() for key, value in critic.state_dict().items()},
                "feature_count": config.feature_count, "hidden_size": config.hidden_size}, args.output/"critic.pt")
    record.update(checkpoint_sha256=checkpoint["checkpoint_sha256"], wall_s=time.monotonic()-started)
    (args.output/"update.json").write_text(json.dumps(record, indent=2)+"\n")
    print(json.dumps({"checkpoint_sha256": checkpoint["checkpoint_sha256"], "episodes": len(episodes), "wins": wins,
                      "mean_return": record["mean_episode_return"], "ev_before": record["explained_variance_before"],
                      "last": history[-1], "wall_s": record["wall_s"]}), flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("update", "check"):
        command = commands.add_parser(name)
        command.add_argument("--policy", type=Path, required=True, help="the checkpoint that collected the rollouts")
        command.add_argument("--reference", type=Path, required=True, help="frozen KL reference, e.g. the BC checkpoint")
        command.add_argument("--critic", type=Path, help="critic.pt of the previous update (absent: a new critic)")
        command.add_argument("--collect", type=Path, action="append", required=True)
        command.add_argument("--output", type=Path)
        command.add_argument("--timeout-ms", type=int, default=60000)
        command.add_argument("--learning-rate", type=float, default=3e-5)
        command.add_argument("--critic-learning-rate", type=float, default=3e-4)
        command.add_argument("--policy-epochs", type=int, default=4, help="0: fit only the critic (warm-up)")
        command.add_argument("--critic-epochs", type=int, default=4)
        command.add_argument("--clip", type=float, default=0.2)
        command.add_argument("--entropy-coef", type=float, default=0.003)
        command.add_argument("--kl-coef", type=float, default=0.02)
        command.add_argument("--target-kl", type=float, default=0.02)
        command.add_argument("--gamma-per-s", type=float, default=0.99)
        command.add_argument("--lam", type=float, default=0.95)
        command.add_argument("--rows-per-batch", type=int, default=24000)
        command.add_argument("--prepass-batch", type=int, default=64)
        command.add_argument("--max-grad-norm", type=float, default=0.5)
        command.add_argument("--max-logp-diff", type=float, default=1e-3)
        command.add_argument("--threads", type=int, default=16)
        command.add_argument("--device", default="cpu")
        command.add_argument("--seed", type=int, default=11)
    args = parser.parse_args()
    if args.command == "update" and args.output is None:
        parser.error("update needs --output")
    update(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
