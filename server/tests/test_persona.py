from dataclasses import dataclass
from pathlib import Path

import pytest

from gothic_ai import persona as persona_module
from gothic_ai.characters import Card
from gothic_ai.gamedata import ou
from gothic_ai.gamedata.ou import find_ou_file, load_npc_lines, voice_index
from gothic_ai.persona import (
    BRIEF_SAMPLES,
    DEFAULT_VOICE,
    FEMALE_VOICE,
    VOICES,
    PersonaProvider,
)
from gothic_ai.tts import TtsClient
from tests.test_dialogue_text import _binsafe_line
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
def game_dir(tmp_path: Path) -> Path:
    """OU.BIN with Babera's lines (and one of the hero's in her dialog), Edda's, Matteo's and Hilda's."""
    lines = _lines("BABERA", "16", 6) + [("DIA_BABERA_TALK0_15_01", "Что нового?")]
    lines += _lines("EDDA", "17", 6) + _lines("MATTEO", "09", 6) + _lines("HILDA", "17", 6)
    cutscene = tmp_path / "game" / "_work" / "Data" / "Scripts" / "CONTENT" / "CUTSCENE"
    cutscene.mkdir(parents=True)
    data = b"ZenGin Archive\nEND\n" + b"".join(_binsafe_line(name, text) for name, text in lines)
    (cutscene / "OU.BIN").write_bytes(data)
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
    expected = {npc: load_npc_lines(game_dir, npc) for npc in (BABERA, EDDA, TRADER, HILDA)}
    expected_samples = {
        npc: (tuple(persona_module.pick_samples([text for _, text in lines])), voice_index(lines))
        for npc, lines in expected.items()
    }
    reads: list[Path] = []
    parses: list[bytes] = []
    read_bytes = Path.read_bytes
    parse = ou.parse_ou_bin

    def counted_read(path: Path) -> bytes:
        reads.append(path)
        return read_bytes(path)

    def counted_parse(data: bytes) -> dict[str, str]:
        parses.append(data)
        return parse(data)

    monkeypatch.setattr(Path, "read_bytes", counted_read)
    monkeypatch.setattr(ou, "parse_ou_bin", counted_parse)
    personas = PersonaProvider(game_dir, {})
    ou_file = find_ou_file(game_dir)
    assert reads == [ou_file] and len(parses) == 1  # startup, before any live callback
    personas.get(BABERA, "Бабера")
    personas.get("bau_934_babera", "Бабера")
    personas.brief(BABERA, "Бабера")
    personas.get(BABERA, "Бабера-другая")  # another name: another persona, the same lines
    for npc, voice in (
        (BABERA, FEMALE_VOICE), (EDDA, FEMALE_VOICE), (TRADER, DEFAULT_VOICE), (HILDA, VOICES[HILDA])
    ):
        assert personas._original(npc) == expected_samples[npc]
        persona = personas.get(npc, npc)
        assert persona.voice == voice
        assert "\n".join(f"- {text}" for text in expected_samples[npc][0]) in persona.instructions
        personas.brief(npc, npc)
    assert reads == [ou_file] and len(parses) == 1

    # A new provider reads its own startup snapshot, even in the same directory.
    text = "Новая реплика Эдды для следующего запуска."
    assert ou_file is not None
    ou_file.write_bytes(_binsafe_line("DIA_EDDA_NEW_09_00", text))
    later = PersonaProvider(game_dir, {})
    assert reads == [ou_file, ou_file] and len(parses) == 2
    assert later._original(EDDA) == ((text,), "09")
    assert later.get(EDDA, "Эдда").voice == DEFAULT_VOICE
    assert personas._original(EDDA) == expected_samples[EDDA]
    assert reads == [ou_file, ou_file] and len(parses) == 2


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
