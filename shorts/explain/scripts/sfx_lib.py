#!/usr/bin/env python3
"""The explainer's sound library: the kinds of sfx.json made from the CC0 packs, ready for the composition.

    python3 scripts/sfx_lib.py [--packs ~/.local/share/gothic-ai/sfx/packs] [--lib ~/.local/share/gothic-ai/sfx/lib]
 -> lib/<kind>-<n>.wav   48 kHz stereo, 16 bit: its peak at the kind's `lead`, the same loudness for every variant
                         of a kind (RMS of its sound), a short fade at both ends, `lowpass` where the kind has one
    lib/index.json       per file: the source, its licence, the peak and the length

The packs are the unpacked downloads of the sources in sfx.json (one folder each, named as its key). Only ffmpeg
runs here; the levels of the mix are set later by gothic_shorts.levels.
"""

import argparse
import json
import math
import struct
import subprocess
from pathlib import Path

ROOT = Path.home() / ".local/share/gothic-ai/sfx"
MANIFEST = Path(__file__).resolve().parents[1] / "sfx.json"
RATE = 48000
RMS_DB = -18.0  # every variant's sound at this RMS (dBFS), so a kind's variants are equal and `gain` alone sets a kind
ONSET_DB = -40.0  # below the peak: where the sound begins and ends
FADE = 0.004  # s where the start or the end cuts into sound, so it does not click
ATTACK = 0.001  # s of a sound's own onset out of silence: soft enough for the click check, short enough for a knock


def decode(path: Path, lowpass: int | None = None) -> list[float]:
    """Mono float samples at RATE."""
    af = ["-af", f"lowpass=f={lowpass}"] if lowpass else []
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), *af, "-ac", "1", "-ar", str(RATE),
                          "-f", "f32le", "-"], check=True, capture_output=True).stdout
    return list(struct.unpack(f"<{len(raw) // 4}f", raw))


def shape(x: list[float], lead: float) -> tuple[list[float], float]:
    """The sound from its onset, its peak moved to `lead` (silence before it, or its start cut with a fade), the
    tail after it falls under ONSET_DB cut, and the RMS of the sound brought to RMS_DB. Returns the samples and the
    gain applied (dB)."""
    peak = max(range(len(x)), key=lambda i: abs(x[i]))
    floor = abs(x[peak]) * 10 ** (ONSET_DB / 20)
    on = next(i for i in range(len(x)) if abs(x[i]) > floor)
    off = max(i for i in range(len(x)) if abs(x[i]) > floor) + 1
    want = round(lead * RATE)
    start = peak - want
    if start < on:  # the peak comes sooner than `lead` after the onset: silence in front
        y = [0.0] * (on - start) + x[on:off]
        first = on - start
    else:  # the peak comes later: start `lead` before it
        y = x[start:off]
        first = 0
    # the sound rises from its first sample over a raised cosine, at most half way to its peak: a knock keeps its
    # attack, but no corner out of silence (gothic_shorts.clicks would stop the mix on it)
    fade_in = max(1, min(round((ATTACK if start < on else FADE) * RATE), (want - first) // 2))
    for i in range(fade_in):
        y[first + i] *= 0.5 - 0.5 * math.cos(math.pi * i / fade_in)
    fade_out = min(round(FADE * RATE), len(y) // 2)
    for i in range(fade_out):
        y[-1 - i] *= i / fade_out
    sound = [v for v in y if v]
    rms = math.sqrt(sum(v * v for v in sound) / len(sound))
    gain = RMS_DB - 20 * math.log10(rms)
    k = 10 ** (gain / 20)
    y = [v * k for v in y]
    # the RMS of a sharp knock is far under its peak: never let a peak pass -1 dBFS
    top = max(abs(v) for v in y)
    if top > 10 ** (-1 / 20):
        k = 10 ** (-1 / 20) / top
        y = [v * k for v in y]
        gain += 20 * math.log10(k)
    return y, gain


def write(path: Path, y: list[float]) -> None:
    pcm = b"".join(struct.pack("<hh", s, s) for s in (max(-32767, min(32767, round(v * 32767))) for v in y))
    header = b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack(
        "<IHHIIHH", 16, 1, 2, RATE, RATE * 4, 4, 16) + b"data" + struct.pack("<I", len(pcm))
    path.write_bytes(header + pcm)


def build(packs: Path, lib: Path, manifest: dict) -> dict:
    lib.mkdir(parents=True, exist_ok=True)
    index = {}
    for kind, spec in manifest["kinds"].items():
        for n, rel in enumerate(spec["files"]):
            source = rel.split("/", 1)[0]
            if source not in manifest["sources"]:
                raise SystemExit(f"{kind}: {rel} is not from a listed source")
            src = packs / rel
            if not src.is_file():
                raise SystemExit(f"{src} is missing: unpack {manifest['sources'][source]['url']} into {packs / source}")
            y, gain = shape(decode(src, spec.get("lowpass")), spec["lead"])
            name = f"{kind}-{n}.wav"
            write(lib / name, y)
            index[name] = {"from": rel, "license": manifest["sources"][source]["license"], "lead": spec["lead"],
                           "length": round(len(y) / RATE, 3), "gain_db": round(gain, 1)}
    (lib / "index.json").write_text(json.dumps(index, indent=1, ensure_ascii=False) + "\n")
    return index


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--packs", type=Path, default=ROOT / "packs")
    p.add_argument("--lib", type=Path, default=ROOT / "lib")
    args = p.parse_args()
    index = build(args.packs.expanduser(), args.lib.expanduser(), json.loads(MANIFEST.read_text()))
    print(f"{len(index)} sounds -> {args.lib}")


if __name__ == "__main__":
    main()
