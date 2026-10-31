"""Deterministic balance, voiced mastering and publication gates for shorts.

Every measurement taps the very same stem graph used by the mix. Word timings,
not signal dynamics, control ducking. All times refer to the joined video.
"""

from __future__ import annotations

import functools
import json
import math
import re
import subprocess
import tempfile
from collections.abc import Callable
from dataclasses import dataclass, field, replace
from pathlib import Path

from gothic_shorts import clicks

DUAL_MONO = "pan=stereo|c0=0.5*c0+0.5*c1|c1=0.5*c0+0.5*c1"
DUAL_MONO_MAX_DIFF_DB = -90.0
AAC_FALLBACK = ("-c:a", "aac", "-b:a", "256k", "-aac_pns", "0", "-aac_is", "0", "-aac_ms", "1")
BASE_LUFS = -14.0
TRUE_PEAK_DBTP = -1.0
# R128 prints linear peaks to 0.001. Scale by a power of two before metering,
# then undo it and include the rounding bound (under 0.0000001 dB at the ceiling).
TRUE_PEAK_METER_GAIN = 65536
TRUE_PEAK_METADATA_HALF_STEP = 0.0005
VOICE_TOLERANCE_LU = 0.5
MASTER_CORRECTION_TOLERANCE_LU = 0.1
MAX_MASTER_CORRECTIONS = 2
MAX_BALANCE_CORRECTIONS = 2
CEILING_HEADROOM_LU = 0.5
# -3 dBFS: AAC and intersample headroom under the -1 dBTP ceiling (sharp spell attacks overshot it at 0.79)
PEAK_LIMIT = 0.71
# A sub-threshold pilot follows the limiter's shared gain without influencing it.
LIMITER_PILOT = 0.01
# Keep a quiet bed that is still audible; at S−14 with another −6 dB under
# speech it disappeared behind the voice. Music stays at least 3 LU below effects.
MUSIC_BELOW_VOICE_LU = 11.0
MUSIC_BELOW_GAME_LU = 3.0
MUSIC_DUCK_DB = 3.0
# Allow extra speech-window energy without extreme ducking
# when a pause is unusually quiet or unrepresentative of the music bed.
MAX_MUSIC_DUCK_DB = 18.0
# Keep effects well below the voice: at S−6 / S−3 they competed with it.
# The ending's quill stays at the same level as effects in pauses.
GAME_BELOW_VOICE_LU = 12.0
GAME_PAUSE_BELOW_VOICE_LU = 8.0
ENDING_BELOW_VOICE_LU = GAME_PAUSE_BELOW_VOICE_LU
ENDING_CHECK_BELOW_VOICE_LU = ENDING_BELOW_VOICE_LU - 1.0
LEVEL_TOLERANCE_LU = 1.5
RAMP_SECONDS = 0.150
# ebur128's summary is rounded to tenths; keep game gain on the safe side.
METER_RESOLUTION_LU = 0.1

Window = tuple[float, float]


class LevelsError(ValueError):
    """A voiced short failed calibration, rendering or a publication gate."""


def merge_speech_windows(windows: list[Window], duration: float) -> list[Window]:
    """Merge touching/overlapping speech intervals and clip them to the body."""
    if not math.isfinite(duration) or duration < 0:
        raise ValueError("Invalid speech duration")
    if any(not math.isfinite(start) or not math.isfinite(end) or end <= start
           for start, end in windows):
        raise ValueError("Invalid speech window")
    merged: list[Window] = []
    for start, end in sorted(windows):
        start, end = max(0.0, start), min(duration, end)
        if end <= start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def speech_windows(plan: dict) -> tuple[list[Window], float]:
    """First to last word of each line, clipped to the body (never the sting)."""
    windows, offset = [], 0.0
    for beat in plan["beats"]:
        for line in beat.get("lines", []):
            words = line["words"]
            if not words:
                raise ValueError("A voiced line needs word timings")
            at = offset + line["at"] - beat["start"]
            start, end = at + words[0][1], at + words[-1][2]
            if not math.isfinite(start) or not math.isfinite(end) or end <= start:
                raise ValueError("Invalid speech window")
            windows.append((start, end))
        offset += beat["end"] - beat["start"]
    return merge_speech_windows(windows, offset), offset


def pause_windows(speech: list[Window], duration: float) -> list[Window]:
    pauses, cursor = [], 0.0
    for start, end in merge_speech_windows(speech, duration):
        if start > cursor:
            pauses.append((cursor, start))
        cursor = end
    if cursor < duration:
        pauses.append((cursor, duration))
    return pauses


def music_windows(speech: list[Window], duration: float,
                  fade_durations: tuple[float, float]) -> tuple[list[Window], list[Window], bool]:
    """Select music plateaus; retain original pauses if fewer than 400 ms remain.

    Only music measurements use these windows. A boundary next to speech loses
    its duck ramp, while the body's outer boundaries lose only the music fades.
    """
    fade_in, fade_out = fade_durations
    if any(not math.isfinite(value) or value < 0 for value in fade_durations):
        raise ValueError("Music fade durations must be finite and non-negative")
    speech = merge_speech_windows(speech, duration)
    pauses = pause_windows(speech, duration)

    def clip(windows: list[Window]) -> list[Window]:
        return [(max(start, fade_in), min(end, duration - fade_out))
                for start, end in windows
                if min(end, duration - fade_out) > max(start, fade_in)]

    music_speech = clip(speech)
    music_pauses = clip([(start + (RAMP_SECONDS if start > 0 else 0),
                          end - (RAMP_SECONDS if end < duration else 0))
                         for start, end in pauses])
    fallback = bool(pauses) and sum(end - start for start, end in music_pauses) < 0.4 - 1e-6
    return music_speech, pauses if fallback else music_pauses, fallback


def duck_filter(windows: list[Window], attenuation_db: float) -> str:
    """Full attenuation throughout each word window, ramps in the adjacent pauses.

    The maximum of overlapping ramps avoids a gain jump between nearby lines.
    Fixed 1-ms audio frames bound timing error independently of input packet sizes.
    """
    if not windows or attenuation_db <= 0:
        return "anull"
    ramps = [
        f"max(0,min(1,min((t-({start:.9f})+{RAMP_SECONDS})/{RAMP_SECONDS},"
        f"(({end:.9f})+{RAMP_SECONDS}-t)/{RAMP_SECONDS})))"
        for start, end in windows
    ]
    # A balanced tree of max(): ffmpeg's expression parser has a depth limit, so a chain of one max per
    # line fails on a long video (137 lines); the tree's depth grows with log2 of the line count.
    while len(ramps) > 1:
        ramps = [f"max({a},{b})" for a, b in zip(ramps[::2], ramps[1::2])] + ramps[len(ramps) - len(ramps) % 2:]
    envelope = ramps[0]
    # Linear amplitude ramps; the plateau is exactly the requested dB reduction.
    gain = 10 ** (-attenuation_db / 20)
    return (f"aresample=48000,asetnsamples=n=48:p=0,"
            f"volume='1-({1 - gain:.12f})*({envelope})':eval=frame")


def body_gain_filter(gain_db: float, total: float, ending_gain_db: float = 0.0) -> str:
    """Lower gain in the body's fade; raise it only once the ending has begun.

    Raising gain during the body's 60 ms fade can overpower that fade and make
    the last effects louder than their calibrated level. Keep that body gain
    until the boundary, then ramp up over the ending's first 60 ms instead.
    """
    if gain_db == ending_gain_db:
        return "anull" if gain_db == 0 else f"volume={gain_db:.9f}dB"
    gain, ending = 10 ** (gain_db / 20), 10 ** (ending_gain_db / 20)
    start, end = ((total, total + 0.06) if ending_gain_db > gain_db
                  else (max(0.0, total - 0.06), total))
    return (f"aresample=48000,asetnsamples=n=48:p=0,"
            f"volume='if(lt(t,{start:.9f}),{gain:.12f},if(lt(t,{end:.9f}),"
            f"{gain:.12f}+({ending - gain:.12f})*(t-{start:.9f})/{end - start:.9f},"
            f"{ending:.12f}))':eval=frame")


def measure(inputs: list[str], graph: str, output: str,
            windows: list[Window] | None = None, *, meter_gain_db: float = 0.0) -> float:
    """EBU R128 integrated LUFS after exact sample trims, before any mastering.

    Other outputs of graph must already be consumed. Disjoint windows are joined
    directly, then 100 ms of meter-only silence flushes the final R128 block.
    Empty/silent selections return -inf; explicit selections shorter than an
    R128 block are rejected rather than silently passing a check.
    The common mastering gain usually clears R128's absolute gate. If it does
    not, normalize the selected signal's measured peak for one metering retry.
    Subtract every measurement-only gain to preserve the stem's actual level.
    """
    if windows is not None:
        if not windows:
            return -math.inf
        if sum(end - start for start, end in windows) < 0.4 - 1e-6:
            raise ValueError("At least 400 ms of audio is needed for an EBU R128 measurement")
        branches = "".join(f"[level_split{i}]" for i in range(len(windows)))
        graph += f";[{output}]asplit={len(windows)}{branches}"
        for i, (start, end) in enumerate(windows):
            graph += (f";[level_split{i}]atrim=start={start:.9f}:end={end:.9f},"
                      f"asetpts=PTS-STARTPTS[level_piece{i}]")
        graph += ";" + "".join(f"[level_piece{i}]" for i in range(len(windows)))
        graph += f"concat=n={len(windows)}:v=0:a=1[level_selection]"
        output = "level_selection"

    def meter(gain_db: float) -> tuple[float, str]:
        gain = f"volume={gain_db:.9f}dB:precision=double," if gain_db else ""
        metering_graph = f"{graph};[{output}]{gain}apad=pad_dur=0.1,ebur128=peak=true[level_meter]"
        result = subprocess.run(
            ["ffmpeg", "-hide_banner", "-nostats", *inputs, "-filter_complex", metering_graph,
             "-map", "[level_meter]", "-vn", "-f", "null", "-"],
            check=True, capture_output=True, text=True,
        )
        readings = re.findall(r"\bI:\s*(-?\d+(?:\.\d+)?|[-+]?inf)\s+LUFS", result.stderr)
        if not readings:
            raise ValueError("ffmpeg did not return an EBU R128 integrated loudness")
        return float(readings[-1]), result.stderr

    value, diagnostic = meter(meter_gain_db)
    if value > -70 and math.isfinite(value):
        return value - meter_gain_db
    peaks = re.findall(r"\bPeak:\s*(-?\d+(?:\.\d+)?|[-+]?inf)\s+dBFS", diagnostic)
    peak = float(peaks[-1]) if peaks else -math.inf
    if not math.isfinite(peak):
        return -math.inf  # Actual silence cannot be made measurable by amplification.
    extra_gain = max(0.0, -10.0 - peak)
    if not extra_gain:
        return -math.inf
    gain = meter_gain_db + extra_gain
    value, _ = meter(gain)
    return value - gain if value > -70 and math.isfinite(value) else -math.inf


def music_target(speech_lufs: float, game_pause_lufs: float | None = None) -> float:
    if not math.isfinite(speech_lufs):
        raise ValueError("Voice must have measurable EBU R128 loudness")
    target = speech_lufs - MUSIC_BELOW_VOICE_LU
    # A silent game stem (gameVolume 0) measures -inf: there is no game to stay under, not a muted bed.
    if game_pause_lufs is None or not math.isfinite(game_pause_lufs):
        return target
    return min(target, game_pause_lufs - MUSIC_BELOW_GAME_LU)


def music_gain(speech_lufs: float, music_pause_lufs: float,
               game_pause_lufs: float | None = None) -> float:
    if not math.isfinite(music_pause_lufs):
        raise ValueError("Music pauses must have measurable EBU R128 loudness")
    return music_target(speech_lufs, game_pause_lufs) - music_pause_lufs


def _finite(value: float | None) -> float | None:
    return value if value is not None and math.isfinite(value) else None


def check_balance(speech_lufs: float | None, music_pause_lufs: float | None,
                  music_speech_lufs: float | None, game_speech_lufs: float | None,
                  game_pause_lufs: float | None, ending_lufs: float | None = None) -> dict:
    """The table is also the machine-readable publication gate; None means absent."""
    voiced = speech_lufs is not None
    s = _finite(speech_lufs)
    rows = []

    def row(key: str, value: float | None, target: float | None, description: str,
            ok: bool | None) -> None:
        rows.append({"id": key, "measured_lufs": _finite(value), "target_lufs": _finite(target),
                     "target": description, "ok": ok})

    row("voice", speech_lufs, None, "S (reference)", s is not None if voiced else None)
    target = music_target(s, game_pause_lufs) if s is not None else None
    for key, value, duck in (("music_pause", music_pause_lufs, 0),
                             ("music_speech", music_speech_lufs, MUSIC_DUCK_DB)):
        goal = target - duck if target is not None else None
        ok = (value == goal or math.isfinite(value) and abs(value - goal) <= LEVEL_TOLERANCE_LU + 1e-6
              if value is not None and goal is not None else None)
        description = (f"min(S - {MUSIC_BELOW_VOICE_LU:g}, game_pause - {MUSIC_BELOW_GAME_LU:g})"
                       f" - {duck:g} LU ± {LEVEL_TOLERANCE_LU:g}"
                       if voiced else "measured")
        row(key, value, goal, description, ok)
    for key, value, below in (("game_speech", game_speech_lufs, GAME_BELOW_VOICE_LU),
                              ("game_pause", game_pause_lufs, GAME_PAUSE_BELOW_VOICE_LU),
                              ("ending", ending_lufs, ENDING_CHECK_BELOW_VOICE_LU)):
        goal = s - below if s is not None else None
        row(key, value, goal, f"≤ S - {below:g} LU" if voiced else "measured",
            value <= goal + 1e-6 if value is not None and goal is not None else None)
    for key, value, reference, name, below, tolerance in (
        ("game_below_voice", game_pause_lufs, s, "voice", GAME_PAUSE_BELOW_VOICE_LU, 0),
        ("music_below_game", music_pause_lufs, game_pause_lufs, "game_pause",
         MUSIC_BELOW_GAME_LU, LEVEL_TOLERANCE_LU),
    ):
        # A silent game stem (-inf) leaves nothing for the music to stay under: the row does not apply.
        goal = reference - below if reference is not None and math.isfinite(reference) else None
        allowance = f" + {tolerance:g}" if tolerance else ""
        row(key, value, goal, f"≤ {name} - {below:g} LU{allowance}" if voiced else "measured",
            value <= goal + tolerance + 1e-6
            if voiced and value is not None and goal is not None else None)
    return {"voiced": voiced, "ok": all(r["ok"] is not False for r in rows), "rows": rows}


def check_mastering(report: dict, voice_lufs: float, true_peak_dbtp: float,
                    click_times: list[float]) -> None:
    report["rows"].extend([
        {"id": "voice_mastered", "measured_lufs": _finite(voice_lufs), "target_lufs": BASE_LUFS,
         "target": f"{BASE_LUFS:g} LUFS ± {VOICE_TOLERANCE_LU:g}",
         "ok": math.isfinite(voice_lufs) and abs(voice_lufs - BASE_LUFS) <= VOICE_TOLERANCE_LU},
        {"id": "true_peak", "measured_dbtp": _finite(true_peak_dbtp), "target_dbtp": TRUE_PEAK_DBTP,
         "target": f"≤ {TRUE_PEAK_DBTP:g} dBTP (AAC)",
         "ok": math.isfinite(true_peak_dbtp) and true_peak_dbtp <= TRUE_PEAK_DBTP},
        {"id": "clicks", "count": len(click_times), "times_seconds": click_times,
         "target": "0 clicks (AAC)", "ok": not click_times},
    ])
    report["ok"] = all(row["ok"] is not False for row in report["rows"])


def dual_mono_peak(path: Path) -> float:
    """Maximum decoded L - R in dBFS; volumedetect's silence floor is -91 dB."""
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", "-i", str(path), "-vn", "-af",
         "pan=mono|c0=c0-c1,volumedetect", "-f", "null", "-"],
        check=True, capture_output=True, text=True,
    )
    reading = re.search(r"max_volume:\s*(-?\d+(?:\.\d+)?)\s+dB", result.stderr)
    if not reading:
        raise ValueError("ffmpeg did not return the decoded L - R peak")
    return float(reading[1])


def check_dual_mono(report: dict, path: Path) -> None:
    difference_db = dual_mono_peak(path)
    report["rows"].append({
        "id": "dual_mono", "measured_db": difference_db, "target_db": DUAL_MONO_MAX_DIFF_DB,
        "target": f"L - R ≤ {DUAL_MONO_MAX_DIFF_DB:g} dBFS (AAC)",
        "ok": difference_db <= DUAL_MONO_MAX_DIFF_DB,
    })
    report["ok"] = all(row["ok"] is not False for row in report["rows"])


def check_music_full_windows(report: dict, *, music_pause_lufs: float | None = None,
                             music_speech_lufs: float | None = None) -> dict:
    """Fades may be quieter than the plateau target, but must not exceed its ceiling."""
    rows = {row["id"]: row for row in report["rows"]}
    for key, value in (("music_pause", music_pause_lufs), ("music_speech", music_speech_lufs)):
        if value is None:
            continue
        goal = rows[key]["target_lufs"]
        report["rows"].append({
            "id": f"{key}_full", "measured_lufs": _finite(value), "target_lufs": goal,
            "target": f"≤ {key} target + {LEVEL_TOLERANCE_LU:g} LU (full windows)",
            "ok": value <= goal + LEVEL_TOLERANCE_LU + 1e-6 if goal is not None else None,
        })
    report["ok"] = all(row["ok"] is not False for row in report["rows"])
    return report


def check_mastered_balance(stems: dict[str, Path], speech_lufs: float,
                           speech: list[Window], pauses: list[Window], ending: list[Window],
                           music_speech: list[Window] | None = None,
                           music_pauses: list[Window] | None = None) -> dict:
    """Validate decoded AAC stems under the actual mix limiter, using the final voice as S."""
    def reading(track: str, windows: list[Window]) -> float | None:
        if track not in stems or not windows:
            return None
        return measure(["-i", str(stems[track])], "[0:a]anull[stem]", "stem", windows)

    music_speech = speech if music_speech is None else music_speech
    music_pauses = pauses if music_pauses is None else music_pauses
    report = check_balance(speech_lufs, reading("music", music_pauses), reading("music", music_speech),
                           reading("game", speech), reading("game", pauses), reading("game", ending))
    return check_music_full_windows(
        report,
        music_pause_lufs=reading("music", pauses) if pauses != music_pauses else None,
        music_speech_lufs=reading("music", speech) if speech != music_speech else None,
    )


def print_balance(report: dict) -> None:
    print(f"{'Track / window':<20} {'Measured':>10} {'Target':>12}  Rule / result")
    for row in report["rows"]:
        value = row.get("measured_lufs", row.get("measured_dbtp", row.get("measured_db", row.get("count"))))
        target = row.get("target_lufs", row.get("target_dbtp", row.get("target_db")))
        precision = 6 if "measured_dbtp" in row else 1
        value = "—" if value is None else f"{value:.{precision}f}"
        target = "—" if target is None else f"{target:.1f}"
        result = "ок" if row["ok"] else "нет" if row["ok"] is False else "—"
        print(f"{row['id']:<20} {value:>10} {target:>12}  {row['target']}  {result}")
        if row.get("times_seconds"):
            print("  Clicks at (s): " + ", ".join(f"{at:.6f}" for at in row["times_seconds"]))


def master_gain(speech_lufs: float) -> float:
    if not math.isfinite(speech_lufs):
        raise ValueError("The voice has no measurable loudness for mastering")
    return BASE_LUFS - speech_lufs


def master_correction(voice_lufs: float) -> float:
    """Correct decoded voice loudness; leave an unavailable reading to the final gate."""
    if not math.isfinite(voice_lufs):
        return 0.0
    correction = BASE_LUFS - voice_lufs
    return correction if abs(correction) > MASTER_CORRECTION_TOLERANCE_LU + 1e-6 else 0.0


def ending_correction(voice_lufs: float, ending_lufs: float | None) -> float:
    """Recalibrate the ending after common gain corrections and actual voice limiting."""
    if ending_lufs is None or not all(math.isfinite(value) for value in (voice_lufs, ending_lufs)):
        return 0.0
    correction = voice_lufs - ENDING_BELOW_VOICE_LU - CEILING_HEADROOM_LU - ending_lufs
    return correction if abs(correction) > MASTER_CORRECTION_TOLERANCE_LU + 1e-6 else 0.0


def balance_correction(balance: dict, *, music_duck_db: float = MUSIC_DUCK_DB) -> dict[str, float]:
    """Lower decoded effects and retarget music at the fixed common mastering gain."""
    readings = {row["id"]: row["measured_lufs"] for row in balance["rows"]}
    correction = {"game_gain_db": 0.0, "game_duck_db": 0.0,
                  "music_gain_db": 0.0, "music_duck_db": 0.0}
    s = readings["voice"]
    if s is None or not math.isfinite(s):
        return correction
    pause, speech = readings["game_pause"], readings["game_speech"]
    if pause is not None and math.isfinite(pause):
        correction["game_gain_db"] = min(0.0, s - GAME_PAUSE_BELOW_VOICE_LU - CEILING_HEADROOM_LU - pause)
        pause += correction["game_gain_db"]
    if speech is not None and math.isfinite(speech):
        # Body attenuation already lowers speech; duck only its remaining excess.
        speech += correction["game_gain_db"]
        correction["game_duck_db"] = max(0.0, speech - (s - GAME_BELOW_VOICE_LU - CEILING_HEADROOM_LU))
    target = music_target(s, pause)
    measured = readings["music_pause"]
    if measured is not None and math.isfinite(measured):
        difference = target - measured
        if abs(difference) > LEVEL_TOLERANCE_LU + 1e-6:
            correction["music_gain_db"] = difference
    measured = readings["music_speech"]
    if measured is not None and math.isfinite(measured):
        # The pause gain also changes speech: correct its remaining error with
        # duck depth in this same pass, without pulling pauses off their target.
        difference = measured + correction["music_gain_db"] - (target - MUSIC_DUCK_DB)
        if abs(difference) > LEVEL_TOLERANCE_LU + 1e-6:
            depth = min(MAX_MUSIC_DUCK_DB, max(0.0, music_duck_db + difference))
            correction["music_duck_db"] = depth - music_duck_db
    return {key: value if abs(value) > 1e-6 else 0.0 for key, value in correction.items()}


def master_filter(speech_lufs: float, length: float, *, voice_probe: bool = False,
                  gain_correction_db: float = 0.0) -> str:
    pilot = f"aeval='val(0)|val(1)|{LIMITER_PILOT}':c=2.1," if voice_probe else ""
    return (f"volume={master_gain(speech_lufs) + gain_correction_db:.9f}dB,{pilot}"
            f"alimiter=limit={PEAK_LIMIT}:level=false:latency=true,"
            f"afade=t=out:st={length - 0.08:.3f}:d=0.08")


@functools.lru_cache(maxsize=1)
def aac_encoder() -> tuple[str, ...]:
    """The published AAC: AudioToolbox's where ffmpeg has it (macOS), else ffmpeg's own at 256k.

    ffmpeg's own encoder at 192k turned one voice transient of an AI take into a lone +0.15 dBFS
    sample, 3 dB over the limiter (-2.50 dBTP before it); at 256k, and with AudioToolbox, it stays
    near -2.6. Disable PNS and intensity stereo and force M/S to preserve identical channels on noise.
    Popen keeps the probe out of tests that replace subprocess.run.
    """
    with subprocess.Popen(["ffmpeg", "-hide_banner", "-encoders"], stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, text=True) as probe:
        listing = probe.communicate()[0]
    if re.search(r"^\s*A\S*\s+aac_at\b", listing, re.MULTILINE):
        return ("-c:a", "aac_at", "-b:a", "192k")
    return AAC_FALLBACK


def meter_file(path: Path) -> tuple[float, float]:
    """Meter original loudness and a conservative, precise peak including the tail."""
    graph = ("ebur128@loudness=framelog=quiet,"
             f"volume={TRUE_PEAK_METER_GAIN}:precision=double,apad=pad_dur=0.1,"
             "ebur128@peaks=peak=true:metadata=1:framelog=quiet,"
             "ametadata=print:key=lavfi.r128.true_peak")
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", "-i", str(path), "-vn", "-af", graph,
         "-f", "null", "-"], check=True, capture_output=True, text=True,
    )
    # Padding and gain belong only to the peak meter, after the loudness meter.
    loudness = re.search(r"\[ebur128@loudness [^\n]*\] Summary:\s*Integrated loudness:\s*"
                         r"I:\s*(-?\d+(?:\.\d+)?|[-+]?inf)\s+LUFS", result.stderr)
    peaks = [float(value) for value in re.findall(r"\blavfi\.r128\.true_peak=([^\s]+)", result.stderr)]
    if not loudness or not peaks:
        raise ValueError("ffmpeg did not return decoded loudness and true peak")
    if any(not math.isfinite(peak) or peak < 0 for peak in peaks):
        raise ValueError("ffmpeg returned invalid true-peak metadata")
    # 100 ms of silence flushes the last partial block and resampler tail.
    peak = max(peaks)
    peak = (peak + TRUE_PEAK_METADATA_HALF_STEP) / TRUE_PEAK_METER_GAIN if peak > 0 else 0.0
    return float(loudness[1]), 20 * math.log10(peak) if peak > 0 else -math.inf


@dataclass
class AudioGraph:
    inputs: list[str]
    filters: list[str]
    tracks: dict[str, str]
    music_duck_db: float = MUSIC_DUCK_DB
    speech: list[Window] = field(default_factory=list)

    def _stems(self, music_duck_correction_db: float = 0.0,
               speech: list[Window] | None = None) -> tuple[list[str], dict[str, str]]:
        filters, tracks = list(self.filters), dict(self.tracks)
        windows = self.speech if speech is None else speech
        if "music" in tracks and windows:
            depth = min(MAX_MUSIC_DUCK_DB, max(0.0, self.music_duck_db + music_duck_correction_db))
            # Rebuild the full envelope so a negative correction reduces the
            # original duck, including its ramps, without boosting above unity.
            filters.append(f"[{tracks['music']}]{duck_filter(windows, depth)}[music_ducked]")
            tracks["music"] = "music_ducked"
        return filters, tracks

    def measure(self, track: str, windows: list[Window] | None = None, *,
                meter_gain_db: float = 0.0) -> float:
        filters, tracks = self._stems()
        graph = filters + [f"[{label}]anullsink" for key, label in tracks.items() if key != track]
        return measure(self.inputs, ";".join(graph), tracks[track], windows,
                       meter_gain_db=meter_gain_db)

    def mix(self, *, stem_probes: bool = False, ending_gain_correction_db: float = 0.0,
            ending_start: float | None = None, balance_corrections: dict[str, float] | None = None,
            speech: list[Window] | None = None) -> str:
        corrections = balance_corrections or {}
        filters, tracks = self._stems(corrections.get("music_duck_db", 0.0), speech)
        game_gain = corrections.get("game_gain_db", 0.0)
        game_duck = corrections.get("game_duck_db", 0.0)
        if ending_gain_correction_db or game_gain or game_duck:
            if ending_start is None:
                raise ValueError("A game correction needs the body's duration")
            if game_duck and not speech:
                raise ValueError("A ducking correction needs speech windows")
            game_filter = body_gain_filter(game_gain, ending_start, ending_gain_correction_db)
            filters.append(f"[{tracks['game']}]{game_filter},"
                           f"{duck_filter(speech or [], game_duck)}[game_adjusted]")
            tracks["game"] = "game_adjusted"
        if corrections.get("music_gain_db"):
            filters.append(f"[{tracks['music']}]volume={corrections['music_gain_db']:.9f}dB[music_adjusted]")
            tracks["music"] = "music_adjusted"
        if stem_probes:
            for track, label in tracks.items():
                filters.append(f"[{label}]asplit=2[master_{track}_mix][master_{track}_probe]")
                tracks[track] = f"master_{track}_mix"
        return ";".join(filters + ["".join(f"[{label}]" for label in tracks.values()) +
                                  f"amix=inputs={len(tracks)}:duration=first:normalize=0"])

    def master_graph(self, speech_lufs: float, length: float, *, gain_correction_db: float = 0.0,
                     ending_gain_correction_db: float = 0.0, ending_start: float | None = None,
                     balance_corrections: dict[str, float] | None = None,
                     speech: list[Window] | None = None) -> str:
        """Output stereo [a] and every stem under the mix's exact limiting gain.

        The limiter applies one gain to every channel. Its quiet constant third
        channel reveals that gain, including the final fade, even on silent or
        cancelling samples. Discard the pilot from the published stereo. Delay
        compensation in master_filter keeps the stems and envelope aligned.
        """
        processing = master_filter(speech_lufs, length, voice_probe=True,
                                   gain_correction_db=gain_correction_db)
        mix = self.mix(stem_probes=True, ending_gain_correction_db=ending_gain_correction_db,
                       ending_start=ending_start, balance_corrections=balance_corrections, speech=speech)
        envelopes = "".join(f"[master_{track}_envelope]" for track in self.tracks)
        graph = [f"{mix},{processing},asplit=2[master_mix][master_pilot]",
                 "[master_mix]pan=stereo|c0=c0|c1=c0[a]",
                 f"[master_pilot]pan=stereo|c0=c2|c1=c2,volume={1 / LIMITER_PILOT:g},"
                 f"asplit={len(self.tracks)}{envelopes}"]
        for track in self.tracks:
            graph.extend([
                f"[master_{track}_probe]volume={master_gain(speech_lufs) + gain_correction_db:.9f}dB"
                f"[master_{track}_boosted]",
                f"[master_{track}_boosted][master_{track}_envelope]amultiply[{track}_mastered]",
            ])
        return ";".join(graph)


def audio_graph(joined: Path, music: Path | None, voice: list[tuple[Path, float]],
                speech: list[Window], total: float, length: float, *,
                music_volume: float = 1.0, music_gain_db: float = 0.0,
                music_duck_db: float = MUSIC_DUCK_DB,
                game_gain_db: float = 0.0, game_duck_db: float = 0.0,
                ending_gain_db: float = 0.0, music_fades: bool = True) -> AudioGraph:
    """One source of truth for the mix and all calibration/validation taps.

    Disable music_fades when the supplied music stem already carries its fades.
    """
    inputs, filters, tracks = ["-i", str(joined)], [], {"game": "g"}
    if music is not None:
        inputs += ["-i", str(music)]
    if voice:
        filters.append(f"[0:a]aformat=sample_rates=48000:channel_layouts=stereo,{DUAL_MONO},"
                       f"atrim=duration={length},"
                       f"{body_gain_filter(game_gain_db, total, ending_gain_db)},"
                       f"{duck_filter(speech, game_duck_db)}[g]")
        first = len(inputs) // 2
        for k, (wav, at) in enumerate(voice):
            inputs += ["-i", str(wav)]
            trim = f"atrim=start={-at},asetpts=PTS-STARTPTS," if at < 0 else ""
            ms = max(0, round(at * 1000))
            filters.append(f"[{first + k}:a]aformat=sample_rates=48000:channel_layouts=stereo,{DUAL_MONO},"
                           f"{trim}adelay={ms}|{ms}[v{k}]")
        filters.append("".join(f"[v{k}]" for k in range(len(voice))) +
                       f"amix=inputs={len(voice)}:duration=longest:normalize=0,"
                       f"apad=whole_dur={length},atrim=duration={length}[vo]")
        tracks["voice"] = "vo"
    else:
        filters.append(f"[0:a]aformat=sample_rates=48000:channel_layouts=stereo,{DUAL_MONO}[g]")
    if music is not None:
        calibrated_volume = f"{music_gain_db:.9f}dB" if math.isfinite(music_gain_db) else "0"
        volume = calibrated_volume if voice else str(music_volume)
        format_audio = f"aformat=sample_rates=48000:channel_layouts=stereo,{DUAL_MONO},"
        fades = f",afade=t=in:d=1,afade=t=out:st={total - 1.5}:d=1.5" if music_fades else ""
        filters.append(f"[1:a]{format_audio}atrim=0:{total},asetpts=PTS-STARTPTS,"
                       f"volume={volume}{fades}[m]")
        tracks["music"] = "m"
    return AudioGraph(inputs, filters, tracks, music_duck_db, speech if voice else [])


def calibrate(joined: Path, music: Path | None, voice: list[tuple[Path, float]],
              speech: list[Window], total: float, length: float, *,
              music_volume: float = 1.0, music_fades: bool = True,
              music_fade_durations: tuple[float, float] = (0.0, 0.0)) -> tuple[AudioGraph, dict]:
    pauses = pause_windows(speech, total)
    music_speech_windows, music_pause_windows, music_pause_fallback = music_windows(
        speech, total, (1.0, 1.5) if music_fades else music_fade_durations,
    )
    music_speech_fallback = (bool(speech) and
                             sum(end - start for start, end in music_speech_windows) < 0.4 - 1e-6)
    if music_speech_fallback:
        music_speech_windows = merge_speech_windows(speech, total)
    music_window_report = {
        "music_speech_windows": music_speech_windows,
        "music_pause_windows": music_pause_windows,
        "music_speech_windows_fallback": music_speech_fallback,
        "music_speech_windows_fallback_reason": (
            "Music speech plateau is shorter than 400 ms; using original speech windows"
            if music_speech_fallback else None
        ),
        "music_pause_windows_fallback": music_pause_fallback,
        "music_pause_windows_fallback_reason": (
            "Music pause plateau is shorter than 400 ms; using original pause windows"
            if music_pause_fallback else None
        ),
    }
    raw = audio_graph(joined, music, voice, speech, total, length,
                      music_volume=music_volume, music_fades=music_fades,
                      music_duck_db=0.0 if voice else MUSIC_DUCK_DB)
    if not voice:
        game = raw.measure("game")
        music_level = raw.measure("music", music_pause_windows) if music is not None else None
        report = check_balance(None, music_level, None, None, game)
        report["music_duck_db"] = MUSIC_DUCK_DB
        report.update(music_window_report)
        return raw, report
    if not speech:
        raise ValueError("The voiced plan has no speech windows inside the video")
    s = raw.measure("voice")
    if not math.isfinite(s):
        raise ValueError("The voice track has no measurable EBU R128 loudness")
    meter_gain = master_gain(s)
    ending_windows = [(total, length)] if length > total else []
    ending = (raw.measure("game", ending_windows, meter_gain_db=meter_gain)
              if ending_windows else -math.inf)
    ending_gain = s - ENDING_BELOW_VOICE_LU - CEILING_HEADROOM_LU - ending if math.isfinite(ending) else 0.0
    game_pause = raw.measure("game", pauses, meter_gain_db=meter_gain) if pauses else -math.inf
    game_gain = (min(0.0, s - GAME_PAUSE_BELOW_VOICE_LU - CEILING_HEADROOM_LU - game_pause)
                 if math.isfinite(game_pause) else 0.0)
    if game_gain < 0:
        game_gain -= METER_RESOLUTION_LU
    if not ending_windows:
        ending_gain = game_gain
    game = audio_graph(joined, None, voice, speech, total, length,
                       game_gain_db=game_gain, ending_gain_db=ending_gain)
    game_speech = game.measure("game", speech, meter_gain_db=meter_gain)
    game_db = max(0.0, game_speech - (s - GAME_BELOW_VOICE_LU - CEILING_HEADROOM_LU))
    if game_db > 0:
        game_db += METER_RESOLUTION_LU
    game = audio_graph(joined, None, voice, speech, total, length,
                       game_gain_db=game_gain, game_duck_db=game_db, ending_gain_db=ending_gain)
    game_pause = game.measure("game", pauses, meter_gain_db=meter_gain) if pauses else None
    music_pause = (raw.measure("music", music_pause_windows if pauses else music_speech_windows,
                               meter_gain_db=meter_gain)
                   if music is not None else None)
    music_db = music_gain(s, music_pause, game_pause) if music_pause is not None else 0.0
    music_duck = MUSIC_DUCK_DB
    if music is not None and pauses:
        music_speech = raw.measure("music", music_speech_windows, meter_gain_db=meter_gain)
        if math.isfinite(music_speech) and music_pause is not None and math.isfinite(music_pause):
            # A common gain puts raw pauses on target; compensate the bed's
            # own dynamics as well to apply the configured speech duck below that target.
            music_duck = min(MAX_MUSIC_DUCK_DB, max(0.0, MUSIC_DUCK_DB + music_speech - music_pause))
            lower, upper = 0.0, MAX_MUSIC_DUCK_DB
            for attempt in range(9) if music_pause_fallback else ():
                # Only the reported fallback retains ramps/fades in its pauses.
                # Preserve its bounded refinement when no measurable plateau
                # exists; normal plateaus use the raw-level formula unchanged.
                ducked_pause = replace(raw, music_duck_db=music_duck).measure(
                    "music", music_pause_windows, meter_gain_db=meter_gain,
                )
                music_db = music_gain(s, ducked_pause, game_pause)
                error = music_speech - music_duck - ducked_pause + MUSIC_DUCK_DB
                tolerance = LEVEL_TOLERANCE_LU if attempt == 0 else METER_RESOLUTION_LU
                if abs(error) <= tolerance + 1e-6 or attempt == 8:
                    break
                if error > 0:
                    lower = music_duck
                else:
                    upper = music_duck
                refined = (lower + upper) / 2
                if refined == music_duck:
                    break  # A depth bound leaves any remaining error to the gate.
                music_duck = refined
    final = audio_graph(joined, music, voice, speech, total, length,
                        music_gain_db=music_db, music_duck_db=music_duck,
                        game_gain_db=game_gain, game_duck_db=game_db,
                        ending_gain_db=ending_gain, music_fades=music_fades)
    report = check_balance(
        s, final.measure("music", music_pause_windows, meter_gain_db=meter_gain)
        if music is not None and pauses else None,
        final.measure("music", music_speech_windows, meter_gain_db=meter_gain) if music is not None else None,
        final.measure("game", speech, meter_gain_db=meter_gain), game_pause,
        final.measure("game", ending_windows, meter_gain_db=meter_gain) if ending_windows else None,
    )
    check_music_full_windows(
        report,
        music_pause_lufs=final.measure("music", pauses, meter_gain_db=meter_gain)
        if music is not None and pauses != music_pause_windows else None,
        music_speech_lufs=final.measure("music", speech, meter_gain_db=meter_gain)
        if music is not None and speech != music_speech_windows else None,
    )
    report.update({"speech_lufs": s, "master_gain_db": meter_gain,
                   "music_gain_db": _finite(music_db), "music_duck_db": music_duck,
                   "game_gain_db": game_gain, "game_duck_db": game_db,
                   "ending_gain_db": ending_gain, "speech_windows": speech, "pause_windows": pauses,
                   "ending_windows": ending_windows})
    report.update(music_window_report)
    return final, report


def _gate_error(report: dict, stage: str) -> str:
    failed = ", ".join(row["id"] for row in report["rows"] if row["ok"] is False)
    reason = f" ({failed})" if failed else ""
    return f"Audio {stage} check failed{reason}; the short was not published"


def _master_voiced(audio: AudioGraph | None, report: dict | None, out: Path, length: float, *,
                   inputs: list[str] | None = None,
                   run: Callable[[list[str]], None] | None = None,
                   detect: Callable[[Path], list[float]] | None = None) -> None:
    """Master calibrated stems; also used by the comparison cutter's legacy wrapper."""
    out.unlink(missing_ok=True)
    if audio is None or report is None:
        raise LevelsError("Voiced mastering needs the calibrated stems and balance report")
    inputs = audio.inputs if inputs is None else inputs
    detect_clicks = clicks.detect if detect is None else detect

    def render_command(command: list[str]) -> None:
        if run is None:
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        else:
            run(command)

    with tempfile.TemporaryDirectory(dir=out.parent) as pending:
        candidate = Path(pending) / out.name
        stems = {track: Path(pending) / f"{track}.m4a" for track in audio.tracks}
        gain_correction = 0.0
        ending_gain_correction = 0.0
        balance_corrections = {"game_gain_db": 0.0, "game_duck_db": 0.0,
                               "music_gain_db": 0.0, "music_duck_db": 0.0}
        ending_start = report["ending_windows"][0][0] if report["ending_windows"] else None
        body_end = ending_start if ending_start is not None else length
        report["master_passes"] = 0
        report["master_corrections_db"] = []
        report["ending_corrections_db"] = []
        report["balance_corrections_db"] = []

        def render() -> float:
            # Every stem shares the actual mix limiter's gain and final fade.
            graph = audio.master_graph(
                report["speech_lufs"], length, gain_correction_db=gain_correction,
                ending_gain_correction_db=ending_gain_correction, ending_start=body_end,
                balance_corrections=dict(balance_corrections), speech=report["speech_windows"],
            )
            command = ["ffmpeg", "-y", *inputs, "-filter_complex", graph,
                       "-map", "0:v", "-map", "[a]", "-c:v", "copy", *aac_encoder(),
                       "-ar", "48000", str(candidate)]
            for track, path in stems.items():
                command += ["-map", f"[{track}_mastered]", "-vn", *aac_encoder(),
                            "-ar", "48000", str(path)]
            render_command(command)
            report["master_passes"] += 1
            return meter_file(stems["voice"])[0]

        def measure_balance(voice_lufs: float) -> dict:
            return check_mastered_balance(stems, voice_lufs, report["speech_windows"],
                                          report["pause_windows"], report["ending_windows"],
                                          report.get("music_speech_windows", report["speech_windows"]),
                                          report.get("music_pause_windows", report["pause_windows"]))

        for attempt in range(MAX_MASTER_CORRECTIONS + 1):
            voice_lufs = render()
            report["master_common_passes"] = attempt + 1
            report["master_gain_db"] = master_gain(report["speech_lufs"]) + gain_correction
            correction = master_correction(voice_lufs)
            if not correction or attempt == MAX_MASTER_CORRECTIONS:
                break
            gain_correction += correction
            report["master_corrections_db"].append(correction)
        balance = measure_balance(voice_lufs)
        for _ in range(MAX_BALANCE_CORRECTIONS):
            ending = next(row["measured_lufs"] for row in balance["rows"] if row["id"] == "ending")
            correction = ending_correction(voice_lufs, ending)
            adjustments = balance_correction(
                balance, music_duck_db=audio.music_duck_db + balance_corrections["music_duck_db"],
            )
            if not correction and not any(adjustments.values()):
                break
            ending_gain_correction += correction
            if correction:
                report["ending_corrections_db"].append(correction)
            if any(adjustments.values()):
                report["balance_corrections_db"].append(adjustments)
                for key, value in adjustments.items():
                    balance_corrections[key] += value
            voice_lufs = render()
            balance = measure_balance(voice_lufs)
        for key, value in balance_corrections.items():
            if value:
                report[key] = report.get(key, 0.0) + value
        if ending_start is not None:
            report["ending_gain_db"] += ending_gain_correction
            report["ending_gain_correction_db"] = ending_gain_correction
            report["ending_target_lufs"] = voice_lufs - ENDING_BELOW_VOICE_LU - CEILING_HEADROOM_LU
        _, peak = meter_file(candidate)
        report["pre_master_rows"] = report["rows"]
        report.update(balance)
        report["balance_stage"] = "mastered_aac"
        check_mastering(report, voice_lufs, peak, detect_clicks(candidate))
        check_dual_mono(report, candidate)
        if not report["ok"]:
            raise LevelsError(_gate_error(report, "mastering"))
        candidate.replace(out)


def publish(joined: Path, music: Path | None, voice: list[tuple[Path, float]],
            speech: list[Window], total: float, length: float, out: Path, *,
            music_volume: float = 1.0, music_fades: bool = True,
            music_fade_durations: tuple[float, float] = (0.0, 0.0),
            _master: Callable[..., None] | None = None) -> dict:
    """Calibrate, master and publish a voiced short with its decoded AAC report.

    Removes stale output/report first. Every calibration, ffmpeg or gate failure
    writes an error report, removes the output and raises LevelsError. Disable
    music_fades for a music stem whose fades were already rendered upstream;
    pass their durations as music_fade_durations to exclude them from metering.
    All clip offsets and speech intervals use the joined video's timeline;
    total is the body's duration, length includes the optional ending.
    """
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    report_path = out.with_suffix(".levels.json")
    report_path.unlink(missing_ok=True)
    report = check_balance(-math.inf, None, None, None, None)
    try:
        if not voice:
            raise LevelsError("Voiced publication needs voice clips")
        speech = merge_speech_windows(speech, total)
        audio, report = calibrate(joined, music, voice, speech, total, length,
                                  music_volume=music_volume, music_fades=music_fades,
                                  music_fade_durations=music_fade_durations)
        if not report["ok"] or any(row["ok"] is False for row in report["rows"]):
            raise LevelsError(_gate_error(report, "balance"))
        if _master is None:
            _master_voiced(audio, report, out, length)
        else:
            # Preserve the comparison cutter's existing master() integration hook.
            _master(audio.inputs, audio.mix(), out, length, voiced=True, audio=audio, report=report)
        if not report["ok"] or any(row["ok"] is False for row in report["rows"]):
            raise LevelsError(_gate_error(report, "mastering"))
        print_balance(report)
        report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        out.unlink(missing_ok=True)
        report.update(ok=False, error=str(error))
        print_balance(report)
        report_path.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
        raise LevelsError(str(error)) from error
    return report
