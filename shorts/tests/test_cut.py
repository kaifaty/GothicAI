import json
import shutil
import subprocess
import sys
from array import array
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest
from PIL import Image

from gothic_shorts import bed, cli, compare, cut, levels
from gothic_shorts.shot import Shot
from gothic_shorts.timeline import Timeline, build, read_events


def _ffmpeg(*args):
    subprocess.run(
        ["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", *args],
        check=True, capture_output=True,
    )


def _events(path, events):
    path.write_text("\n".join(json.dumps({"seg": "main", **event}) for event in events) + "\n")


@pytest.fixture(scope="module")
def synthetic_assets(tmp_path_factory):
    if shutil.which("ffmpeg") is None or shutil.which("ffprobe") is None:
        pytest.skip("ffmpeg and ffprobe are required for synthetic cut publication")
    root = tmp_path_factory.mktemp("ai-cut")
    take, brand, music = (root / name for name in ("take", "brand", "music"))
    for directory in (take, brand, music, take / "run"):
        directory.mkdir()
    (take / "run/Gothic.ini").write_text("[SOUND]\nmusicVolume=0\nsoundVolume=1\nworldSounds=0\n")
    _ffmpeg(
        "-f", "lavfi", "-i", "testsrc2=size=108x192:rate=30:duration=18",
        "-an", "-c:v", "libx264", "-pix_fmt", "yuv420p", str(take / "main.mp4"),
    )
    # Broadband effects are intentionally independent of the voiced tones.
    # Leave a long tail pause so music fades/duck ramps do not dominate its
    # integrated pause loudness: this fixture is meant to pass every gate.
    _ffmpeg(
        "-f", "lavfi", "-i", "anoisesrc=color=pink:seed=19:sample_rate=48000:duration=18",
        "-af", "volume=-36dB,afade=t=in:d=0.06,afade=t=out:st=17.94:d=0.06",
        "-ac", "2", "-c:a", "pcm_s16le", str(take / "main.wav"),
    )
    # Two takes of the hero retain a 6 dB expressive difference. The speaker as
    # a whole must match the two NPCs, despite their very different raw gains.
    for name, frequency, gain, rate in [
        ("hero-1.wav", 700, -4, 24000),
        ("hero-2.wav", 700, -10, 24000),
        ("main.voice-01.wav", 1000, -15, 22050),
        ("main.voice-02.wav", 1500, 2, 32000),
    ]:
        _ffmpeg(
            "-f", "lavfi", "-i", f"sine=frequency={frequency}:sample_rate={rate}:duration=1.2",
            "-af", f"volume={gain}dB,afade=t=in:d=0.06,afade=t=out:st=1.14:d=0.06",
            "-ac", "1", "-c:a", "pcm_s16le", str(take / name),
        )
    events = [
        {"kind": "rec_start", "frame": 0},
        {"kind": "hero", "frame": 30, "ms": 1200, "text": "Привет, Ксардас.",
         "wav": str(take / "hero-1.wav")},
        {"kind": "heard", "frame": 67, "text": "Привет, Ксардас."},
        {"kind": "caption", "frame": 75, "text": "Он думает"},
        {"kind": "npc_line", "frame": 120, "ms": 900, "npc": "XARDAS", "name": "Ксардас",
         "text": "Ты снова здесь?"},
        {"kind": "npc_voice", "frame": 121, "npc": "XARDAS", "line": "ai-1",
         "wav": str(take / "main.voice-01.wav"), "sample": 192600},
        {"kind": "npc_voice_end", "frame": 157, "npc": "XARDAS"},
        {"kind": "caption", "frame": 165, "text": ""},
        {"kind": "hero", "frame": 180, "ms": 1200, "text": "А Диего где?",
         "wav": str(take / "hero-2.wav")},
        {"kind": "npc_line", "frame": 261, "ms": 900, "npc": "DIEGO", "name": "Диего",
         "text": "Я здесь."},
        {"kind": "npc_voice", "frame": 262, "npc": "DIEGO", "line": "ai-2",
         "wav": str(take / "main.voice-02.wav"), "sample": 417600},
        {"kind": "npc_voice_end", "frame": 297, "npc": "DIEGO"},
        {"kind": "rec_stop", "frame": 540, "frames": 540},
    ]
    _events(take / "events.jsonl", events)
    # The first 2 seconds are dirty: a correct bed starts at the clean tone.
    _ffmpeg(
        "-f", "lavfi", "-i", "sine=frequency=90:sample_rate=48000:duration=2",
        "-f", "lavfi", "-i", "sine=frequency=220:sample_rate=48000:duration=20",
        "-filter_complex", "[0:a][1:a]concat=n=2:v=0:a=1[m]", "-map", "[m]",
        "-c:a", "flac", str(music / "khorinis-countryside.flac"),
    )
    (music / "catalog.tsv").write_text("file\tclean_from_s\nkhorinis-countryside.flac\t2\n")
    _ffmpeg(
        "-f", "lavfi", "-i", "color=c=blue:s=108x192:r=30:d=1.2",
        "-f", "lavfi", "-i", "sine=frequency=350:sample_rate=48000:duration=1.2",
        "-af", "afade=t=in:d=0.06,afade=t=out:st=1.14:d=0.06",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", str(brand / "sting.mp4"),
    )
    shot = Shot("probe", "Проба", "rec main", hook="Я соврал NPC")
    return SimpleNamespace(take=take, brand=brand, music=music, events=events, shot=shot)


def _small_cut(monkeypatch, assets):
    monkeypatch.setattr(cut, "W", 108)
    monkeypatch.setattr(cut, "H", 192)
    monkeypatch.setattr(cut, "BRAND", assets.brand)
    monkeypatch.setattr(bed, "MUSIC", assets.music)

    def overlays(on_screen, work, *, brand=True):
        rendered = []
        for i, text in enumerate(on_screen):
            png = work / f"{text.kind}{i}.png"
            # A real alpha overlay exercises ffmpeg's PNG inputs at tiny size;
            # the normal text renderer has its own safe-area/font regression below.
            Image.new("RGBA", (108, 12), (255, 214, 102, 128)).save(png)
            rendered.append(cut.Overlay(png, text.start, text.end, 20))
        return rendered

    monkeypatch.setattr(cut, "_overlays", overlays)


@pytest.fixture(scope="module")
def published_cut(synthetic_assets):
    with pytest.MonkeyPatch.context() as monkeypatch:
        _small_cut(monkeypatch, synthetic_assets)
        out = cut.cut(synthetic_assets.shot, synthetic_assets.take)
    return SimpleNamespace(
        **vars(synthetic_assets), out=out,
        report=json.loads(out.with_suffix(".levels.json").read_text()),
        timeline=build(read_events(synthetic_assets.take / "events.jsonl")),
    )


def _rows(report):
    return {row["id"]: row for row in report["rows"]}


def _level(path, windows):
    return levels.measure(["-i", str(path)], "[0:a]anull[v]", "v", windows)


def _pcm(path):
    return subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-i", str(path), "-map", "0:a:0",
         "-f", "s16le", "-ar", "48000", "-ac", "2", "-"],
        check=True, capture_output=True,
    ).stdout


@pytest.mark.parametrize("brand", [True, False])
@pytest.mark.parametrize("overlay_count", [0, 2])
def test_ai_filters_only_overlay_text_and_append_the_ending(tmp_path, monkeypatch, brand, overlay_count):
    video, audio = tmp_path / "main.mp4", tmp_path / "main.wav"
    overlays = [cut.Overlay(tmp_path / f"line{i}.png", 0, 2, 20) for i in range(overlay_count)]
    run = Mock(return_value=SimpleNamespace(returncode=0))
    monkeypatch.setattr(cut.subprocess, "run", run)
    monkeypatch.setattr(cut, "BRAND", tmp_path / "brand")

    cut._join(Shot("probe", "Проба", "rec main", brand=brand), video, audio,
              Timeline(3), overlays, tmp_path, 1.2 if brand else 0)

    command = run.call_args.args[0]
    inputs = [Path(command[i + 1]) for i, arg in enumerate(command) if arg == "-i"]
    assert inputs == [video, audio, *(overlay.png for overlay in overlays),
                      *([cut.BRAND / "sting.mp4"] if brand else [])]
    graph = command[command.index("-filter_complex") + 1]
    assert graph.count("overlay=") == overlay_count
    if brand:
        index = 2 + overlay_count
        assert f"[{index}:v]scale=" in graph
        assert f"[{index}:a]aformat=" in graph
        assert "[bodyv][bodya][stingv][stinga]concat=n=2:v=1:a=1" in graph
    else:
        assert "[stingv]" not in graph and "[stinga]" not in graph


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="ffmpeg is required")
def test_effects_body_and_sting_render_as_dual_mono(tmp_path, monkeypatch):
    monkeypatch.setattr(cut, "BRAND", tmp_path)
    source = tmp_path / "effects.wav"
    _ffmpeg(
        "-f", "lavfi", "-i",
        "aevalsrc=0.1*sin(2*PI*440*t)|0*t:s=48000:d=3:c=stereo",
        "-c:a", "pcm_f32le", str(source),
    )
    _ffmpeg(
        "-f", "lavfi", "-i",
        "aevalsrc=0.1*sin(2*PI*550*t)|0.2*sin(2*PI*770*t):s=48000:d=1:c=stereo",
        "-c:a", "aac", str(tmp_path / "sting.mp4"),
    )
    tl = Timeline(3, cuts=[(1, 1.5)])
    out, duration = cut.render_effects(
        Shot("probe", "Probe", "rec main", brand=True), source, tl, tmp_path / "joined.wav", 1,
    )
    decoded = subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-i", str(out), "-f", "f32le", "-"],
        check=True, capture_output=True,
    ).stdout
    samples = array("f", decoded)
    if sys.byteorder != "little":
        samples.byteswap()
    assert samples[::2] == samples[1::2]
    body = samples[round(0.1 * 48000) * 2:round((duration - 0.1) * 48000) * 2:2]
    ending = samples[round((duration + 0.1) * 48000) * 2:-round(0.1 * 48000) * 2:2]
    assert max(abs(sample) for sample in body) == pytest.approx(0.05, abs=0.001)
    assert max(abs(sample) for sample in ending) > 0.05


@pytest.mark.parametrize("brand", [True, False])
@pytest.mark.parametrize("cover_kind", [None, "default", "custom"])
def test_compare_filters_keep_text_and_slow_marks_and_append_the_ending(
    tmp_path, monkeypatch, capsys, brand, cover_kind,
):
    if cover_kind and not Path("/System/Library/Fonts/Supplemental/Arial Black.ttf").is_file():
        pytest.skip("macOS Arial Black is unavailable")
    plan = {
        "brand": brand, "labels": {"ru": ["До", "После"]},
        "hook": {"ru": "Проверка"}, "hook_s": 2,
        "beats": [
            {"segment": "main", "start": 0, "end": 3,
             "slow": {"from": 1, "to": 2, "ru": "Замедлено"},
             "lines": [{"at": 1, "file": "voice.wav", "words": [["Проверка", 0, 0.8]]}]},
            {"segment": "next", "start": 0, "end": 3, "title": {"ru": "Следом"}},
        ],
    }
    if cover_kind:
        plan["cover"] = {"segment": "clean", "at": 1.8, "title": {"ru": "МАГИЯ", "en": "MAGIC"}}
        if cover_kind == "custom":
            plan["cover"]["sub"] = {"ru": "СВОЯ СТРОКА", "en": "CUSTOM LINE"}
    plan_file = tmp_path / "plan.json"
    plan_file.write_text(json.dumps(plan))
    commands, concat_lists = [], []

    def run(command):
        commands.append(command)
        if "concat" in command:
            concat_lists.append(Path(command[command.index("-i") + 1]).read_text())
        if Path(command[-1]).name == "cover-frame.png":
            Image.new("RGB", (1080, 1920), (30, 60, 90)).save(command[-1])

    text_image = Mock()
    caption_image = Mock()
    publish = Mock()
    monkeypatch.setattr(compare, "ROOT", tmp_path)
    monkeypatch.setattr(compare, "BRAND", tmp_path / "brand")
    monkeypatch.setattr(compare.sys, "argv", ["cut.py", "probe", "ru", str(plan_file)])
    monkeypatch.setattr(compare, "text_image", text_image)
    monkeypatch.setattr(compare, "caption_image", caption_image)
    monkeypatch.setattr(compare, "run", run)
    monkeypatch.setattr(compare.subprocess, "run", Mock(return_value=SimpleNamespace(stdout="1.2")))
    monkeypatch.setattr(compare.levels, "publish", publish)
    render_cover = Mock(wraps=compare.cover.render)
    monkeypatch.setattr(compare.cover, "render", render_cover)

    compare.main()

    beats = [command for command in commands if "-filter_complex" in command and "-frames:v" not in command]
    assert len(beats) == 2
    for command, expected in zip(beats, [["t0.png", "hook.png", "s0.png", "c0_0_0.png"],
                                        ["t1.png"]], strict=True):
        inputs = [Path(command[i + 1]) for i, arg in enumerate(command) if arg == "-i"]
        assert [path.name for path in inputs if path.suffix == ".png"] == expected
        graph = command[command.index("-filter_complex") + 1]
        assert graph.count("overlay=") == len(expected)
        assert graph.count("overlay=0:0") == len(expected)
        assert (f"[{len(inputs) - 1}]aformat=sample_rates=48000:channel_layouts=stereo,"
                f"{levels.DUAL_MONO},atrim=") in graph  # effects follow the remaining image inputs
    assert "enable='between(t,1,2)'" in beats[0][beats[0].index("-filter_complex") + 1]
    text_layers = {call.args[0].name: call.args[1] for call in text_image.call_args_list}
    assert [(item[3], item[5]) for item in text_layers["s0.png"]] == [(40, "r"), (1000, "r")]
    assert [item[3] for item in text_layers["t1.png"]] == [40, 1000, 810]
    assert caption_image.call_count == 1
    ending = [command for command in commands if str(compare.BRAND / "sting.mp4") in command]
    assert len(ending) == int(brand)
    assert len(concat_lists) == 1
    assert ("sting.mp4" in concat_lists[0]) is brand
    assert publish.call_args.args[4:6] == (6, 7.2 if brand else 6)
    cover_path = tmp_path / "render/cover-ru.jpg"
    assert cover_path.is_file() is bool(cover_kind)
    output = capsys.readouterr().out
    if cover_kind:
        with Image.open(cover_path) as image:
            assert image.size == (1080, 1920)
            assert image.format == "JPEG"
        assert cover_path.stat().st_size <= 2_000_000
        assert str(cover_path) in output
        assert render_cover.call_args.args[1:] == (
            "МАГИЯ", "СВОЯ СТРОКА" if cover_kind == "custom" else "БЫЛО → СТАЛО",
        )
        extraction = next(command for command in commands if Path(command[-1]).name == "cover-frame.png")
        assert [extraction[i + 1] for i, arg in enumerate(extraction) if arg == "-ss"] == ["1.8", "1.8"]
        assert [Path(extraction[i + 1]) for i, arg in enumerate(extraction) if arg == "-i"] == [
            tmp_path / ".shorts/compare/probe/off/clean.mp4",
            tmp_path / ".shorts/compare/probe/on/clean.mp4",
        ]
        graph = extraction[extraction.index("-filter_complex") + 1]
        assert graph.count(f"scale=1080:960,{compare.GRADE}") == 2
        assert "vstack" in graph
        assert "overlay" not in graph
    else:
        assert "cover: none" in output
        render_cover.assert_not_called()


@pytest.mark.skipif(
    not Path("/System/Library/Fonts/Supplemental/Arial Black.ttf").is_file(),
    reason="macOS Arial Black is unavailable",
)
def test_compare_cover_stacks_clean_graded_takes_with_ffmpeg(tmp_path):
    if shutil.which("ffmpeg") is None:
        pytest.skip("ffmpeg is required for a comparison cover")
    for take, color in [("off", "0x502010"), ("on", "0x102050")]:
        (tmp_path / take).mkdir()
        _ffmpeg(
            "-f", "lavfi", "-i", f"color=c={color}:s=108x96:r=10:d=0.4",
            "-c:v", "libx264", "-pix_fmt", "yuv420p", str(tmp_path / take / "scene.mp4"),
        )
    plan = {"cover": {"segment": "scene", "at": 0.2, "title": {"en": "MAGIC"}}}
    path = compare.render_cover(plan, tmp_path, "en", tmp_path / "short-en.mp4")
    assert path == tmp_path / "cover-en.jpg"
    with Image.open(path) as image:
        assert image.size == (1080, 1920)
        top, bottom = image.getpixel((100, 100)), image.getpixel((100, 1800))
    assert top[0] > 80 and top[0] > top[1] > top[2]
    assert bottom[2] > 80 and bottom[2] > bottom[1] > bottom[0]


def test_synthetic_cut_publishes_calibrated_voice_and_complete_artifacts(published_cut):
    result = published_cut
    report, out = result.report, result.out
    rows = _rows(report)
    assert report["ok"] is True
    assert report["balance_stage"] == "mastered_aac"
    assert rows["voice_mastered"]["measured_lufs"] == pytest.approx(-14, abs=0.5)
    assert rows["true_peak"]["measured_dbtp"] <= -1
    assert rows["clicks"]["count"] == 0
    assert rows["game_speech"]["ok"] is True
    assert rows["game_pause"]["ok"] is True
    assert rows["music_pause"]["ok"] is True
    assert rows["music_speech"]["ok"] is True
    assert rows["ending"]["ok"] is True
    assert out.stat().st_size > 0
    assert out.with_suffix(".png").stat().st_size > 0
    transcript = out.with_suffix(".md").read_text()
    assert "Ксардас" in transcript and "Диего" in transcript and "С финалом:" in transcript
    assert report["ending_windows"][0][0] == pytest.approx(result.timeline.cut_duration, abs=1 / 30)
    assert report["ending_windows"][0][1] - report["ending_windows"][0][0] == pytest.approx(1.2, abs=1 / 30)
    # The preview's last tile must actually show the appended blue sting.
    with Image.open(out.with_suffix(".png")) as sheet:
        red, green, blue = sheet.convert("RGB").getpixel((5 * 270 + 135, 480 + 240))
    assert blue > 240 and red < 10 and green < 10


def test_speech_windows_include_npcs_at_sample_time_after_pause_cuts(published_cut):
    result = published_cut
    tl = result.timeline
    assert tl.cuts, "the fixture must exercise kept pieces rather than one continuous recording"
    expected = [
        (tl.to_cut(1), tl.to_cut(2.2)),
        (tl.to_cut(192600 / 48000), tl.to_cut(192600 / 48000 + 1.2)),
        (tl.to_cut(6), tl.to_cut(7.2)),
        (tl.to_cut(417600 / 48000), tl.to_cut(417600 / 48000 + 1.2)),
    ]
    assert len(result.report["speech_windows"]) == len(expected)
    for actual, wanted in zip(result.report["speech_windows"], expected, strict=True):
        assert actual == pytest.approx(wanted, abs=1 / 48000)
    assert len([line for line in tl.lines if not line.hero]) == 2


def test_each_speaker_is_leveled_together_without_flattening_each_line(published_cut):
    result = published_cut
    voice = result.take / "cut-ru/voice.wav"
    windows = result.report["speech_windows"]
    hero = _level(voice, [windows[0], windows[2]])
    xardas = _level(voice, [windows[1]])
    diego = _level(voice, [windows[3]])
    assert max(hero, xardas, diego) - min(hero, xardas, diego) <= 0.5
    first, second = _level(voice, [windows[0]]), _level(voice, [windows[2]])
    assert first - second == pytest.approx(6, abs=0.3)
    # Each mono source rate is resampled onto the game's 48 kHz clock.
    assert cut._duration(voice) == pytest.approx(result.timeline.cut_duration, abs=1 / 48000)


def test_short_npc_reply_publishes_with_the_combined_voice_reference(tmp_path, synthetic_assets, monkeypatch):
    assets = synthetic_assets
    take = tmp_path / "short-reply"
    shutil.copytree(assets.take, take, ignore=shutil.ignore_patterns("cut-*", "short-*"))
    events = []
    for event in assets.events:
        event = dict(event)
        if "wav" in event:
            event["wav"] = str(take / Path(event["wav"]).name)
        events.append(event)
    _events(take / "events.jsonl", events)
    _ffmpeg(
        "-f", "lavfi", "-i", "sine=frequency=1000:sample_rate=22050:duration=0.25",
        "-af", "volume=-15dB,afade=t=in:d=0.01,afade=t=out:st=0.24:d=0.01",
        "-ac", "1", "-c:a", "pcm_s16le", str(take / "main.voice-01.wav"),
    )
    _small_cut(monkeypatch, assets)
    out = cut.cut(replace(assets.shot, brand=False), take)
    report = json.loads(out.with_suffix(".levels.json").read_text())
    assert report["ok"] is True
    assert _rows(report)["voice_mastered"]["measured_lufs"] == pytest.approx(-14, abs=0.5)
    start, end = report["speech_windows"][1]
    assert end - start == pytest.approx(0.25, abs=1 / 22050)


@pytest.mark.parametrize("old_format", ["npc-mixed-in-game", "game-music", "world-sounds"])
def test_old_takes_require_refilming(tmp_path, synthetic_assets, monkeypatch, old_format):
    assets = synthetic_assets
    take = tmp_path / "old-take"
    shutil.copytree(assets.take, take, ignore=shutil.ignore_patterns("cut-*", "short-*"))
    if old_format == "npc-mixed-in-game":
        _events(take / "events.jsonl", [event for event in assets.events if event["kind"] != "npc_voice"])
    elif old_format == "game-music":
        (take / "run/Gothic.ini").write_text("[SOUND]\nmusicVolume=0.35\nworldSounds=0\n")
    else:  # filmed before worldSounds: the map's looping place sounds are in the effects
        (take / "run/Gothic.ini").write_text("[SOUND]\nmusicVolume=0\n")
    _small_cut(monkeypatch, assets)
    with pytest.raises(cut.CutError, match="переснимите дубль"):
        cut.cut(assets.shot, take)
    assert not (take / "short-ru.mp4").exists()


def test_loud_effects_under_speech_are_corrected_or_block_publication(
    tmp_path, synthetic_assets, monkeypatch,
):
    assets = synthetic_assets
    take = tmp_path / "loud-effects"
    shutil.copytree(assets.take, take, ignore=shutil.ignore_patterns("cut-*", "short-*"))
    _ffmpeg(
        "-f", "lavfi", "-i", "anoisesrc=color=pink:seed=19:sample_rate=48000:duration=18",
        "-af", "afade=t=in:d=0.06,afade=t=out:st=17.94:d=0.06",
        "-ac", "2", "-c:a", "pcm_s16le", str(take / "main.wav"),
    )
    _small_cut(monkeypatch, assets)
    out = take / "short-ru.mp4"
    try:
        cut.cut(replace(assets.shot, brand=False), take)
    except cut.CutError:
        report = json.loads(out.with_suffix(".levels.json").read_text())
        assert report["ok"] is False
        assert report["error"]
        assert any(row["ok"] is False for row in report["rows"])
        assert not out.exists()
    else:
        report = json.loads(out.with_suffix(".levels.json").read_text())
        rows = _rows(report)
        assert report["ok"] is True
        assert report["game_gain_db"] < 0 or report["game_duck_db"] > 0
        assert rows["game_speech"]["measured_lufs"] <= rows["voice_mastered"]["measured_lufs"] - 6
        assert rows["game_speech"]["ok"] is True


def test_cli_lang_en_changes_subtitles_and_keeps_identical_audio(published_cut, monkeypatch):
    result = published_cut
    _small_cut(monkeypatch, result)
    translations = {text: f"EN {i}" for i, text in enumerate(cut.texts(result.shot, result.timeline))}
    (result.take / "subs.en.json").write_text(json.dumps(translations))
    monkeypatch.setattr(cli, "_shot", lambda arg: result.shot)
    monkeypatch.setattr(cli, "_config", lambda: None)
    monkeypatch.setattr(cli, "_take", lambda config, shot, arg: result.take)
    monkeypatch.setattr("sys.argv", ["gothic-shorts", "cut", "probe", str(result.take), "--lang", "en"])
    with pytest.raises(SystemExit) as exit_status:
        cli.main()
    assert exit_status.value.code == 0
    out = result.take / "short-en.mp4"
    report = json.loads(out.with_suffix(".levels.json").read_text())
    assert report["ok"] is True
    assert report["speech_windows"] == result.report["speech_windows"]
    assert _pcm(out) == _pcm(result.out)
    assert "EN " in out.with_suffix(".md").read_text()


@pytest.mark.skipif(not Path(cut.BRAND_FONT).is_file(), reason="macOS PT Serif collection is unavailable")
def test_brand_pngs_use_pt_serif_bold_warm_outline_and_shorts_safe_area(tmp_path, monkeypatch):
    font_calls = []
    real_truetype = cut.ImageFont.truetype

    def truetype(font, size, *args, **kwargs):
        font_calls.append((font, kwargs.get("index", 0)))
        return real_truetype(font, size, *args, **kwargs)

    monkeypatch.setattr(cut.ImageFont, "truetype", truetype)
    shown = [
        cut.Shown("hook", "Я попробовал обмануть Ксардаса", 0, 2.8),
        cut.Shown("caption", "Он реагирует, пока думает", 3, 5),
        cut.Shown("line", "Ксардас, меня прислал Ли. Он очень просил отдать посох.", 0, 3, hero=True),
        cut.Shown("line", "Ты смеешь испытывать моё терпение?" * 4, 3, 8, name="Ксардас"),
    ]
    overlays = cut._overlays(shown, tmp_path, brand=True)
    assert font_calls
    assert set(font_calls) == {("/System/Library/Fonts/Supplemental/PTSerif.ttc", 3)}
    for item, overlay in zip(shown, overlays, strict=True):
        with Image.open(overlay.png) as image:
            left, top, right, bottom = image.getchannel("A").getbbox()
            assert 60 <= left < right <= 930  # to the left of the Shorts UI
            assert 150 <= overlay.y + top < overlay.y + bottom <= 1540
            colors = {rgba for count, rgba in image.getcolors(image.width * image.height)}
        assert (20, 12, 8, 255) in colors
        assert (0, 0, 0, 255) not in colors
        expected = (255, 214, 102) if item.hero or item.kind == "caption" else (255, 255, 255)
        assert (*expected, 255) in colors
        if item.name:
            assert (230, 190, 120, 255) in colors
