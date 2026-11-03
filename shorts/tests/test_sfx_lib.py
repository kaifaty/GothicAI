import importlib.util
import json
import math
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

SPEC = importlib.util.spec_from_file_location("sfx_lib", Path(__file__).parents[1] / "explain/scripts/sfx_lib.py")
sfx_lib = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sfx_lib)

RATE = sfx_lib.RATE


def knock(at: float, length: float = 0.4, level: float = 0.5, decay: float = 40.0) -> list[float]:
    """Silence, then a decaying 300 Hz knock starting at `at`."""
    out = []
    for i in range(round(length * RATE)):
        t = i / RATE - at
        out.append(0.0 if t < 0 else level * math.exp(-decay * t) * math.sin(2 * math.pi * 300 * t + 1.2))
    return out


def peak_at(y: list[float]) -> float:
    return max(range(len(y)), key=lambda i: abs(y[i])) / RATE


def rms_db(y: list[float]) -> float:
    sound = [v for v in y if v]
    return 20 * math.log10(math.sqrt(sum(v * v for v in sound) / len(sound)))


@pytest.mark.parametrize("onset,lead", [(0.05, 0.25), (0.05, 0.002), (0.2, 0.01)])
def test_shape_puts_the_peak_at_lead_and_levels_the_sound(onset, lead):
    y, _ = sfx_lib.shape(knock(onset), lead)
    assert peak_at(y) == pytest.approx(lead, abs=0.002)
    assert rms_db(y) == pytest.approx(sfx_lib.RMS_DB, abs=1.5) or max(abs(v) for v in y) <= 10 ** (-1 / 20) + 1e-6
    assert max(abs(v) for v in y) <= 10 ** (-1 / 20) + 1e-6
    assert abs(y[0]) < 1e-3 and abs(y[-1]) < 1e-3  # faded ends: no click where it starts or stops


def test_variants_of_a_kind_come_out_equally_loud():
    quiet, _ = sfx_lib.shape(knock(0.05, level=0.05, decay=8), 0.01)
    loud, _ = sfx_lib.shape(knock(0.05, level=0.9, decay=8), 0.01)
    assert rms_db(quiet) == pytest.approx(rms_db(loud), abs=0.2)


@pytest.mark.skipif(shutil.which("ffmpeg") is None, reason="ffmpeg is required")
def test_build_makes_each_variant_and_its_index(tmp_path):
    packs = tmp_path / "packs" / "src"
    packs.mkdir(parents=True)
    raw = b"".join(struct.pack("<f", v) for v in knock(0.1))
    for name in ("a", "b"):
        subprocess.run(["ffmpeg", "-v", "error", "-f", "f32le", "-ar", str(RATE), "-ac", "1", "-i", "-",
                        str(packs / f"{name}.ogg")], input=raw, check=True)
    manifest = {"sources": {"src": {"url": "https://example.org", "license": "CC0 1.0"}},
                "kinds": {"tap": {"lead": 0.01, "files": ["src/a.ogg", "src/b.ogg"]}}}
    index = sfx_lib.build(tmp_path / "packs", tmp_path / "lib", manifest)
    assert sorted(index) == ["tap-0.wav", "tap-1.wav"]
    assert json.loads((tmp_path / "lib/index.json").read_text())["tap-1.wav"]["license"] == "CC0 1.0"
    probe = json.loads(subprocess.run(["ffprobe", "-v", "error", "-show_streams", "-of", "json",
                                       str(tmp_path / "lib/tap-0.wav")], capture_output=True, text=True).stdout)
    stream = probe["streams"][0]
    assert (stream["sample_rate"], stream["channels"], stream["codec_name"]) == ("48000", 2, "pcm_s16le")


def test_build_refuses_a_file_from_an_unlisted_source(tmp_path):
    manifest = {"sources": {}, "kinds": {"tap": {"lead": 0.01, "files": ["elsewhere/a.ogg"]}}}
    with pytest.raises(SystemExit, match="not from a listed source"):
        sfx_lib.build(tmp_path, tmp_path / "lib", manifest)


def test_the_manifest_lists_only_cc0_sources_and_uses_each_kind_from_them():
    manifest = json.loads(sfx_lib.MANIFEST.read_text())
    assert all(s["license"] == "CC0 1.0" for s in manifest["sources"].values())
    for kind in manifest["kinds"].values():
        assert kind["files"] and all(f.split("/", 1)[0] in manifest["sources"] for f in kind["files"])


PACKS = sfx_lib.ROOT / "packs"


@pytest.mark.skipif(not PACKS.is_dir() or shutil.which("ffmpeg") is None, reason="the CC0 packs are not on this machine")
def test_the_real_library_has_no_corner_the_click_gate_would_stop(tmp_path):
    from gothic_shorts import clicks

    sfx_lib.build(PACKS, tmp_path, json.loads(sfx_lib.MANIFEST.read_text()))
    found = {f.name: clicks.detect(f) for f in sorted(tmp_path.glob("*.wav"))}
    assert {name: hits for name, hits in found.items() if hits} == {}
