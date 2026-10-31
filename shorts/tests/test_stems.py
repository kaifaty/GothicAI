import math
import shutil
import subprocess
import sys
import wave
from array import array

import pytest

from gothic_shorts import levels, stems
from gothic_shorts.timeline import Timeline, build


def tone(path, duration, amplitude, rate=24_000):
    samples = array("h", (round(32767 * amplitude * math.sin(2 * math.pi * 500 * i / rate))
                          for i in range(round(rate * duration))))
    if sys.byteorder != "little":
        samples.byteswap()
    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(rate)
        audio.writeframes(samples.tobytes())
    return path


def test_collect_uses_samples_wav_lengths_and_keeps_orphan_npc_voice(tmp_path):
    hero = tone(tmp_path / "hero.wav", 1.5, 0.1)
    npc = tone(tmp_path / "npc.wav", 2, 0.1)
    orphan = tone(tmp_path / "orphan.wav", 0.5, 0.1)
    events = [
        {"kind": "hero", "frame": 30, "wav": str(hero), "text": "Эй", "ms": 500},
        {"kind": "npc_voice", "frame": 121, "npc": "A", "wav": str(npc),
         "sample": 192_001, "line": "id-a"},
        {"kind": "npc_line", "frame": 121, "npc": "A", "text": "Да?", "ms": 8000},
        {"kind": "npc_voice", "frame": 240, "npc": "B", "wav": str(orphan),
         "sample": 384_000, "line": "id-b"},
        {"kind": "rec_stop", "frame": 300, "frames": 300},
    ]
    clips = stems.collect(events, build(events))
    assert [clip.who for clip in clips] == ["hero", "A", "B"]
    assert [(clip.start, clip.end) for clip in clips] == [
        (1.0, 2.5), (pytest.approx(192_001 / 48_000), pytest.approx(192_001 / 48_000 + 2)),
        (8.0, 8.5),
    ]


def test_speech_windows_keep_npc_audio_and_map_cuts():
    from pathlib import Path

    tl = Timeline(10, cuts=[(3, 5)])
    clips = [stems.Clip("hero", Path("hero.wav"), 1, 2),
             stems.Clip("A", Path("npc.wav"), 2.5, 5.5),
             stems.Clip("B", Path("npc2.wav"), 8, 12)]
    assert stems.speech_windows(clips, tl) == [(1, 2), (2.5, 3.5), (6, 8)]


def test_collect_rejects_legacy_npc_and_missing_hero_audio(tmp_path):
    with pytest.raises(ValueError, match="npc_voice.*переснимите дубль"):
        stems.collect([{"kind": "npc_line", "frame": 0, "npc": "A", "text": "Да"}], Timeline(1))
    with pytest.raises(ValueError, match="hero.*переснимите дубль"):
        stems.collect([{"kind": "hero", "frame": 0, "wav": str(tmp_path / "missing.wav")}], Timeline(1))


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="ffmpeg is required")
@pytest.mark.parametrize("durations", [(0.25,), (0.08,), (0.1, 0.15)])
def test_short_speaker_audio_is_measurable_without_silence_dilution(tmp_path, durations):
    hero = tone(tmp_path / "hero.wav", 1, 0.05)
    clips = [stems.Clip("hero", hero, 0, 1)]
    for index, duration in enumerate(durations):
        wav = tone(tmp_path / f"npc-{index}.wav", duration, 0.01)
        clips.append(stems.Clip("A", wav, 2 + index, 2 + index + duration))

    gains = stems.speaker_gains(clips)
    assert all(math.isfinite(gain) for gain in gains.values())
    assert gains["A"] - gains["hero"] == pytest.approx(20 * math.log10(5), abs=0.3)
    rendered = stems.render(clips, Timeline(5), tmp_path / "voice.wav")
    # The measurement repeat must never add speech to the actual voice stem.
    after_reply = levels.measure(["-i", str(rendered)], "[0:a]anull[v]", "v", [(3.5, 4.5)])
    assert after_reply == -math.inf


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="ffmpeg is required")
def test_short_silent_speaker_is_still_rejected(tmp_path):
    silent = tone(tmp_path / "silent.wav", 0.25, 0)
    with pytest.raises(ValueError, match="Голос A не имеет измеримой громкости"):
        stems.speaker_gains([stems.Clip("A", silent, 0, 0.25)])


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="ffmpeg is required")
def test_speakers_share_loudness_but_retain_dynamics_across_their_utterances(tmp_path):
    quiet = tone(tmp_path / "quiet.wav", 1, 0.01)
    louder = tone(tmp_path / "louder.wav", 1, 0.03)
    npc = tone(tmp_path / "npc.wav", 2, 0.15)
    clips = [stems.Clip("hero", quiet, 1, 2), stems.Clip("hero", louder, 3, 4),
             stems.Clip("A", npc, 5, 7)]
    tl = Timeline(8, cuts=[(2.2, 2.8)])
    rendered = stems.render(clips, tl, tmp_path / "voice.wav")

    def loudness(windows):
        return levels.measure(["-i", str(rendered)], "[0:a]anull[voice]", "voice", windows)

    hero_level = loudness([(1, 2), (2.4, 3.4)])
    npc_level = loudness([(4.4, 6.4)])
    assert hero_level == pytest.approx(levels.BASE_LUFS, abs=0.3)
    assert npc_level == pytest.approx(hero_level, abs=0.3)
    assert loudness([(2.4, 3.4)]) - loudness([(1, 2)]) == pytest.approx(20 * math.log10(3), abs=0.3)


@pytest.mark.skipif(not shutil.which("ffmpeg"), reason="ffmpeg is required")
def test_stereo_speakers_are_folded_before_gain_and_rendered_as_dual_mono(tmp_path):
    source = tone(tmp_path / "source.wav", 1.5, 0.05)
    clips = []
    for index, (speaker, right) in enumerate([("left", "0*c0"), ("center", "c0")]):
        path = tmp_path / f"{speaker}.wav"
        subprocess.run(
            ["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(source),
             "-af", f"pan=stereo|c0=c0|c1={right}", str(path)],
            check=True, capture_output=True,
        )
        clips.append(stems.Clip(speaker, path, index * 2, index * 2 + 1.5))

    gains = stems.speaker_gains(clips)
    # Folding halves the left-only signal's amplitude in both output channels.
    assert gains["left"] - gains["center"] == pytest.approx(20 * math.log10(2), abs=0.1)
    out = stems.render(clips, Timeline(4), tmp_path / "voice.wav")
    for start, end in [(0.1, 1.4), (2.1, 3.4)]:
        assert levels.measure(["-i", str(out)], "[0:a]anull[v]", "v", [(start, end)]) == pytest.approx(
            levels.BASE_LUFS, abs=0.3,
        )
    decoded = subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-i", str(out), "-f", "f32le", "-"],
        check=True, capture_output=True,
    ).stdout
    samples = array("f", decoded)
    if sys.byteorder != "little":
        samples.byteswap()
    assert samples[::2] == samples[1::2]
