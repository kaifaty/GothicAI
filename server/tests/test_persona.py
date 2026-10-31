import hashlib
import json
import threading
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace

import pytest

from gothic_ai import persona as persona_module
from gothic_ai.characters import Card
from gothic_ai.gamedata import ou
from gothic_ai.gamedata.ou import voice_index
from gothic_ai.persona import (
    BRIEF_SAMPLES,
    DEFAULT_VOICE,
    FEMALE_VOICE,
    VOICES,
    PersonaProvider,
)
from gothic_ai.tts import TtsClient
from tests.test_tts import fake_tts

LOBART, HILDA, MALETH, VINO, ANDRE = (
    "BAU_950_LOBART",
    "BAU_951_HILDA",
    "BAU_954_MALETH",
    "BAU_952_VINO",
    "MIL_311_ANDRE",
)
# women (voices 16 and 17) and a man (09) without voices of their own
BABERA, EDDA, TRADER = "BAU_934_BABERA", "VLK_471_EDDA", "VLK_416_MATTEO"


@dataclass(frozen=True)
class VoicedCard(Card):
    """A card as the cards pass will write it (step 2b): with a voice of its own."""

    voice: str = ""


def _card(npc: str, name: str, **extra) -> Card:
    ru = f"Ты — {name}, торговец с рыночной площади. Ты гордый и прижимистый."
    scales = {"greed": 5, "honesty": 3, "courage": 2, "talk": 3, "pushy": 2, "strangers": 2}
    cls = VoicedCard if "voice" in extra else Card
    return cls(npc, name, ru, "A proud, tight-fisted market trader.", **scales, **extra)


def _lines(key: str, voice: str, n: int) -> list[tuple[str, str]]:
    text = "Реплика {} номер {}, длинная, как в игре бывает."
    return [(f"DIA_{key}_TALK{i}_{voice}_{i:02d}", text.format(key.lower(), i)) for i in range(n)]


@pytest.fixture
def game_dir(tmp_path: Path, monkeypatch) -> Path:
    """Native OU messages with Babera's, Edda's, Matteo's and Hilda's lines, without installation files."""
    lines = _lines("BABERA", "16", 6) + [("DIA_BABERA_TALK0_15_01", "Что нового?")]
    lines += _lines("EDDA", "17", 6) + _lines("MATTEO", "09", 6) + _lines("HILDA", "17", 6)
    (tmp_path / "game").mkdir()
    messages = [(f"{name}.WAV".encode("ascii"), text.encode("cp1251")) for name, text in lines]
    monkeypatch.setattr(ou, "native_messages", lambda game: messages)
    return tmp_path / "game"


def test_the_scene_npcs_have_voices_of_their_own():
    personas = PersonaProvider(None, {})
    voices = {npc: personas.get(npc, npc).voice for npc in (LOBART, HILDA, MALETH, VINO, ANDRE)}
    assert voices == {npc: VOICES[npc] for npc in voices}
    assert "woman" in voices[HILDA] and "woman" not in voices[LOBART]
    # one original actor each: told apart by age and timbre
    assert len(set(voices.values())) == 5 and DEFAULT_VOICE not in voices.values()
    assert personas.get("vlk_1_nobody", "Никто").voice == DEFAULT_VOICE  # no game lines: a man's as before


def test_voice_index_is_the_majority():
    lines = _lines("HILDA", "17", 3) + _lines("HILDA", "05", 2)  # Lobart chimes in now and then
    assert voice_index(lines) == "17"
    assert voice_index([("DIA_HILDA_HALLO", "без номера"), ("OUTRO_X_Y", "чужое")]) is None
    assert voice_index([]) is None


def test_the_voice_order(game_dir):
    cards = {
        BABERA: _card(BABERA, "Бабера"),
        TRADER: _card(TRADER, "Маттео", voice="A proud trader with a nasal voice"),
        HILDA: _card(HILDA, "Хильда", voice="A card's voice for Hilda"),
    }
    personas = PersonaProvider(game_dir, cards)
    assert personas.get(HILDA, "Хильда").voice == VOICES[HILDA]  # the hand-written one first
    assert personas.get(TRADER, "Маттео").voice == "A proud trader with a nasal voice"  # then the card's
    assert personas.get(BABERA, "Бабера").voice == FEMALE_VOICE  # then the original actor's: a woman
    assert personas.get(EDDA, "Эдда").voice == FEMALE_VOICE
    assert personas.get("VLK_1_NOBODY", "Никто").voice == DEFAULT_VOICE
    # the card still makes the persona, as before
    p = personas.get(TRADER, "Маттео")
    assert cards[TRADER].ru in p.instructions and p.traits == "A proud, tight-fisted market trader."
    assert "Реплика matteo номер" in p.instructions  # with its own lines


def test_the_game_lines_are_read_once(game_dir, monkeypatch):
    calls: list[Path] = []
    load = persona_module.game_lines

    def counted(path: Path) -> dict[str, str] | None:
        calls.append(path)
        return load(path)

    monkeypatch.setattr(persona_module, "game_lines", counted)
    personas = PersonaProvider(game_dir, {})
    personas.warm()  # the server reads them ahead, in a thread (cli.py)
    personas.get(BABERA, "Бабера")
    personas.get("bau_934_babera", "Бабера")
    personas.brief(BABERA, "Бабера")
    personas.get(BABERA, "Бабера-другая")  # another name: another persona, the same lines
    personas.get(EDDA, "Эдда")  # another npc: the same reading of the game
    assert calls == [game_dir]


def test_get_waits_for_the_first_warm_read(game_dir, monkeypatch):
    loading, resume, requesting, warmed = (threading.Event() for _ in range(4))
    load = persona_module.game_lines

    def blocked(path):
        loading.set()
        if not resume.wait(2.0):
            raise TimeoutError("first OU read was not released")
        return load(path)

    monkeypatch.setattr(persona_module, "game_lines", blocked)
    personas = PersonaProvider(game_dir, {})
    result = []

    def warm():
        personas.warm()
        warmed.set()

    def get():
        requesting.set()
        result.append(personas.get(BABERA, "Babera"))

    warmer = threading.Thread(target=warm, daemon=True)
    reader = threading.Thread(target=get, daemon=True)
    warmer.start()
    try:
        assert loading.wait(1.0)
        reader.start()
        assert requesting.wait(1.0)
        reader.join(0.1)
        assert reader.is_alive(), "get returned before the first OU read finished"
    finally:
        resume.set()
        warmer.join(2.0)
        if reader.ident is not None:
            reader.join(2.0)
    assert warmed.is_set() and not warmer.is_alive() and not reader.is_alive()
    assert len(result) == 1
    assert "Реплика babera номер" in result[0].instructions
    assert result[0].voice == FEMALE_VOICE


def test_missing_base_retries_without_breaking_cached_personas(tmp_path, monkeypatch, caplog):
    game, selected, cache = tmp_path / "game", tmp_path / "base", tmp_path / "cache"
    game.mkdir()
    monkeypatch.setenv("OPENGOTHIC_BASE", "")
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", str(cache))
    monkeypatch.setenv("STORYGRAPH_TOOL", __file__)
    clock = SimpleNamespace(now=100.0)
    monkeypatch.setattr(persona_module.time, "monotonic", lambda: clock.now)
    load, attempts, exports = persona_module.game_lines, [], []
    thread_class, workers = threading.Thread, []
    exporting, resume = threading.Event(), threading.Event()

    def worker(**kwargs):
        thread = thread_class(**kwargs)
        workers.append(thread)
        return thread

    def counted(path):
        attempts.append(clock.now)
        return load(path)

    text = "A recovered original speech line for the character."

    def export(*args, **kwargs):
        exports.append(args)
        exporting.set()
        assert resume.wait(2.0), "get/brief waited for the background OU export"
        return SimpleNamespace(stdout=json.dumps({
            "format": "gothic-ou-messages", "version": 1,
            "source": {"path": "original-ou", "sha256": "b" * 64},
            "native_sha256": "c" * 64,
            "messages": [["DIA_BABERA_TEST_16_00.WAV", text]],
        }).encode(), stderr=b"")

    monkeypatch.setattr(persona_module, "game_lines", counted)
    monkeypatch.setattr(persona_module.threading, "Thread", worker)
    monkeypatch.setattr(ou.subprocess, "run", export)
    personas = PersonaProvider(game, {})
    personas.warm()
    first = personas.get(BABERA, "Babera")
    assert personas._original(BABERA) == ((), None)
    assert text not in first.instructions and text not in personas.brief(BABERA, "Babera")
    clock.now = 129.9
    assert personas.get(BABERA, "Babera") is first
    clock.now = 130.0
    assert text not in personas.brief(BABERA, "Babera")
    workers[-1].join(2.0)
    assert not workers[-1].is_alive()
    assert attempts == [100.0, 130.0] and exports == []
    assert len(caplog.records) == 1
    assert "no ready base" in caplog.text and "run the game once with the installation" in caplog.text

    selected.mkdir()
    manifest = json.dumps({"format": "gothic-base-files", "version": 1, "files": []}).encode()
    (selected / "base-files.json").write_bytes(manifest)
    (selected / "base.json").write_text(json.dumps({
        "format": "gothic-base", "version": 2, "fingerprint": "a" * 64,
        "manifest": {
            "path": "base-files.json", "size": len(manifest),
            "sha256": hashlib.sha256(manifest).hexdigest(),
        },
    }))
    pointer = cache / "current" / f"{hashlib.sha256(str(game.resolve()).encode()).hexdigest()}.json"
    pointer.parent.mkdir(parents=True)
    pointer.write_text(json.dumps({
        "version": 1, "gothic": str(game.resolve()),
        "base": {"path": str(selected), "fingerprint": "a" * 64},
    }))
    clock.now = 159.9
    assert personas.get(BABERA, "Babera") is first
    assert attempts == [100.0, 130.0]
    clock.now = 160.0
    try:
        assert personas.get(BABERA, "Babera") is first
        assert exporting.wait(1.0)
        assert text not in personas.brief(BABERA, "Babera")
        assert personas.get(BABERA, "Babera") is first
    finally:
        resume.set()
        workers[-1].join(2.0)
    assert not workers[-1].is_alive()
    recovered = personas.get(BABERA, "Babera")
    assert text in recovered.instructions and recovered.voice == FEMALE_VOICE
    assert text in personas.brief(BABERA, "Babera")
    clock.now = 200.0
    assert personas.get(BABERA, "Babera") is recovered
    assert attempts == [100.0, 130.0, 160.0] and len(exports) == 1
    assert len(caplog.records) == 1


def test_brief_labels_the_card_as_its_own(game_dir):
    cards = {TRADER: _card(TRADER, "Маттео"), BABERA: _card(BABERA, "Бабера")}
    personas = PersonaProvider(game_dir, cards)
    brief = personas.brief(TRADER, "Маттео").split("\n")
    assert brief[0] == "Карточка Маттео (написана для него самого: «ты» — это Маттео):"
    assert brief[1] == cards[TRADER].ru and brief[2] == cards[TRADER].traits_ru()
    assert brief[3] == "Так Маттео говорит в игре:"
    samples = brief[4:]
    assert len(samples) == BRIEF_SAMPLES and all(s.startswith("- Реплика matteo номер") for s in samples)
    assert personas.brief(TRADER, "Маттео") == "\n".join(brief)  # the same every time: the prompt caches

    assert personas.brief(BABERA, "Бабера").startswith("Карточка Бабера (написана для неё самой: «ты» —")
    assert "Что нового?" not in personas.brief(BABERA, "Бабера")  # not the hero's line
    # no card, no game: its name only; a hand-written biography goes first
    assert PersonaProvider(None, {}).brief(VINO, "Вино").split("\n") == [
        "Карточка Вино (написана для него самого: «ты» — это Вино):",
        "Ты — Вино, житель острова Хоринис.",
    ]
    assert "некромант" in PersonaProvider(None, {}).brief("NONE_100_XARDAS", "Ксардас")


async def test_the_tts_gets_each_npcs_own_voice(game_dir):
    personas = PersonaProvider(game_dir, {})
    async with fake_tts() as (url, requests):
        tts = TtsClient(url)
        for npc, name in ((LOBART, "Лобарт"), (HILDA, "Хильда"), (BABERA, "Бабера")):
            p = personas.get(npc, name)
            await tts.speak("Ну?", p.npc, p.voice)

    voices = {r["voice"]["id"]: r["voice"]["description"] for r in requests}
    assert list(voices) == [LOBART, HILDA, BABERA]  # the voice is the npc's symbol
    assert "woman" in voices[HILDA] and voices[HILDA] != voices[LOBART]
    assert voices[BABERA] == FEMALE_VOICE
