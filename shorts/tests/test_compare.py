"""Comparison dubbing publishes synthetic takes through the real FFmpeg audio path."""

import copy
import json
import shutil
import subprocess
import sys
from types import SimpleNamespace

import pytest
from PIL import Image, ImageDraw
from test_dub import (
    CONFIG,
    _assert_publication,
    _assert_unchanged,
    _audio_pcm,
    _packet_hashes,
    _stale_outputs,
    _streams,
    _tts_sound,
)
from test_dub_publication import _assert_dual_mono

from gothic_shorts import cli, compare, dub, levels

ENGLISH = ["First change", "Another change", "The next scene", "The last change"]


def _ffmpeg(*args):
    subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-y", *map(str, args)],
        check=True, capture_output=True,
    )


def _plan(takes, *, brand=True, music=True):
    plan = {
        "takes": str(takes),
        "brand": brand,
        "labels": {"ru": ["Было", "Стало"], "en": ["Before", "After"]},
        "hook": {"ru": "Два изменения", "en": "Two changes"},
        "hook_s": 0.8,
        "beats": [
            {"segment": "first", "start": 2, "end": 8,
             "title": {"ru": "Первый", "en": "First"},
             "lines": [
                 {"at": 3, "file": "voice/01.wav",
                  "words": [["Первое", 0.25, 0.8], ["изменение", 1.1, 1.75]]},
                 {"at": 5.5, "file": "voice/02.wav",
                  "words": [["Второе", 0.3, 0.8], ["изменение", 1, 1.5]]},
             ]},
            {"segment": "second", "start": 3, "end": 9,
             "title": {"ru": "Следующий", "en": "Next"},
             "lines": [
                 {"at": 4, "file": "voice/03.wav",
                  "words": [["Следующая", 0.4, 1], ["сцена", 1.3, 1.9]]},
                 {"at": 6.5, "file": "voice/04.wav",
                  "words": [["Последнее", 0.2, 0.8], ["изменение", 1.1, 1.7]]},
             ]},
        ],
    }
    if music:
        # Voiced publication must calibrate this deliberately unsuitable draft volume.
        plan["music"] = {"file": "music.wav", "volume": 100}
    return plan


@pytest.fixture(scope="module")
def comparison_assets(tmp_path_factory):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for synthetic comparison dubbing")
    directory = tmp_path_factory.mktemp("comparison-assets")
    takes, brand = directory / "takes", directory / "brand"
    for name in ("off", "on", "voice"):
        (takes / name).mkdir(parents=True)
    brand.mkdir()
    for index, segment in enumerate(("first", "second")):
        for side, color in (("off", "blue"), ("on", "green")):
            _ffmpeg(
                "-f", "lavfi", "-i", f"color=c={color}:s=96x96:r=30:d=10",
                "-an", "-c:v", "libx264", "-pix_fmt", "yuv420p", takes / side / f"{segment}.mp4",
            )
        _ffmpeg(
            "-f", "lavfi", "-i", f"sine=frequency={440 + 110 * index}:sample_rate=48000:duration=10",
            "-af", "volume=-20dB", "-ac", "2", "-c:a", "pcm_s16le", takes / "on" / f"{segment}.wav",
        )
    for index, (first, last) in enumerate(((0.25, 1.75), (0.3, 1.5), (0.4, 1.9), (0.2, 1.7)), 1):
        seconds = last - first
        _ffmpeg(
            "-f", "lavfi", "-i", f"sine=frequency={800 + index * 100}:sample_rate=48000:duration={seconds}",
            "-af", (f"volume=-12dB,afade=t=in:d=0.06,afade=t=out:st={seconds - 0.06}:d=0.06,"
                    f"adelay=delays={round(first * 48000)}S:all=1,apad=whole_dur={last + 0.2}"),
            "-c:a", "pcm_s16le", takes / "voice" / f"{index:02d}.wav",
        )
    _ffmpeg(
        "-f", "lavfi", "-i", "sine=frequency=220:sample_rate=48000:duration=12",
        "-af", "afade=t=in:d=0.5,afade=t=out:st=11.5:d=0.5",
        "-ac", "2", "-c:a", "pcm_s16le", takes / "music.wav",
    )
    _ffmpeg(
        "-f", "lavfi", "-i", "color=c=red:s=96x192:r=30:d=2.2",
        "-f", "lavfi", "-i", "sine=frequency=660:sample_rate=48000:duration=2.2",
        "-af", "volume=-20dB,afade=t=in:d=0.06,afade=t=out:st=2.14:d=0.06",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-ar", "48000", "-ac", "2",
        brand / "sting.mp4",
    )
    placeholder = directory / "published.mp4"
    _ffmpeg(
        "-f", "lavfi", "-i", "color=c=black:s=16x16:r=30:d=14.2",
        "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=14.2",
        "-c:v", "mpeg4", "-c:a", "aac", "-ar", "48000", "-ac", "2", placeholder,
    )
    return SimpleNamespace(takes=takes, brand=brand, placeholder=placeholder)


@pytest.fixture(autouse=True)
def no_external_models(monkeypatch):
    def unexpected(*args, **kwargs):
        raise AssertionError("comparison tests must never request real voices or GPU work")

    for name in ("say", "voxcpm", "job", "clone"):
        monkeypatch.setattr(dub, name, unexpected)


def _small_comparison(monkeypatch, brand):
    monkeypatch.setattr(compare, "W", 96)
    monkeypatch.setattr(compare, "H", 192)
    monkeypatch.setattr(compare, "HALF", 96)
    monkeypatch.setattr(compare, "BRAND", brand)

    def overlay(path, text):
        image = Image.new("RGBA", (96, 192))
        # Different strings produce different pixels, so retaining Russian video
        # is observable even though the test replaces only font rendering.
        color = sum(text.encode()) % 256
        ImageDraw.Draw(image).rectangle((0, 88, 95, 94), fill=(color, 255 - color, 40, 150))
        image.save(path)

    monkeypatch.setattr(compare, "text_image", lambda path, items: overlay(path, items[0][0]))
    monkeypatch.setattr(compare, "caption_image", lambda path, words, current: overlay(path, words[current]))


@pytest.mark.parametrize("encoder", ["platform", "ffmpeg-aac-256k"])
@pytest.mark.parametrize("signal", ["tones", "noise"])
def test_unvoiced_master_folds_stereo_before_loudnorm(
    tmp_path, comparison_assets, monkeypatch, encoder, signal,
):
    if encoder == "ffmpeg-aac-256k":
        monkeypatch.setattr(levels, "aac_encoder", lambda: levels.AAC_FALLBACK)
    source = tmp_path / "panned.wav"
    sound = (
        "aevalsrc=0.1*sin(2*PI*440*t)|0.07*sin(2*PI*660*t):s=48000:d=3:c=stereo"
        if signal == "tones" else
        "anoisesrc=d=3:c=pink:a=0.2:s=48000:seed=42[left];"
        "anoisesrc=d=3:c=pink:a=0.1:s=48000:seed=43[right];[left][right]amerge=inputs=2"
    )
    _ffmpeg(
        "-f", "lavfi", "-i", sound,
        "-af", "afade=t=in:d=0.06,afade=t=out:st=2.94:d=0.06", "-c:a", "pcm_f32le", source,
    )
    out = tmp_path / "unvoiced.mp4"

    compare.master(
        ["-i", str(comparison_assets.placeholder), "-i", str(source)],
        "[1:a]atrim=duration=3", out, 3,
    )

    audio, = _streams(out)
    assert audio["codec_name"] == "aac" and audio["channels"] == 2
    _assert_dual_mono(out)


def test_unvoiced_master_removes_audio_that_fails_dual_mono(
    tmp_path, comparison_assets, monkeypatch,
):
    out = tmp_path / "unvoiced.mp4"
    real_run = compare.run

    def divergent_channels(cmd):
        cmd = list(cmd)
        index = cmd.index("-filter_complex") + 1
        cmd[index] = cmd[index].replace("[a]", ",pan=stereo|c0=c0|c1=0*c1[a]")
        real_run(cmd)

    monkeypatch.setattr(compare, "run", divergent_channels)
    monkeypatch.setattr(levels, "aac_encoder", lambda: levels.AAC_FALLBACK)

    with pytest.raises(levels.LevelsError, match="dual_mono"):
        compare.master(
            ["-i", str(comparison_assets.placeholder)], "[0:a]atrim=duration=3", out, 3,
        )

    assert not out.exists()


@pytest.mark.parametrize("brand,music", [(True, True), (False, False)], ids=["brand-and-music", "plain"])
def test_comparison_dub_publishes_the_russian_effects_ending_and_picture(
    tmp_path, monkeypatch, comparison_assets, brand, music,
):
    assets = comparison_assets
    assert not (assets.brand / "seal-corner.png").exists()  # brand now needs only the ending
    ws = tmp_path / "comparison"
    ws.mkdir()
    plan = _plan(assets.takes, brand=brand, music=music)
    plan_file = "plan.comparison.json"
    (ws / plan_file).write_text(json.dumps(plan))
    (ws / "script.en.txt").write_text("\n".join(ENGLISH) + "\n")
    assert not (ws / "words.ru.json").exists()  # the compare plan already carries word timing
    _small_comparison(monkeypatch, assets.brand)
    captured, english_slots, cloned = [], [], []
    real_publish, real_stem, real_check = levels.publish, dub._english_stem, dub._check_duration

    def publish(joined, bed, voice, speech, total, length, out, **kwargs):
        assert total == 12
        if captured:
            assert length == captured[0].length  # includes the encoded ending's AAC padding
        else:
            assert length == pytest.approx(14.2 if brand else 12, abs=1 / 30)
        assert kwargs.get("music_fades", True) is True
        assert (bed is not None) is music
        if bed is not None:
            assert bed == assets.takes / "music.wav"
        captured.append(SimpleNamespace(
            pcm=_audio_pcm(joined), speech=copy.deepcopy(speech),
            video=_packet_hashes(joined, "v:0"), length=length,
        ))
        if out.name == "dub.en.mp4":
            assert len(voice) == 1 and voice[0][1] == 0
            assert float(_streams(voice[0][0])[0]["duration"]) == pytest.approx(total, abs=1 / 48000)
        return real_publish(joined, bed, voice, speech, total, length, out, **kwargs)

    def clone(config, workspace, texts_en, lang, tts=None, reclone=False):
        cloned.append((workspace, texts_en, lang, tts, reclone))
        return [
            _tts_sound(ws / f"voice/en/raw/{index:02d}.wav", seconds=1.15, frequency=900 + index * 100)
            for index in range(1, len(texts_en) + 1)
        ]

    def english_stem(done, line_slots, recordings, names, total, work, *, clean=False):
        assert clean is True
        english_slots.extend(line_slots)
        return real_stem(done, line_slots, recordings, names, total, work, clean=clean)

    def check_duration(length, russian_duration):
        assert length == captured[0].length  # preflight uses the encoded ending too
        real_check(length, russian_duration)

    monkeypatch.setattr(levels, "publish", publish)
    monkeypatch.setattr(dub, "clone", clone)
    monkeypatch.setattr(dub, "_english_stem", english_stem)
    monkeypatch.setattr(dub, "_check_duration", check_duration)
    monkeypatch.setattr(sys, "argv", ["cut.py", "synthetic", "ru", str(ws / plan_file)])
    compare.main()
    russian = ws / "render/short-ru.mp4"
    assert russian.is_file() and russian.with_suffix(".png").is_file()
    _stale_outputs(ws / "render")
    done = dub.dub_compare(CONFIG, ws, plan_file=plan_file, tts="inworld", reclone=True)

    assert cloned == [(ws, ENGLISH, "en", "inworld", True)]
    assert done.wav == ws / "render/dub.en.wav" and done.preview == ws / "render/dub.en.mp4"
    assert len(done.rows) == 4 and not done.too_long
    assert len(captured) == 2
    assert captured[1].pcm == captured[0].pcm  # effects and ending survive the RU picture mux byte for byte
    assert captured[1].video == _packet_hashes(russian, "v:0")
    windows, total = levels.speech_windows(plan)
    assert total == 12
    assert captured[0].speech == windows
    for actual, expected in zip(windows, [(1.25, 2.75), (3.8, 5), (7.4, 8.9), (9.7, 11.2)], strict=True):
        assert actual == pytest.approx(expected)
    assert [slot.start for slot in english_slots] == pytest.approx([start for start, _ in windows])
    assert [slot.end for slot in english_slots] == pytest.approx([
        windows[1][0] - dub.GAP_S, windows[2][0] - dub.GAP_S,
        windows[3][0] - dub.GAP_S, total - dub.TAIL_S,
    ])
    assert [window[0] for window in captured[1].speech] == pytest.approx([start for start, _ in windows])
    report = _assert_publication(done, russian)
    assert report["speech_windows"] == [list(window) for window in captured[1].speech]


@pytest.fixture
def comparison_workspace(tmp_path, monkeypatch, comparison_assets):
    assets = comparison_assets
    ws = tmp_path / "comparison"
    directory = ws / "render"
    directory.mkdir(parents=True)
    takes = tmp_path / "takes"
    shutil.copytree(assets.takes, takes)
    plan = _plan(takes)
    (ws / "plan.json").write_text(json.dumps(plan))
    (ws / "script.en.txt").write_text("\n".join(ENGLISH) + "\n")
    russian = directory / "short-ru.mp4"
    shutil.copyfile(assets.placeholder, russian)
    russian.with_suffix(".levels.json").write_text(json.dumps({
        "ok": True, "rows": [{"id": "voice_mastered", "measured_lufs": -14}],
    }))
    monkeypatch.setattr(compare, "BRAND", assets.brand)
    return SimpleNamespace(ws=ws, directory=directory, takes=takes, plan=plan, russian=russian)


@pytest.mark.parametrize("invalid", [
    "takes-missing", "takes-not-directory", "off-video", "on-video", "on-audio", "music",
    "script-missing", "script-count", "plan-missing", "report-missing", "report-false", "language", "length",
])
def test_comparison_input_refusals_preserve_the_previous_dub(
    comparison_workspace, monkeypatch, invalid,
):
    case = comparison_workspace
    lang = "de" if invalid == "language" else "en"
    before = _stale_outputs(case.directory)
    if lang != "en":
        before.update(_stale_outputs(case.directory, lang))
    if invalid == "takes-missing":
        case.plan.pop("takes")
    elif invalid == "takes-not-directory":
        case.plan["takes"] = str(case.ws / "missing-takes")
    elif invalid in ("off-video", "on-video", "on-audio"):
        side, suffix = {"off-video": ("off", "mp4"), "on-video": ("on", "mp4"),
                        "on-audio": ("on", "wav")}[invalid]
        (case.takes / side / f"first.{suffix}").unlink()
    elif invalid == "music":
        (case.takes / "music.wav").unlink()
    elif invalid == "script-missing":
        (case.ws / "script.en.txt").unlink()
    elif invalid == "script-count":
        (case.ws / "script.en.txt").write_text("One\nTwo\n")  # two beats, four voiced lines
    elif invalid == "report-missing":
        case.russian.with_suffix(".levels.json").unlink()
    elif invalid == "report-false":
        case.russian.with_suffix(".levels.json").write_text('{"ok": false}')
    elif invalid == "length":
        case.plan["brand"] = False
    (case.ws / "plan.json").write_text(json.dumps(case.plan))
    if invalid == "plan-missing":
        (case.ws / "plan.json").unlink()

    def unexpected_build(*args, **kwargs):
        raise AssertionError("invalid inputs must be refused before assembling or clearing outputs")

    monkeypatch.setattr(compare, "build", unexpected_build)
    match = ("--takes" if invalid.startswith("takes") else
             "shorts/compare/cut.py" if invalid.startswith("report") else None)
    with pytest.raises(dub.DubError, match=match):
        dub.dub_compare(CONFIG, case.ws, lang)
    _assert_unchanged(before)


@pytest.mark.parametrize("source", ["plan", "override", "plan-home", "override-home"])
def test_comparison_takes_paths_expand_home_and_cli_overrides_the_plan(
    comparison_workspace, monkeypatch, tmp_path, source,
):
    case = comparison_workspace
    monkeypatch.setenv("HOME", str(tmp_path))
    supplied = "~/takes" if source.endswith("home") else str(case.takes)
    override = supplied if source.startswith("override") else None
    case.plan["takes"] = str(case.ws / "wrong-takes") if override is not None else supplied
    (case.ws / "plan.json").write_text(json.dumps(case.plan))
    # The output of `words` has a different timeline and must not be consulted.
    (case.ws / "words.ru.json").write_text('{"lines": "not comparison timing"}')
    before = _stale_outputs(case.directory)
    seen = []

    def build(plan, takes, lang, work):
        seen.append(takes)
        assert plan == case.plan and lang == "ru" and work.is_dir()
        assert all(not path.exists() for path in before)
        raise dub.DubError("assembly reached after all validation")

    monkeypatch.setattr(compare, "build", build)
    with pytest.raises(dub.DubError, match="assembly reached"):
        dub.dub_compare(CONFIG, case.ws, takes=override)
    assert seen == [case.takes]
    assert all(not path.exists() for path in before)


@pytest.mark.parametrize("configured", [False, True])
def test_compare_cut_keeps_legacy_takes_default_and_accepts_the_plan_path(tmp_path, monkeypatch, configured):
    plan = _plan("~/configured-takes", brand=False)
    if not configured:
        plan.pop("takes")
    plan_file = tmp_path / "plan.json"
    plan_file.write_text(json.dumps(plan))
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.setattr(compare, "ROOT", tmp_path)
    monkeypatch.setattr(sys, "argv", ["cut.py", "probe", "ru", str(plan_file)])
    seen = []

    def build(plan, takes, lang, work):
        seen.append(takes)
        raise RuntimeError("assembly reached")

    monkeypatch.setattr(compare, "build", build)
    with pytest.raises(RuntimeError, match="assembly reached"):
        compare.main()
    assert seen == [tmp_path / ("configured-takes" if configured else ".shorts/compare/probe")]


@pytest.mark.parametrize("comparison", [True, False], ids=["comparison", "explainer"])
def test_dub_cli_routes_workspace_plans_and_forwards_comparison_takes(
    tmp_path, monkeypatch, comparison, capsys,
):
    ws, takes = tmp_path, tmp_path / "selected-takes"
    plan = _plan(takes) if comparison else {"beats": [{"dur": 12}]}
    (ws / "custom-plan.json").write_text(json.dumps(plan))
    done = dub.Dub(ws / "render/dub.en.wav", ws / "render/dub.en.mp4", 14.2, 14.2, -14, -14)
    calls = []

    def no_shot(short):
        raise cli.ShotError("workspace")

    monkeypatch.setattr(cli, "_shot", no_shot)
    monkeypatch.setattr(cli, "_config", lambda: CONFIG)
    monkeypatch.setattr(cli, "dub_compare", lambda *args: calls.append(("compare", args)) or done)
    monkeypatch.setattr(cli, "dub_explainer", lambda *args: calls.append(("explain", args)) or done)
    monkeypatch.setattr(sys, "argv", [
        "gothic-shorts", "dub", str(ws), "--plan", "custom-plan.json", "--takes", str(takes),
        "--tts", "voxcpm", "--reclone",
    ])
    with pytest.raises(SystemExit) as exit_info:
        cli.main()
    assert exit_info.value.code == 0
    common = (CONFIG, ws, "en", "custom-plan.json", "voxcpm", True)
    expected = [("compare", (*common, str(takes)))] if comparison else [("explain", common)]
    assert calls == expected
    output = capsys.readouterr().out
    assert "voice_mastered" in output and str(done.wav) in output and str(done.preview) in output
