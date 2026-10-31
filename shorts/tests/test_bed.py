import math
import struct
import subprocess
import wave

import pytest

from gothic_shorts import bed


@pytest.fixture
def music(tmp_path, monkeypatch):
    music = tmp_path / "music"
    music.mkdir()
    monkeypatch.setattr(bed, "MUSIC", music)
    wav = tmp_path / "source.wav"
    # The first second represents a dirty attack; the clean portion is audibly quieter.
    samples = [
        round((0.7 if i < 48000 else 0.1) * 32767 * math.sin(2 * math.pi * 440 * i / 48000))
        for i in range(4 * 48000)
    ]
    with wave.open(str(wav), "wb") as audio:
        audio.setparams((1, 2, 48000, 0, "NONE", "not compressed"))
        audio.writeframes(struct.pack(f"<{len(samples)}h", *samples))
    subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-y", "-i", str(wav), str(music / "theme.flac")],
        check=True, capture_output=True,
    )
    (music / "catalog.tsv").write_text("file\tclean_from_s\ntheme.flac\t1.25\n")
    return music


def test_bed_uses_catalog_clean_start_and_exact_body_duration(music, tmp_path):
    out = bed.render("theme", None, 2.0, tmp_path / "bed.wav")
    with wave.open(str(out), "rb") as audio:
        assert (audio.getframerate(), audio.getnchannels(), audio.getnframes()) == (48000, 2, 96000)
        samples = struct.unpack(f"<{audio.getnframes() * 2}h", audio.readframes(audio.getnframes()))
    assert max(abs(sample) for sample in samples) < 0.11 * 32767


def test_bed_without_clean_start_requires_explicit_override(music, tmp_path):
    (music / "catalog.tsv").write_text("file\tclean_from_s\ntheme.flac\t\n")
    with pytest.raises(bed.BedError, match="no clean start.*bed_start"):
        bed.render("theme", None, 2.0, tmp_path / "bed.wav")
    out = bed.render("theme", 0.0, 0.5, tmp_path / "bed.wav")
    with wave.open(str(out), "rb") as audio:
        samples = struct.unpack(f"<{audio.getnframes() * 2}h", audio.readframes(audio.getnframes()))
    assert max(abs(sample) for sample in samples) > 0.4 * 32767


def test_bed_cannot_silently_end_before_the_body(music, tmp_path):
    with pytest.raises(bed.BedError, match="only 2.75 s.*needs 3.00 s"):
        bed.render("theme", None, 3.0, tmp_path / "bed.wav")


def test_stereo_bed_is_averaged_into_identical_channels(music, tmp_path):
    subprocess.run(
        ["ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "lavfi", "-i",
         "aevalsrc=0.1*sin(2*PI*440*t)|0.2*sin(2*PI*770*t):s=48000:d=1:c=stereo",
         str(music / "stereo.flac")],
        check=True, capture_output=True,
    )
    out = bed.render("stereo", 0, 1, tmp_path / "bed.wav")
    with wave.open(str(out), "rb") as audio:
        assert audio.getnchannels() == 2
        samples = struct.unpack(f"<{audio.getnframes() * 2}h", audio.readframes(audio.getnframes()))
    assert samples[::2] == samples[1::2]
    expected = [
        round(32768 * (0.05 * math.sin(2 * math.pi * 440 * i / 48000)
                       + 0.1 * math.sin(2 * math.pi * 770 * i / 48000)))
        for i in range(48000)
    ]
    assert max(abs(actual - target) for actual, target in zip(samples[::2], expected, strict=True)) <= 2
