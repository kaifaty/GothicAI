import json
import math
import re
import shutil
import subprocess
from array import array
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest

from gothic_shorts import clicks, compare, levels


@pytest.fixture(scope="module")
def tone_inputs():
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic loudness measurements")
    return ["-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:duration=12"]


def _tone_level(inputs, gain_db=0, *, windows=None, extra="anull"):
    return levels.measure(
        inputs, f"[0:a]volume={gain_db}dB,{extra}[track]", "track", windows=windows,
    )


def _rows(report):
    return {row["id"]: row for row in report["rows"]}


def _difference_peak(path):
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", "-i", str(path), "-af",
         "pan=mono|c0=c0-c1,volumedetect", "-f", "null", "-"],
        check=True, capture_output=True, text=True,
    )
    return float(re.search(r"max_volume:\s*(-?[\d.]+)\s+dB", result.stderr)[1])


def _reference_lufs(inputs, filters):
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", *inputs, "-af", f"{filters},ebur128=peak=true",
         "-vn", "-f", "null", "-"], check=True, capture_output=True, text=True,
    )
    return float(re.findall(r"\bI:\s*(-?\d+(?:\.\d+)?|[-+]?inf)\s+LUFS", result.stderr)[-1])


def test_speech_windows_use_words_beat_starts_and_joined_timeline():
    plan = {
        "beats": [
            {
                "start": 10, "end": 16,
                "lines": [
                    {"at": 10, "words": [["first", 0.4, 0.8], ["last", 1.2, 2]]},
                    {"at": 11, "words": [["overlap", 0.5, 1], ["last", 1, 2]]},
                    {"at": 8, "words": [["before", 0, 1], ["last", 1, 3]]},
                    {"at": 15, "words": [["end", 0, 0.4], ["after", 1, 2]]},
                ],
            },
            {
                "start": 20, "end": 24,
                "lines": [{"at": 21, "words": [["next", 0.2, 0.6], ["last", 1, 1.5]]}],
            },
        ],
    }
    windows, duration = levels.speech_windows(plan)
    assert duration == 10
    for actual, expected in zip(windows, [(0, 3), (5, 7), (7.2, 8.5)], strict=True):
        assert actual == pytest.approx(expected)
    pauses = levels.pause_windows(windows, duration)
    for actual, expected in zip(pauses, [(3, 5), (7, 7.2), (8.5, 10)], strict=True):
        assert actual == pytest.approx(expected)


def test_empty_speech_and_full_speech_have_complementary_pauses():
    windows, duration = levels.speech_windows({"beats": [{"start": 2, "end": 7}]})
    assert windows == []
    assert duration == 5
    assert levels.pause_windows(windows, duration) == [(0, 5)]
    assert levels.pause_windows([(0, 5)], duration) == []


@pytest.mark.parametrize("duration,fallback", [(1.698, True), (1.7, False)])
def test_music_plateaus_fall_back_only_below_one_r128_block(duration, fallback):
    speech = [(0.35, 1.35)]
    music_speech, music_pauses, used_fallback = levels.music_windows(speech, duration, (0, 0))
    assert music_speech == speech
    assert used_fallback is fallback
    expected = levels.pause_windows(speech, duration) if fallback else [(0, 0.2), (1.5, duration)]
    for actual, interval in zip(music_pauses, expected, strict=True):
        assert actual == pytest.approx(interval)


def test_merge_speech_windows_sorts_merges_and_clips_to_the_body():
    windows = [(8, 12), (3, 5), (-2, 1), (1, 3), (20, 21), (-4, -1), (6, 7)]
    assert levels.merge_speech_windows(windows, 10) == [(0, 5), (6, 7), (8, 10)]
    assert levels.merge_speech_windows([], 0) == []
    assert windows == [(8, 12), (3, 5), (-2, 1), (1, 3), (20, 21), (-4, -1), (6, 7)]


@pytest.mark.parametrize("windows,duration", [
    ([(1, 1)], 10), ([(2, 1)], 10), ([(math.nan, 2)], 10),
    ([(1, math.inf)], 10), ([], -1), ([], math.nan), ([], math.inf),
])
def test_merge_speech_windows_rejects_invalid_times(windows, duration):
    with pytest.raises(ValueError):
        levels.merge_speech_windows(windows, duration)


@pytest.mark.parametrize("voice_gain,music_gain", [(-4, 0), (-10, -12)])
def test_music_calibration_reaches_configured_voice_offset(tone_inputs, voice_gain, music_gain):
    pauses = [(1, 3), (9, 11)]
    speech = _tone_level(tone_inputs, voice_gain)
    original = _tone_level(tone_inputs, music_gain, windows=pauses)
    correction = levels.music_gain(speech, original)
    calibrated = _tone_level(tone_inputs, music_gain + correction, windows=pauses)
    assert math.isfinite(speech)
    assert correction == pytest.approx(speech - levels.MUSIC_BELOW_VOICE_LU - original)
    assert calibrated == pytest.approx(speech - levels.MUSIC_BELOW_VOICE_LU, abs=0.5)


@pytest.mark.parametrize("game_gain", [-2, -24])
def test_music_calibration_respects_actual_game_pauses(tone_inputs, game_gain):
    pauses = [(1, 3), (9, 11)]
    speech = _tone_level(tone_inputs)
    game = _tone_level(tone_inputs, game_gain, windows=pauses)
    original = _tone_level(tone_inputs, windows=pauses)
    correction = levels.music_gain(speech, original, game)
    target = min(speech - levels.MUSIC_BELOW_VOICE_LU, game - levels.MUSIC_BELOW_GAME_LU)
    assert correction == pytest.approx(target - original)
    assert _tone_level(tone_inputs, correction, windows=pauses) == pytest.approx(target, abs=0.5)


def test_music_duck_matches_configured_depth_and_leaves_distant_pauses_unchanged(tone_inputs):
    speech = [(4, 8)]
    pauses = [(1, 3), (9, 11)]
    envelope = levels.duck_filter(speech, levels.MUSIC_DUCK_DB)
    original_speech = _tone_level(tone_inputs, windows=speech)
    original_pauses = _tone_level(tone_inputs, windows=pauses)
    ducked_speech = _tone_level(tone_inputs, windows=speech, extra=envelope)
    ducked_pauses = _tone_level(tone_inputs, windows=pauses, extra=envelope)
    assert ducked_speech - original_speech == pytest.approx(-levels.MUSIC_DUCK_DB, abs=0.2)
    assert ducked_pauses == pytest.approx(original_pauses, abs=0.1)


def test_duck_of_a_long_video_stays_within_ffmpeg_expression_depth():
    # 137 voiced lines (an 8-minute explainer): a chain of one max() per line exceeded ffmpeg's depth limit.
    speech = [(i * 3.0 + 0.1, i * 3.0 + 2.0) for i in range(200)]
    envelope = levels.duck_filter(speech, levels.MUSIC_DUCK_DB)
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "sine=f=440:d=601",
                    "-af", envelope, "-f", "null", "-"], check=True, capture_output=True)


def test_duck_ramps_smoothly_before_and_after_the_word_window(tone_inputs):
    envelope = levels.duck_filter([(4, 8)], levels.MUSIC_DUCK_DB)
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", *tone_inputs, "-af", envelope,
         "-f", "f32le", "-ac", "1", "-ar", "48000", "-"],
        check=True, capture_output=True,
    )
    samples = array("f")
    samples.frombytes(result.stdout)

    def rms(at):
        part = samples[round((at - 0.01) * 48000):round((at + 0.01) * 48000)]
        return math.sqrt(sum(value * value for value in part) / len(part))

    baseline = rms(3.8)
    floor = baseline * 10 ** (-levels.MUSIC_DUCK_DB / 20)
    assert rms(4.05) == pytest.approx(floor, rel=0.01)
    assert rms(7.95) == pytest.approx(floor, rel=0.01)
    assert floor < rms(3.925) < baseline
    assert floor < rms(8.075) < baseline
    assert rms(8.2) == pytest.approx(baseline, rel=0.01)


@pytest.fixture(scope="module")
def balanced_tracks(tone_inputs):
    music_gain = min(-levels.MUSIC_BELOW_VOICE_LU,
                     -levels.GAME_PAUSE_BELOW_VOICE_LU - levels.MUSIC_BELOW_GAME_LU)
    return {
        "speech_lufs": _tone_level(tone_inputs),
        "music_pause_lufs": _tone_level(tone_inputs, music_gain),
        "music_speech_lufs": _tone_level(tone_inputs, music_gain - levels.MUSIC_DUCK_DB),
        "game_speech_lufs": _tone_level(tone_inputs, -levels.GAME_BELOW_VOICE_LU),
        "game_pause_lufs": _tone_level(tone_inputs, -levels.GAME_PAUSE_BELOW_VOICE_LU),
        "ending_lufs": _tone_level(tone_inputs, -levels.ENDING_BELOW_VOICE_LU),
    }


def test_balance_accepts_calibrated_synthetic_tracks(balanced_tracks):
    report = levels.check_balance(**balanced_tracks)
    assert report["ok"] is True
    rows = _rows(report)
    assert set(rows) == {
        "voice", "music_pause", "music_speech", "game_speech", "game_pause", "ending",
        "game_below_voice", "music_below_game",
    }
    music_target = min(balanced_tracks["speech_lufs"] - levels.MUSIC_BELOW_VOICE_LU,
                       balanced_tracks["game_pause_lufs"] - levels.MUSIC_BELOW_GAME_LU)
    assert rows["music_pause"]["target_lufs"] == pytest.approx(music_target)
    assert rows["music_speech"]["target_lufs"] == pytest.approx(music_target - levels.MUSIC_DUCK_DB)
    assert rows["game_speech"]["target_lufs"] == pytest.approx(
        balanced_tracks["speech_lufs"] - levels.GAME_BELOW_VOICE_LU)
    assert rows["game_pause"]["target_lufs"] == pytest.approx(
        balanced_tracks["speech_lufs"] - levels.GAME_PAUSE_BELOW_VOICE_LU)
    assert rows["ending"]["target_lufs"] == pytest.approx(
        balanced_tracks["speech_lufs"] - levels.ENDING_CHECK_BELOW_VOICE_LU)
    assert rows["game_below_voice"]["target_lufs"] == pytest.approx(
        balanced_tracks["speech_lufs"] - levels.GAME_PAUSE_BELOW_VOICE_LU)
    assert rows["music_below_game"]["target_lufs"] == pytest.approx(
        balanced_tracks["game_pause_lufs"] - levels.MUSIC_BELOW_GAME_LU)
    assert all({"id", "measured_lufs", "target_lufs", "ok", "target"} <= row.keys()
               for row in report["rows"])
    for key, below in (("game_speech", levels.GAME_BELOW_VOICE_LU),
                       ("game_pause", levels.GAME_PAUSE_BELOW_VOICE_LU),
                       ("ending", levels.ENDING_CHECK_BELOW_VOICE_LU)):
        assert rows[key]["target"] == f"≤ S - {below:g} LU"
    assert rows["game_below_voice"]["target"] == (
        f"≤ voice - {levels.GAME_PAUSE_BELOW_VOICE_LU:g} LU"
    )


def test_balance_rejects_music_that_is_too_loud(balanced_tracks):
    excess = levels.LEVEL_TOLERANCE_LU + 0.1
    tracks = {**balanced_tracks,
              "music_pause_lufs": balanced_tracks["music_pause_lufs"] + excess,
              "music_speech_lufs": balanced_tracks["music_speech_lufs"] + excess}
    report = levels.check_balance(**tracks)
    assert report["ok"] is False
    assert _rows(report)["music_pause"]["ok"] is False
    assert _rows(report)["music_speech"]["ok"] is False


@pytest.mark.parametrize("direction", [-1, 1])
def test_balance_rejects_music_with_incorrect_speech_duck(balanced_tracks, direction):
    error = direction * (levels.LEVEL_TOLERANCE_LU + 0.1)
    report = levels.check_balance(**{
        **balanced_tracks, "music_speech_lufs": balanced_tracks["music_speech_lufs"] + error,
    })
    assert report["ok"] is False
    assert _rows(report)["music_pause"]["ok"] is True
    assert _rows(report)["music_speech"]["ok"] is False


def test_balance_rejects_game_audio_that_is_too_loud_during_speech(balanced_tracks):
    excess = min(0.1, levels.LEVEL_TOLERANCE_LU / 2)
    report = levels.check_balance(**{
        **balanced_tracks,
        "game_speech_lufs": balanced_tracks["speech_lufs"] - levels.GAME_BELOW_VOICE_LU + excess,
    })
    assert report["ok"] is False
    assert _rows(report)["game_speech"]["ok"] is False


def test_balance_rejects_game_audio_that_is_too_loud_in_pauses(balanced_tracks):
    excess = min(0.1, levels.LEVEL_TOLERANCE_LU / 2)
    report = levels.check_balance(**{
        **balanced_tracks,
        "game_pause_lufs": balanced_tracks["speech_lufs"] - levels.GAME_PAUSE_BELOW_VOICE_LU + excess,
    })
    assert report["ok"] is False
    assert _rows(report)["game_pause"]["ok"] is False
    assert _rows(report)["game_below_voice"]["ok"] is False


def test_balance_rejects_music_above_the_game_ordering_limit(balanced_tracks):
    report = levels.check_balance(**{
        **balanced_tracks,
        "music_pause_lufs": (balanced_tracks["game_pause_lufs"] - levels.MUSIC_BELOW_GAME_LU
                             + levels.LEVEL_TOLERANCE_LU + 0.1),
    })
    assert report["ok"] is False
    assert _rows(report)["music_below_game"]["ok"] is False


def test_balance_rejects_an_ending_above_the_publication_ceiling(balanced_tracks):
    report = levels.check_balance(**{
        **balanced_tracks,
        "ending_lufs": balanced_tracks["speech_lufs"] - levels.ENDING_CHECK_BELOW_VOICE_LU + 1,
    })
    assert report["ok"] is False
    assert _rows(report)["ending"]["ok"] is False


def test_ending_keeps_its_calibration_target_and_publication_ceiling():
    voice = -14
    ceiling = voice - levels.ENDING_CHECK_BELOW_VOICE_LU
    calibration_target = voice - levels.ENDING_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU
    report = levels.check_balance(voice, None, None, None, None, ceiling + 1)
    assert report["ok"] is False
    assert _rows(report)["ending"]["ok"] is False
    assert _rows(report)["ending"]["target_lufs"] == ceiling
    assert levels.ending_correction(voice, calibration_target + 1) == pytest.approx(-1)


def test_unvoiced_balance_only_reports_levels(balanced_tracks):
    report = levels.check_balance(**{**balanced_tracks, "speech_lufs": None})
    assert report["ok"] is True
    assert all(row["ok"] is None for row in report["rows"])


def test_measurement_reports_silence_short_audio_and_empty_windows_as_unavailable(tone_inputs):
    silent = ["-f", "lavfi", "-i", "anullsrc=sample_rate=48000:channel_layout=mono:duration=1"]
    short = ["-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=48000:duration=0.1"]
    assert levels.measure(silent, "[0:a]anull[track]", "track") == -math.inf
    assert levels.measure(silent, "[0:a]anull[track]", "track", meter_gain_db=20) == -math.inf
    assert levels.measure(short, "[0:a]anull[track]", "track") == -math.inf
    assert _tone_level(tone_inputs, windows=[]) == -math.inf


@pytest.mark.parametrize("gain_db", [-65, -105, -180])
def test_measurement_recovers_quiet_float_audio_below_the_absolute_gate(tone_inputs, gain_db):
    original = _tone_level(tone_inputs, windows=[(1, 3), (9, 11)])
    graph = f"[0:a]aformat=sample_fmts=flt,volume={gain_db}dB[track]"
    windows = [(1, 3), (9, 11)]
    for meter_gain in (0, 19.4, 40):
        measured = levels.measure(
            tone_inputs, graph, "track", windows, meter_gain_db=meter_gain,
        )
        assert measured == pytest.approx(original + gain_db, abs=0.2)


@pytest.mark.parametrize("gain_db,earlier_amplitude", [(0, 0), (-90, 0), (0, 0.01)])
def test_measurement_includes_energy_in_the_last_partial_r128_block(gain_db, earlier_amplitude):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic loudness measurements")
    inputs = ["-f", "lavfi", "-i",
              f"aevalsrc='if(lt(t,8.2),{earlier_amplitude},0.79*min((t-8.2)/0.005,1))"
              "*sin(2*PI*4000*t)':s=48000:d=8.29"]
    selection = f"atrim=6:8.29,asetpts=PTS-STARTPTS,volume={gain_db}dB:precision=double"
    unflushed = _reference_lufs(inputs, selection)
    # An independent reference flushes the partial block. Amplify only its
    # measurement in the quiet case, then undo that gain in the expected LUFS.
    reference_gain = -gain_db
    expected = _reference_lufs(
        inputs, f"{selection},volume={reference_gain}dB:precision=double,apad=pad_dur=0.1",
    ) - reference_gain
    assert math.isfinite(expected)
    if earlier_amplitude:
        assert -70 < unflushed < expected - 10  # A finite earlier reading also misses the loud tail.
    else:
        assert unflushed <= -70
    measured = levels.measure(inputs, f"[0:a]volume={gain_db}dB[track]", "track", [(6, 8.29)])
    assert measured == pytest.approx(expected, abs=0.2)


def test_short_explicit_selection_cannot_pass_the_balance_check_as_silence(tone_inputs):
    with pytest.raises(ValueError, match="400 ms"):
        _tone_level(tone_inputs, windows=[(1, 1.1), (2, 2.1)])


def test_silent_voice_and_unmeasurable_music_cannot_be_calibrated():
    with pytest.raises(ValueError, match="measurable"):
        levels.music_gain(-math.inf, -20)
    with pytest.raises(ValueError, match="measurable"):
        levels.music_gain(-22, -math.inf)
    music_pause = -22 - levels.MUSIC_BELOW_VOICE_LU
    report = levels.check_balance(
        -math.inf, music_pause, music_pause - levels.MUSIC_DUCK_DB, -28, -10,
    )
    assert report["ok"] is False
    assert _rows(report)["voice"]["ok"] is False


def test_calibration_uses_the_render_stems_and_caps_loud_game_pauses(tmp_path, tone_inputs):
    paths = {}
    for track, gain, duration in [("game", 0, 12), ("music", -6, 12), ("voice", -12, 4)]:
        paths[track] = tmp_path / f"{track}.wav"
        gain_filter = f"volume={gain}dB"
        if track == "game":
            gain_filter += ",afade=t=out:st=11.94:d=0.06"
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", *tone_inputs, "-t", str(duration),
             "-af", gain_filter, "-c:a", "pcm_s16le", str(paths[track])],
            check=True, capture_output=True,
        )
    speech, pauses = [(4, 8)], [(0, 4), (8, 12)]
    voice = [(paths["voice"], 4)]
    calibrated, report = levels.calibrate(
        paths["game"], paths["music"], voice, speech, 12, 12, music_volume=0.001,
    )
    assert report["ok"] is True
    assert report["game_duck_db"] > 0
    assert "sidechaincompress" not in calibrated.mix()
    music_target = min(calibrated.measure("voice") - levels.MUSIC_BELOW_VOICE_LU,
                       calibrated.measure("game", pauses) - levels.MUSIC_BELOW_GAME_LU)
    assert calibrated.measure("music", report["music_pause_windows"]) == pytest.approx(
        music_target, abs=0.5,
    )
    # Calibration headroom survives a small decoded-level shift.
    assert report["game_gain_db"] < 0
    pause_level = calibrated.measure("game", pauses)
    pause_target = calibrated.measure("voice") - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU
    assert pause_level <= pause_target
    # R128 rounds to tenths; ignore float error at the tolerance boundary.
    assert pause_level == pytest.approx(pause_target, abs=0.2 + 1e-6)
    assert calibrated.measure("game", speech) <= (
        calibrated.measure("voice") - levels.GAME_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU
    )

    # The plan's music volume has no effect on a voiced graph; only calibrated dB do.
    other_volume = levels.audio_graph(
        paths["game"], paths["music"], voice, speech, 12, 12, music_volume=0.9,
        music_gain_db=report["music_gain_db"], game_gain_db=report["game_gain_db"],
        music_duck_db=report["music_duck_db"],
        game_duck_db=report["game_duck_db"], ending_gain_db=report["ending_gain_db"],
    )
    assert other_volume.mix() == calibrated.mix()


def test_calibration_keeps_game_pauses_that_are_quiet_enough(tmp_path, tone_inputs):
    paths = {}
    game_gain = -12 - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU - 1
    for track, gain, duration in [("game", game_gain, 12), ("voice", -12, 4)]:
        paths[track] = tmp_path / f"{track}.wav"
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", *tone_inputs, "-t", str(duration),
             "-af", f"volume={gain}dB", "-c:a", "pcm_s16le", str(paths[track])],
            check=True, capture_output=True,
        )
    speech, pauses = [(4, 8)], [(0, 4), (8, 12)]
    voice = [(paths["voice"], 4)]
    calibrated, report = levels.calibrate(paths["game"], None, voice, speech, 12, 12)
    assert report["ok"] is True
    assert report["game_gain_db"] == 0
    original = levels.audio_graph(paths["game"], None, voice, speech, 12, 12)
    assert calibrated.measure("game", pauses) == pytest.approx(original.measure("game", pauses), abs=0.5)


@pytest.mark.parametrize("game_gain,codec", [
    (-32, "pcm_s24le"), (-45, "pcm_s24le"), (-65, "pcm_f32le"), (-105, "pcm_f32le"),
])
def test_calibration_lowers_music_below_quiet_effects(tmp_path, tone_inputs, game_gain, codec):
    paths = {}
    for track, gain, duration in [("game", game_gain, 12), ("music", 0, 12), ("voice", -12, 4)]:
        paths[track] = tmp_path / f"{track}.wav"
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", *tone_inputs, "-t", str(duration),
             "-af", f"volume={gain}dB", "-c:a", codec, str(paths[track])],
            check=True, capture_output=True,
        )
    speech, pauses = [(4, 8)], [(0, 4), (8, 12)]
    calibrated, report = levels.calibrate(
        paths["game"], paths["music"], [(paths["voice"], 4)], speech, 12, 12,
    )
    assert report["ok"] is True
    assert report["game_gain_db"] == 0
    meter_gain = report["master_gain_db"]
    voice = calibrated.measure("voice")
    game = calibrated.measure("game", pauses, meter_gain_db=meter_gain)
    music = calibrated.measure("music", report["music_pause_windows"], meter_gain_db=meter_gain)
    music_speech = calibrated.measure("music", report["music_speech_windows"], meter_gain_db=meter_gain)
    assert game - levels.MUSIC_BELOW_GAME_LU < voice - levels.MUSIC_BELOW_VOICE_LU
    assert music == pytest.approx(
        min(voice - levels.MUSIC_BELOW_VOICE_LU, game - levels.MUSIC_BELOW_GAME_LU),
        abs=levels.LEVEL_TOLERANCE_LU,
    )
    assert music_speech == pytest.approx(music - levels.MUSIC_DUCK_DB, abs=levels.LEVEL_TOLERANCE_LU)
    assert _rows(report)["music_below_game"]["ok"] is True
    if game_gain <= -45:
        # A measurement-only boost clears the gate even if the master gain cannot.
        assert calibrated.measure("music", report["music_speech_windows"]) == pytest.approx(
            music_speech, abs=0.2,
        )
        assert music_speech < -70
        if game_gain <= -65:
            assert music_speech + meter_gain < -70
        assert _rows(report)["music_speech"]["measured_lufs"] == music_speech
        assert _rows(report)["music_speech"]["ok"] is True


def test_calibration_leaves_headroom_in_effects_and_ending(tmp_path, tone_inputs):
    paths = {}
    for track, gain, duration in [("game", 0, 14), ("voice", -12, 4)]:
        paths[track] = tmp_path / f"{track}.wav"
        source = f"sine=frequency=1000:sample_rate=48000:duration={duration}"
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", source,
             "-af", f"volume={gain}dB", "-c:a", "pcm_s16le", str(paths[track])],
            check=True, capture_output=True,
        )
    voice, speech = [(paths["voice"], 4)], [(4, 8)]
    raw = levels.audio_graph(paths["game"], None, voice, speech, 12, 14)
    assert raw.measure("game", [(12, 14)]) > raw.measure("voice")
    calibrated, report = levels.calibrate(paths["game"], None, voice, speech, 12, 14)
    ending = calibrated.measure("game", [(12, 14)])
    s = calibrated.measure("voice")
    assert report["ok"] is True
    assert report["ending_gain_db"] < 0
    assert ending == pytest.approx(s - (levels.ENDING_BELOW_VOICE_LU + levels.CEILING_HEADROOM_LU), abs=0.2)
    assert ending <= s - levels.ENDING_CHECK_BELOW_VOICE_LU
    assert _rows(report)["ending"]["ok"] is True
    rows = _rows(report)
    assert rows["game_pause"]["measured_lufs"] == pytest.approx(
        s - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU, abs=0.2)
    assert rows["game_speech"]["measured_lufs"] == pytest.approx(
        s - levels.GAME_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU, abs=0.2)
    shift = levels.CEILING_HEADROOM_LU * 0.6
    shifted = levels.check_balance(s, None, None, rows["game_speech"]["measured_lufs"] + shift,
                                   rows["game_pause"]["measured_lufs"] + shift, ending + shift)
    assert shifted["ok"] is True
    assert _rows(shifted)["game_pause"]["target_lufs"] == s - levels.GAME_PAUSE_BELOW_VOICE_LU
    assert _rows(shifted)["game_speech"]["target_lufs"] == s - levels.GAME_BELOW_VOICE_LU
    assert _rows(shifted)["ending"]["target_lufs"] == s - levels.ENDING_CHECK_BELOW_VOICE_LU


def test_calibration_boosts_a_quiet_ending_without_a_body_surge(tmp_path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic loudness measurements")
    game, voice = tmp_path / "game.wav", tmp_path / "voice.wav"
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "sine=frequency=1000:sample_rate=48000:duration=12", "-f", "lavfi", "-i",
         "sine=frequency=1000:sample_rate=48000:duration=2", "-filter_complex",
         "[0:a]afade=t=out:st=11.94:d=0.06[body];[1:a]volume=0.01[ending];"
         "[body][ending]concat=n=2:v=0:a=1[a]", "-map", "[a]", "-c:a", "pcm_s24le", str(game)],
        check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "sine=frequency=1000:sample_rate=48000:duration=4", "-af", "volume=-12dB",
         "-c:a", "pcm_s24le", str(voice)], check=True, capture_output=True,
    )
    calibrated, report = levels.calibrate(game, None, [(voice, 4)], [(4, 8)], 12, 14)
    rows = _rows(report)
    s = report["speech_lufs"]
    # the ending tone is 40 dB under the body; it is lifted to its place under the voice
    assert report["ending_gain_db"] > 27 - levels.ENDING_BELOW_VOICE_LU
    assert report["ok"] is True
    assert rows["game_pause"]["measured_lufs"] <= s - levels.GAME_PAUSE_BELOW_VOICE_LU
    assert rows["ending"]["measured_lufs"] == pytest.approx(
        s - (levels.ENDING_BELOW_VOICE_LU + levels.CEILING_HEADROOM_LU), abs=0.2)
    # No sample in the body's fade may exceed the same body with a constant gain.
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", *calibrated.inputs,
         "-filter_complex", ";".join(calibrated.filters + ["[vo]anullsink"]), "-map", "[g]",
         "-f", "f32le", "-ac", "1", "-ar", "48000", "-"], check=True, capture_output=True,
    )
    samples = array("f")
    samples.frombytes(result.stdout)
    body_peak = max(abs(value) for value in samples[round(11.94 * 48000):12 * 48000])
    assert body_peak <= 0.125 * 10 ** (report["game_gain_db"] / 20)


@pytest.mark.parametrize("ending_gain_db", [-18, -3, 18])
def test_body_gain_ramps_on_the_side_that_preserves_the_body_fade(tone_inputs, ending_gain_db):
    result = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", *tone_inputs,
         "-af", levels.body_gain_filter(-6, 6, ending_gain_db),
         "-f", "f32le", "-ac", "1", "-ar", "48000", "-"],
        check=True, capture_output=True,
    )
    samples = array("f")
    samples.frombytes(result.stdout)

    def rms(at):
        part = samples[round((at - 0.005) * 48000):round((at + 0.005) * 48000)]
        return math.sqrt(sum(value * value for value in part) / len(part))

    body = rms(5.9)
    ending = rms(6.1)
    assert ending == pytest.approx(body * 10 ** ((ending_gain_db + 6) / 20), rel=0.01)
    if ending_gain_db > -6:
        assert rms(5.99) == pytest.approx(body, rel=0.01)
        assert body < rms(6.01) < rms(6.03) < rms(6.05) < ending
    else:
        assert body > rms(5.95) > rms(5.97) > rms(5.99) > ending


def test_unvoiced_graph_keeps_plan_volume_and_does_not_duck(tmp_path):
    graph = levels.audio_graph(tmp_path / "game.mp4", tmp_path / "music.wav", [], [], 12, 14,
                               music_volume=0.8)
    assert graph.filters == [
        f"[0:a]aformat=sample_rates=48000:channel_layouts=stereo,{levels.DUAL_MONO}[g]",
        f"[1:a]aformat=sample_rates=48000:channel_layouts=stereo,{levels.DUAL_MONO},"
        "atrim=0:12,asetpts=PTS-STARTPTS,volume=0.8,afade=t=in:d=1,"
        "afade=t=out:st=10.5:d=1.5[m]",
    ]
    assert graph.mix().endswith("[g][m]amix=inputs=2:duration=first:normalize=0")
    ignored_voiced_gains = levels.audio_graph(
        tmp_path / "game.mp4", tmp_path / "music.wav", [], [], 12, 14,
        music_volume=0.8, music_gain_db=-20, game_gain_db=-12, game_duck_db=6, ending_gain_db=-18,
    )
    assert ignored_voiced_gains.inputs == graph.inputs
    assert ignored_voiced_gains.filters == graph.filters


@pytest.mark.parametrize("voiced", [False, True])
@pytest.mark.parametrize("right,expected_delta", [("0", -3.0), ("0.1*sin(2*PI*440*t)", 0.0)])
def test_game_is_folded_before_measurement_without_changing_centered_loudness(
    tmp_path, tone_inputs, voiced, right, expected_delta,
):
    inputs = ["-f", "lavfi", "-i", f"aevalsrc='0.1*sin(2*PI*440*t)|{right}':s=48000:d=12:c=stereo"]
    original = levels.measure(inputs, "[0:a]anull[g]", "g")
    audio = levels.audio_graph(
        tmp_path / "game.wav", None, [(tmp_path / "voice.wav", 0)] if voiced else [], [], 12, 12,
    )
    audio.inputs = inputs + (tone_inputs if voiced else [])
    assert audio.measure("game") - original == pytest.approx(expected_delta, abs=0.1)


@pytest.fixture
def comparison_cut():
    return compare


def _mock_mastered_balance(monkeypatch, report):
    """Keep loop/peak/click unit tests independent of synthetic stem decoding."""
    report.update(speech_windows=[(4, 8)], pause_windows=[(0, 4), (8, 12)], ending_windows=[])
    monkeypatch.setattr(levels, "dual_mono_peak", lambda _path: -91.0)
    monkeypatch.setattr(levels, "check_mastered_balance", lambda _stems, voice, *_windows:
                        levels.check_balance(
                            voice, None, None,
                            voice - levels.GAME_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU,
                            voice - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU))


@pytest.mark.parametrize("speech_excess,pause_excess,body_change,extra_duck", [
    (0.5, 1.5, -1.5, 0),  # Lowering the whole body also resolves the speech excess.
    (1.5, -0.5, 0, 1.5),  # Quiet pauses need no attenuation; duck only the speech.
    (2.5, 1.5, -1.5, 1),
    (-0.5, -0.5, 0, 0),  # Already quiet effects are never raised toward the ceiling.
])
def test_balance_correction_attenuates_effects_without_double_ducking(
    speech_excess, pause_excess, body_change, extra_duck,
):
    game_speech = -14 - levels.GAME_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU + speech_excess
    game_pause = -14 - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU + pause_excess
    report = levels.check_balance(-14, None, None, game_speech, game_pause)
    correction = levels.balance_correction(report)
    assert correction.get("game_gain_db", 0) == pytest.approx(body_change)
    assert correction.get("game_duck_db", 0) == pytest.approx(extra_duck)
    assert correction.get("music_gain_db", 0) == 0
    corrected = levels.check_balance(
        -14, None, None, game_speech + body_change - extra_duck, game_pause + body_change,
    )
    assert corrected["ok"] is True


@pytest.mark.parametrize("excess,expected", [
    (4, -4),
    (-4, 4),
    (1, 0),  # The existing music tolerance does not trigger another render.
])
def test_balance_correction_retargets_music_below_quiet_decoded_effects(
    excess, expected,
):
    target = min(-14 - levels.MUSIC_BELOW_VOICE_LU, -35 - levels.MUSIC_BELOW_GAME_LU)
    music_pause = target + excess
    music_speech = music_pause - levels.MUSIC_DUCK_DB
    report = levels.check_balance(-14, music_pause, music_speech, -38, -35)
    correction = levels.balance_correction(report)
    assert correction.get("game_gain_db", 0) == 0
    assert correction.get("game_duck_db", 0) == 0
    assert correction.get("music_gain_db", 0) == pytest.approx(expected)
    corrected = levels.check_balance(-14, music_pause + expected, music_speech + expected, -38, -35)
    assert corrected["ok"] is True


@pytest.mark.parametrize("pause_excess,speech_excess,gain,duck", [
    (4, 6, -4, 2),
    (-4, -4 - levels.MUSIC_DUCK_DB, 4, -levels.MUSIC_DUCK_DB),
    # Correct speech too when the pause gain moves it off target.
    (levels.MUSIC_DUCK_DB, 0, -levels.MUSIC_DUCK_DB, -levels.MUSIC_DUCK_DB),
    (0, 3, 0, 3),
    (0, -levels.MUSIC_DUCK_DB, 0, -levels.MUSIC_DUCK_DB),
    (1, 0, 0, 0),  # Keep the existing tolerance for both rows.
])
def test_balance_correction_adjusts_music_gain_and_duck_in_one_step(
    pause_excess, speech_excess, gain, duck,
):
    target = -14 - levels.MUSIC_BELOW_VOICE_LU
    music_pause = target + pause_excess
    music_speech = target - levels.MUSIC_DUCK_DB + speech_excess
    report = levels.check_balance(-14, music_pause, music_speech, None, None)
    correction = levels.balance_correction(report)
    assert correction["music_gain_db"] == pytest.approx(gain)
    assert correction["music_duck_db"] == pytest.approx(duck)
    corrected = levels.check_balance(
        -14, music_pause + gain, music_speech + gain - duck, None, None,
    )
    assert corrected["ok"] is True


@pytest.mark.parametrize("depth,speech_excess,duck", [
    (0, -3, 0),
    (2, -5, -2),
    (levels.MAX_MUSIC_DUCK_DB - 1, 4, 1),
    (levels.MAX_MUSIC_DUCK_DB, 3, 0),
])
def test_balance_correction_keeps_music_duck_within_configured_bounds(
    depth, speech_excess, duck,
):
    music_pause = -14 - levels.MUSIC_BELOW_VOICE_LU
    music_speech = music_pause - levels.MUSIC_DUCK_DB + speech_excess
    report = levels.check_balance(-14, music_pause, music_speech, None, None)
    correction = levels.balance_correction(report, music_duck_db=depth)
    assert correction["music_gain_db"] == 0
    assert correction["music_duck_db"] == pytest.approx(duck)
    assert 0 <= depth + correction["music_duck_db"] <= levels.MAX_MUSIC_DUCK_DB


@pytest.mark.parametrize("linear_peak,accepted", [(0.892, False), (0.891, True)])
def test_decoded_peak_metadata_decides_the_gate_when_the_summary_rounds_to_minus_one(
    monkeypatch, capsys, linear_peak, accepted,
):
    scaled_peak = round(linear_peak * levels.TRUE_PEAK_METER_GAIN, 3)
    stderr = (
        f"lavfi.r128.true_peak={scaled_peak}\n"
        "lavfi.r128.true_peak=0.0\n"
        "[ebur128@loudness @ 0x1] Summary:\n\n  Integrated loudness:\n"
        "  I: -14.0 LUFS\n"
        "[ebur128@peaks @ 0x2] Summary:\n\n  Integrated loudness:\n"
        "  I: 10.0 LUFS\n  True peak:\n    Peak: 95.3 dBFS\n"
    )
    run = Mock(return_value=SimpleNamespace(stderr=stderr))
    monkeypatch.setattr(levels.subprocess, "run", run)
    voice, peak = levels.meter_file(Path("short.mp4"))
    expected = 20 * math.log10(
        (scaled_peak + levels.TRUE_PEAK_METADATA_HALF_STEP) / levels.TRUE_PEAK_METER_GAIN,
    )
    assert voice == -14  # Ignore the amplified/padded peak meter's loudness summary.
    assert peak == pytest.approx(expected, abs=1e-12)
    graph = run.call_args.args[0][run.call_args.args[0].index("-af") + 1]
    assert graph.index("ebur128@loudness") < graph.index("volume=") < graph.index("apad=")
    assert f"volume={levels.TRUE_PEAK_METER_GAIN}:precision=double" in graph
    assert "apad=pad_dur=0.1,ebur128@peaks=peak=true:metadata=1" in graph
    report = levels.check_balance(voice, None, None, None, None)
    levels.check_mastering(report, voice, peak, [])
    assert _rows(report)["true_peak"]["ok"] is accepted
    assert report["ok"] is accepted
    levels.print_balance(report)
    assert f"{peak:.6f}" in capsys.readouterr().out


@pytest.mark.parametrize("metadata", [None, "nan", "inf", "-0.1"])
def test_decoded_peak_requires_valid_metadata_instead_of_a_rounded_fallback(monkeypatch, metadata):
    stderr = "[ebur128@loudness @ 0x1] Summary:\n\nIntegrated loudness:\nI: -14.0 LUFS\nPeak: -1.0 dBFS\n"
    if metadata is not None:
        stderr += f"lavfi.r128.true_peak={metadata}\n"
    run = Mock(return_value=SimpleNamespace(stderr=stderr))
    monkeypatch.setattr(levels.subprocess, "run", run)
    with pytest.raises(ValueError, match="true[- ]peak"):
        levels.meter_file(Path("short.mp4"))


@pytest.mark.parametrize("balanced", [True, False])
def test_comparison_checks_and_writes_levels_before_publication(
    tmp_path, monkeypatch, capsys, comparison_cut, balanced,
):
    plan = {
        "brand": False,
        "labels": {"ru": ["До", "После"]},
        "hook": {"ru": "Проверка"}, "hook_s": 2,
        "music": {"file": "music.wav", "volume": 0.8},
        "beats": [{"segment": "main", "start": 0, "end": 12, "lines": [
            {"at": 4, "file": "voice.wav", "words": [["Проверка", 0, 4]]},
        ]}],
    }
    plan_file = tmp_path / "plan.json"
    plan_file.write_text(json.dumps(plan))
    out = tmp_path / "render" / "short-ru.mp4"
    out.parent.mkdir()
    out.write_bytes(b"stale short must not survive a failed balance check")
    game_speech = -22 - levels.GAME_BELOW_VOICE_LU + (0 if balanced else 1)
    music_pause = min(-22 - levels.MUSIC_BELOW_VOICE_LU,
                      -22 - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.MUSIC_BELOW_GAME_LU)
    report = levels.check_balance(
        -22, music_pause, music_pause - levels.MUSIC_DUCK_DB,
        game_speech, -22 - levels.GAME_PAUSE_BELOW_VOICE_LU,
    )
    audio = SimpleNamespace(inputs=["-i", "game.mp4"], mix=lambda: "[0:a]anull")
    calibrate = Mock(return_value=(audio, report))
    master = Mock(side_effect=lambda _inputs, _mix, path, _length, **_kwargs: path.touch())
    monkeypatch.setattr(comparison_cut, "ROOT", tmp_path)
    monkeypatch.setattr(comparison_cut.sys, "argv", ["cut.py", "probe", "ru", str(plan_file)])
    monkeypatch.setattr(comparison_cut, "text_image", Mock())
    monkeypatch.setattr(comparison_cut, "caption_image", Mock())
    monkeypatch.setattr(comparison_cut, "run", Mock())
    monkeypatch.setattr(comparison_cut, "master", master)
    monkeypatch.setattr(levels, "calibrate", calibrate)

    if balanced:
        comparison_cut.main()
        master.assert_called_once_with(
            audio.inputs, audio.mix(), out, 12.0, voiced=True, audio=audio, report=report,
        )
        assert out.exists()
    else:
        with pytest.raises(SystemExit, match="balance check failed"):
            comparison_cut.main()
        master.assert_not_called()
        assert not out.exists()
    assert calibrate.call_args.kwargs["music_volume"] == 1.0
    assert json.loads(out.with_suffix(".levels.json").read_text()) == report
    output = capsys.readouterr().out
    assert all(track in output for track in ("voice", "music_pause", "music_speech", "game_speech"))
    assert ("ок" if balanced else "нет") in output


@pytest.mark.parametrize("voice_lufs,peak,click_times,failed_row", [
    (-14.0, -1.1, [], None),
    (-14.0, -0.9, [], "true_peak"),
    (-14.0, -1.1, [2.25], "clicks"),
    (-14.6, -1.1, [], "voice_mastered"),
])
def test_voiced_master_checks_decoded_audio_before_publication(
    tmp_path, monkeypatch, comparison_cut, voice_lufs, peak, click_times, failed_row,
):
    report = levels.check_balance(
        -28, None, None, -28 - levels.GAME_BELOW_VOICE_LU,
        -28 - levels.GAME_PAUSE_BELOW_VOICE_LU,
    )
    report["speech_lufs"] = -28
    _mock_mastered_balance(monkeypatch, report)
    audio = levels.AudioGraph(["-i", "joined.mp4"], ["[0:a]asplit=2[g][vo]"],
                              {"game": "g", "voice": "vo"})
    commands = []

    def render(command):
        commands.append(command)
        for argument in command:
            if argument.endswith(("short-ru.mp4", "voice.m4a")):
                Path(argument).touch()

    monkeypatch.setattr(comparison_cut, "run", render)
    passes = 3 if failed_row == "voice_mastered" else 1
    meter = Mock(side_effect=[(voice_lufs, -4)] * passes + [(-13, peak)])
    detector = Mock(return_value=click_times)
    monkeypatch.setattr(levels, "meter_file", meter)
    monkeypatch.setattr(comparison_cut.clicks, "detect", detector)
    out = tmp_path / "short-ru.mp4"
    out.write_bytes(b"stale")
    if failed_row is None:
        comparison_cut.master(["-i", "joined.mp4"], "[0:a]anull", out, 12,
                              voiced=True, audio=audio, report=report)
        assert out.exists()
    else:
        with pytest.raises(ValueError, match="mastering check failed"):
            comparison_cut.master(["-i", "joined.mp4"], "[0:a]anull", out, 12,
                                  voiced=True, audio=audio, report=report)
        assert not out.exists()
        assert not list(tmp_path.rglob("*.mp4"))
        assert _rows(report)[failed_row]["ok"] is False
    assert _rows(report)["clicks"]["times_seconds"] == click_times
    assert detector.call_args.args[0].name == out.name
    assert meter.call_args_list[-1].args[0] == detector.call_args.args[0]
    assert len(commands) == report["master_passes"] == passes
    assert len(report["master_corrections_db"]) == passes - 1
    graph = commands[0][commands[0].index("-filter_complex") + 1]
    assert "volume=14.000000000dB" in graph
    assert graph.count("alimiter=") == 1
    assert f"alimiter=limit={levels.PEAK_LIMIT}:level=false:latency=true" in graph
    assert "aeval='val(0)|val(1)|0.01':c=2.1" in graph
    assert "amultiply[voice_mastered]" in graph
    assert "loudnorm" not in graph
    json.dumps(report, allow_nan=False)


@pytest.mark.parametrize("readings,corrections,passes_ok", [
    ([-14.1], [], True),
    ([-13.9], [], True),
    ([-14.8, -14.1], [0.8], True),
    ([-14.8, -14.3, -14.0], [0.8, 0.3], True),
    ([-13.7, -14.0], [-0.3], True),
    ([-15.0, -14.8, -14.7], [1.0, 0.8], False),
])
def test_master_corrections_change_only_common_gain_and_stop_after_two(
    tmp_path, monkeypatch, comparison_cut, readings, corrections, passes_ok,
):
    report = levels.check_balance(
        -28, None, None, -28 - levels.GAME_BELOW_VOICE_LU,
        -28 - levels.GAME_PAUSE_BELOW_VOICE_LU,
    )
    report["speech_lufs"] = -28
    _mock_mastered_balance(monkeypatch, report)
    audio = levels.AudioGraph(["-i", "joined.mp4"], ["[0:a]asplit=2[g][vo]"],
                              {"game": "g", "voice": "vo"})
    mix = audio.mix()
    rendered_graphs = []

    def render(command):
        rendered_graphs.append(command[command.index("-filter_complex") + 1])
        for argument in command:
            if argument.endswith(("short-ru.mp4", "voice.m4a")):
                Path(argument).touch()

    monkeypatch.setattr(comparison_cut, "run", render)
    meter = Mock(side_effect=[(value, -4) for value in readings] + [(-13, -1.9)])
    detector = Mock(return_value=[])
    monkeypatch.setattr(levels, "meter_file", meter)
    monkeypatch.setattr(comparison_cut.clicks, "detect", detector)
    out = tmp_path / "short-ru.mp4"
    if passes_ok:
        comparison_cut.master(audio.inputs, mix, out, 12, voiced=True, audio=audio, report=report)
    else:
        with pytest.raises(ValueError, match="mastering check failed"):
            comparison_cut.master(audio.inputs, mix, out, 12, voiced=True, audio=audio, report=report)
    assert out.exists() is passes_ok
    assert report["ok"] is passes_ok
    assert report["master_passes"] == len(rendered_graphs) == len(readings) <= 3
    assert report["master_corrections_db"] == pytest.approx(corrections)
    assert report["master_gain_db"] == pytest.approx(14 + sum(corrections))
    assert detector.call_count == 1
    # Across renders only the mix and stem taps of the same common gain may change.
    gain = 14.0
    for index, graph in enumerate(rendered_graphs):
        assert graph.count(f"volume={gain:.9f}dB") == len(audio.tracks) + 1
        assert graph.replace(f"volume={gain:.9f}dB", "COMMON_GAIN") == rendered_graphs[0].replace(
            "volume=14.000000000dB", "COMMON_GAIN",
        )
        if index < len(corrections):
            gain += corrections[index]
    assert audio.mix() == mix


def test_mastering_rejects_effects_still_above_the_caps_after_two_corrections(
    tmp_path, monkeypatch, comparison_cut,
):
    report = levels.check_balance(
        -28, None, None, -28 - levels.GAME_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU,
        -28 - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU,
    )
    report["speech_lufs"] = -28
    _mock_mastered_balance(monkeypatch, report)
    # Simulate an unresponsive source/render: attenuation fails to change the
    # decoded levels. The bound must preserve the normal publication failure.
    balance = Mock(side_effect=lambda _stems, voice, *_windows:
                   levels.check_balance(
                       voice, None, None, voice - levels.GAME_BELOW_VOICE_LU + 1,
                       voice - levels.GAME_PAUSE_BELOW_VOICE_LU + 1))
    monkeypatch.setattr(levels, "check_mastered_balance", balance)
    audio = levels.AudioGraph(["-i", "joined.mp4"], ["[0:a]asplit=2[g][vo]"],
                              {"game": "g", "voice": "vo"})

    def render(command):
        for argument in command:
            if argument.endswith(("short-ru.mp4", "voice.m4a")):
                Path(argument).touch()

    monkeypatch.setattr(comparison_cut, "run", render)
    monkeypatch.setattr(levels, "meter_file", Mock(return_value=(-14, -1.9)))
    monkeypatch.setattr(comparison_cut.clicks, "detect", Mock(return_value=[]))
    out = tmp_path / "short-ru.mp4"
    out.write_bytes(b"stale")
    with pytest.raises(ValueError, match="mastering check failed"):
        comparison_cut.master(audio.inputs, audio.mix(), out, 12,
                              voiced=True, audio=audio, report=report)
    assert report["master_common_passes"] == 1
    assert report["master_passes"] == balance.call_count == levels.MAX_BALANCE_CORRECTIONS + 1
    assert len(report["balance_corrections_db"]) == levels.MAX_BALANCE_CORRECTIONS == 2
    assert _rows(report)["game_pause"]["ok"] is False
    assert _rows(report)["game_speech"]["ok"] is False
    assert report["ok"] is False
    assert not out.exists()


@pytest.mark.parametrize("failed_row", ["true_peak", "clicks", "voice_mastered"])
def test_mastering_failure_writes_report_and_click_times(
    tmp_path, monkeypatch, comparison_cut, capsys, failed_row,
):
    plan = {
        "brand": False, "labels": {"ru": ["До", "После"]},
        "hook": {"ru": "Проверка"}, "hook_s": 2,
        "beats": [{"segment": "main", "start": 0, "end": 12, "lines": [
            {"at": 4, "file": "voice.wav", "words": [["Проверка", 0, 4]]},
        ]}],
    }
    plan_file = tmp_path / "plan.json"
    plan_file.write_text(json.dumps(plan))
    out = tmp_path / "render" / "short-ru.mp4"
    out.parent.mkdir()
    out.touch()
    report = levels.check_balance(
        -22, None, None, -22 - levels.GAME_BELOW_VOICE_LU,
        -22 - levels.GAME_PAUSE_BELOW_VOICE_LU,
    )
    report["speech_lufs"] = -22
    _mock_mastered_balance(monkeypatch, report)
    audio = levels.AudioGraph(["-i", "game.mp4"], ["[0:a]asplit=2[g][vo]"],
                              {"game": "g", "voice": "vo"})

    def render(command):
        for argument in command:
            if argument.endswith(("short-ru.mp4", "voice.m4a")):
                Path(argument).touch()

    monkeypatch.setattr(comparison_cut.sys, "argv", ["cut.py", "probe", "ru", str(plan_file)])
    monkeypatch.setattr(comparison_cut, "text_image", Mock())
    monkeypatch.setattr(comparison_cut, "caption_image", Mock())
    monkeypatch.setattr(comparison_cut, "run", render)
    monkeypatch.setattr(levels, "calibrate", Mock(return_value=(audio, report)))
    voice_readings = [(-15, -4)] * 3 if failed_row == "voice_mastered" else [(-14, -4)]
    monkeypatch.setattr(levels, "meter_file", Mock(side_effect=voice_readings + [
        (-13, -0.9 if failed_row == "true_peak" else -1.1),
    ]))
    times = [2.25] if failed_row == "clicks" else []
    monkeypatch.setattr(comparison_cut.clicks, "detect", Mock(return_value=times))
    with pytest.raises(SystemExit, match="mastering check failed"):
        comparison_cut.main()
    assert not out.exists()
    written = json.loads(out.with_suffix(".levels.json").read_text())
    assert written["ok"] is False
    assert _rows(written)[failed_row]["ok"] is False
    assert _rows(written)["clicks"]["times_seconds"] == times
    printed = capsys.readouterr().out
    assert failed_row in printed
    if times:
        assert "2.250000" in printed


@pytest.mark.parametrize("speech", [-30, -20, -10])
def test_voice_based_master_gain_reaches_base(tone_inputs, speech):
    assert levels.master_gain(speech) + speech == levels.BASE_LUFS
    original = _tone_level(tone_inputs, speech)
    mastered = _tone_level(tone_inputs, speech, extra=levels.master_filter(original, 12))
    assert mastered == pytest.approx(levels.BASE_LUFS, abs=levels.VOICE_TOLERANCE_LU)


def test_synthetic_voiced_mix_publishes_voice_at_base_after_aac(
    tmp_path, tone_inputs, comparison_cut, monkeypatch,
):
    joined, voice, out = (tmp_path / name for name in ("joined.mp4", "voice.wav", "short-ru.mp4"))
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=1:d=12", *tone_inputs, "-map", "0:v", "-map", "1:a",
         "-af", "volume=-12dB", "-c:v", "mpeg4", "-c:a", "aac", str(joined)],
        check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", *tone_inputs,
         "-c:a", "pcm_s16le", str(voice)],
        check=True, capture_output=True,
    )
    audio, report = levels.calibrate(joined, None, [(voice, 0)], [(0, 12)], 12, 12)
    assert report["ok"] is True
    render = Mock(wraps=comparison_cut.run)
    monkeypatch.setattr(comparison_cut, "run", render)
    comparison_cut.master(audio.inputs, audio.mix(), out, 12, voiced=True, audio=audio, report=report)
    rows = _rows(report)
    assert rows["voice_mastered"]["measured_lufs"] == pytest.approx(-14, abs=0.5)
    assert rows["true_peak"]["measured_dbtp"] <= -1
    assert rows["clicks"]["count"] == 0
    assert report["ok"] is True
    assert out.stat().st_size > 0
    assert render.call_count == report["master_passes"] == 1
    assert report["master_corrections_db"] == []


def test_voice_correction_uses_the_mix_limiter_and_matches_a_published_voice_window(
    tmp_path, comparison_cut, monkeypatch,
):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic mastering")
    voice, joined, out = (tmp_path / name for name in ("voice.wav", "joined.mkv", "short-ru.mp4"))
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "aevalsrc='0.03*sin(2*PI*1000*t)|0':s=48000:d=6:c=stereo",
         "-c:a", "pcm_s24le", str(voice)], check=True, capture_output=True,
    )
    # Keep impulse energy proportional to the effects target so calibrated
    # peaks still exercise the mix limiter as that target changes.
    impulse_width = 0.008 * 10 ** (-levels.GAME_BELOW_VOICE_LU / 10)
    impulses = (f"aevalsrc='0|if(lt(t,3)*lt(mod(t,0.1),{impulse_width}),"
                f"0.2*sin(PI*mod(t,0.1)/{impulse_width})^2*sin(2*PI*1000*mod(t,0.1)),0)'"
                ":s=48000:d=6:c=stereo")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=1:d=6", "-f", "lavfi", "-i", impulses,
         "-c:v", "mpeg4", "-c:a", "pcm_s24le", str(joined)], check=True, capture_output=True,
    )
    audio, report = levels.calibrate(joined, None, [(voice, 0)], [(0, 6)], 6, 6)
    assert report["ok"] is True  # Effects are quiet enough in LUFS, despite their high peaks.
    measured_voice_windows = {}
    measured_full_lufs = {}
    real_meter = levels.meter_file

    def meter(path):
        if path.name in ("voice.m4a", out.name):
            # Effects stop at 3 s, leaving an independent voice-only window in both channels.
            measured_voice_windows[path.name] = levels.measure(
                ["-i", str(path)], "[0:a]anull[v]", "v", [(3.2, 5.8)],
            )
            measured_full_lufs[path.name] = _reference_lufs(["-i", str(path)], "anull")
        return real_meter(path)

    monkeypatch.setattr(levels, "meter_file", meter)
    comparison_cut.master(audio.inputs, audio.mix(), out, 6, voiced=True, audio=audio, report=report)
    rows = _rows(report)
    # The limiter lowers only the first half; the late window is louder than the full voice.
    assert measured_full_lufs[out.name] == pytest.approx(-14.0, abs=0.2)
    assert measured_full_lufs["voice.m4a"] == pytest.approx(-14.0, abs=0.2)
    assert measured_voice_windows[out.name] == pytest.approx(measured_voice_windows["voice.m4a"], abs=0.1)
    assert rows["voice_mastered"]["measured_lufs"] == pytest.approx(measured_full_lufs["voice.m4a"], abs=0.1)
    assert rows["voice_mastered"]["ok"] is True
    assert rows["true_peak"]["ok"] is True
    assert rows["clicks"]["count"] == 0
    assert report["ok"] is True
    assert 1 < report["master_common_passes"] == report["master_passes"] <= 3
    assert out.exists()


def test_unvoiced_master_keeps_original_two_pass_filters(tmp_path, monkeypatch, comparison_cut):
    measured = {"input_i": "-19.1", "input_tp": "-3.2", "input_lra": "2.0",
                "input_thresh": "-29.1", "target_offset": "0.1"}
    probe = Mock(return_value=SimpleNamespace(stderr=json.dumps(measured)))
    render = Mock()
    monkeypatch.setattr(comparison_cut.subprocess, "run", probe)
    monkeypatch.setattr(comparison_cut, "run", render)
    monkeypatch.setattr(levels, "dual_mono_peak", lambda _path: -91.0)
    comparison_cut.master(["-i", "joined.mp4"], "[0:a]anull", tmp_path / "short.mp4", 12)
    graph = render.call_args.args[0][render.call_args.args[0].index("-filter_complex") + 1]
    assert graph == (
        f"[0:a]anull,aformat=sample_rates=48000:channel_layouts=stereo,{levels.DUAL_MONO},"
        "alimiter=limit=0.75:level=false,loudnorm=I=-14:TP=-1.5:LRA=11:"
        "measured_I=-19.1:measured_TP=-3.2:measured_LRA=2.0:measured_thresh=-29.1:"
        "offset=0.1:linear=true,afade=t=out:st=11.920:d=0.08[a]"
    )


def test_peaky_voice_reaches_base_with_bounded_common_gain_corrections(
    tmp_path, comparison_cut, monkeypatch,
):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic mastering")
    voice, joined, out = (tmp_path / name for name in ("voice.wav", "joined.mkv", "short-ru.mp4"))
    # Smooth 2-ms peaks have a high crest factor without introducing clicks.
    peaky_voice = (
        "aevalsrc='(0.03+if(lt(mod(t,0.1),0.002),"
        "0.15*pow(sin(PI*mod(t,0.1)/0.002),2),0))*sin(2*PI*1000*t)':s=48000:d=6"
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", peaky_voice,
         "-c:a", "pcm_s24le", str(voice)], check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=1:d=6", "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo:d=6",
         "-c:v", "mpeg4", "-c:a", "pcm_s24le", str(joined)], check=True, capture_output=True,
    )
    audio, report = levels.calibrate(joined, None, [(voice, 0)], [(0, 6)], 6, 6)
    calibrated_mix = audio.mix()
    assert report["ok"] is True
    measured_voice = []
    real_meter = levels.meter_file

    def meter(path):
        result = real_meter(path)
        if path.name == "voice.m4a":
            measured_voice.append(result[0])
        return result

    monkeypatch.setattr(levels, "meter_file", meter)
    comparison_cut.master(audio.inputs, calibrated_mix, out, 6, voiced=True, audio=audio, report=report)
    rows = _rows(report)
    assert measured_voice[0] <= levels.BASE_LUFS - 0.5
    assert measured_voice[-1] == pytest.approx(levels.BASE_LUFS, abs=0.2)
    assert rows["voice_mastered"]["measured_lufs"] == measured_voice[-1]
    assert 1 < len(measured_voice) == report["master_passes"] <= 3
    assert len(report["master_corrections_db"]) == len(measured_voice) - 1 <= 2
    assert audio.mix() == calibrated_mix
    assert rows["voice_mastered"]["ok"] is True
    assert rows["true_peak"]["ok"] is True
    assert rows["clicks"]["count"] == 0
    assert report["ok"] is True
    assert out.stat().st_size > 0


@pytest.mark.parametrize("with_music", [False, True])
def test_mastering_recalibrates_decoded_effects_after_peaky_voice_corrections(
    tmp_path, comparison_cut, monkeypatch, with_music,
):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic mastering")
    voice, joined, out = (tmp_path / name for name in ("voice.wav", "joined.mkv", "short-ru.mp4"))
    peaky_voice = (
        "aevalsrc='(0.03+if(lt(mod(t,0.1),0.002),"
        "0.4*pow(sin(PI*mod(t,0.1)/0.002),2),0))*sin(2*PI*1000*t)':s=48000:d=4"
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", peaky_voice,
         "-c:a", "pcm_f32le", str(voice)], check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=1:d=14", "-f", "lavfi", "-i",
         "sine=frequency=440:sample_rate=48000:duration=14",
         "-c:v", "mpeg4", "-c:a", "pcm_f32le", str(joined)], check=True, capture_output=True,
    )
    music = tmp_path / "music.wav" if with_music else None
    if music is not None:
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
             "sine=frequency=220:sample_rate=48000:duration=12",
             "-c:a", "pcm_f32le", str(music)], check=True, capture_output=True,
        )
    audio, report = levels.calibrate(joined, music, [(voice, 4)], [(4, 8)], 12, 14)
    assert report["ok"] is True
    pre_master_rows = [row.copy() for row in report["rows"]]
    measured_voice, candidate_levels, balances, common_gains = [], {}, [], []
    real_meter = levels.meter_file
    real_balance, real_graph = levels.check_mastered_balance, audio.master_graph

    def balance(*args):
        result = real_balance(*args)
        balances.append(_rows(result))
        return result

    def graph(*args, **kwargs):
        common_gains.append(kwargs["gain_correction_db"])
        return real_graph(*args, **kwargs)

    def meter(path):
        result = real_meter(path)
        if path.name == "voice.m4a":
            measured_voice.append(result[0])
        elif path.name == out.name and not with_music:
            # These windows contain only effects in the actual mixed AAC.
            candidate_levels["game_pause"] = levels.measure(
                ["-i", str(path)], "[0:a]anull[mixed]", "mixed", [(0, 3.8), (8.2, 11.8)],
            )
            candidate_levels["ending"] = levels.measure(
                ["-i", str(path)], "[0:a]anull[mixed]", "mixed", [(12, 14)],
            )
        return result

    monkeypatch.setattr(levels, "meter_file", meter)
    monkeypatch.setattr(levels, "check_mastered_balance", balance)
    monkeypatch.setattr(audio, "master_graph", graph)
    original_mix = audio.mix()
    comparison_cut.master(audio.inputs, original_mix, out, 14,
                          voiced=True, audio=audio, report=report)
    rows = _rows(report)
    # Before refining the decoded effects, the limiter has taken at least 0.5 LU
    # from the voice; compensating the voice lifts the effects above both caps.
    assert measured_voice[0] <= levels.BASE_LUFS - 0.5
    assert balances[0]["game_pause"]["ok"] is False
    assert balances[0]["game_speech"]["ok"] is False
    assert 1 <= len(report["balance_corrections_db"]) <= 2
    assert 1 < len(balances) <= levels.MAX_BALANCE_CORRECTIONS + 1
    assert common_gains[report["master_common_passes"] - 1:] == pytest.approx(
        [sum(report["master_corrections_db"])] * len(balances),
    )
    assert measured_voice[-1] == pytest.approx(-14, abs=levels.VOICE_TOLERANCE_LU)
    assert rows["voice_mastered"]["measured_lufs"] == measured_voice[-1]
    assert rows["voice_mastered"]["ok"] is True
    for key, below in (("game_pause", levels.GAME_PAUSE_BELOW_VOICE_LU),
                       ("game_speech", levels.GAME_BELOW_VOICE_LU),
                       ("ending", levels.ENDING_CHECK_BELOW_VOICE_LU)):
        assert rows[key]["target_lufs"] == pytest.approx(measured_voice[-1] - below)
        assert rows[key]["ok"] is True
    assert rows["game_pause"]["measured_lufs"] == pytest.approx(
        measured_voice[-1] - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU, abs=0.2)
    assert rows["game_speech"]["measured_lufs"] <= (
        measured_voice[-1] - levels.GAME_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU + 0.2)
    assert rows["ending"]["measured_lufs"] == pytest.approx(
        measured_voice[-1] - levels.ENDING_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU, abs=0.5)
    if with_music:
        assert any(step["music_gain_db"] for step in report["balance_corrections_db"])
        assert balances[0]["music_pause"]["ok"] is False
        assert rows["music_pause"]["ok"] is True
        assert rows["music_speech"]["ok"] is True
        assert rows["music_below_game"]["ok"] is True
    else:
        for key in ("game_pause", "ending"):
            assert rows[key]["measured_lufs"] == pytest.approx(candidate_levels[key], abs=0.2)
    assert report["ending_corrections_db"]
    assert report["pre_master_rows"] == pre_master_rows
    assert all(row["ok"] is not False for row in report["pre_master_rows"])
    assert report["balance_stage"] == "mastered_aac"
    assert rows["true_peak"]["ok"] is True
    assert rows["clicks"]["count"] == 0
    assert audio.mix() == original_mix
    assert report["ok"] is True
    assert out.stat().st_size > 0


def test_mastering_checks_all_decoded_stems_of_a_balanced_music_mix(tmp_path, comparison_cut):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic mastering")
    joined, voice, music, out = (
        tmp_path / name for name in ("joined.mkv", "voice.wav", "music.wav", "short-ru.mp4")
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=1:d=12", "-f", "lavfi", "-i",
         "sine=frequency=440:sample_rate=48000:duration=12", "-af", "volume=-20dB",
         "-c:v", "mpeg4", "-c:a", "pcm_f32le", str(joined)], check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "sine=frequency=1000:sample_rate=48000:duration=4", "-af", "volume=-12dB",
         "-c:a", "pcm_f32le", str(voice)], check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "sine=frequency=220:sample_rate=48000:duration=12",
         "-c:a", "pcm_f32le", str(music)], check=True, capture_output=True,
    )
    audio, report = levels.calibrate(joined, music, [(voice, 4)], [(4, 8)], 12, 12)
    assert report["ok"] is True
    pre_master_rows = [row.copy() for row in report["rows"]]
    comparison_cut.master(audio.inputs, audio.mix(), out, 12, voiced=True, audio=audio, report=report)
    rows = _rows(report)
    assert report["balance_stage"] == "mastered_aac"
    assert rows["voice"]["measured_lufs"] == rows["voice_mastered"]["measured_lufs"]
    assert rows["voice_mastered"]["measured_lufs"] == pytest.approx(-14, abs=0.2)
    for key in ("music_pause", "music_speech", "game_pause", "game_speech",
                "game_below_voice", "music_below_game", "voice_mastered", "true_peak", "clicks"):
        assert rows[key]["ok"] is True
    assert report["pre_master_rows"] == pre_master_rows
    assert report["ok"] is True
    assert out.stat().st_size > 0


@pytest.mark.parametrize("unsafe_limit", [True, False], ids=["earlier-limiter", "current-limiter"])
def test_mastering_rejects_a_true_peak_hidden_by_the_rounded_summary(
    tmp_path, comparison_cut, monkeypatch, unsafe_limit,
):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic mastering")
    # AAC at 192k reproduces the earlier limiter's overshoot on every platform.
    # The current limiter uses the production 256k fallback, which must also preserve dual mono.
    encoder = ("-c:a", "aac", "-b:a", "192k") if unsafe_limit else levels.AAC_FALLBACK
    monkeypatch.setattr(levels, "aac_encoder", lambda: encoder)
    if unsafe_limit:
        monkeypatch.setattr(levels, "PEAK_LIMIT", 0.79)
    else:
        assert levels.PEAK_LIMIT == 0.71
    voice, joined, out = (tmp_path / name for name in ("voice.wav", "joined.mkv", "short-ru.mp4"))
    peaky_voice = (
        "aevalsrc='(0.03+if(lt(mod(t,0.1),0.002),"
        "0.4*pow(sin(PI*mod(t,0.1)/0.002),2),0))*sin(2*PI*12000*t+0.49)':s=48000:d=3"
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", peaky_voice,
         "-c:a", "pcm_f32le", str(voice)], check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=1:d=3", "-f", "lavfi", "-i", "anullsrc=r=48000:cl=stereo:d=3",
         "-c:v", "mpeg4", "-c:a", "pcm_f32le", str(joined)], check=True, capture_output=True,
    )
    audio, report = levels.calibrate(joined, None, [(voice, 0)], [(0, 3)], 3, 3)
    assert report["ok"] is True
    candidate_peak = {}
    real_meter = levels.meter_file

    def meter(path):
        result = real_meter(path)
        if path.name == out.name:
            # Independently compare the decoded candidate's detailed and rounded R128 output.
            probe = subprocess.run(
                ["ffmpeg", "-hide_banner", "-nostats", "-i", str(path), "-vn", "-af",
                 "ebur128=peak=true:metadata=1,ametadata=print:key=lavfi.r128.true_peak",
                 "-f", "null", "-"], check=True, capture_output=True, text=True,
            ).stderr
            peaks = [float(line.split("lavfi.r128.true_peak=", 1)[1])
                     for line in probe.splitlines() if "lavfi.r128.true_peak=" in line]
            candidate_peak["detailed"] = 20 * math.log10(max(peaks))
            candidate_peak["rounded"] = float(probe.rsplit("Peak:", 1)[1].split()[0])
        return result

    monkeypatch.setattr(levels, "meter_file", meter)
    if unsafe_limit:
        with pytest.raises(ValueError, match="mastering check failed"):
            comparison_cut.master(audio.inputs, audio.mix(), out, 3,
                                  voiced=True, audio=audio, report=report)
    else:
        comparison_cut.master(audio.inputs, audio.mix(), out, 3,
                              voiced=True, audio=audio, report=report)
    rows = _rows(report)
    if unsafe_limit:
        assert candidate_peak["rounded"] == levels.TRUE_PEAK_DBTP
        assert candidate_peak["detailed"] > levels.TRUE_PEAK_DBTP
    else:
        assert candidate_peak["detailed"] <= levels.TRUE_PEAK_DBTP
        assert rows["true_peak"]["measured_dbtp"] <= levels.TRUE_PEAK_DBTP
        assert out.stat().st_size > 0
    # The old linear metadata itself rounds to 0.001, about 0.005 dB here.
    assert rows["true_peak"]["measured_dbtp"] == pytest.approx(candidate_peak["detailed"], abs=0.005)
    assert rows["true_peak"]["ok"] is (not unsafe_limit)
    assert rows["voice_mastered"]["ok"] is True
    assert rows["clicks"]["ok"] is True
    assert rows["clicks"]["count"] == 0
    assert report["ok"] is (not unsafe_limit)
    assert out.exists() is (not unsafe_limit)


def test_decoded_peak_catches_a_peak_in_the_last_eighty_milliseconds(tmp_path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic true-peak measurements")
    path = tmp_path / "last-partial-block.wav"
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "aevalsrc='if(gte(t,1),0.97,0.1)*sin(2*PI*1000*t)':s=48000:d=1.08",
         "-c:a", "pcm_f32le", str(path)], check=True, capture_output=True,
    )
    _, peak = levels.meter_file(path)
    # Meter-only padding makes the final 80 ms available in precise metadata.
    assert peak == pytest.approx(20 * math.log10(0.97), abs=0.000001)
    report = levels.check_balance(-14, None, None, None, None)
    levels.check_mastering(report, -14, peak, [])
    assert _rows(report)["true_peak"]["ok"] is False
    assert report["ok"] is False


@pytest.mark.parametrize("amplitude,accepted", [(0.8914, False), (0.8912, True)])
@pytest.mark.parametrize("duration,tail_start", [(1.0, 0.0), (1.08, 1.0), (0.08, 0.0)])
def test_decoded_peak_resolves_small_exceedances_in_full_and_partial_blocks(
    tmp_path, amplitude, accepted, duration, tail_start,
):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic true-peak measurements")
    path = tmp_path / "rounded-peak.wav"
    signal = f"aevalsrc='if(lt(t,{tail_start}),0.1,{amplitude})*sin(2*PI*1000*t)':s=48000:d={duration}"
    fade = ("afade=t=in:d=0.1,afade=t=out:st=0.9:d=0.1" if duration == 1 else
            f"afade=t=in:d=0.01,afade=t=out:st={duration - 0.01}:d=0.01")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", signal,
         "-af", fade, "-c:a", "pcm_f32le", str(path)], check=True, capture_output=True,
    )
    raw = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(path), "-f", "f32le", "-"],
        check=True, capture_output=True,
    ).stdout
    sample_peak = 20 * math.log10(max(map(abs, array("f", raw))))
    loudness, peak = levels.meter_file(path)
    assert peak >= sample_peak
    assert peak == pytest.approx(sample_peak, abs=0.000001)
    # R128 integrated loudness needs a 400 ms block; the 80 ms case tests only peaks.
    if duration >= 0.4:
        # File-meter loudness uses the original audio, without measure()'s block flush.
        assert loudness == _reference_lufs(["-i", str(path)], "anull")
    report = levels.check_balance(-14, None, None, None, None)
    levels.check_mastering(report, -14, peak, [])
    assert _rows(report)["true_peak"]["ok"] is accepted
    assert report["ok"] is accepted


def test_decoded_aac_tail_peak_is_not_hidden_by_summary_rounding(tmp_path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic true-peak measurements")
    path = tmp_path / "faded-tail.m4a"
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "aevalsrc='if(gte(t,1),0.895,0.01)*sin(2*PI*1000*t)':s=48000:d=1.08",
         "-af", "afade=t=out:st=1:d=0.08", "-c:a", "aac", "-b:a", "192k", str(path)],
        check=True, capture_output=True,
    )
    diagnostic = subprocess.run(
        ["ffmpeg", "-hide_banner", "-nostats", "-i", str(path), "-af",
         "ebur128=peak=true:metadata=1,ametadata=print:key=lavfi.r128.true_peak", "-f", "null", "-"],
        check=True, capture_output=True, text=True,
    ).stderr
    rounded_peak = float(diagnostic.rsplit("Peak:", 1)[1].split()[0])
    block_peaks = [float(line.split("lavfi.r128.true_peak=", 1)[1])
                   for line in diagnostic.splitlines() if "lavfi.r128.true_peak=" in line]
    assert max(rounded_peak, 20 * math.log10(max(block_peaks))) == levels.TRUE_PEAK_DBTP
    _, peak = levels.meter_file(path)
    assert peak == pytest.approx(-0.978572, abs=0.002)
    report = levels.check_balance(-14, None, None, None, None)
    levels.check_mastering(report, -14, peak, [])
    assert _rows(report)["true_peak"]["ok"] is False
    assert report["ok"] is False


def test_four_aac_beats_keep_the_ending_on_target_after_voice_corrections(
    tmp_path, comparison_cut, monkeypatch,
):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic mastering")
    parts = []
    for index in range(5):
        ending = index == 4
        duration = 2.2 if ending else 3
        frequency = 1000 if ending else 440
        # This regression isolates the ending: body effects stay comfortably
        # below their speech ceiling while the voice receives common gain.
        gain = -12 if ending else -14 - levels.GAME_BELOW_VOICE_LU
        fade = "" if ending else ",afade=t=in:d=0.06,afade=t=out:st=2.94:d=0.06"
        part = tmp_path / f"part-{index}.mp4"
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
             f"color=c=black:s=16x16:r=30:d={duration}", "-f", "lavfi", "-i",
             f"sine=frequency={frequency}:sample_rate=48000:duration={duration}",
             "-af", f"volume={gain}dB{fade}", "-c:v", "mpeg4", "-c:a", "aac",
             "-b:a", "192k", str(part)], check=True, capture_output=True,
        )
        parts.append(part)
    playlist = tmp_path / "parts.txt"
    playlist.write_text("".join(f"file '{part}'\n" for part in parts))
    joined, voice, out = (tmp_path / name for name in ("joined.mp4", "voice.wav", "short-ru.mp4"))
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "concat", "-safe", "0",
         "-i", str(playlist), "-c", "copy", str(joined)], check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "aevalsrc='(0.03+if(lt(mod(t,0.1),0.002),"
         "0.15*pow(sin(PI*mod(t,0.1)/0.002),2),0))*sin(2*PI*1000*t)':s=48000:d=4",
         "-c:a", "pcm_f32le", str(voice)], check=True, capture_output=True,
    )
    audio, report = levels.calibrate(joined, None, [(voice, 4)], [(4, 8)], 12, 14.2)
    assert report["ok"] is True
    balances, gains = [], []
    real_balance, real_graph = levels.check_mastered_balance, audio.master_graph

    def balance(*args):
        result = real_balance(*args)
        balances.append(_rows(result))
        return result

    def graph(*args, **kwargs):
        gains.append(kwargs["gain_correction_db"])
        return real_graph(*args, **kwargs)

    monkeypatch.setattr(levels, "check_mastered_balance", balance)
    monkeypatch.setattr(audio, "master_graph", graph)
    comparison_cut.master(audio.inputs, audio.mix(), out, 14.2,
                          voiced=True, audio=audio, report=report)
    first, final = balances[0], _rows(report)
    common_correction = sum(report["master_corrections_db"])
    assert common_correction >= 0.8
    first_voice = first["voice"]["measured_lufs"]
    target = first_voice - levels.ENDING_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU
    # The original error is the correction received by the smooth ending while
    # the voice loses loudness at the limiter; AAC joins and the window agree.
    assert first["ending"]["measured_lufs"] - target == pytest.approx(
        common_correction + levels.BASE_LUFS - first_voice, abs=0.2,
    )
    assert report["ending_corrections_db"]
    assert gains[report["master_common_passes"] - 1:] == pytest.approx(
        [common_correction] * (len(report["ending_corrections_db"]) + 1),
    )
    for track in ("voice", "game_speech", "game_pause"):
        assert final[track]["measured_lufs"] == pytest.approx(first[track]["measured_lufs"], abs=0.1)
    published_ending = levels.measure(
        ["-i", str(out)], "[0:a]anull[mixed]", "mixed", [(12, 14.2)],
    )
    assert published_ending == pytest.approx(report["ending_target_lufs"], abs=0.5)
    assert final["ending"]["measured_lufs"] == pytest.approx(published_ending, abs=0.1)
    assert report["ok"] is True
    assert out.stat().st_size > 0


def test_mastering_calibrates_an_ending_audible_only_in_the_last_90_ms(tmp_path, comparison_cut):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic mastering")
    joined, voice, out = (tmp_path / name for name in ("joined.mkv", "voice.wav", "short-ru.mp4"))
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=100:d=8.29", "-f", "lavfi", "-i",
         "aevalsrc='if(lt(t,8.2),0,0.79*min((t-8.2)/0.005,1)*sin(2*PI*4000*t))':s=48000:d=8.29",
         "-c:v", "mpeg4", "-c:a", "pcm_f32le", str(joined)], check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "sine=frequency=1000:sample_rate=48000:duration=4", "-c:a", "pcm_f32le", str(voice)],
        check=True, capture_output=True,
    )
    inputs = ["-i", str(joined)]
    selection = "atrim=6:8.29,asetpts=PTS-STARTPTS"
    assert _reference_lufs(inputs, selection) <= -70
    raw_ending = _reference_lufs(inputs, f"{selection},apad=pad_dur=0.1")
    assert raw_ending > -70

    audio, report = levels.calibrate(joined, None, [(voice, 0)], [(0, 4)], 6, 8.29)
    assert report["ok"] is True
    assert math.isfinite(report["ending_gain_db"])
    assert report["ending_gain_db"] == pytest.approx(
        report["speech_lufs"] - levels.ENDING_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU - raw_ending,
        abs=0.2,
    )
    assert _rows(report)["ending"]["measured_lufs"] == pytest.approx(
        report["speech_lufs"] - levels.ENDING_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU, abs=0.2,
    )
    comparison_cut.master(audio.inputs, audio.mix(), out, 8.29,
                          voiced=True, audio=audio, report=report)
    rows = _rows(report)
    published_ending = _reference_lufs(["-i", str(out)], f"{selection},apad=pad_dur=0.1")
    assert math.isfinite(rows["ending"]["measured_lufs"])
    assert rows["ending"]["measured_lufs"] == pytest.approx(published_ending, abs=0.1)
    assert published_ending == pytest.approx(report["ending_target_lufs"], abs=0.5)
    assert published_ending <= (
        rows["voice_mastered"]["measured_lufs"] - levels.ENDING_CHECK_BELOW_VOICE_LU)
    assert rows["ending"]["ok"] is True
    assert rows["true_peak"]["ok"] is True
    assert rows["clicks"]["count"] == 0
    assert report["ok"] is True
    assert out.stat().st_size > 0


@pytest.fixture(scope="module")
def publication_inputs(tmp_path_factory, tone_inputs):
    root = tmp_path_factory.mktemp("publication-inputs")
    joined, voice, music = (root / name for name in ("joined.mkv", "voice.wav", "music.wav"))
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=1:d=14", "-f", "lavfi", "-i",
         "sine=frequency=440:sample_rate=48000:duration=14", "-af",
         "volume=-20dB,afade=t=in:d=0.06,afade=t=out:st=13.94:d=0.06",
         "-c:v", "mpeg4", "-c:a", "pcm_f32le", str(joined)], check=True, capture_output=True,
    )
    for path, frequency, duration, filters in [
        (voice, 1000, 4, "volume=-12dB,afade=t=in:d=0.06,afade=t=out:st=3.94:d=0.06"),
        (music, 220, 12, "anull"),
    ]:
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
             f"sine=frequency={frequency}:sample_rate=48000:duration={duration}",
             "-af", filters, "-c:a", "pcm_f32le", str(path)], check=True, capture_output=True,
        )
    return joined, music, [(voice, 4)], [(4, 8)], 12, 14


def _music_with_speech_delta(path, delta_db, *, ramps=True):
    gain = 10 ** (delta_db / 20)
    # Reach the new level at 4 s and hold it through 8 s; smooth transitions in
    # the adjacent pauses keep the synthetic source free of edit clicks.
    envelope = ("max(0,min(1,min((t-3.85)/0.15,(8.15-t)/0.15)))"
                if ramps else "between(t,4,8)")
    source = (f"aevalsrc='0.125*(1+({gain - 1:.12f})*({envelope}))"
              "*sin(2*PI*220*t)':s=48000:d=12")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", source,
         "-c:a", "pcm_f32le", str(path)], check=True, capture_output=True,
    )


def _music_with_fade_bump(path):
    envelope = "max(0,min(1,min((t-0.1)/0.1,(0.9-t)/0.1)))"
    source = f"aevalsrc='0.125*(1+15*({envelope}))*sin(2*PI*220*t)':s=48000:d=12"
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", source,
         "-c:a", "pcm_f32le", str(path)], check=True, capture_output=True,
    )


@pytest.mark.parametrize("speech_duration", [1.3, 0.8])
def test_calibration_falls_back_when_fades_leave_less_than_one_speech_r128_block(
    tmp_path, tone_inputs, speech_duration,
):
    game, voice, music = (tmp_path / name for name in ("game.wav", "voice.wav", "music.wav"))
    # Compensate the linear fade-in's RMS loss so speech stays calibratable
    # with the configured duck, even when the whole window lies on the fade.
    speech_boost = math.sqrt(3) / min(speech_duration, 1)
    sources = [
        (game, "sine=frequency=440:sample_rate=48000:duration=6", "volume=-20dB"),
        (voice, f"sine=frequency=1000:sample_rate=48000:duration={speech_duration}", "anull"),
        (music, f"aevalsrc='0.125*if(lt(t,{speech_duration}),{speech_boost:.12f},1)"
                "*sin(2*PI*220*t)':s=48000:d=6", "anull"),
    ]
    for path, source, filters in sources:
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", source,
             "-af", filters, "-c:a", "pcm_f32le", str(path)], check=True, capture_output=True,
        )
    speech = [(0, speech_duration)]

    _audio, report = levels.calibrate(game, music, [(voice, 0)], speech, 6, 6)

    assert report["ok"] is True
    assert report["music_speech_windows"] == speech
    assert report["music_speech_windows_fallback"] is True
    assert "400 ms" in report["music_speech_windows_fallback_reason"]
    assert _rows(report)["music_speech"]["ok"] is True
    assert math.isfinite(_rows(report)["music_speech"]["measured_lufs"])


def test_publish_rejects_loud_music_inside_a_fade_even_when_its_plateau_is_balanced(
    tmp_path, publication_inputs,
):
    joined, _music, voice, speech, total, length = publication_inputs
    music, out = tmp_path / "fade-bump.wav", tmp_path / "short.mp4"
    _music_with_fade_bump(music)

    with pytest.raises(levels.LevelsError, match="music_pause_full"):
        levels.publish(joined, music, voice, speech, total, length, out)

    report = json.loads(out.with_suffix(".levels.json").read_text())
    rows = _rows(report)
    assert report["ok"] is False
    assert not out.exists()
    assert rows["music_pause"]["ok"] is True
    assert rows["music_speech"]["ok"] is True
    assert rows["music_pause_full"]["ok"] is False
    assert rows["music_pause_full"]["measured_lufs"] > (
        rows["music_pause"]["target_lufs"] + levels.LEVEL_TOLERANCE_LU
    )


@pytest.mark.parametrize("speech,failed_row", [
    ([(4, 8)], "music_pause_full"),
    ([(0, 4)], "music_speech_full"),
])
def test_decoded_music_guard_checks_fades_in_both_full_window_types(
    tmp_path, tone_inputs, speech, failed_row,
):
    music, encoded = tmp_path / "fade-bump.wav", tmp_path / "music.m4a"
    _music_with_fade_bump(music)
    filters = ("afade=t=in:d=1,afade=t=out:st=10.5:d=1.5,"
               f"{levels.duck_filter(speech, levels.MUSIC_DUCK_DB)},volume=-20dB")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", str(music), "-af", filters,
         "-c:a", "aac", "-b:a", "192k", str(encoded)], check=True, capture_output=True,
    )
    music_speech, music_pauses, fallback = levels.music_windows(speech, 12, (1, 1.5))
    assert fallback is False
    plateau = levels.measure(["-i", str(encoded)], "[0:a]anull[m]", "m", music_pauses)

    report = levels.check_mastered_balance(
        {"music": encoded}, plateau + levels.MUSIC_BELOW_VOICE_LU,
        speech, levels.pause_windows(speech, 12), [],
        music_speech=music_speech, music_pauses=music_pauses,
    )

    rows = _rows(report)
    assert rows["music_pause"]["ok"] is True
    assert rows["music_speech"]["ok"] is True
    assert rows[failed_row]["ok"] is False
    assert rows[failed_row]["measured_lufs"] > (
        rows[failed_row]["target_lufs"] + levels.LEVEL_TOLERANCE_LU
    )
    assert report["ok"] is False


@pytest.mark.parametrize("delta_db", [3, -levels.MUSIC_DUCK_DB / 3, 0])
def test_publish_calibrates_music_duck_to_the_raw_music_dynamics(
    tmp_path, publication_inputs, delta_db,
):
    joined, _music, voice, speech, total, length = publication_inputs
    music, out = tmp_path / "dynamic-music.wav", tmp_path / "short.mp4"
    _music_with_speech_delta(music, delta_db)

    report = levels.publish(joined, music, voice, speech, total, length, out, music_fades=False)

    assert out.stat().st_size > 0
    assert report["ok"] is True
    assert report["balance_stage"] == "mastered_aac"
    assert report["music_duck_db"] == pytest.approx(levels.MUSIC_DUCK_DB + delta_db, abs=0.3)
    for rows in (_rows(report), _rows({"rows": report["pre_master_rows"]})):
        for key in ("music_pause", "music_speech"):
            assert rows[key]["ok"] is True
            assert rows[key]["measured_lufs"] == pytest.approx(
                rows[key]["target_lufs"], abs=levels.LEVEL_TOLERANCE_LU,
            )
        assert rows["music_speech"]["target_lufs"] == pytest.approx(
            rows["music_pause"]["target_lufs"] - levels.MUSIC_DUCK_DB,
        )
    assert json.loads(out.with_suffix(".levels.json").read_text())["music_duck_db"] == report["music_duck_db"]


@pytest.mark.parametrize("pre_faded,fade_in,fade_out", [(False, 1, 1.5), (True, 1.3, 1.8)])
def test_publish_measures_music_plateaus_outside_short_pause_ramps_and_fades(
    tmp_path, tone_inputs, pre_faded, fade_in, fade_out,
):
    joined, voice, music, out = (tmp_path / name for name in (
        "joined.mkv", "voice.wav", "music.wav", "short.mp4",
    ))
    total = 14.3
    # The long first/last pauses lie on music fades. Most interior pauses are
    # only 250–500 ms; removing the duck ramps leaves 700 ms of true plateau.
    speech = [(0.8, 1.8), (2.05, 3.05), (3.4, 4.4), (4.9, 5.9), (6.35, 7.35),
              (7.6, 8.6), (9, 10), (10.5, 11.5), (11.8, 12.8)]
    expected_pauses = [(3.2, 3.25), (4.55, 4.75), (6.05, 6.2), (8.75, 8.85), (10.15, 10.35)]
    expected_speech = [(max(start, fade_in), min(end, total - fade_out)) for start, end in speech]
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         f"color=c=black:s=16x16:r=10:d={total}", "-f", "lavfi", "-i",
         f"sine=frequency=440:sample_rate=48000:duration={total}", "-af",
         f"volume=-20dB,afade=t=in:d=0.06,afade=t=out:st={total - 0.06}:d=0.06",
         "-c:v", "mpeg4", "-c:a", "pcm_f32le", str(joined)], check=True, capture_output=True,
    )
    active = "+".join(
        f"max(0,min(1,min((t-{start})/0.02,({end}-t)/0.02)))" for start, end in speech
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         f"aevalsrc='0.03*({active})*sin(2*PI*1000*t)':s=48000:d={total}",
         "-c:a", "pcm_f32le", str(voice)], check=True, capture_output=True,
    )
    # Explain's Remotion stem already carries these fades; calibrating must use
    # their durations without applying them to the audio for a second time.
    filters = (f"afade=t=in:d={fade_in},afade=t=out:st={total - fade_out}:d={fade_out}"
               if pre_faded else "anull")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         f"sine=frequency=220:sample_rate=48000:duration={total}", "-af", filters,
         "-c:a", "pcm_f32le", str(music)], check=True, capture_output=True,
    )

    report = levels.publish(
        joined, music, [(voice, 0)], speech, total, total, out,
        music_fades=not pre_faded, music_fade_durations=(fade_in, fade_out),
    )

    assert report["ok"] is True
    assert report["balance_stage"] == "mastered_aac"
    assert report["music_duck_db"] == pytest.approx(levels.MUSIC_DUCK_DB, abs=0.3)
    assert report["music_pause_windows_fallback"] is False
    for key, expected in (("music_pause_windows", expected_pauses),
                          ("music_speech_windows", expected_speech)):
        for actual, interval in zip(report[key], expected, strict=True):
            assert actual == pytest.approx(interval)
    # A separate raw-stem reading at the physical plateau prevents an average
    # over fades/ramps from hiding music that is too loud between spoken lines.
    plateau = levels.measure(["-i", str(music)], "[0:a]anull[m]", "m", expected_pauses)
    pre_rows = _rows({"rows": report["pre_master_rows"]})
    calibration_gain = report["music_gain_db"] - sum(
        step.get("music_gain_db", 0) for step in report["balance_corrections_db"]
    )
    calibration_target = min(
        pre_rows["voice"]["measured_lufs"] - levels.MUSIC_BELOW_VOICE_LU,
        pre_rows["game_pause"]["measured_lufs"] - levels.MUSIC_BELOW_GAME_LU,
    )
    assert plateau + calibration_gain == pytest.approx(
        calibration_target, abs=0.5,
    )
    for rows in (pre_rows, _rows(report)):
        target = min(
            rows["voice"]["measured_lufs"] - levels.MUSIC_BELOW_VOICE_LU,
            rows["game_pause"]["measured_lufs"] - levels.MUSIC_BELOW_GAME_LU,
        )
        assert rows["music_pause"]["target_lufs"] == pytest.approx(target)
        assert rows["music_pause"]["measured_lufs"] == pytest.approx(
            rows["music_pause"]["target_lufs"], abs=0.5,
        )
        assert rows["music_speech"]["ok"] is True
    assert out.stat().st_size > 0
    saved = json.loads(out.with_suffix(".levels.json").read_text())
    assert saved["music_pause_windows_fallback"] is False
    assert saved["music_pause_windows"] == [list(window) for window in report["music_pause_windows"]]
    assert saved["music_speech_windows"] == [list(window) for window in report["music_speech_windows"]]


def test_calibration_and_publication_account_for_music_ramps_in_short_pauses(tmp_path, tone_inputs):
    joined, voice, music, out = (tmp_path / name for name in (
        "joined.mkv", "voice.wav", "music.wav", "short.mp4",
    ))
    speech = [(0, 1), (1.2, 2.2), (2.4, 3.4), (3.6, 4.6), (4.8, 5.8)]
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=1:d=6", "-f", "lavfi", "-i",
         "sine=frequency=440:sample_rate=48000:duration=6", "-af",
         "volume=-20dB,afade=t=in:d=0.06,afade=t=out:st=5.94:d=0.06",
         "-c:v", "mpeg4", "-c:a", "pcm_f32le", str(joined)],
        check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "sine=frequency=1000:sample_rate=48000:duration=6", "-af",
         "afade=t=in:d=0.06,afade=t=out:st=5.94:d=0.06",
         "-c:a", "pcm_f32le", str(voice)], check=True, capture_output=True,
    )
    # Keep raw music steady so duck ramps in the short pauses require
    # fallback refinement rather than leaving their error within tolerance.
    source = "aevalsrc='0.125*sin(2*PI*220*t)':s=48000:d=6"
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", source,
         "-c:a", "pcm_f32le", str(music)], check=True, capture_output=True,
    )

    _audio, calibrated = levels.calibrate(
        joined, music, [(voice, 0)], speech, 6, 6, music_fades=False,
    )
    assert calibrated["ok"] is True
    report = levels.publish(
        joined, music, [(voice, 0)], speech, 6, 6, out, music_fades=False,
    )

    assert report["ok"] is True
    assert report["balance_stage"] == "mastered_aac"
    assert out.stat().st_size > 0
    for measured in (calibrated, report):
        assert measured["music_pause_windows_fallback"] is True
        assert measured["music_pause_windows"] == levels.pause_windows(speech, 6)
        assert measured["music_speech_windows"] == speech
    for rows in (_rows(calibrated), _rows(report)):
        for key in ("music_pause", "music_speech"):
            assert rows[key]["ok"] is True
            assert rows[key]["measured_lufs"] == pytest.approx(
                rows[key]["target_lufs"], abs=levels.LEVEL_TOLERANCE_LU,
            )
    assert _rows(report)["clicks"]["count"] == 0
    assert json.loads(out.with_suffix(".levels.json").read_text())["ok"] is True


@pytest.mark.parametrize("delta_db,expected_duck", [
    (levels.MAX_MUSIC_DUCK_DB - levels.MUSIC_DUCK_DB + 12, levels.MAX_MUSIC_DUCK_DB),
    (-levels.MUSIC_DUCK_DB - 6, 0),
])
def test_calibration_bounds_music_duck_without_weakening_the_balance_gate(
    tmp_path, publication_inputs, delta_db, expected_duck,
):
    joined, _music, voice, speech, total, length = publication_inputs
    music = tmp_path / "extreme-music.wav"
    # Keep extreme energy entirely in speech: loud adjacent ramps can make
    # R128 gate out the quiet pauses and erase the intended raw level gap.
    # Bound the source amplitude to preserve the measured raw level gap.
    amplitude = 0.125 * 10 ** (-max(0, delta_db) / 20)
    gain = 10 ** (delta_db / 20)
    source = (f"aevalsrc='{amplitude:.12f}*(1+({gain - 1:.12f})*between(t,4,8))"
              "*sin(2*PI*220*t)':s=48000:d=12")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", source,
         "-c:a", "pcm_f32le", str(music)], check=True, capture_output=True,
    )

    _audio, report = levels.calibrate(
        joined, music, voice, speech, total, length, music_fades=False,
    )

    assert report["music_duck_db"] == expected_duck
    assert _rows(report)["music_pause"]["ok"] is True
    assert _rows(report)["music_speech"]["ok"] is False
    assert report["ok"] is False


@pytest.mark.parametrize("voiced", [False, True])
def test_calibration_keeps_default_music_duck_without_both_window_types(publication_inputs, voiced):
    joined, music, voice, _speech, total, length = publication_inputs
    _audio, report = levels.calibrate(
        joined, music, voice if voiced else [], [(0, total)] if voiced else [], total, length,
        music_fades=False,
    )
    assert report["music_duck_db"] == levels.MUSIC_DUCK_DB
    assert report["ok"] is True


def test_mastering_corrects_music_pause_gain_and_reduces_duck_in_one_render(
    tmp_path, publication_inputs, monkeypatch,
):
    joined, music, voice, speech, total, length = publication_inputs
    _audio, report = levels.calibrate(
        joined, music, voice, speech, total, length, music_fades=False,
    )
    original_gain, original_duck = report["music_gain_db"], report["music_duck_db"]
    # Model a decoded/rendered music stem whose pause is 4 LU too loud while
    # speech is still in tolerance. Lowering its gain needs a shallower duck in
    # the same pass, otherwise fixing the pause pushes speech below its target.
    report["music_gain_db"] += 4
    report["music_duck_db"] += 3
    audio = levels.audio_graph(
        joined, music, voice, speech, total, length,
        music_gain_db=report["music_gain_db"], music_duck_db=report["music_duck_db"],
        game_gain_db=report["game_gain_db"], game_duck_db=report["game_duck_db"],
        ending_gain_db=report["ending_gain_db"], music_fades=False,
    )
    real_balance, balances = levels.check_mastered_balance, []

    def balance(*args):
        result = real_balance(*args)
        balances.append(_rows(result))
        return result

    monkeypatch.setattr(levels, "check_mastered_balance", balance)
    out = tmp_path / "short.mp4"
    levels._master_voiced(audio, report, out, length)

    assert balances[0]["music_pause"]["ok"] is False
    assert balances[0]["music_speech"]["ok"] is True
    assert len(report["balance_corrections_db"]) == 1
    correction = report["balance_corrections_db"][0]
    assert correction["music_gain_db"] == pytest.approx(-4, abs=0.3)
    assert correction["music_duck_db"] == pytest.approx(-3, abs=0.3)
    assert report["music_gain_db"] == pytest.approx(original_gain, abs=0.3)
    assert report["music_duck_db"] == pytest.approx(original_duck, abs=0.3)
    assert report["master_passes"] == report["master_common_passes"] + 1
    assert _rows(report)["music_pause"]["ok"] is True
    assert _rows(report)["music_speech"]["ok"] is True
    assert report["ok"] is True
    assert out.stat().st_size > 0


def test_publish_balanced_synthetic_video_writes_the_final_report(
    tmp_path, publication_inputs, capsys,
):
    out = tmp_path / "short.mp4"
    out.write_bytes(b"stale video")
    report_path = out.with_suffix(".levels.json")
    report_path.write_text('{"stale": true}')

    report = levels.publish(*publication_inputs, out)

    assert out.stat().st_size > 0
    assert report["ok"] is True
    assert report["balance_stage"] == "mastered_aac"
    assert report["pre_master_rows"]
    rows = _rows(report)
    assert rows["voice_mastered"]["measured_lufs"] == pytest.approx(-14, abs=0.5)
    assert rows["true_peak"]["measured_dbtp"] <= -1
    assert rows["clicks"]["count"] == 0
    assert all(row["ok"] is not False for row in report["rows"])
    assert json.loads(report_path.read_text()) == json.loads(json.dumps(report, allow_nan=False))
    assert "error" not in report
    output = capsys.readouterr().out
    assert all(track in output for track in ("voice", "music_pause", "game_speech", "true_peak", "clicks"))


@pytest.mark.parametrize("encoder", [None, levels.AAC_FALLBACK],
                         ids=["platform-aac", "ffmpeg-aac-256k"])
@pytest.mark.parametrize("sound", ["tones", "pink-noise"])
def test_publish_folds_panned_game_stereo_music_and_mono_voice(
    tmp_path, publication_inputs, monkeypatch, encoder, sound,
):
    if encoder is not None:
        monkeypatch.setattr(levels, "aac_encoder", lambda: encoder)
    source, _music, voice, speech, total, length = publication_inputs
    joined, music, out = (tmp_path / name for name in ("panned.mkv", "stereo.wav", "short.mp4"))
    inputs = ["-i", str(source)]
    game_channel = "0:a"
    game_filters = "pan=stereo|c0=c0|c1=0*c0"
    music_source = "aevalsrc='0.05*sin(2*PI*220*t)|0.08*sin(2*PI*330*t)':s=48000:d=12:c=stereo"
    if sound == "pink-noise":
        inputs += ["-f", "lavfi", "-i", f"anoisesrc=d={length}:c=pink:a=0.02:s=48000:seed=42"]
        game_channel = "1:a"
        game_filters += f",afade=t=in:d=0.06,afade=t=out:st={length - 0.06}:d=0.06"
        music_source = (
            "anoisesrc=d=12:c=pink:a=0.05:s=48000:seed=43[l];"
            "anoisesrc=d=12:c=pink:a=0.08:s=48000:seed=44[r];[l][r]amerge=inputs=2"
        )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", *inputs,
         "-map", "0:v", "-map", game_channel, "-af", game_filters,
         "-c:v", "copy", "-c:a", "pcm_f32le", str(joined)],
        check=True, capture_output=True,
    )
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         music_source,
         "-c:a", "pcm_f32le", str(music)], check=True, capture_output=True,
    )
    report = levels.publish(joined, music, voice, speech, total, length, out)
    rows = _rows(report)
    assert out.stat().st_size > 0
    assert report["ok"] is True
    assert _difference_peak(out) <= -90
    assert rows["dual_mono"]["measured_db"] <= -90
    assert rows["dual_mono"]["ok"] is True
    assert rows["voice_mastered"]["measured_lufs"] == pytest.approx(-14, abs=levels.VOICE_TOLERANCE_LU)
    assert rows["true_peak"]["measured_dbtp"] <= -1
    assert rows["clicks"]["count"] == 0
    assert clicks.detect(out) == []
    assert (rows["voice"]["measured_lufs"] > rows["game_pause"]["measured_lufs"] >
            rows["music_pause"]["measured_lufs"])
    assert (rows["voice"]["measured_lufs"] > rows["game_speech"]["measured_lufs"] >
            rows["music_speech"]["measured_lufs"])


def test_publish_rejects_a_candidate_with_different_channels(tmp_path, publication_inputs, monkeypatch):
    original_graph = levels.AudioGraph.master_graph

    def panned_master(self, *args, **kwargs):
        return original_graph(self, *args, **kwargs).replace(
            "[master_mix]pan=stereo|c0=c0|c1=c0[a]",
            "[master_mix]pan=stereo|c0=c0|c1=0*c0[a]",
        )

    monkeypatch.setattr(levels.AudioGraph, "master_graph", panned_master)
    out = tmp_path / "short.mp4"
    out.write_bytes(b"stale video")
    with pytest.raises(levels.LevelsError, match=r"mastering check failed \(dual_mono\)"):
        levels.publish(*publication_inputs, out)
    report = json.loads(out.with_suffix(".levels.json").read_text())
    assert report["ok"] is False
    assert _rows(report)["dual_mono"]["measured_db"] > -90
    assert _rows(report)["dual_mono"]["ok"] is False
    assert not out.exists()
    assert not list(tmp_path.rglob("*.mp4"))


def test_publish_rejects_loud_music_and_replaces_stale_artifacts(
    tmp_path, publication_inputs, monkeypatch,
):
    out = tmp_path / "short.mp4"
    out.write_bytes(b"stale video must be removed")
    report_path = out.with_suffix(".levels.json")
    report_path.write_text('{"stale": true}')
    real_music_gain = levels.music_gain
    # Exercise real calibration and its measurements, with a deliberately bad
    # music coefficient rather than a fabricated failed report.
    monkeypatch.setattr(levels, "music_gain", lambda *args: real_music_gain(*args) + 12)

    with pytest.raises(levels.LevelsError) as failure:
        levels.publish(*publication_inputs, out)

    report = json.loads(report_path.read_text())
    assert not out.exists()
    assert report["ok"] is False
    assert _rows(report)["music_pause"]["ok"] is False
    assert _rows(report)["music_speech"]["ok"] is False
    assert report["error"] == str(failure.value)
    assert report["error"]
    assert "stale" not in report


@pytest.mark.parametrize("failure", [
    ValueError("voice is unmeasurable"),
    subprocess.CalledProcessError(1, ["ffmpeg"], stderr="invalid audio input"),
])
def test_publish_records_calibration_and_ffmpeg_errors_after_clearing_old_files(
    tmp_path, monkeypatch, failure,
):
    out = tmp_path / "short.mp4"
    report_path = out.with_suffix(".levels.json")
    out.write_bytes(b"old video")
    report_path.write_text('{"old": true}')

    def calibrate(*_args, **_kwargs):
        assert not out.exists()
        assert not report_path.exists()
        out.write_bytes(b"partial output must be removed too")
        raise failure

    monkeypatch.setattr(levels, "calibrate", calibrate)
    with pytest.raises(levels.LevelsError) as raised:
        levels.publish(tmp_path / "joined.mp4", None, [(tmp_path / "voice.wav", 0)],
                       [(0, 4)], 4, 4, out)

    report = json.loads(report_path.read_text())
    assert not out.exists()
    assert report["ok"] is False
    assert report["error"] == str(raised.value)
    assert report["error"]
    assert "old" not in report
    assert isinstance(raised.value, ValueError)


def test_publish_records_ffmpeg_mastering_failure_and_removes_partial_candidate(tmp_path, monkeypatch):
    joined, out = tmp_path / "joined.mp4", tmp_path / "short.mp4"
    out.write_bytes(b"stale video")
    report_path = out.with_suffix(".levels.json")
    report_path.write_text('{"old": true}')
    audio = levels.AudioGraph(["-i", str(joined)], ["[0:a]asplit=2[g][vo]"],
                              {"game": "g", "voice": "vo"})
    report = levels.check_balance(
        -22, None, None, -22 - levels.GAME_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU,
        -22 - levels.GAME_PAUSE_BELOW_VOICE_LU - levels.CEILING_HEADROOM_LU,
    )
    report.update(speech_lufs=-22, speech_windows=[(0, 4)], pause_windows=[(4, 8)], ending_windows=[])
    monkeypatch.setattr(levels, "calibrate", Mock(return_value=(audio, report)))

    def broken_render(command, **_kwargs):
        candidate = Path(command[command.index("-ar") + 2])
        assert candidate.name == out.name
        assert candidate.parent != out.parent
        candidate.write_bytes(b"incomplete AAC render")
        raise subprocess.CalledProcessError(1, command, stderr=b"encoder stopped")

    renderer = Mock(side_effect=broken_render)
    monkeypatch.setattr(levels.subprocess, "run", renderer)
    with pytest.raises(levels.LevelsError) as failure:
        levels.publish(joined, None, [(tmp_path / "voice.wav", 0)], [(0, 4)], 8, 8, out)

    renderer.assert_called_once()
    saved = json.loads(report_path.read_text())
    assert saved["ok"] is False
    assert saved["master_passes"] == 0
    assert saved["error"] == str(failure.value)
    assert "ffmpeg" in saved["error"]
    assert list(tmp_path.iterdir()) == [report_path]


@pytest.mark.parametrize("voiced", [False, True])
def test_music_fades_can_be_disabled_without_changing_the_music_edges(publication_inputs, voiced):
    joined, music, voice, _speech, total, length = publication_inputs

    def music_samples(fades):
        audio = levels.audio_graph(joined, music, voice if voiced else [], [], total, length,
                                   music_fades=fades)
        graph = audio.filters + [f"[{output}]anullsink" for track, output in audio.tracks.items()
                                 if track != "music"]
        result = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", *audio.inputs,
             "-filter_complex", ";".join(graph), "-map", "[m]",
             "-f", "f32le", "-ac", "1", "-ar", "48000", "-"],
            check=True, capture_output=True,
        )
        return array("f", result.stdout)

    plain, faded = music_samples(False), music_samples(True)

    def rms(samples, start, end):
        part = samples[round(start * 48000):round(end * 48000)]
        return math.sqrt(sum(value * value for value in part) / len(part))

    middle = rms(plain, 5, 6)
    assert rms(faded, 5, 6) == pytest.approx(middle, rel=0.001)
    for start, end in [(0.1, 0.2), (11.8, 11.9)]:
        assert rms(plain, start, end) == pytest.approx(middle, rel=0.001)
        assert rms(faded, start, end) < middle * 0.25


def test_music_target_ignores_a_silent_game_stem():
    assert levels.music_target(-14.0, -math.inf) == pytest.approx(-14.0 - levels.MUSIC_BELOW_VOICE_LU)
    assert levels.music_target(-14.0, None) == pytest.approx(-14.0 - levels.MUSIC_BELOW_VOICE_LU)


def test_music_below_game_does_not_apply_without_game():
    report = levels.check_balance(-14.0, -25.0, -28.0, -math.inf, -math.inf)
    rows = {row["id"]: row for row in report["rows"]}
    assert rows["music_below_game"]["ok"] is None
    assert rows["music_pause"]["ok"] is True
