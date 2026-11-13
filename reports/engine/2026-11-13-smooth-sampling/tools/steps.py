#!/usr/bin/env python3
"""Clips with sparse keys on straight lines and on the curve, measured and filmed (README.md of this folder): one
run of scripts/bench/anim-smooth-clip.bench, where every take is recorded twice, `<take>-line` (sample off, "было")
and `<take>-curve` (sample on, "стало").

  steps.py <bench out> [<out dir>]

Prints the table; with <out dir> also writes there:
  key.txt    the parts of the clip and the table: per take the change of the step of the hands, the head and the
             chest between two frames (an_hand, an_head, an_chest of metrics.csv, cm): the largest and the 95th
             percentile on the lines and on the curve, their ratio against the threshold; for the takes of the
             whole body the slide of a planted foot along the ground, cm a frame;
  steps.png  the change of the hands' step over time, both ways, a panel a gesture;
  clip.mp4   "было | стало" side by side, take by take, the game's sound of the curve's take.
"""
import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

TAKES = [("g3", "Жест 03"), ("g8", "Жест 08"), ("g9", "Жест 09"), ("g6", "Жест 06"),
         ("sword", "Тренировка с мечом (T_1HSFREE)"), ("sit", "Сел и встал (T_STAND_2_SIT, S_SIT)"),
         ("dance", "Танец (T_DANCE_01)")]
GESTURES = ("g3", "g8", "g9", "g6")
LIMIT = 0.65          # the curve's largest change of a step against the lines'
LEAD = 0.5            # seconds of a take before its line starts
FONTS = ["/System/Library/Fonts/Supplemental/Arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"]
YELLOW = (255, 214, 120)


def load(src, seg):
    with open(src / "metrics.csv") as f:
        rows = [r for r in csv.DictReader(f) if r["seg"] == seg]
    if not rows:
        return None
    ev = [e for e in map(json.loads, (src / "events.jsonl").read_text().splitlines()) if e.get("seg") == seg]
    col = lambda c: np.array([float(r[c]) if r.get(c, "") not in ("", "-") else np.nan for r in rows])  # noqa: E731
    out = {"src": src, "seg": seg, "frames": len(rows), "said": [e for e in ev if e["kind"] == "output"],
           "fps": next((e["fps"] for e in ev if e["kind"] == "rec"), 30), "sm": col("sm_on"),
           "layers": sorted({r["an_layers"] for r in rows})}
    for c in ("an_hand", "an_head", "an_chest", "an_hand_v", "an_head_v", "an_pop"):
        out[c] = col(c)[2:]  # the first two frames of a take have no second difference
    # a planted foot along the ground, cm a frame (the final ankle of the foot IK's probe, world)
    slide = []
    for leg in ("l", "r"):
        x, z, plant = col(f"{leg}_ankf_x"), col(f"{leg}_ankf_z"), col(f"{leg}_plant")
        step = np.hypot(np.diff(x), np.diff(z))
        slide.append(step[(plant[1:] > 0.9) & (plant[:-1] > 0.9)])
    out["slide"] = np.concatenate(slide)
    return out


def stat(x):
    x = x[~np.isnan(x)]
    return (float(x.max()), float(np.percentile(x, 95))) if len(x) else (float("nan"), float("nan"))


def table(pairs):
    lines = ["| дубль | кисть на прямых: наиб. / p95, см | на кривой | отношение наиб. | голова: прямые → кривая (отношение) |"
             " грудь: прямые → кривая (отношение) | скорость кисти, см/с: прямые / кривая | порог 0,65 |",
             "|---|---|---|---|---|---|---|---|"]
    ok = True
    for seg, title, a, b in pairs:
        h0, h1 = stat(a["an_hand"]), stat(b["an_hand"])
        d0, d1 = stat(a["an_head"]), stat(b["an_head"])
        c0, c1 = stat(a["an_chest"]), stat(b["an_chest"])
        ratio = [h1[0] / h0[0], d1[0] / d0[0], c1[0] / c0[0]]
        verdict = "—"
        if seg in GESTURES:
            good = max(ratio) <= LIMIT
            ok = ok and good
            verdict = "да" if good else "нет"
        lines.append(f"| {title} | {h0[0]:.2f} / {h0[1]:.2f} | {h1[0]:.2f} / {h1[1]:.2f} | {ratio[0]:.2f} |"
                     f" {d0[0]:.2f} → {d1[0]:.2f} ({ratio[1]:.2f}) | {c0[0]:.2f} → {c1[0]:.2f} ({ratio[2]:.2f}) |"
                     f" {np.nanmax(a['an_hand_v']):.0f} / {np.nanmax(b['an_hand_v']):.0f} | {verdict} |")
    lines += ["", "Стопы: скольжение опорной стопы по земле за кадр, см (кадры, где стопа опорная два кадра подряд).", "",
              "| дубль | на прямых: средн. / p95 / наиб. | на кривой | опорных кадров: прямые / кривая |", "|---|---|---|---|"]
    for seg, title, a, b in pairs:
        s0, s1 = a["slide"], b["slide"]
        if len(s0) < 10 or len(s1) < 10:
            continue
        lines.append(f"| {title} | {s0.mean():.2f} / {np.percentile(s0, 95):.2f} / {s0.max():.2f} |"
                     f" {s1.mean():.2f} / {np.percentile(s1, 95):.2f} / {s1.max():.2f} | {len(s0)} / {len(s1)} |")
    return lines, ok


def font(size):
    from PIL import ImageFont
    for f in FONTS:
        if Path(f).exists():
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def caption(tmp, n, title, size):
    from PIL import Image, ImageDraw
    w, h = size
    im = Image.new("RGBA", (2 * w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    f = font(34)
    for x, text in ((40, "было: прямые между ключами"), (w + 40, "стало: кривая через ключи")):
        d.rectangle((x - 12, 24, x + d.textlength(text, font=f) + 12, 76), fill=(0, 0, 0, 150))
        d.text((x, 30), text, fill=(255, 255, 255, 255), font=f)
    d.rectangle((28, h - 70, 52 + d.textlength(title, font=f), h - 18), fill=(0, 0, 0, 150))
    d.text((40, h - 64), title, fill=YELLOW, font=f)
    cap = tmp / f"cap{n}.png"
    im.save(cap)
    return cap


def part(a, b, tmp, n, title, size):
    """A take of both ways side by side, from LEAD before the line starts (a take without a line: from its start)."""
    fps = b["fps"]
    t_l = max(0.0, (a["said"][0]["frame"] if a["said"] else 0) / fps - LEAD)
    t_r = max(0.0, (b["said"][0]["frame"] if b["said"] else 0) / fps - LEAD)
    dur = min(a["frames"] / fps - t_l, b["frames"] / fps - t_r) - 0.05
    w, h = size
    out = tmp / f"part{n}.mp4"
    wav = b["src"] / f"{b['seg']}.wav"
    cmd = ["ffmpeg", "-v", "error", "-y", "-ss", f"{t_l:.3f}", "-t", f"{dur:.3f}", "-i", str(a["src"] / f"{a['seg']}.mp4"),
           "-ss", f"{t_r:.3f}", "-t", f"{dur:.3f}", "-i", str(b["src"] / f"{b['seg']}.mp4"),
           "-i", str(caption(tmp, n, title, size))]
    if wav.exists():
        cmd += ["-ss", f"{t_r:.3f}", "-t", f"{dur:.3f}", "-i", str(wav)]
    else:
        cmd += ["-f", "lavfi", "-t", f"{dur:.3f}", "-i", "anullsrc=r=48000:cl=stereo"]
    cmd += ["-filter_complex", f"[0:v]scale={w}:{h},setsar=1[l];[1:v]scale={w}:{h},setsar=1[r];"
            "[l][r]hstack=inputs=2[s];[s][2:v]overlay=0:0[v]", "-map", "[v]", "-map", "3:a", "-r", str(fps),
            "-c:v", "libx264", "-crf", "17", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ar", "48000", "-ac", "2",
            "-b:a", "160k", "-shortest", str(out)]
    subprocess.run(cmd, check=True)
    return out, dur


def plot(pairs, out):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    pairs = [p for p in pairs if p[0] in GESTURES]
    fig, axes = plt.subplots(len(pairs), 1, figsize=(12, 2.5 * len(pairs)), constrained_layout=True, squeeze=False)
    for ax, (_, title, a, b) in zip(axes[:, 0], pairs):
        for name, r, color in (("прямые (было)", a, "#9a9a9a"), ("кривая (стало)", b, "#1f6fb4")):
            t0 = r["said"][0]["frame"] / r["fps"] if r["said"] else 0
            ax.plot((np.arange(len(r["an_hand"])) + 2) / r["fps"] - t0, r["an_hand"], color=color, lw=1.1, label=name)
        ax.set_title(f"{title}: изменение шага кисти между кадрами, см", fontsize=10)
        ax.set_xlabel("с от начала реплики")
        ax.grid(alpha=0.3)
        ax.legend(loc="upper right")
    fig.savefig(out, dpi=110)


def main(argv):
    if not argv:
        sys.exit(__doc__)
    src = Path(argv[0])
    pairs = []
    for seg, title in TAKES:
        a, b = load(src, f"{seg}-line"), load(src, f"{seg}-curve")
        if a is None or b is None:
            continue
        if np.nanmax(a["sm"]) != 0 or np.nanmin(b["sm"]) != 1:
            sys.exit(f"{seg}: the takes are not one on the lines and one on the curve (sm_on)")
        pairs.append((seg, title, a, b))
    lines, ok = table(pairs)
    key = ["Клипы с редкими ключами: прямые между ключами и кривая через ключи "
           "(reports/engine/2026-11-13-smooth-sampling). Стенд anim-smooth-clip.bench, оба варианта в одном прогоне.",
           f"прогон — {src}", ""]
    if len(argv) > 1:
        out = Path(argv[1])
        out.mkdir(parents=True, exist_ok=True)
        probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height",
                                "-of", "csv=p=0", str(src / f"{pairs[0][0]}-line.mp4")], capture_output=True, text=True, check=True)
        size = tuple(int(v) for v in probe.stdout.strip().split(","))
        with tempfile.TemporaryDirectory() as t:
            tmp = Path(t)
            parts = [part(a, b, tmp, n, title, size) for n, (_, title, a, b) in enumerate(pairs)]
            (tmp / "list.txt").write_text("".join(f"file '{p}'\n" for p, _ in parts))
            subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "concat", "-safe", "0", "-i", str(tmp / "list.txt"),
                            "-c", "copy", str(out / "clip.mp4")], check=True)
        key.append("clip.mp4: слева «было» (прямые), справа «стало» (кривая); звук игры — дубля «стало».")
        at = 0.0
        for (_, title, _, _), (_, dur) in zip(pairs, parts):
            key.append(f"  {at:6.2f} с — {title}, {dur:.2f} с")
            at += dur
        key.append("")
        plot(pairs, out / "steps.png")
    key += ["Изменение шага сустава между двумя кадрами (вторая разность положения в пространстве модели), см: "
            "an_hand — кисти (большая из двух), an_head — голова, как её несёт корпус, an_chest — грудь.", ""] + lines
    key += ["", "порог по жестам выдержан" if ok else "порог по жестам НЕ выдержан"]
    if len(argv) > 1:
        (Path(argv[1]) / "key.txt").write_text("\n".join(key) + "\n")
    print("\n".join(key))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main(sys.argv[1:])
