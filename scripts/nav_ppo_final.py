#!/usr/bin/env python3
"""One gq batch: qualify action modes on validation, then measure both on test.

Uses the existing drive, CV, 240 s, zero teacher intervention. Two concurrent
8-runner measurements share the caller's 24-thread/11-GiB allocation. Never
submits nested queue jobs and never starts training. All outputs stay on Ryzen.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
from collections import Counter
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ryzen_game import game_environment

SEEDS = (18102027, 18102028, 18102029)
REGIONS = {"market", "harbour", "onar", "lobart", "port_streets", "upper_streets"}


def read(path):
    return json.loads(Path(path).read_text())


def write(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def configurations():
    return [(policy, mode, seed) for mode in ("argmax", "sample")
            for seed in ((SEEDS[0],) if mode == "argmax" else SEEDS)
            for policy in ("round2", "iter35")]


def measurement_path(root, split, policy, mode, seed):
    return root / split / policy / mode / str(seed)


def checked_metrics(directory):
    metrics = read(directory / "metrics.json")
    expected = Counter({region: 10 for region in REGIONS})
    if metrics["all"]["n"] != 60 or Counter(ep["region"] for ep in metrics["episodes"]) != expected:
        raise ValueError(f"Incomplete regional measurement: {directory}")
    if metrics["all"]["teacher_control_decisions"] != 0:
        raise ValueError(f"Teacher intervention in measurement: {directory}")
    return metrics


def mode_summary(metrics):
    """Pool all three prespecified seeds; never pick the luckiest seed."""
    groups = [value["all"] for value in metrics]
    n = sum(value["n"] for value in groups)
    decisions = sum(value["decisions"] for value in groups)
    elapsed = sum(ep["elapsed_ms"] for value in metrics for ep in value["episodes"])
    return dict(n=n, arrived=sum(value["arrived"] for value in groups),
                arrival_fraction=sum(value["arrived"] for value in groups) / n,
                turn_fraction=sum(value["action_counts"]["turn_only"] for value in groups) / max(decisions, 1),
                blocked_fraction=sum(value["blocked_ms"] for value in groups) / max(elapsed, 1))


def select_modes(root):
    """This function can only access validation, even if test results exist."""
    result = dict(selection_source="validation_only", game_policy="iter35", seeds=list(SEEDS),
                  rule="highest pooled arrival fraction, then least turning, then least blocked time; argmax on tie",
                  by_policy={})
    for policy in ("round2", "iter35"):
        summaries = {}
        for mode in ("argmax", "sample"):
            seeds = (SEEDS[0],) if mode == "argmax" else SEEDS
            summaries[mode] = mode_summary([checked_metrics(measurement_path(root, "validation", policy, mode, seed))
                                            for seed in seeds])
        def score(mode):
            value = summaries[mode]
            return value["arrival_fraction"], -value["turn_fraction"], -value["blocked_fraction"], mode == "argmax"
        result["by_policy"][policy] = dict(selected_mode=max(summaries, key=score), metrics=summaries)
    result["game_action_mode"] = result["by_policy"]["iter35"]["selected_mode"]
    return result


def retain_tail(directory, tail_ms=10_000):
    """Keep exact final sensor records for later failure analysis; trim raw bulk.

    Trajectory, all actions, metadata, road guidance and computed metrics remain.
    The tail has the original NAVDEMO header/layout/timestamps; no re-encoding.
    """
    import nav_demos
    source = directory / "samples.bin.gz"
    destination = directory / "tail-10s.bin.gz"
    if not source.exists():
        if not destination.exists():
            raise ValueError(f"Neither original nor retained sensor tail: {directory}")
        return
    metadata = read(directory / "episode.json")
    with gzip.open(source, "rb") as stream:
        header = stream.read(nav_demos.HEADER.size)
    count, cutoff, end, terminal = 0, None, None, None
    with gzip.open(destination, "wb", compresslevel=1) as stream:
        stream.write(header)
        for chunk in nav_demos.iter_chunks(source):
            if cutoff is None:
                terminal = int(chunk["time_ms"][0]) + metadata["elapsed_ms"]
                cutoff = terminal - tail_ms
            end = int(chunk["time_ms"][-1])
            tail = chunk[chunk["time_ms"] >= cutoff]
            stream.write(tail.tobytes())
            count += len(tail)
    if end is None or not count:
        raise ValueError(f"Empty recording or sensor tail: {source}")
    write(directory / "retained-sensors.json", dict(source_decisions=metadata["decisions"],
          tail_decisions=count, first_time_ms=cutoff, last_time_ms=end, terminal_time_ms=terminal,
          file=destination.name, navdemo_version=nav_demos.episode_version(destination)))
    source.unlink()


class FinalBatch:
    def __init__(self, args):
        self.args = args
        self.task = args.task
        self.source = Path(__file__).resolve().parent
        self.root = self.task / "final"
        self.root.mkdir(exist_ok=True)
        self.env = dict(os.environ, OPENBLAS_NUM_THREADS="1", OMP_NUM_THREADS="1", MKL_NUM_THREADS="1")
        self.weights = {"round2": self.task / "reference/round2.navbc",
                        "iter35": self.root / "policy.navbc"}

    def command(self, command, log):
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("a") as stream:
            subprocess.run(list(map(str, command)), check=True, cwd=self.source.parent,
                           env=self.env, stdout=stream, stderr=subprocess.STDOUT)

    def common(self):
        return ["--runner", self.task / "build/opengothic/gothic-body-runner",
                "--assets", Path(os.environ.get("GOTHIC_DIR", Path.home() / "gothic-ai/game-settings")), "--save", self.task / "save_slot_1.sav",
                "--cache", self.task / "cache", "--links", self.source.parent / "reports/navigation/2026-10-16-teacher/raw/accepted-links.txt",
                "--roads", self.source.parent / "server/src/gothic_ai/gamedata/places.json",
                "--physics", "cv", "--limit-ms", "240000"]

    def measure(self, split, configuration):
        policy, mode, seed = configuration
        directory = measurement_path(self.root, split, policy, mode, seed)
        if not (directory / "metrics.json").exists():
            if shutil.disk_usage(self.task).free < 25 * 1024**3:
                raise ValueError("Less than 25 GiB free before final drive")
            pairs = self.task / "validation/holdout.jsonl" if split == "validation" else self.source.parent / "reports/navigation/2026-10-18-rl-env/holdout-cv.jsonl"
            self.command([self.args.python, "-I", self.source / "nav_rl.py", "drive", *self.common(),
                          "--pairs", pairs, "--output", directory, "--weights", self.weights[policy],
                          "--action-mode", mode, "--seed", seed, "--parallel", "8", "--resume"], directory / "drive.log")
            self.command([self.args.python, "-I", self.source / "nav_rl.py", "summarize",
                          "--source", directory, "--output", directory], directory / "metrics.log")
        metrics = checked_metrics(directory)
        # Preserve full recorded input for one existing C++/NumPy acceptance
        # fixture. All other full recordings can be trimmed immediately.
        parity_source = split == "test" and policy == "iter35" and mode == "argmax"
        if parity_source and not (self.root / "parity/passed.json").exists():
            self.parity(directory)
        for episode in metrics["episodes"]:
            retain_tail(Path(episode["episode"]))
        print(json.dumps(dict(phase=split, policy=policy, mode=mode, seed=seed,
                              arrived=metrics["all"]["arrived"])), flush=True)

    def parity(self, directory):
        output = self.root / "parity"
        self.command([self.args.python, "-I", self.source / "check_nav_policy.py", "--output", output,
                      "--weights", self.weights["iter35"], "--source", directory, "--steps", "1000"], self.root / "parity.log")
        self.command([self.task / "build/opengothic/gothic-navpolicy-contract", self.weights["iter35"],
                      output / "parity.bin.gz"], self.root / "parity.log")
        write(output / "passed.json", dict(passed=True, steps=1000, tolerance=1e-4))

    def run(self):
        state = read(self.task / "run/state.json")
        if state["stop_reason"] != "manual_stop_after_validation_decline" or state["best_iteration"] != 35:
            raise ValueError("Final batch requires the explicit manual stop and selected iteration 35")
        original = self.task / "run/iter-000035/update/policy.navbc"
        if self.weights["iter35"].exists():
            if self.weights["iter35"].read_bytes() != original.read_bytes():
                raise ValueError("Final checkpoint differs from frozen iteration 35")
        else:
            shutil.copy2(original, self.weights["iter35"])
        weight_hashes = {key: hashlib.sha256(path.read_bytes()).hexdigest() for key, path in self.weights.items()}
        # The complete validation phase finishes before any test process starts.
        for split in ("validation", "test"):
            if split == "test":
                selected = select_modes(self.root)
                destination = self.root / "mode-selection.json"
                if destination.exists() and read(destination) != selected:
                    raise ValueError("Refusing to revise action mode selected before the test")
                write(destination, selected)
            with ThreadPoolExecutor(max_workers=2) as pool:
                futures = [pool.submit(self.measure, split, config) for config in configurations()]
                for future in futures:
                    future.result()
        for mode in ("argmax", "sample"):
            seeds = (SEEDS[0],) if mode == "argmax" else SEEDS
            for seed in seeds:
                output = self.root / "control" / mode / str(seed)
                self.command([self.args.python, "-I", self.source / "nav_drive.py", "collect", *self.common(),
                              "--pairs", self.task / "control-pairs.jsonl", "--output", output,
                              "--weights", self.weights["iter35"], "--action-mode", mode,
                              "--mixing-seed", seed, "--split", "control", "--resume"], output / "drive.log")
                self.command([self.args.python, "-I", self.source / "nav_rl.py", "summarize",
                              "--source", output, "--output", output], output / "metrics.log")
        write(self.root / "completed.json", dict(completed=True, time=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
              iteration=35, measurements=16, validation_pairs=60, test_pairs=60, sample_seeds=list(SEEDS),
              weight_sha256=weight_hashes,
              mode_selection=read(self.root / "mode-selection.json")))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--task", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    args = parser.parse_args()
    if sys.platform != "linux" or not args.task.is_absolute() or not args.python.is_absolute():
        parser.error("Run under gq on Ryzen with absolute task and venv Python paths")
    os.environ.update(game_environment())
    FinalBatch(args).run()


if __name__ == "__main__":
    main()
