"""Explainer publication uses real FFmpeg audio, with Remotion replaced by prepared stems."""

import copy
import json
import shutil
import subprocess
import sys
import wave
from array import array

import pytest

from gothic_shorts import cli, clicks, explain, levels, studio


def _rows(report):
    return {row["id"]: row for row in report["rows"]}


def test_speech_windows_use_absolute_line_words_and_merge_clip_to_rounded_body():
    plan = {
        "beats": [{"dur": 4.11}, {"dur": 5.91}],
        "lines": [
            [{"text": "first", "start": 4.2, "end": 4.5},
             {"text": "last", "start": 5.1, "end": 6}],
            [],
            [{"text": "before", "start": -0.5, "end": 0.2},
             {"text": "one", "start": 0.5, "end": 1}],
            [{"text": "overlap", "start": 0.8, "end": 1.5},
             {"text": "last", "start": 2, "end": 2.3}],
            [{"text": "end", "start": 9, "end": 9.4},
             {"text": "after", "start": 9.8, "end": 12}],
            [{"text": "outside", "start": 20, "end": 21}],
        ],
    }
    original = copy.deepcopy(plan)

    windows, total, length = explain.speech_windows(plan)

    assert total == pytest.approx(301 / 30)
    assert length == pytest.approx(301 / 30 + 66 / 30)
    for actual, expected in zip(windows, [(0, 2.3), (4.2, 6), (9, total)], strict=True):
        assert actual == pytest.approx(expected)
    assert plan == original


@pytest.mark.parametrize("brand,ending", [(False, 0), (True, 66 / 30)])
def test_speech_windows_match_remotion_rounding_and_optional_sting(brand, ending):
    # Math.round(4.5) is 5 in Remotion; Python's round(4.5) would give 4.
    windows, total, length = explain.speech_windows({
        "brand": brand, "beats": [{"dur": 0.075}, {"dur": 0.075}], "lines": [[], []],
    })
    assert windows == []
    assert total == pytest.approx(5 / 30)
    assert length == pytest.approx(total + ending)


def test_sound_spans_follow_remotion_frames_and_stop_at_freeze():
    spans = explain.sound_spans({"beats": [
        {"kind": "card", "dur": 1},
        {"kind": "footage", "dur": 2, "freeze": 1},       # silent from its 30th frame
        {"kind": "footage", "dur": 0.075},                # Math.round(92.25): frames 90..92
        {"kind": "footage", "dur": 1, "freeze": 0.1},     # 3/30 >= 0.1 although ceil(0.1 * 30) is 4
        {"kind": "footage", "dur": 1, "freeze": 5},       # past its end: the whole beat
        {"kind": "footage", "dur": 1, "freeze": 0},       # frozen from the first frame: no sound
    ]})

    expected = [(0, 1), (1, 2), (90 / 30, 92 / 30), (92 / 30, 95 / 30), (122 / 30, 152 / 30)]
    assert len(spans) == len(expected)
    for actual, span in zip(spans, expected, strict=True):
        assert actual == pytest.approx(span)


def _wav(path, signal, *, rate=48_000, sample_format="s16"):
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i",
         f"aevalsrc='{signal}':s={rate}:d=3:c=stereo", "-c:a", f"pcm_{sample_format}le", str(path)],
        check=True, capture_output=True,
    )


def _samples(path):
    with wave.open(str(path), "rb") as source:
        return array("h", source.readframes(source.getnframes()))


def test_fade_edges_removes_hard_cuts_and_keeps_samples_outside_spans(tmp_path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for the synthetic stem")
    path = tmp_path / "game.wav"
    # A silent card, then footage starting mid-wave at 1 s and cut mid-wave to other footage at 1.5 s,
    # which stops at 2.5 s; the sting fades in on its own after the last span.
    _wav(path, "if(lt(t,1),0,if(lt(t,1.5),0.5*sin(2*PI*440*t+1),if(lt(t,2.5),0.5*sin(2*PI*660*t+2),"
               "0.3*min(1,(t-2.5)/0.05)*sin(2*PI*220*(t-2.5)))))")
    assert [round(hit, 2) for hit in clicks.detect(path)] == [1, 1.5, 2.5]
    before = _samples(path)

    explain.fade_edges(path, [(0, 1), (1, 1.5), (1.5, 2.5)])

    assert clicks.detect(path) == []
    after = _samples(path)
    assert len(after) == len(before)
    rate, ramp = 48_000, round(explain.EDGE_FADE * 48_000)
    for start in (1, 1.5):
        first = 2 * round(start * rate)
        assert after[first] == after[first + 1] == 0  # each span starts silent
    for end in (1.5, 2.5):
        assert abs(after[2 * round(end * rate) - 1]) <= 16384 / ramp + 1  # and ends one step from it
    middle = slice(2 * (rate + ramp), 2 * (round(1.5 * rate) - ramp))
    assert after[middle] == before[middle]
    sting = slice(2 * round(2.5 * rate), None)
    assert after[sting] == before[sting]


def test_fade_edges_rejects_non_16_bit_stems(tmp_path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for the synthetic stem")
    path = tmp_path / "game.wav"
    _wav(path, "0.5*sin(2*PI*440*t)", sample_format="s32")

    with pytest.raises(explain.ExplainError, match="expected 16-bit PCM"):
        explain.fade_edges(path, [(0, 1)])


@pytest.fixture(scope="module")
def synthetic_stems(tmp_path_factory):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic explainer publication")
    directory = tmp_path_factory.mktemp("explainer-stems")
    subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
         "color=c=black:s=16x16:r=30:d=14.2", "-an", "-c:v", "mpeg4",
         str(directory / "picture.mp4")], check=True, capture_output=True,
    )
    for name, frequency, duration, filters in [
        ("voice", 1000, 4,
         "volume=-12dB,afade=t=in:d=0.06,afade=t=out:st=3.94:d=0.06,"
         "adelay=4000,apad=whole_dur=14.2"),
        ("game", 440, 14.2,
         "volume=-20dB,afade=t=in:d=0.06,afade=t=out:st=14.14:d=0.06"),
        ("music", 220, 12,
         "afade=t=in:d=0.5,afade=t=out:st=11.5:d=0.5,apad=whole_dur=14.2"),
    ]:
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
             f"sine=frequency={frequency}:sample_rate=48000:duration={duration}",
             "-af", filters, "-c:a", "pcm_f32le", str(directory / f"{name}.wav")],
            check=True, capture_output=True,
        )
    return directory


def _plan(*, music=True, lang="en"):
    plan = {
        "lang": lang,
        "hook": {"text": "Synthetic explainer", "until": 2},
        "beats": [{"id": "one", "kind": "footage", "src": "footage.mp4", "dur": 12}],
        "lines": [[{"text": "first", "start": 4, "end": 5},
                   {"text": "last", "start": 7, "end": 8}]],
        "voice": [{"file": "voice.wav", "start": 4}],
        # Deliberately unsuitable draft levels must not reach publication.
        "gameVolume": 0,
        "duck": 0,
        "brandVolume": 100,
    }
    if music:
        plan["music"] = {"file": "music.wav", "volume": 100, "duck": 1,
                         "fadeIn": 0.5, "fadeOut": 0.5}
    return plan


def _prepared_render(monkeypatch, synthetic_stems):
    calls = []

    def render(ws, plan, directory):
        calls.append((ws, copy.deepcopy(plan)))
        directory.mkdir(parents=True, exist_ok=True)
        for name in ("picture.mp4", "voice.wav", "game.wav", "music.wav"):
            shutil.copyfile(synthetic_stems / name, directory / name)

    monkeypatch.setattr(explain, "render", render)
    return calls


def _run_cli(monkeypatch, *arguments):
    monkeypatch.setattr(sys, "argv", ["gothic-shorts", "explain", *map(str, arguments)])
    with pytest.raises(SystemExit) as exit_info:
        cli.main()
    return exit_info.value.code


@pytest.mark.parametrize("format,width,height", [
    (None, 1080, 1920), ("tall", 1080, 1920), ("wide", 1920, 1080),
], ids=["default-tall", "tall", "wide"])
def test_render_passes_plan_dimensions_and_preserves_stems(tmp_path, monkeypatch, format, width, height):
    plan = _plan()
    if format is not None:
        plan["format"] = format
    original = copy.deepcopy(plan)
    commands = []
    monkeypatch.setattr(explain, "_run", lambda command, **kwargs: commands.append((command, kwargs)))
    faded = []
    monkeypatch.setattr(explain, "fade_edges", lambda path, spans: faded.append((path, spans)))

    explain.render(tmp_path, plan, tmp_path)

    assert faded == [(tmp_path / "game.wav", [(0, 12)])]
    assert len(commands) == 4
    for (command, kwargs), stem in zip(commands, (None, "voice", "game", "music"), strict=True):
        assert command[:4] == ["npx", "remotion", "render", "Explainer"]
        assert command[command.index("--width") + 1] == str(width)
        assert command[command.index("--height") + 1] == str(height)
        assert kwargs == {"cwd": explain.PROJECT}
        props = json.loads((tmp_path / f"{stem or 'picture'}.json").read_text())
        expected = {**plan, "stem": stem} if stem else plan
        assert props == expected
    assert plan == original


def test_render_rejects_unknown_format_before_starting_remotion(tmp_path, monkeypatch):
    commands = []
    monkeypatch.setattr(explain, "_run", lambda *args, **kwargs: commands.append(args))

    with pytest.raises(explain.ExplainError, match="format must be tall or wide"):
        explain.render(tmp_path, {**_plan(), "format": "square"}, tmp_path)

    assert commands == []


@pytest.mark.parametrize("music,format", [(True, None), (False, None), (True, "wide")],
                         ids=["music-defaults", "no-music-custom-paths", "wide-shared-levels"])
def test_cli_publishes_synthetic_stems_with_shared_levels(
    tmp_path, synthetic_stems, monkeypatch, capsys, music, format,
):
    ws = tmp_path / "explainers" / "synthetic"
    ws.mkdir(parents=True)
    plan = _plan(music=music)
    if format is not None:
        plan["format"] = format
    plan_file = "plan.json" if music else "plan.other.json"
    (ws / plan_file).write_text(json.dumps(plan))
    calls = _prepared_render(monkeypatch, synthetic_stems)
    published = []
    real_publish = levels.publish

    def publish(joined, bed, voice, speech, total, length, out, **kwargs):
        published.append((joined, bed, voice, speech, total, length, out, kwargs))
        assert joined.is_file()
        assert joined.name == "joined.mkv"
        assert len(voice) == 1 and voice[0][0].name == "voice.wav" and voice[0][1] == 0
        assert speech == [(4, 8)]
        assert total == 12 and length == 14.2
        assert kwargs["music_fades"] is False
        assert kwargs["music_fade_durations"] == ((0.5, 0.5) if music else (0, 0))
        return real_publish(joined, bed, voice, speech, total, length, out, **kwargs)

    monkeypatch.setattr(levels, "publish", publish)
    if music:
        out = ws / "render" / "short-en.mp4"
        arguments = [ws]
    else:
        monkeypatch.setattr(studio, "ROOT", ws.parent)
        out = tmp_path / "custom" / "published.mp4"
        arguments = [ws.name, "--plan", plan_file, "--out", out]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(b"stale video")
    out.with_suffix(".levels.json").write_text('{"stale": true}')

    assert _run_cli(monkeypatch, *arguments) == 0

    assert calls == [(ws, plan)]
    assert len(published) == 1
    assert (published[0][1] is not None) is music
    assert published[0][6] == out
    assert out.stat().st_size > 0
    report = json.loads(out.with_suffix(".levels.json").read_text())
    assert report["ok"] is True
    assert report["balance_stage"] == "mastered_aac"
    assert "error" not in report and "stale" not in report
    rows = _rows(report)
    assert rows["voice_mastered"]["measured_lufs"] == pytest.approx(-14, abs=0.5)
    assert rows["true_peak"]["measured_dbtp"] <= -1
    assert rows["clicks"]["count"] == 0
    assert all(row["ok"] is not False for row in report["rows"])
    assert rows["ending"]["measured_lufs"] <= rows["voice_mastered"]["measured_lufs"] - 3
    if not music:
        for name in ("music_pause", "music_speech", "music_below_game"):
            assert rows[name]["measured_lufs"] is None
            assert rows[name]["ok"] is None
    output = capsys.readouterr().out
    assert all(track in output for track in ("voice", "music_pause", "game_speech", "true_peak", "clicks"))


@pytest.mark.parametrize("fade_settings,expected", [
    ({}, (1, 1.5)),
    ({"fadeIn": 0, "fadeOut": 0}, (0, 0)),
    ({"fadeIn": 0.75, "fadeOut": 2}, (0.75, 2)),
])
def test_publish_forwards_plan_fades_with_remotion_defaults(
    tmp_path, monkeypatch, fade_settings, expected,
):
    plan = _plan()
    plan["music"] = {"file": "music.wav", "volume": 1, **fade_settings}
    (tmp_path / "plan.json").write_text(json.dumps(plan))
    monkeypatch.setattr(explain, "render", lambda *args: None)
    monkeypatch.setattr(explain, "_run", lambda *args: None)
    forwarded = []
    monkeypatch.setattr(levels, "publish", lambda *args, **kwargs: forwarded.append(kwargs))

    explain.publish(tmp_path)

    assert len(forwarded) == 1
    assert forwarded[0]["music_fades"] is False
    assert forwarded[0]["music_fade_durations"] == expected


def test_publish_measures_plateaus_of_prefaded_music_using_plan_fades(
    tmp_path, synthetic_stems, monkeypatch,
):
    # All three middle pauses are short; the longer edge pauses fall on the
    # fades already baked into Remotion's stem. Only their plateaus set gain.
    speech = [(0.8, 2.8), (3.3, 5.3), (5.8, 7.8), (8.3, 9.3)]
    plan = _plan()
    plan["lines"] = [[{"text": f"line-{index}", "start": start, "end": end}]
                     for index, (start, end) in enumerate(speech)]
    plan["music"].update(fadeIn=1, fadeOut=3)
    (tmp_path / "plan.json").write_text(json.dumps(plan))
    stems = tmp_path / "stems"
    stems.mkdir()
    for name in ("picture.mp4", "game.wav"):
        shutil.copyfile(synthetic_stems / name, stems / name)
    envelope = "+".join(
        f"max(0,min(1,min((t-{start})/0.06,({end}-t)/0.06)))"
        for start, end in speech
    )
    for name, frequency, filters in [
        ("voice", 1000, f"volume=-12dB,aeval='val(0)*({envelope})',apad=whole_dur=14.2"),
        ("music", 220, "afade=t=in:d=1,afade=t=out:st=9:d=3,apad=whole_dur=14.2"),
    ]:
        subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
             f"sine=frequency={frequency}:sample_rate=48000:duration=12",
             "-af", filters, "-c:a", "pcm_f32le", str(stems / f"{name}.wav")],
            check=True, capture_output=True,
        )
    _prepared_render(monkeypatch, stems)

    out = explain.publish(tmp_path)

    assert out.stat().st_size > 0
    report = json.loads(out.with_suffix(".levels.json").read_text())
    assert report["ok"] is True
    # Subtracting tenth-LU readings can leave a few ULPs beyond the boundary.
    assert report["music_duck_db"] == pytest.approx(levels.MUSIC_DUCK_DB, abs=0.3 + 1e-12)
    expected_pauses = [(2.95, 3.15), (5.45, 5.65), (7.95, 8.15)]
    for actual, expected in zip(report["music_pause_windows"], expected_pauses, strict=True):
        assert actual == pytest.approx(expected)
    expected_speech = [(1, 2.8), (3.3, 5.3), (5.8, 7.8), (8.3, 9)]
    for actual, expected in zip(report["music_speech_windows"], expected_speech, strict=True):
        assert actual == pytest.approx(expected)
    rows = _rows(report)
    target = min(
        rows["voice_mastered"]["measured_lufs"] - levels.MUSIC_BELOW_VOICE_LU,
        rows["game_pause"]["measured_lufs"] - levels.MUSIC_BELOW_GAME_LU,
    )
    assert rows["music_pause"]["measured_lufs"] == pytest.approx(
        target, abs=0.5,
    )
    assert rows["music_speech"]["measured_lufs"] == pytest.approx(
        target - levels.MUSIC_DUCK_DB, abs=0.5,
    )
    assert all(row["ok"] is not False for row in report["rows"])


def test_cli_rejects_loud_music_removes_stale_video_and_writes_failed_report(
    tmp_path, synthetic_stems, monkeypatch, capsys,
):
    ws = tmp_path / "synthetic"
    ws.mkdir()
    plan = _plan()
    (ws / "plan.json").write_text(json.dumps(plan))
    calls = _prepared_render(monkeypatch, synthetic_stems)
    out = ws / "render" / "short-en.mp4"
    out.parent.mkdir()
    out.write_bytes(b"stale video must be removed")
    report_path = out.with_suffix(".levels.json")
    report_path.write_text('{"stale": true}')
    real_music_gain = levels.music_gain
    # Raw music loudness is calibrated away. Make the real calibration choose a
    # bad coefficient, so the real measurements and publication gate reject it.
    monkeypatch.setattr(levels, "music_gain", lambda *args: real_music_gain(*args) + 12)

    assert _run_cli(monkeypatch, ws) == 1

    assert calls == [(ws, plan)]
    assert not out.exists()
    report = json.loads(report_path.read_text())
    assert report["ok"] is False
    assert report["error"]
    assert "stale" not in report
    rows = _rows(report)
    assert rows["music_pause"]["ok"] is False
    assert rows["music_speech"]["ok"] is False
    assert "error:" in capsys.readouterr().err
