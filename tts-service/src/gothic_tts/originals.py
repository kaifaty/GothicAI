"""Voice snapshots of plot npc's from the game's own Russian speech: ~20-25 s of calm lines per character,
for Inworld voice cloning. In the game only: shorts keep designed voices (the user, 2026-10-27).

A snapshot is the character's own lines (its dialogs, its voice index), neither shouted nor whispered,
3-6 s phrases from different dialogs first, evened out and joined with short pauses. Cutscenes are left
out: they carry music and reverb. The snapshots are the game's audio, so they stay outside the repository.

usage: uv run python scripts/voice_snapshots.py <game dir> [--ou ou.json] [--out DIR] [ID ...]
  --ou: the storygraph tool's `--ou-messages` output (it is run when omitted, ~2 min)
  --out: default ~/.local/share/gothic-ai/voices/original (WAVs and manifest.json)
"""

import argparse
import collections
import hashlib
import json
import logging
import os
import re
import shutil
import struct
import subprocess
import tempfile
from collections.abc import Callable
from pathlib import Path

import numpy as np

from gothic_tts import base

RATE = 44100
REPO = Path(__file__).resolve().parents[3]
STORYGRAPH = REPO / "OpenGothic" / "ai_bridge" / "storygraph" / "build" / "storygraph"
DEFAULT_OUT = Path.home() / ".local/share/gothic-ai/voices/original"
TARGET_S = (20.0, 28.0)
GAP_S = 0.35
ALGORITHM_VERSION = 3
ORIGINAL_LINES = json.loads(Path(__file__).with_name("original_lines.json").read_text())
log = logging.getLogger(__name__)

# id (primary instance) -> name, dialog tokens (first part of a line name after DIA_/ADDON_), other instances
CHARACTERS = {
    "NONE_100_XARDAS": ("Ксардас", ["XARDAS"], []),
    "VLK_439_VATRAS": ("Ватрас", ["VATRAS"], ["VLK_439_VATRAS_DI"]),
    "BAU_4300_ADDON_CAVALORN": ("Кавалорн", ["CAVALORN"], []),
    "PAL_200_HAGEN": ("Лорд Хаген", ["LORD_HAGEN", "HAGEN"], []),
    "PC_PSIONIC": ("Лестер", ["LESTER", "PC_LESTER"], ["PC_PSIONIC_DI"]),
    "PC_THIEF_NW": ("Диего", ["DIEGO", "DIEGOOW", "DIEGONW", "DIEGODI"], ["PC_THIEFOW", "PC_THIEF_DI"]),
    "PC_MAGE_NW": ("Милтен", ["MILTEN", "MILTENOW", "MILTENNW"], ["PC_MAGE_OW", "PC_MAGE_DI"]),
    "PC_FIGHTER_NW_VOR_DJG": (
        "Горн",
        ["GORN", "GORNOW", "GORNNW", "GORNDJG"],
        ["PC_FIGHTER_OW", "PC_FIGHTER_DJG", "PC_FIGHTER_NW_NACH_DJG", "PC_FIGHTER_DI"],
    ),
    "SLD_800_LEE": ("Ли", ["LEE"], ["SLD_800_LEE_DI"]),
    "VLK_449_LARES": ("Ларес", ["LARES"], ["VLK_449_LARES_DI"]),
    "KDF_500_PYROKAR": ("Пирокар", ["PYROKAR"], []),
    "KDF_501_SERPENTES": ("Серпентес", ["SERPENTES"], []),
    "KDF_502_ULTHAR": ("Ультар", ["ULTHAR"], []),
    "PAL_203_LOTHAR": ("Лотар", ["LOTHAR"], []),
    "PAL_250_GAROND": ("Гаронд", ["GAROND"], []),
    "PAL_252_PARCIVAL": ("Парцифаль", ["PARCIVAL"], []),
    "MIL_311_ANDRE": ("Лорд Андрэ", ["ANDRE"], []),
    "BAU_900_ONAR": ("Онар", ["ONAR"], []),
    "SLD_801_TORLOF": ("Торлоф", ["TORLOF"], ["SLD_801_TORLOF_DI"]),
    "SLD_809_BENNET": ("Беннет", ["BENNET"], ["SLD_809_BENNET_DI"]),
    "SLD_806_SYLVIO": ("Сильвио", ["SYLVIO", "SYLVIODJG"], ["DJG_700_SYLVIO"]),
    "SLD_811_WOLF": ("Вольф", ["WOLF"], ["SLD_811_WOLF_DI"]),
    "VLK_444_JACK": ("Джек", ["JACK"], ["VLK_444_JACK_DI"]),
    "DJG_705_ANGAR": ("Ангар", ["ANGAR", "ANGARDJG"], ["DJG_705_ANGAR_NW", "DJG_705_ANGAR_DI"]),
    "DJG_713_BIFF": ("Бифф", ["BIFF"], ["DJG_713_BIFF_NW", "DJG_713_BIFF_DI"]),
    "NONE_110_URSHAK": ("Ур-Шак", ["URSHAK"], []),
    "KDW_1400_ADDON_SATURAS_NW": ("Сатурас", ["SATURAS"], ["KDW_14000_ADDON_SATURAS_ADW"]),
    "KDW_1401_ADDON_CRONOS_NW": ("Кронос", ["CRONOS"], ["KDW_14010_ADDON_CRONOS_ADW"]),
    "KDW_1402_ADDON_NEFARIUS_NW": ("Нефариус", ["NEFARIUS"], ["KDW_14020_ADDON_NEFARIUS_ADW"]),
    "KDW_1403_ADDON_MYXIR_NW": ("Миксир", ["MYXIR"], ["KDW_14030_ADDON_MYXIR_ADW"]),
    "KDW_1404_ADDON_RIORDIAN_NW": ("Риордиан", ["RIORDIAN"], ["KDW_14040_ADDON_RIORDIAN_ADW"]),
    "KDW_1405_ADDON_MERDARION_NW": ("Мердарион", ["MERDARION"], ["KDW_14050_ADDON_MERDARION_ADW"]),
    "PIR_1320_ADDON_GREG": ("Грег", ["GREG"], ["PIR_1300_ADDON_GREG_NW"]),
    "BDT_1090_ADDON_RAVEN": ("Ворон", ["RAVEN"], []),
    "BDT_10014_ADDON_THORUS": ("Торус", ["THORUS"], ["VLK_2000_THORUS"]),
    "BDT_1083_ADDON_ESTEBAN": ("Эстебан", ["ESTEBAN"], []),
}
PREFIXES = ("DIA_ADDON_", "DIA_")  # cutscenes (INTRO_/OUTRO_) carry music and reverb
LINE = re.compile(r"(.*)_(\d+)_(\d+)$")


def speech_index(selected: base.Base) -> dict[str, tuple[base.Base, str]]:
    """Line name -> verified native WAV; the importer already selected each sound's winner."""
    index = selected.json("audio/audio.json")
    if index.get("format") != "gothic-audio" or index.get("version") != 1:
        raise RuntimeError("unsupported native audio index")
    out = {}
    for name, sound in index["sounds"].items():
        if sound["class"] != "speech" or not name.endswith(".WAV"):
            continue
        payload = index["payloads"][sound["payload"]]
        # Native strings preserve source bytes as Latin-1; the previous VDF reader used cp1251.
        key = name.encode("latin1").decode("cp1251").upper()[:-4]
        out[key] = (selected, "audio/" + payload["file"])
    if not out:
        raise RuntimeError(f"no speech WAVs in base {selected.path}")
    return out


def own_lines(names, tokens: list[str]) -> list[tuple[str, str]]:
    """(line name, voice) of the character's own dialogs, its own voice only (the majority there)."""
    found = []
    for name in names:
        m = LINE.match(name)
        if not m:
            continue
        rest = m[1]
        for p in PREFIXES:
            if rest.startswith(p):
                rest = rest[len(p) :]
                break
        if rest != m[1] and any(rest == t or rest.startswith(t + "_") for t in tokens):
            found.append((name, m[2]))
    voices = collections.Counter(v for _, v in found if v != "15")
    if not voices:
        return []
    voice = voices.most_common(1)[0][0]
    return [(n, v) for n, v in found if v == voice]


# IMA/DVI WAVE uses low nibble first, a fresh predictor/index per channel in each block,
# and four encoded bytes per channel at a time. This uses the integer product rule of
# OpenAL Soft's FmtIMA4 decoder (core/voice.cpp), including truncation toward zero.
_IMA_STEPS = (
    7,
    8,
    9,
    10,
    11,
    12,
    13,
    14,
    16,
    17,
    19,
    21,
    23,
    25,
    28,
    31,
    34,
    37,
    41,
    45,
    50,
    55,
    60,
    66,
    73,
    80,
    88,
    97,
    107,
    118,
    130,
    143,
    157,
    173,
    190,
    209,
    230,
    253,
    279,
    307,
    337,
    371,
    408,
    449,
    494,
    544,
    598,
    658,
    724,
    796,
    876,
    963,
    1060,
    1166,
    1282,
    1411,
    1552,
    1707,
    1878,
    2066,
    2272,
    2499,
    2749,
    3024,
    3327,
    3660,
    4026,
    4428,
    4871,
    5358,
    5894,
    6484,
    7132,
    7845,
    8630,
    9493,
    10442,
    11487,
    12635,
    13899,
    15289,
    16818,
    18500,
    20350,
    22358,
    24633,
    27086,
    29794,
    32767,
)
_IMA_ADJUST = (-1, -1, -1, -1, 2, 4, 6, 8) * 2
# One byte's two signed deltas and final index; table lookup keeps long speech imports cheap.
_IMA_BYTES = []
for _index, _step in enumerate(_IMA_STEPS):
    _row = []
    for _byte in range(256):
        _lo, _hi = _byte & 15, _byte >> 4
        _d1 = ((2 * (_lo & 7) + 1) * _step) // 8 * (-1 if _lo & 8 else 1)
        _next = min(88, max(0, _index + _IMA_ADJUST[_lo]))
        _d2 = ((2 * (_hi & 7) + 1) * _IMA_STEPS[_next]) // 8 * (-1 if _hi & 8 else 1)
        _row.append((_d1, _d2, min(88, max(0, _next + _IMA_ADJUST[_hi]))))
    _IMA_BYTES.append(_row)
del _index, _step, _row, _byte, _lo, _hi, _d1, _next, _d2


def _ima_channel(encoded: bytes, predictor: int, index: int) -> np.ndarray:
    if not 0 <= index <= 88:
        raise ValueError(f"invalid IMA ADPCM step index {index}")
    deltas = [predictor]
    for byte in encoded:
        a, b, index = _IMA_BYTES[index][byte]
        deltas.extend((a, b))
    samples = np.cumsum(deltas, dtype=np.int64)
    if samples.min() < -32768 or samples.max() > 32767:
        # Saturation is part of predictor state, so clipping only the final array is wrong.
        value = predictor
        for i, delta in enumerate(deltas[1:], 1):
            value = max(-32768, min(32767, value + delta))
            samples[i] = value
    return samples.astype(np.int16)


def decode_wav(raw: bytes) -> np.ndarray:
    """Decode PCM16 or IMA ADPCM RIFF/WAVE to mono float PCM at RATE without external tools."""
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("speech is not a RIFF/WAVE file")
    chunks = {}
    offset = 12
    while offset + 8 <= len(raw):
        kind, size = struct.unpack_from("<4sI", raw, offset)
        start = offset + 8
        if start + size > len(raw):
            raise ValueError(f"truncated WAVE {kind!r} chunk")
        chunks[kind] = raw[start : start + size]
        offset = start + size + (size & 1)
    fmt, data = chunks[b"fmt "], chunks[b"data"]
    tag, channels, rate, _, block, bits = struct.unpack_from("<HHIIHH", fmt)
    if channels not in (1, 2) or not rate:
        raise ValueError(f"unsupported WAVE channels/rate: {channels}/{rate}")
    if tag == 1 and bits == 16:
        pcm = np.frombuffer(data, dtype="<i2").reshape(-1, channels)
    elif tag == 0x11 and bits == 4:
        if len(fmt) < 20 or block < channels * 4 or not data:
            raise ValueError("invalid IMA ADPCM WAVE format")
        per_block = struct.unpack_from("<H", fmt, 18)[0]
        blocks = []
        for start in range(0, len(data), block):
            body = data[start : start + block]
            if len(body) < channels * 4:
                raise ValueError("truncated IMA ADPCM block")
            decoded = []
            for channel in range(channels):
                predictor, index, reserved = struct.unpack_from("<hBB", body, channel * 4)
                if reserved:
                    raise ValueError("invalid IMA ADPCM block header")
                payload = body[channels * 4 :]
                encoded = b"".join(
                    payload[i + channel * 4 : i + channel * 4 + 4]
                    for i in range(0, len(payload), channels * 4)
                )
                decoded.append(_ima_channel(encoded, predictor, index)[:per_block])
            blocks.append(np.column_stack(decoded))
        # Keep complete encoded blocks, as the previous snapshot conversion did. Speech
        # archives' fact counts can cut a block's quiet tail and change line selection.
        pcm = np.concatenate(blocks)
    else:
        raise ValueError(f"unsupported WAVE format 0x{tag:x}/{bits} bits")
    mono = pcm.astype(np.float32).mean(axis=1) / 32768
    if rate != RATE and len(mono):
        # Speech archives use 44100 Hz; PCM recordings may use another rate.
        count = round(len(mono) * RATE / rate)
        mono = np.interp(np.arange(count) * rate / RATE, np.arange(len(mono)), mono).astype(np.float32)
    return mono


def decode(selected: base.Base, relative: str) -> np.ndarray:
    return decode_wav(selected.read(relative))


def trim(x: np.ndarray, floor_db: float = -45) -> np.ndarray:
    win = int(0.01 * RATE)
    if len(x) < win * 3:
        return x
    frames = x[: len(x) // win * win].reshape(-1, win)
    db = 20 * np.log10(np.sqrt((frames**2).mean(1)) + 1e-9)
    loud = np.nonzero(db > floor_db)[0]
    if not len(loud):
        return x[:0]
    a, b = max(0, loud[0] - 3), min(len(frames), loud[-1] + 6)  # 30 ms before, 60 ms tail
    return x[a * win : b * win]


def rms_db(x: np.ndarray) -> float:
    return float(20 * np.log10(np.sqrt((x**2).mean()) + 1e-9))


def build(cid: str, index, texts, out: Path) -> dict:
    name, tokens, aliases = CHARACTERS[cid]
    pinned = ORIGINAL_LINES.get(cid)
    for line in pinned or []:
        if line not in index:
            raise ValueError(f"pinned original voice {cid}: missing speech line {line}")
    lines = own_lines(index.keys(), tokens)
    if pinned:
        known = {line for line, _ in lines}
        lines.extend((line, LINE.fullmatch(line)[2]) for line in pinned if line not in known)
    # Retain the old normalization pool even when the selected lines are pinned.
    cands, pinned_cands = [], {}
    for line, voice in lines:
        text = texts.get(line, "")
        required = pinned is not None and line in pinned
        if not text and not required:
            continue
        try:
            audio = trim(decode(*index[line]))
            if required and not len(audio):
                raise ValueError("no speech samples")
        except (ValueError, OSError, KeyError, struct.error, RuntimeError) as error:
            if required:
                raise ValueError(f"pinned original voice {cid}: cannot decode {line}: {error}") from error
            raise
        dur = len(audio) / RATE
        candidate = {
            "line": line, "text": text, "s": dur, "db": rms_db(audio), "audio": audio, "voice": voice
        }
        if required:
            pinned_cands[line] = candidate
        if text and 1.2 <= dur <= 12.0:
            cands.append(candidate)
    if not cands:
        if pinned:
            raise ValueError(f"pinned original voice {cid}: no normalization lines")
        return {"id": cid, "name": name, "error": "no lines"}
    med = float(np.median([c["db"] for c in cands]))
    # calm first: no shouts, short questions are mostly a tone, not the voice; then looser for the few-lined
    passes = [
        lambda c: (
            c["s"] <= 8
            and "!" not in c["text"]
            and not ("?" in c["text"] and len(c["text"]) < 25)
            and abs(c["db"] - med) <= 3
        ),
        lambda c: c["s"] <= 8 and "!" not in c["text"] and abs(c["db"] - med) <= 4.5,
        lambda c: abs(c["db"] - med) <= 6,
    ]
    if pinned:
        picked = [pinned_cands[line] for line in pinned]
    else:
        picked, dialogs, total = [], set(), 0.0
        for last, ok in enumerate(passes, 1 - len(passes)):
            calm = sorted(
                (c for c in cands if ok(c) and c not in picked),
                key=lambda c: (abs(c["s"] - 4.5), c["line"]),
            )
            for c in calm:  # 3-6 s phrases first: whole sentences, more of them per snapshot
                dialog = LINE.match(c["line"])[1]
                if (dialog in dialogs and last) or total + c["s"] > TARGET_S[1]:
                    continue
                picked.append(c)
                dialogs.add(dialog)
                total += c["s"] + GAP_S
                if total >= TARGET_S[0]:
                    break
            if total >= TARGET_S[0]:
                break
        picked.sort(key=lambda c: c["line"])
    calm = [c for c in cands if passes[0](c)]
    gap = np.zeros(int(GAP_S * RATE), dtype=np.float32)
    parts = []
    for c in picked:
        parts += [c["audio"] * 10 ** ((med - c["db"]) / 20), gap]  # even lines out to the voice's median
    audio = np.concatenate(parts[:-1])
    audio *= 10 ** ((-20.0 - rms_db(audio)) / 20)
    audio = np.clip(audio * min(1.0, 0.9 / max(1e-9, np.abs(audio).max())), -1, 1)
    wav = out / f"{cid}.wav"
    write_wav(wav, audio)
    return {
        "id": cid,
        "name": name,
        "aliases": aliases,
        "voice": picked[0]["voice"],
        "seconds": round(len(audio) / RATE, 1),
        "own_lines": len(lines),
        "calm_lines": len(calm),
        "file": str(wav),
        "lines": [{"line": c["line"], "text": c["text"], "s": round(c["s"], 2)} for c in picked],
    }


def write_wav(path: Path, x: np.ndarray) -> None:
    pcm = (x * 32767).astype("<i2").tobytes()
    hdr = (
        b"RIFF"
        + struct.pack("<I", 36 + len(pcm))
        + b"WAVEfmt "
        + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
    )
    path.write_bytes(hdr + b"data" + struct.pack("<I", len(pcm)) + pcm)


def _texts(selected: base.Base, ou: Path | None) -> dict[str, str]:
    if ou is not None:
        raw = ou.read_bytes()
    else:
        tool = Path(os.environ.get("STORYGRAPH_TOOL", STORYGRAPH))
        if not tool.is_file():
            raise RuntimeError(f"native OU reader is missing: {tool}; set STORYGRAPH_TOOL")
        # The base itself is the positional directory: the child never probes the installation.
        # It owns another lease for the entire export, just like the server's OU reader.
        try:
            raw = subprocess.run(
                [str(tool), str(selected.path), "--ou-messages"],
                env={**os.environ, "OPENGOTHIC_BASE": str(selected.path)},
                capture_output=True,
                check=True,
                timeout=1800,
            ).stdout
        except (OSError, subprocess.SubprocessError) as error:
            detail = (getattr(error, "stderr", b"") or b"").decode(errors="replace").strip()
            raise RuntimeError(f"native OU reader failed: {error}: {detail[-1000:]}") from error
    messages = json.loads(raw)["messages"]
    if not messages:
        raise RuntimeError(f"no OU speech texts in base {selected.path}")
    texts = {}
    for name, text in messages:
        name = name.encode("latin1").decode("ascii", "replace").upper()
        if name.endswith(".WAV"):
            texts[name[:-4]] = text.encode("latin1").decode("cp1251", "replace").strip()
    if not texts:
        raise RuntimeError(f"no OU speech texts in base {selected.path}")
    return texts


def prepare(
    game_dir: Path,
    out: Path = DEFAULT_OUT,
    *,
    ou: Path | None = None,
    ids: list[str] | None = None,
    progress: Callable[[int, int], None] | None = None,
) -> Path:
    """Reuse matching snapshots or rebuild them synchronously; callers run this in a worker thread."""
    game = Path(game_dir).expanduser().resolve()
    out = Path(out).expanduser().resolve()
    for stale in out.glob(".snapshots-*"):
        if stale.is_dir():
            shutil.rmtree(stale)
    with base.resolve(game) as selected:
        return _prepare(selected, out, ou=ou, ids=ids, progress=progress)


def _prepare(
    selected: base.Base,
    out: Path,
    *,
    ou: Path | None,
    ids: list[str] | None,
    progress: Callable[[int, int], None] | None,
) -> Path:
    source = {"base_fingerprint": selected.fingerprint}
    if ou is not None:
        source["ou_sha256"] = hashlib.sha256(ou.read_bytes()).hexdigest()
    only = ids or list(CHARACTERS)
    for cid in only:
        if cid not in CHARACTERS:
            raise ValueError(f"unknown original voice {cid}")
    path = out / "manifest.json"
    try:
        previous = json.loads(path.read_text())
    except (FileNotFoundError, ValueError):
        previous = {}
    entries = previous if isinstance(previous, list) else previous.get("characters", [])
    manifest = {entry["id"]: entry for entry in entries}
    matches = (
        isinstance(previous, dict)
        and previous.get("algorithm_version") == ALGORITHM_VERSION
        and previous.get("source") == source
    )
    prepared = set(previous.get("prepared_ids", [])) if matches else set()
    if all(
        cid in prepared
        and cid in manifest
        and (manifest[cid].get("error") or Path(manifest[cid].get("file", "")).is_file())
        for cid in only
    ):
        return path
    index = speech_index(selected)
    texts = _texts(selected, ou)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".snapshots-", dir=out) as staging:
        for done, cid in enumerate(only, 1):
            try:
                result = build(cid, index, texts, Path(staging))
            except (ValueError, OSError, KeyError, struct.error) as error:
                result = {"id": cid, "name": CHARACTERS[cid][0], "error": str(error)}
            if "file" in result:
                destination = out / f"{cid}.wav"
                Path(result["file"]).replace(destination)
                result["file"] = str(destination)
            else:
                log.warning("original voice %s snapshot failed: %s", cid, result["error"])
            prepared.add(cid)
            manifest[cid] = result
            if progress is not None:
                progress(done, len(only))
        document = {
            "algorithm_version": ALGORITHM_VERSION,
            "source": source,
            "prepared_ids": sorted(prepared),
            "characters": [manifest[c] for c in CHARACTERS if c in manifest],
        }
        temporary = Path(staging) / "manifest.json"
        temporary.write_text(json.dumps(document, ensure_ascii=False, indent=1))
        temporary.replace(path)
    return path


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("game", type=Path)
    parser.add_argument("--ou", type=Path)
    parser.add_argument("--out", type=Path, default=DEFAULT_OUT)
    parser.add_argument("ids", nargs="*")
    args = parser.parse_args()
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    path = prepare(args.game, args.out, ou=args.ou, ids=args.ids)
    only = args.ids or list(CHARACTERS)
    for entry in json.loads(path.read_text())["characters"]:
        if entry["id"] in only:
            print(
                f"{entry['id']:30} {entry['name']:12} {entry.get('seconds', 0):5} s  "
                f"{len(entry.get('lines', []))} lines  voice {entry.get('voice')}",
                entry.get("error", ""),
            )


if __name__ == "__main__":
    main()
