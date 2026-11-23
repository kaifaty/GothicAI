#!/usr/bin/env python3
"""Checks a running gothic-a2f from any machine; the standard library only.

    python3 a2f-service/check.py <url> <line.wav> [emotion] [--csv a2f-weights.csv]

The wav is PCM 16-bit mono at any rate from 8 to 48 kHz. Checks: /health; the line's track (frames =
duration x 30 +-1, weights 0..1, 52 shapes); the refusals (empty body, bad rate, unknown emotion, strength
out of 0..1, 31 s of audio). With --csv, the weights of reports/content/2026-11-hero-head/a2f/a2f-weights.cpp
for the same wav and emotion (its label as name=strength): every second row of the CSV against the track;
the mean difference must stay within the quantisation of a byte (1/255).
"""

import argparse
import base64
import csv
import json
import sys
import time
import urllib.error
import urllib.request
import wave

FPS = 30
SHAPES = 52


def request(url: str, body: bytes | None = None) -> tuple[int, dict, float]:
    req = urllib.request.Request(url, data=body, method="POST" if body is not None else "GET")
    if body is not None:
        req.add_header("Content-Type", "application/octet-stream")
    started = time.perf_counter()
    try:
        with urllib.request.urlopen(req, timeout=60) as response:
            status, raw = response.status, response.read()
    except urllib.error.HTTPError as error:
        status, raw = error.code, error.read()
    return status, json.loads(raw or b"{}"), (time.perf_counter() - started) * 1000


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("url", help="the service, e.g. http://host:47839")
    parser.add_argument("wav")
    parser.add_argument("emotion", nargs="?", default="", help="e.g. anger:0.8,disgust:0.3")
    parser.add_argument("--csv", help="a2f-weights output for the same wav and emotion")
    args = parser.parse_args()
    url = args.url.rstrip("/")
    failures = []

    def check(ok: bool, text: str) -> None:
        print(("ok   " if ok else "FAIL ") + text)
        if not ok:
            failures.append(text)

    status, health, _ = request(f"{url}/health")
    healthy = status == 200 and health.get("ok") is True and health.get("fps") == FPS
    check(healthy, f"health {status} {health}")

    with wave.open(args.wav) as w:
        if w.getsampwidth() != 2 or w.getnchannels() != 1:
            parser.error("the wav must be 16-bit mono")
        rate, pcm = w.getframerate(), w.readframes(w.getnframes())
    seconds = len(pcm) / 2 / rate
    query = f"rate={rate}" + (f"&emotion={args.emotion}" if args.emotion else "")
    status, track, wall = request(f"{url}/v1/face?{query}", pcm)
    check(status == 200, f"track {status} {track.get('error', '')}")
    if status != 200:
        return 1
    data = base64.b64decode(track["data"])
    frames = track["frames"]
    shapes = track["shapes"]
    check(track["fps"] == FPS and len(shapes) == SHAPES and len(set(shapes)) == SHAPES,
          f"fps {track['fps']}, {len(shapes)} shapes")
    check(len(data) == frames * len(shapes), f"data {len(data)} bytes = {frames} frames x {len(shapes)}")
    check(abs(frames - seconds * FPS) <= 1,
          f"{frames} frames for {seconds:.2f} s (expected {seconds * FPS:.1f} +-1)")
    weights = [b / 255 for b in data]  # bytes are 0..255, so weights are 0..1 by construction
    print(f"     audio {seconds:.2f} s: computed in {track['ms']} ms, request {wall:.0f} ms; "
          f"max weight {max(weights):.3f}")

    silence = b"\0\0" * (16000 * 31)
    for name, query, body, expected in (
        ("empty body", "rate=16000", b"", 400),
        ("rate below 8000", "rate=7000", b"\0\0" * 100, 400),
        ("rate above 48000", "rate=96000", b"\0\0" * 100, 400),
        ("unknown emotion", "rate=16000&emotion=rage:0.5", b"\0\0" * 100, 400),
        ("strength above 1", "rate=16000&emotion=anger:1.5", b"\0\0" * 100, 400),
        ("31 s of audio", "rate=16000", silence, 413),
    ):
        status, body_json, _ = request(f"{url}/v1/face?{query}", body)
        check(status == expected and bool(body_json.get("error")), f"refuses {name}: {status} {body_json}")

    if args.csv:
        with open(args.csv, newline="") as f:
            rows = [[float(v) for v in row[1:SHAPES + 1]] for row in csv.reader(f)]
        reference = rows[::2]  # 60 fps of the network -> frames 0, 2, 4 ...
        n = min(len(reference), frames)
        check(abs(len(reference) - frames) <= 1, f"{frames} frames, the CSV has {len(reference)} at 30 fps")
        diffs = [abs(weights[i * SHAPES + k] - reference[i][k]) for i in range(n) for k in range(SHAPES)]
        mean, worst = sum(diffs) / len(diffs), max(diffs)
        share = sum(d > 1 / 255 for d in diffs) / len(diffs)
        check(mean <= 1 / 255, f"a2f-weights: mean difference {mean:.5f} (byte {1 / 255:.5f}), "
              f"max {worst:.4f}, {share:.2%} of weights differ by more than a byte")

    print("passed" if not failures else f"{len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
