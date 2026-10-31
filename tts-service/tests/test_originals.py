"""Synthetic WAVs only; game speech is never committed as a test fixture."""

import hashlib
import json
import struct
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

from gothic_tts import originals


def wav(fmt, data, fact=None):
    chunks = b"fmt " + struct.pack("<I", len(fmt)) + fmt
    if fact is not None:
        chunks += b"fact" + struct.pack("<II", 4, fact)
    chunks += b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(chunks) + 4) + b"WAVE" + chunks


def test_ima_wave_matches_openal_reference():
    # Synthetic vectors evaluated with the vendored OpenAL Soft FmtIMA4 reference in
    # OpenGothic/lib/Tempest/Engine/thirdparty/openal-soft/core/voice.cpp. Its integer
    # product rule, signed saturation, index updates and low-nibble-first order are
    # the reference; no audio decoder executable or game recording is needed here.
    encoded = bytes.fromhex("1032547698badcfeff77")
    expected = np.array(
        [
            25000,
            28079,
            32767,
            32767,
            32767,
            32767,
            32767,
            32767,
            32767,
            28672,
            17500,
            572,
            -20981,
            -32768,
            -32768,
            -32768,
            -32768,
            -32768,
            -32768,
            28670,
            32767,
            -12000,
            -11998,
            -11993,
            -11985,
            -11975,
            -11963,
            -11946,
            -11916,
            -11853,
            -11862,
            -11886,
            -11923,
            -11971,
            -12027,
            -12109,
            -12252,
            -12546,
            -13177,
            -14534,
            -11624,
            -5386,
        ],
        dtype=np.int16,
    )
    block = 4 + len(encoded)
    fmt = struct.pack("<HHIIHHHH", 0x11, 1, originals.RATE, 0, block, 4, 2, 21)
    data = struct.pack("<hBB", 25000, 85, 0) + encoded + struct.pack("<hBB", -12000, 8, 0) + encoded
    # The advisory fact length does not truncate encoded blocks: this keeps game
    # snapshot durations and candidate order identical to the previous conversion.
    decoded = np.rint(originals.decode_wav(wav(fmt, data, fact=39)) * 32768).astype(np.int32)
    assert np.max(np.abs(decoded - expected.astype(np.int32))) <= 1


def test_pcm16_wave_is_lossless():
    expected = np.array([-32768, -12345, -1, 0, 1, 12345, 32767], dtype="<i2")
    fmt = struct.pack("<HHIIHH", 1, 1, originals.RATE, originals.RATE * 2, 2, 16)
    decoded = np.rint(originals.decode_wav(wav(fmt, expected.tobytes())) * 32768).astype(np.int32)
    np.testing.assert_array_equal(decoded, expected)


def test_prepare_reuses_matching_base_and_rebuilds_changes(tmp_path, monkeypatch):
    game = tmp_path / "game-without-installation"
    game.mkdir()
    selected = tmp_path / "base"
    selected.mkdir()
    manifest_bytes = json.dumps({"format": "gothic-base-files", "version": 1, "files": []}).encode()
    (selected / "base-files.json").write_bytes(manifest_bytes)
    metadata = {
        "format": "gothic-base",
        "version": 2,
        "fingerprint": "a" * 64,
        "manifest": {
            "path": "base-files.json",
            "size": len(manifest_bytes),
            "sha256": hashlib.sha256(manifest_bytes).hexdigest(),
        },
    }
    base_json = selected / "base.json"
    base_json.write_text(json.dumps(metadata))
    monkeypatch.setenv("OPENGOTHIC_BASE", str(selected))
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", str(tmp_path / "cache"))
    monkeypatch.setenv("STORYGRAPH_TOOL", __file__)
    messages = [["DIA_XARDAS_TEST_14_00.WAV", "Synthetic line."]]
    exports = []

    def export(argv, **kwargs):
        assert not list(out.glob(".snapshots-*"))
        assert argv[1] == str(selected)
        assert kwargs["env"]["OPENGOTHIC_BASE"] == str(selected)
        exports.append(argv)
        return SimpleNamespace(stdout=json.dumps({"messages": messages}).encode())

    monkeypatch.setattr(originals.subprocess, "run", export)
    monkeypatch.setattr(originals, "speech_index", lambda selected: {})
    built = []
    cid = "NONE_100_XARDAS"
    unavailable = "VLK_439_VATRAS"
    ids = [cid, unavailable]

    def build(selected, index, texts, out):
        built.append(selected)
        if selected == unavailable:
            return {"id": selected, "name": "Vatras", "error": "no lines"}
        path = out / f"{selected}.wav"
        path.write_bytes(b"synthetic sample")
        return {"id": selected, "file": str(path)}

    monkeypatch.setattr(originals, "build", build)
    out = tmp_path / "voices"
    stale = out / ".snapshots-interrupted"
    stale.mkdir(parents=True)
    (stale / "partial.wav").write_bytes(b"incomplete sample")
    progress = []
    path = originals.prepare(game, out, ids=ids, progress=lambda n, total: progress.append((n, total)))
    first = path.read_bytes()
    stale.mkdir()
    (stale / "partial.wav").write_bytes(b"incomplete sample")
    originals.prepare(game, out, ids=ids)
    assert built == ids
    assert path.read_bytes() == first
    assert progress == [(1, 2), (2, 2)]
    assert len(exports) == 1
    assert exports[0][-1] == "--ou-messages"
    assert json.loads(first)["source"] == {"base_fingerprint": "a" * 64}
    metadata["fingerprint"] = "b" * 64
    base_json.write_text(json.dumps(metadata))
    originals.prepare(game, out, ids=ids)
    assert built == ids * 2
    monkeypatch.setattr(originals, "ALGORITHM_VERSION", originals.ALGORITHM_VERSION + 1)
    originals.prepare(game, out, ids=ids)
    assert built == ids * 3


def test_native_speech_reads_manifest_verified_wav(tmp_path):
    pcm = np.array([-32768, -12345, -1, 0, 1, 12345, 32767], dtype="<i2")
    fmt = struct.pack("<HHIIHH", 1, 1, originals.RATE, originals.RATE * 2, 2, 16)
    payload = wav(fmt, pcm.tobytes())
    digest = hashlib.sha256(payload).hexdigest()
    relative = f"audio/{digest[:2]}/{digest}.wav"
    index = {
        "format": "gothic-audio",
        "version": 1,
        "payloads": {digest: {"file": relative.removeprefix("audio/"), "size": len(payload)}},
        "sounds": {
            "DIA_XARDAS_TEST_14_00.WAV": {"class": "speech", "payload": digest},
            "SOUND.WAV": {"class": "sound", "payload": digest},
            "DIA_ADDON_SATURAS_WEIßTDU_14_00.WAV": {"class": "speech", "payload": digest},
        },
    }
    files = {relative: payload, "audio/audio.json": json.dumps(index).encode()}
    for name, raw in files.items():
        destination = tmp_path / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(raw)
    manifest = {
        "format": "gothic-base-files",
        "version": 1,
        "files": [
            {"path": name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}
            for name, raw in files.items()
        ],
    }
    raw = json.dumps(manifest).encode()
    (tmp_path / "base-files.json").write_bytes(raw)
    (tmp_path / "base.json").write_text(json.dumps({
        "format": "gothic-base", "version": 2, "fingerprint": "a" * 64,
        "manifest": {"path": "base-files.json", "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()},
    }))
    with originals.base.Base(tmp_path, tmp_path / "cache") as selected:
        native = originals.speech_index(selected)
        assert set(native) == {"DIA_XARDAS_TEST_14_00", "DIA_ADDON_SATURAS_WEIЯTDU_14_00"}
        decoded = np.rint(originals.decode(*native["DIA_XARDAS_TEST_14_00"]) * 32768).astype(np.int32)
        np.testing.assert_array_equal(decoded, pcm)


def test_base_helper_matches_server_contract():
    root = Path(__file__).resolve().parents[2]
    assert (root / "server/src/gothic_ai/gamedata/base.py").read_bytes() == (
        root / "tts-service/src/gothic_tts/base.py"
    ).read_bytes()


def test_pinned_lines_keep_reference_order_and_fail_explicitly(tmp_path, monkeypatch):
    assert set(originals.ORIGINAL_LINES) == set(originals.CHARACTERS)
    assert len(originals.ORIGINAL_LINES) == 36
    assert all(
        lines and len(lines) == len(set(lines)) and all(originals.LINE.fullmatch(line) for line in lines)
        for lines in originals.ORIGINAL_LINES.values()
    )
    cid = "NONE_100_XARDAS"
    pinned = ["DIA_XARDAS_Z_14_00", "DIA_XARDAS_A_14_00"]
    monkeypatch.setattr(originals, "ORIGINAL_LINES", {cid: pinned})
    monkeypatch.setattr(originals, "RATE", 100)
    index = {line: (None, line) for line in reversed(pinned)}
    texts = dict.fromkeys(pinned, "A shout!")
    samples = {
        pinned[0]: np.full(1300, 0.1, dtype=np.float32),
        pinned[1]: np.full(450, 0.2, dtype=np.float32),
    }
    monkeypatch.setattr(originals, "decode", lambda _, line: samples[line])
    result = originals.build(cid, index, texts, tmp_path)
    assert [entry["line"] for entry in result["lines"]] == pinned
    assert result["seconds"] == 17.9  # Pinned lines bypass the duration and calmness filters.
    del index[pinned[0]]
    with pytest.raises(ValueError, match=f"{cid}: missing speech line {pinned[0]}"):
        originals.build(cid, index, texts, tmp_path)
    index[pinned[0]] = (None, pinned[0])

    def corrupt(_, line):
        raise ValueError("invalid WAV")

    monkeypatch.setattr(originals, "decode", corrupt)
    with pytest.raises(ValueError, match=f"{cid}: cannot decode .*: invalid WAV"):
        originals.build(cid, index, texts, tmp_path)


def test_heuristic_duration_ties_are_broken_by_line_name(tmp_path, monkeypatch):
    monkeypatch.setattr(originals, "CHARACTERS", {"TEST": ("Test", ["TEST"], [])})
    monkeypatch.setattr(originals, "ORIGINAL_LINES", {})
    monkeypatch.setattr(originals, "RATE", 100)
    names = [f"DIA_TEST_{letter}_14_00" for letter in "ABCDEFG"]
    monkeypatch.setattr(originals, "decode", lambda *_: np.full(450, 0.1, dtype=np.float32))
    texts = dict.fromkeys(names, "A calm sentence.")
    first = originals.build("TEST", {line: (None, line) for line in names}, texts, tmp_path)
    before = Path(first["file"]).read_bytes()
    second = originals.build("TEST", {line: (None, line) for line in reversed(names)}, texts, tmp_path)
    assert [entry["line"] for entry in first["lines"]] == names[:5]
    assert first == second
    assert Path(second["file"]).read_bytes() == before
