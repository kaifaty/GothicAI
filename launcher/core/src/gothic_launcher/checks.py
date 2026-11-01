"""Checks the launcher window shows before anything starts: is the game folder usable, and can each
chosen provider be reached. Texts are for the player (Russian); nothing here spends a plan's quota.
"""

from __future__ import annotations

import asyncio
import contextlib
import hashlib
import json
import os
import re
import sys
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any
from urllib.parse import urlsplit

import httpx

from . import base
from .settings import Config

CHATGPT_TOKENS = Path.home() / ".config" / "gothic-ai" / "chatgpt.json"
INWORLD_VOICES = "https://api.inworld.ai/tts/v1/voices"
INWORLD_STREAM = "https://api.inworld.ai/tts/v1/voice:stream"
# a line for a voice that does not exist: Inworld refuses it before speaking, so nothing is billed, and it
# looks at the credits before the voice (402 on an empty account, then 404 for the voice), as tts-service does
INWORLD_CHECK = {
    "text": "Да.",
    "voiceId": "gothic-ai-no-such-voice",
    "modelId": "inworld-tts-2",
    "language": "ru-RU",
    "audioConfig": {"audioEncoding": "PCM", "sampleRateHertz": 24000},
}
DEFAULT_VOXTRAL_URL = "http://127.0.0.1:47830"  # the services' own defaults
DEFAULT_VOXCPM_URL = "ws://127.0.0.1:47831"
# Share of cp1251 Cyrillic letters among the letters of the dialog library.
RUSSIAN_MIN_SHARE = 0.1
TIMEOUT_S = 3.0
STEAM = Path.home() / "Library" / "Application Support" / "Steam"
# Windows Steam and GOG installs inside Wine bottles (CrossOver, Whisky)
BOTTLES = (
    Path.home() / "Library" / "Application Support" / "CrossOver" / "Bottles",
    Path.home() / "Library" / "Containers" / "com.isaacmarovitz.Whisky" / "Bottles",
)
GAME_FOLDERS = ("Gothic II", "Gothic 2 Gold", "Gothic II Gold")
OU_PATH = "_work/Data/Scripts/Content/Cutscene/OU.BIN"


@dataclass(frozen=True)
class Check:
    id: str
    ok: bool
    title: str
    detail: str

    def plain(self) -> dict[str, Any]:
        return asdict(self)


def find_games(extra: list[Path] | None = None) -> list[Path]:
    """Folders that look like Gothic II (they have a Data folder): Steam libraries, Wine bottles with
    Steam or GOG, the usual places. A copy made by hand elsewhere is not found: the player picks it."""
    if sys.platform == "win32":
        steam_roots = []
        for steam in _windows_steam():
            steam_roots += [steam, *_steam_libraries(steam / "steamapps" / "libraryfolders.vdf")]
        places = [root / "steamapps" / "common" for root in steam_roots] + steam_roots
        x86 = Path(os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)")
        places += [Path("C:/"), Path("C:/GOG Games"), x86 / "GOG Galaxy" / "Games", x86 / "JoWooD"]
        places += [Path.home() / "Games"]
        return _gothic_folders(places, extra)
    steam_roots = [STEAM, *_steam_libraries(STEAM / "steamapps" / "libraryfolders.vdf")]
    for bottles in BOTTLES:
        for drive in _children(bottles, lambda p: p / "drive_c"):
            steam_roots += [drive / "Program Files (x86)" / "Steam", drive / "Program Files" / "Steam"]
            steam_roots += [drive / "GOG Games", drive / "Program Files (x86)" / "GOG Galaxy" / "Games"]
    places = [root / "steamapps" / "common" for root in steam_roots] + steam_roots
    places += [Path.home() / "Games", Path("/Applications"), Path.home()]
    return _gothic_folders(places, extra)


def _gothic_folders(places: list[Path], extra: list[Path] | None) -> list[Path]:
    candidates = [place / name for place in places for name in GAME_FOLDERS] + (extra or [])
    found: dict[Path, Path] = {}
    for c in candidates:
        if _child(c, "data") is not None:
            found.setdefault(c.resolve(), c)
    return list(found.values())


def _steam_libraries(vdf: Path) -> list[Path]:
    """`"path"  "/Volumes/Games/SteamLibrary"` lines of libraryfolders.vdf (UTF-8, also on Windows)."""
    try:
        text = vdf.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return []
    return [Path(m.group(1).replace("\\\\", "\\")) for m in re.finditer(r'"path"\s+"([^"]+)"', text)]


def _windows_steam() -> list[Path]:
    """Where Steam says it is installed (its own registry value), then its default folder."""
    roots: list[Path] = []
    with contextlib.suppress(OSError, ImportError):
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            roots.append(Path(winreg.QueryValueEx(key, "SteamPath")[0]))
    x86 = os.environ.get("ProgramFiles(x86)") or r"C:\Program Files (x86)"
    return [*roots, Path(x86) / "Steam"]


def _children(folder: Path, pick: Callable[[Path], Path]) -> list[Path]:
    try:
        return [pick(p) for p in folder.iterdir() if p.is_dir()]
    except OSError:
        return []


def game_folder(folder: Path | None) -> list[Check]:
    """What the game needs from the folder; one check per thing a player can fix."""
    if folder is None:
        return [Check("folder", False, "Папка игры", "Укажите папку установленной Gothic II один раз")]
    try:
        selected = base.resolve(folder)
    except RuntimeError as error:
        base_error = str(error)
    else:
        with selected:
            checks = [Check("base", True, "База игры готова", selected.fingerprint)]
            try:
                checks.append(_russian(_base_cyrillic_share(selected) >= RUSSIAN_MIN_SHARE))
            except (RuntimeError, ValueError, KeyError, TypeError, IndexError) as error:
                checks.append(Check("russian", False, "Русская локализация", str(error)))
            return checks
    data = _child(folder, "data")
    if not folder.is_dir() or data is None:
        return [Check("folder", False, "Папка игры",
                      f"Укажите папку установленной Gothic II один раз. {base_error}")]
    archives = sorted(p.name for p in data.iterdir() if p.suffix.lower() == ".vdf")
    addon = [n for n in archives if "addon" in n.lower()]
    checks = [
        Check(
            "base", True,
            "Лаунчер подготовит базу перед запуском (несколько минут)", "Найдена установка Gothic II",
        ),
        Check("archives", bool(archives), "Архивы Gothic II", _names(archives) or "В Data нет архивов .vdf"),
        Check(
            "addon",
            bool(addon),
            "Аддон «Ночь Ворона»",
            _names(addon) or "Нет архивов аддона: нужна Gothic II Gold или «Ночь Ворона»",
        ),
    ]
    ou = _ou_file(folder)
    if ou is None:
        checks.append(Check("russian", False, "Русская локализация", f"Нет файла реплик {OU_PATH}"))
    else:
        checks.append(_russian(_cyrillic_share(ou.read_bytes()) >= RUSSIAN_MIN_SHARE))
    return checks


def _russian(ok: bool) -> Check:
    return Check(
        "russian", ok, "Русская локализация", "Реплики NPC на русском" if ok else
        "Реплики NPC не на русском: поставьте русскую локализацию (текст и шрифты)",
    )


def _base_cyrillic_share(selected: base.Base) -> float:
    """OU JSON preserves each original single-byte character as its Latin-1 code point."""
    index = selected.json("outputunits/outputunits.json")
    if index["format"] != "gothic-outputunits" or index["version"] != 1:
        raise ValueError("invalid output units index")
    source = index["server"]["ou_py"]
    if type(source) is not int or source < 0:
        raise ValueError("the base has no server OU library")
    record = index["sources"][source]["record"]
    if not isinstance(record, str) or not re.fullmatch(r"[0-9a-f]{64}", record):
        raise ValueError("invalid output units record")
    raw = selected.read(f"outputunits/{record}.json")
    if index["records"][record] != hashlib.sha256(raw).hexdigest():
        raise ValueError("output units record identity differs")
    library = json.loads(raw)
    if library["format"] != 1 or library["kind"] != "cutscene_library":
        raise ValueError("invalid output units library")

    def texts(value):
        if isinstance(value, dict):
            if value.get("class") == "oCMsgConversation":
                yield value["text"].encode("latin1")
            else:
                for child in value.values():
                    yield from texts(child)
        elif isinstance(value, list):
            for child in value:
                yield from texts(child)

    return _cyrillic_share(b"".join(texts(library["blocks"])))


async def providers(config: Config, transport: httpx.AsyncBaseTransport | None = None) -> dict[str, Any]:
    """For each part (brain, voice, speech): its primary and fallback provider and whether they answer."""
    s = config.settings
    async with httpx.AsyncClient(timeout=TIMEOUT_S, trust_env=False, transport=transport) as http:
        brain = asyncio.create_task(_llm(config, http))
        voice = asyncio.gather(
            *(_asr(name, config, http) for name in (s.voice.provider, s.voice.fallback) if name != "none")
        )
        speech = asyncio.gather(
            *(_tts(name, config, http) for name in (s.tts.provider, s.tts.fallback) if name != "none")
        )
        llm_check, asr_checks, tts_checks = await asyncio.gather(brain, voice, speech)
    return {
        "brain": {"enabled": True, "providers": [llm_check.plain()]},
        "voice": {"enabled": s.voice.enabled, "providers": [c.plain() for c in asr_checks]},
        "speech": {"enabled": s.tts.enabled, "providers": [c.plain() for c in tts_checks]},
    }


async def _llm(config: Config, http: httpx.AsyncClient) -> Check:
    llm = config.settings.llm
    match llm.backend:
        case "chatgpt":
            return _chatgpt("chatgpt", "Подписка ChatGPT")
        case "openai":
            ok = bool(config.secrets.openai_api_key)
            return Check("openai", ok, "Ключ OpenAI", "Ключ задан" if ok else "Ключ не задан")
        case "local":
            url = (llm.base_url or "").rstrip("/")
            if not url:
                return Check("local", False, "Свой сервер", "Адрес не задан")
            return await _http("local", "Свой сервер", f"{url}/models", http, any_status=True)
        case _:
            return Check(llm.backend, True, "Заглушка", "Ответы без модели, для проверок")


async def _asr(name: str, config: Config, http: httpx.AsyncClient) -> Check:
    provider = name.partition(":")[0]
    match provider:
        case "inworld":
            return await _inworld(config.secrets.inworld_api_key, http)
        case "voxtral":
            url = (config.settings.voice.voxtral_url or DEFAULT_VOXTRAL_URL).rstrip("/")
            return await _http("voxtral", "Voxtral на своём сервере", f"{url}/health", http)
        case "chatgpt":
            return _chatgpt("chatgpt", "Подписка ChatGPT")
        case "openai":
            ok = bool(config.secrets.openai_api_key)
            return Check("openai", ok, "Ключ OpenAI", "Ключ задан" if ok else "Ключ не задан")
        case _:
            return Check(provider, True, name, "Не проверяется")


async def _tts(name: str, config: Config, http: httpx.AsyncClient) -> Check:
    provider = name.partition(":")[0]
    match provider:
        case "inworld":
            return await _inworld(config.secrets.inworld_api_key, http)
        case "voxcpm":
            url = config.settings.tts.voxcpm_url or DEFAULT_VOXCPM_URL
            return await _tcp("voxcpm", "VoxCPM2 на своём сервере", url)
        case "openai":
            ok = bool(config.secrets.openai_api_key)
            return Check("openai", ok, "Ключ OpenAI", "Ключ задан" if ok else "Ключ не задан")
        case _:
            return Check(provider, True, name, "Не проверяется")


async def _inworld(key: str | None, http: httpx.AsyncClient) -> Check:
    """The key and the credits, at no cost. The free voice list is no such check: an account without
    credits still gets it, while every line is refused with 402."""
    if not key:
        return Check("inworld", False, "Inworld", "Ключ не задан")
    auth = {"Authorization": f"Basic {key}"}
    try:
        resp = await http.post(INWORLD_STREAM, json=INWORLD_CHECK, headers=auth)
        status = resp.status_code
        if status == 200 or (status == 404 and "Unknown voice" in resp.text):
            return Check("inworld", True, "Inworld", "Доступен")  # refused for the voice alone
        if status == 402:
            return Check("inworld", False, "Inworld", "Нет кредитов: пополните счёт Inworld")
        if status not in (401, 403) and status < 500:
            # an answer this check does not know (Inworld changed?): the voice list decides, as before
            resp = await http.get(INWORLD_VOICES, params={"filter": "language=ru"}, headers=auth)
            status = resp.status_code
    except httpx.HTTPError as err:
        return Check("inworld", False, "Inworld", f"Не отвечает: {type(err).__name__}")
    if status in (401, 403):
        return Check("inworld", False, "Inworld", "Ключ не подходит")
    ok = status == 200
    return Check("inworld", ok, "Inworld", "Доступен" if ok else f"Ответил {status}")


def _chatgpt(id_: str, title: str) -> Check:
    ok = CHATGPT_TOKENS.is_file()
    return Check(id_, ok, title, "Вход выполнен" if ok else "Нужно войти в ChatGPT")


async def _http(id_: str, title: str, url: str, http: httpx.AsyncClient, any_status: bool = False) -> Check:
    try:
        resp = await http.get(url)
    except httpx.HTTPError as err:
        return Check(id_, False, title, f"Не отвечает: {type(err).__name__}")
    ok = any_status or resp.status_code == 200
    return Check(id_, ok, title, "Доступен" if ok else f"Ответил {resp.status_code}")


async def _tcp(id_: str, title: str, url: str) -> Check:
    parts = urlsplit(url)
    host, port = parts.hostname, parts.port or (443 if parts.scheme in ("wss", "https") else 80)
    if not host:
        return Check(id_, False, title, "Адрес не задан")
    try:
        _, writer = await asyncio.wait_for(asyncio.open_connection(host, port), TIMEOUT_S)
    except (OSError, TimeoutError) as err:
        return Check(id_, False, title, f"Не отвечает: {type(err).__name__}")
    writer.close()
    with contextlib.suppress(OSError):
        await writer.wait_closed()
    return Check(id_, True, title, "Доступен")


def _child(folder: Path, name: str) -> Path | None:
    """Gothic's folders differ in case between installs (Data, DATA)."""
    try:
        return next((p for p in folder.iterdir() if p.name.lower() == name and p.is_dir()), None)
    except OSError:
        return None


def _ou_file(folder: Path) -> Path | None:
    d: Path | None = folder
    for part in ("_work", "data", "scripts", "content", "cutscene"):
        d = _child(d, part) if d is not None else None
    if d is None:
        return None
    return next((p for p in d.iterdir() if p.name.lower() == "ou.bin"), None)


def _cyrillic_share(data: bytes) -> float:
    """cp1251 letters А-я (0xC0-0xFF) among all letters of the line library."""
    cyrillic = sum(data.count(bytes([b])) for b in range(0xC0, 0x100))
    latin = sum(data.count(bytes([b])) for b in (*range(0x41, 0x5B), *range(0x61, 0x7B)))
    return cyrillic / max(cyrillic + latin, 1)


def _names(names: list[str], limit: int = 3) -> str:
    if not names:
        return ""
    shown = ", ".join(names[:limit])
    return f"{shown} и ещё {len(names) - limit}" if len(names) > limit else shown
