#!/usr/bin/env python3
"""Compare staged content on one Ryzen build, in one quiet ABBA run."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import statistics
import subprocess
import sys

import perf_report


ROOT = Path(__file__).resolve().parents[1]
PLACES = ("harbour", "market", "forest", "interior", "combat")
ORDER = ("A", "B", "B", "A")
NOISE_PCT = 2.0
THRESHOLD_PCT = 5.0


def content_name(value):
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", value):
        raise argparse.ArgumentTypeError("content must be a staging set name or none")
    return value


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__, prog="scripts/perf-content.sh")
    parser.add_argument("a", type=content_name)
    parser.add_argument("b", type=content_name)
    parser.add_argument("places", nargs="*", choices=PLACES)
    parser.add_argument("--noise-pct", type=float, default=NOISE_PCT, help="p95 noise band in percent (default: 2)")
    parser.add_argument("--threshold-pct", type=float, default=THRESHOLD_PCT, help="p95 increase limit in percent (default: 5)")
    args = parser.parse_args(argv)
    args.places = args.places or ["harbour", "market"]
    if len(set(args.places)) != len(args.places):
        parser.error("places must not repeat")
    if not 0 <= args.noise_pct <= args.threshold_pct < float("inf"):
        parser.error("require finite 0 <= noise-pct <= threshold-pct")
    return args


def tree_sha(directory):
    """Hash sorted relative paths and each file's SHA-256, never timestamps."""
    root = Path(directory)
    if not root.is_dir():
        raise ValueError(f"missing content set: {root}")
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"content symlinks are unsupported: {path}")
        if path.is_file():
            digest.update(path.relative_to(root).as_posix().encode() + b"\0")
            with path.open("rb") as stream:
                file_digest = hashlib.sha256()
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    file_digest.update(block)
            digest.update(file_digest.digest())
    return digest.hexdigest()


def p95_verdict(delta_pct, noise_pct, threshold_pct):
    if abs(delta_pct) <= noise_pct:
        return "в шуме"
    return "в пороге" if delta_pct <= threshold_pct else "сверх порога"


def comparison(directory, noise_pct=NOISE_PCT, threshold_pct=THRESHOLD_PCT):
    directory = Path(directory)
    metadata = json.loads((directory / "metadata.json").read_text())
    sides = {side: perf_report.build_report(directory / side) for side in ("A", "B")}
    places = {}
    drawables = set()
    for side, report in sides.items():
        if report["warnings"]:
            raise ValueError("; ".join(report["warnings"]))
        if set(report["places"]) != set(metadata["places"]):
            raise ValueError(f"{side}: incomplete places")
        for source in report["sources"]:
            meta = source["metadata"]
            drawables.add((meta["drawable_width"], meta["drawable_height"]))
    if len(drawables) != 1:
        raise ValueError(f"drawable changed between attempts: {sorted(drawables)}")
    for place in metadata["places"]:
        left, right = (sides[side]["places"][place] for side in ("A", "B"))
        for data in (left, right):
            if len(data["repeats"]) != 2:
                raise ValueError(f"{place}: expected two repeats per side")
        changes = {}
        for metric in ("p50", "p95"):
            def get(stats):
                return stats["frame_ms"][metric]
            change = perf_report.delta(get(left["summary"]), get(right["summary"]),
                                       left["repeats"], right["repeats"], get)
            change["spread_a_ms"], change["spread_b_ms"] = (
                max(values) - min(values) for data in (left, right)
                for values in [[get(repeat["summary"]) for repeat in data["repeats"]]])
            # Repeat spread remains descriptive; calibration defines the noise band.
            del change["noise"], change["within_noise"]
            changes[metric] = change
        counters = {}
        for side, data in (("A", left), ("B", right)):
            frames = []
            submitted = 0
            for path in (directory / side).rglob("frameprofile.csv"):
                rows, _, _, _ = perf_report.read_profile(path)
                selected = [row for row in rows if row["outcome"] == 1
                            and (perf_report.segment_parts(row["segment"]) or (None,))[0] == place]
                submitted += len(selected)
                frames.extend(row for row in selected if row.get("geometry_done") == 1)
            if not frames or any(not row.get(name) for row in frames for name in ("triangles", "draw_calls")):
                raise ValueError(f"{place}/{side}: missing or empty completed geometry counters")
            counters[side] = {name: statistics.fmean(row[name] for row in frames)
                              for name in ("triangles", "draw_calls")}
            counters[side].update(completed_frames=len(frames), submitted_frames=submitted)
        places[place] = {**changes, "within_threshold": changes["p95"]["delta_pct"] <= threshold_pct,
                         "p95_verdict": p95_verdict(changes["p95"]["delta_pct"], noise_pct, threshold_pct),
                         "counters": counters,
                         "repeats": {side: [{"p50_ms": row["summary"]["frame_ms"]["p50"],
                                              "p95_ms": row["summary"]["frame_ms"]["p95"],
                                              "frames": row["summary"]["frames"]}
                                             for row in sides[side]["places"][place]["repeats"]]
                                     for side in ("A", "B")}}
    return {"metadata": metadata, "noise_p95_pct": noise_pct, "threshold_p95_pct": threshold_pct,
            "drawable": list(next(iter(drawables))), "timing": perf_report.TIMING,
            "noise_method": "Absolute p95 delta <= calibrated noise band in percent. "
                            "Repeat max-min spread is descriptive only. "
                            "Percentiles pool frame intervals from both repeats.",
            "places": places}


def table(report):
    lines = ["| Место | p50 A, мс | p50 B, мс | Δ p50 | Разброс p50 A / B, мс | p95 A, мс | p95 B, мс | Δ p95 | Разброс p95 A / B, мс | Вердикт p95 |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---|"]
    for place, data in report["places"].items():
        a, b = data["p50"], data["p95"]
        label = data["p95_verdict"]
        lines.append(f"| {place} | {a['a']:.3f} | {a['b']:.3f} | {a['delta_pct']:+.2f}% | "
                     f"{a['spread_a_ms']:.3f} / {a['spread_b_ms']:.3f} | "
                     f"{b['a']:.3f} | {b['b']:.3f} | {b['delta_pct']:+.2f}% | "
                     f"{b['spread_a_ms']:.3f} / {b['spread_b_ms']:.3f} | {label} |")
    lines += ["", "| Место | Треугольники/кадр A | Треугольники/кадр B | Draw calls/кадр A | Draw calls/кадр B |",
              "|---|---:|---:|---:|---:|"]
    for place, data in report["places"].items():
        a, b = (data["counters"][side] for side in ("A", "B"))
        lines.append(f"| {place} | {a['triangles']:.0f} | {b['triangles']:.0f} | "
                     f"{a['draw_calls']:.1f} | {b['draw_calls']:.1f} |")
    return "\n".join(lines)


def write_report(directory, noise_pct=NOISE_PCT, threshold_pct=THRESHOLD_PCT, output=None):
    report = comparison(directory, noise_pct, threshold_pct)
    meta = report["metadata"]
    report["conclusions"] = []
    if meta["a"] != meta["b"]:
        for place, data in report["places"].items():
            a, b = (data["counters"][side] for side in ("A", "B"))
            triangles = b["triangles"] - a["triangles"]
            report["conclusions"].append(
                f"{place}: {triangles / 1000:+.0f} тыс. треугольников "
                f"({triangles * 100 / a['triangles']:+.1f}%) и "
                f"{b['draw_calls'] - a['draw_calls']:+.1f} вызова отрисовки на кадр; "
                f"время кадра — {data['p95_verdict']} (Δ p95 {data['p95']['delta_pct']:+.2f}%).")
    markdown = (f"# Цена контента квартала\n\nA: `{meta['a']}`; B: `{meta['b']}`. "
                f"Ryzen, Vulkan, drawable {report['drawable'][0]}×{report['drawable'][1]}.\n\n"
                f"Commit: `{meta['git_commit']}`; dirty files: {meta['dirty_files']}. "
                f"Порядок каждого места: A B B A, одна сборка и одна заявка `gq quiet`.\n\n"
                f"Вердикт p95: «в шуме» при модуле Δ ≤ {noise_pct:g}%; "
                f"«в пороге» при Δ ≤ +{threshold_pct:g}% вне шума; «сверх порога» иначе.\n\n"
                + table(report) + "\n\n" + "\n\n".join(report["conclusions"]) + "\n\n"
                + report["timing"] + "\n\n"
                + report["noise_method"] + "\n\n"
                + "Треугольники: Vulkan clipping-stage input primitives, все треугольные проходы "
                "(включая indirect/mesh, тени, глубину, UI и post). Draw calls: команды "
                "графической отрисовки API, включая indirect с нулевой работой. "
                "Средние счётчиков используют только кадры с `geometry_done=1`; "
                "доля завершения — в JSON. Запросы читаются после штатного fence без дополнительного ожидания.\n\n"
                + f"CSV и журналы: `{meta['archive']}`.\n\n"
                + "SHA-256 наборов: `" + json.dumps(meta["content_sha256"], sort_keys=True) + "`.\n")
    destination = Path(directory if output is None else output)
    destination.mkdir(parents=True, exist_ok=True)
    perf_report.write_outputs(destination, "report", report, markdown)
    print(table(report), flush=True)


def run_attempts(directory, content, args):
    directory, content = Path(directory), Path(content)
    base_env = os.environ.copy()
    extra = shlex.split(base_env.get("PERF_ENV", ""))
    # Every attempt explicitly selects its content, including the empty none case.
    extra = [value for value in extra if not value.startswith("OPENGOTHIC_CONTENT=")]
    for place in args.places:
        counts = {"A": 0, "B": 0}
        for side in ORDER:
            counts[side] += 1
            name = args.a if side == "A" else args.b
            selected = "" if name == "none" else str(content / name)
            env = {**base_env, "PERF_REPEAT": "1", "NO_BUILD": "1", "PERF_REMOTE": "1",
                   "OPENGOTHIC_CONTENT": selected,
                   "PERF_ENV": shlex.join([*extra, "OPENGOTHIC_CONTENT=" + selected])}
            out = directory / side / f"attempt-{counts[side]}"
            print(f"perf-content: {place} {side}={name}, repeat {counts[side]}/2", flush=True)
            subprocess.run(["bash", str(ROOT / "scripts/perf.sh"), "__run", str(out), place], env=env, check=True)
    write_report(directory, args.noise_pct, args.threshold_pct)


def launch(args):
    if os.environ.get("BENCH_HOST", "ryzen") != "ryzen":
        raise ValueError("perf-content requires BENCH_HOST=ryzen; no local fallback")
    host = os.environ.get("RYZEN_HOST", "ryzen")
    def remote(*command, **kwargs):
        return subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", host,
                               shlex.join(command)], check=True, **kwargs)
    home = remote("sh", "-c", 'printf "%s" "$HOME"', capture_output=True, text=True).stdout
    task_name = "perf-content-" + ROOT.parent.name
    task = f"{home}/gothic-ai/{task_name}"
    source = task + "/source"
    run_id = "perf-content-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + f"-{os.getpid()}"
    out = ROOT / ".bench" / run_id
    remote_out = task + "/runs/" + run_id
    archive = f"/mnt/shared/gothic-ai/{task_name}/runs/{run_id}"
    staging = Path.home() / ".local/share/gothic-ai/content-staging"
    sha = {name: tree_sha(staging / name) for name in set((args.a, args.b)) if name != "none"}
    remote("df", "-h", home, "/mnt/shared")
    remote("mkdir", "-p", source + "/OpenGothic", source + "/scripts/bench", task + "/content", remote_out)
    out.mkdir(parents=True)
    def sync(local, destination, *flags):
        subprocess.run(["rsync", "-a", *flags, str(local), f"{host}:{destination}"], check=True)
    sync(str(ROOT / "OpenGothic") + "/", source + "/OpenGothic/", "--delete", "--exclude=/build*/", "--exclude=.git")
    sync(str(ROOT / "scripts/convert") + "/", source + "/scripts/convert/", "--delete", "--exclude=/build*/")
    for name in ("perf-content.sh", "perf_content.py", "perf.sh", "perf_report.py", "ryzen_game.py", "base-ensure.sh"):
        sync(ROOT / "scripts" / name, source + "/scripts/")
    for scenario in (ROOT / "scripts/bench").glob("perf-*.bench"):
        sync(scenario, source + "/scripts/bench/")
    for name, digest in sha.items():
        remote("mkdir", "-p", task + "/content/" + name)
        sync(str(staging / name) + "/", task + "/content/" + name + "/", "--delete")
        actual = remote("python3", source + "/scripts/perf_content.py", "__sha", task + "/content/" + name,
                        capture_output=True, text=True).stdout.strip()
        if actual != digest or tree_sha(staging / name) != digest:
            raise ValueError(f"SHA-256 verification failed: {name}")
        print(f"perf-content: SHA-256 verified {name}: {digest}", flush=True)
    # Copy the selected prepared base into this task. Never import/publish/prune shared bases.
    # A caller's OPENGOTHIC_BASE_FINGERPRINT picks the base on Ryzen as it does for perf.sh and the captures
    # (Ryzen's game-base.json may name a base of another importer format than this branch's engine).
    pinned = [f"OPENGOTHIC_BASE_FINGERPRINT={os.environ['OPENGOTHIC_BASE_FINGERPRINT']}"] \
        if os.environ.get("OPENGOTHIC_BASE_FINGERPRINT") else []
    remote("env", *pinned, "python3", source + "/scripts/perf_content.py", "__base", task)
    commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
    dirty = len(subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"], text=True).splitlines())
    metadata = {"a": args.a, "b": args.b, "places": args.places, "order": list(ORDER),
                "git_commit": commit, "dirty_files": dirty, "content_sha256": sha, "archive": archive,
                "settings": {key: os.environ.get(key, default) for key, default in
                             (("PERF_WINDOW", "1728x1117"), ("PERF_VID_RES", "0"),
                              ("PERF_ARGS", ""), ("PERF_ENV", ""), ("PERF_INI", ""))}}
    (out / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    sync(out / "metadata.json", remote_out + "/")
    environment = ["PERF_REMOTE=1", "OPENGOTHIC_BASE=", f"OPENGOTHIC_BASE_CACHE={task}/base-cache",
                   f"OPENGOTHIC_BASE_STORE={task}/base-store", "OPENGOTHIC_VIDEOS_FINGERPRINT=",
                   f"PERF_COMMIT={commit}", f"PERF_DIRTY={dirty}", *pinned]
    environment += [f"{key}={value}" for key, value in metadata["settings"].items()]
    queue = [str(ROOT / "scripts/gq"), "--host", host, "run"]
    jobs = os.environ.get("RYZEN_JOBS", "8")
    subprocess.run([*queue, "--class", "quick", "--threads", jobs, "--mem", "12G", "--max", "30m",
                    "--wait-max", os.environ.get("RYZEN_LOCK_WAIT", "3600"), "--name", "build content perf stand",
                    "--", "env", *environment, f"RYZEN_JOBS={jobs}", "bash", source + "/scripts/perf.sh",
                    "__build", task + "/build-logs"], check=True)
    result = subprocess.run([*queue, "--class", "quiet", "--threads", "4", "--mem", "5G", "--gpu-mem", "4G",
                             "--display", "--max", f"{5 + len(args.places) * 20}m", "--wait-max",
                             os.environ.get("RYZEN_LOCK_WAIT", "3600"), "--name", "content perf ABBA",
                             "--archive", remote_out, "--", "env", *environment, "python3",
                             source + "/scripts/perf_content.py", "__run", remote_out, task + "/content",
                             args.a, args.b, *args.places, "--noise-pct", str(args.noise_pct),
                             "--threshold-pct", str(args.threshold_pct)])
    # Fetch only small final artifacts; raw attempts stay in the checksum-verified archive.
    for name in ("report.md", "report.json"):
        remote_path = archive if result.returncode == 0 else remote_out
        subprocess.run(["rsync", "-a", f"{host}:{remote_path}/{name}", str(out) + "/"],
                       check=result.returncode == 0)
    print(out, flush=True)
    return result.returncode


def prepare_base(task):
    from ryzen_game import game_environment
    environment = game_environment()
    fingerprint = environment["OPENGOTHIC_BASE_FINGERPRINT"]
    source = Path.home() / ".cache/gothic-ai/base" / fingerprint
    if not (source / "base.json").is_file():
        raise ValueError(f"prepared Ryzen base missing: {source}")
    cache = Path(task) / "base-cache"
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / fingerprint
    if not destination.exists():
        subprocess.run(["cp", "-a", "--reflink=auto", str(source), str(destination)], check=True)


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    try:
        if argv and argv[0] == "__sha":
            print(tree_sha(argv[1]))
            return 0
        if argv and argv[0] == "__base":
            prepare_base(argv[1])
            return 0
        if argv and argv[0] == "__run":
            run_attempts(argv[1], argv[2], parse_args(argv[3:]))
            return 0
        return launch(parse_args(argv))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"perf-content: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
