"""The native dialog library, viewed as line name -> subtitle text.

The storygraph child owns the base lease while exporting raw message pairs. Python retains its own
WAV-name, cp1251 and whitespace rules. Voice 15 is the hero.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
from collections import Counter
from collections.abc import Iterable
from pathlib import Path

from gothic_ai.gamedata import base, storygraph

HERO_VOICE = "15"
_WAV_NAME = re.compile(rb"([A-Za-z0-9_]+)\.WAV")
_SHA256 = re.compile(r"[0-9a-f]{64}")


def native_messages(game_dir: Path) -> list[tuple[bytes, bytes]] | None:
    """Read owned raw pairs from the existing native tool, never a released base path."""
    tool = Path(os.environ.get("STORYGRAPH_TOOL", storygraph.TOOL))
    if not tool.exists():
        raise RuntimeError(
            f"Native OU reader is missing: {tool}. Build it with "
            "`cmake -S OpenGothic/ai_bridge/storygraph -B OpenGothic/ai_bridge/storygraph/build -G Ninja && "
            "cmake --build OpenGothic/ai_bridge/storygraph/build`, run the launcher install/update, "
            "or set STORYGRAPH_TOOL."
        )
    try:
        with base.resolve(game_dir) as selected:
            result = subprocess.run(
                [str(tool), str(selected.path), "--ou-messages"],
                env={**os.environ, "OPENGOTHIC_BASE": str(selected.path)},
                check=True,
                capture_output=True,
                timeout=1800,
            )
    except (OSError, subprocess.SubprocessError) as error:
        stderr = getattr(error, "stderr", None) or b""
        if isinstance(stderr, bytes):
            stderr = stderr.decode("utf-8", "replace")
        raise RuntimeError(f"Native OU reader {tool} failed: {error}\n{stderr[-3000:]}") from error
    try:
        data = json.loads(result.stdout)
        if (
            not isinstance(data, dict)
            or data.get("format") != "gothic-ou-messages"
            or type(data.get("version")) is not int
            or data["version"] != 1
        ):
            raise ValueError("expected gothic-ou-messages version 1")
        source, native_sha, messages = data["source"], data["native_sha256"], data["messages"]
        if messages is None:
            if source is not None or native_sha is not None:
                raise ValueError("absent messages require null source and native_sha256")
            return None
        if (
            not isinstance(source, dict)
            or not isinstance(source.get("path"), str)
            or not source["path"]
            or not isinstance(source.get("sha256"), str)
            or not _SHA256.fullmatch(source["sha256"])
            or not isinstance(native_sha, str)
            or not _SHA256.fullmatch(native_sha)
            or not isinstance(messages, list)
        ):
            raise ValueError("invalid source, native_sha256 or messages")
        raw = []
        for pair in messages:
            if not isinstance(pair, list) or len(pair) != 2 or not all(isinstance(s, str) for s in pair):
                raise ValueError("expected raw [name, text] string pairs")
            raw.append((pair[0].encode("latin1"), pair[1].encode("latin1")))
        return raw
    except (ValueError, TypeError, KeyError) as error:
        stderr = result.stderr.decode("utf-8", "replace")
        raise RuntimeError(
            f"Native OU reader {tool} returned invalid JSON: {error}\n{stderr[-3000:]}"
        ) from error


def lines_from_messages(messages: Iterable[tuple[bytes, bytes]], encoding: str = "cp1251") -> dict[str, str]:
    lines: dict[str, str] = {}
    for name, text in messages:
        if match := _WAV_NAME.fullmatch(name):
            lines[match[1].decode("ascii").upper()] = text.decode(encoding, "replace").strip()
    return lines


def npc_dialog_key(symbol: str) -> str:
    """`NONE_100_XARDAS` -> `XARDAS`, `VLK_408_JORA` -> `JORA`: the part after the numeric id."""
    parts = symbol.upper().split("_")
    for i, part in enumerate(parts):
        if part.isdigit():
            return "_".join(parts[i + 1 :])
    return parts[-1]


def npc_lines(lines: dict[str, str], symbol: str) -> list[tuple[str, str]]:
    """Lines spoken by the npc itself in its own dialogs (hero lines excluded), sorted by name."""
    prefix = f"DIA_{npc_dialog_key(symbol)}_"
    out = []
    for name, text in sorted(lines.items()):
        parts = name.split("_")
        if name.startswith(prefix) and len(parts) >= 4 and parts[-2] != HERO_VOICE and text:
            out.append((name, text))
    return out


def voice_index(lines: Iterable[tuple[str, str]]) -> str | None:
    """The original voice actor of an npc's lines (`npc_lines`): the most common `<voice>` in their names.
    The npc's own dialogs may have a line of someone else's now and then, hence the majority."""
    voices = Counter(p[-2] for p in (name.split("_") for name, _ in lines) if len(p) >= 4 and p[-2].isdigit())
    return voices.most_common(1)[0][0] if voices else None


def game_lines(game_dir: Path) -> dict[str, str]:
    """All dialog lines from the ready base; persona.py reads them once for many NPCs."""
    messages = native_messages(game_dir)
    if messages is None:
        raise RuntimeError(f"The selected base has no server OU for {game_dir}")
    return lines_from_messages(messages)


def load_npc_lines(game_dir: Path, symbol: str) -> list[tuple[str, str]]:
    lines = game_lines(game_dir)
    return [] if lines is None else npc_lines(lines, symbol)
