import hashlib
import json
import re
import subprocess

import pytest

from gothic_ai.dialogue import (
    REACT_HINT,
    MoodTag,
    MoodTags,
    SentenceSplitter,
    first_words,
    is_pure_sound,
    mood_of,
    paragraph_end,
    strip_said,
    to_game_text,
    tool_leak,
)
from gothic_ai.gamedata import base, ou
from gothic_ai.gamedata.ou import lines_from_messages, npc_dialog_key, npc_lines
from gothic_ai.persona import pick_samples, render_instructions


def test_first_words_hint_has_no_examples_and_asks_for_a_reaction_to_the_point():
    assert REACT_HINT == (
        "Герой только что это сказал. Отреагируй вслух сразу, одной короткой фразой до восьми слов, "
        "как вырвалось бы само. Зацепись за главное в его словах: переспроси это, оцени или покажи, "
        "как ты к этому относишься. Ответ по существу скажешь следом: не начинай его и ничего не обещай. "
        "На приветствие и прощание, или если сразу сказать нечего, напиши только «—»."
    )


@pytest.mark.parametrize(
    "text",
    [*"хм гм ха ой ли ого ага угу эх ох ах ух тьфу фу ишь ну э м о а что чего".split(),
     "Хммм...", "Ааа!", "Ну, что?!", "Ууугу!", "Ой ли?"],
)
def test_pure_sounds_include_stretched_interjections(text):
    assert is_pure_sound(text)


@pytest.mark.parametrize("text", ["Хм, двести золотых?", "Ишь ты!", "", "—", "Твоё невежество!"])
def test_meaningful_first_words_are_not_pure_sounds(text):
    assert not is_pure_sound(text)


@pytest.mark.parametrize(
    "text",
    [
        "give_item",
        "SET_INTENT(goal=follow)",
        "protoc function give_quest?",
        "protoc",
        "FuNcTiOn",
        "FUNCTIONS",
        "functions.give_quest({...})",
        "to=functions.unknown",
        '"arguments": {}',
        "call_id=abc",
        "<tool_call>",
        '{"name": "unknown"}',
    ],
)
def test_tool_leak_recognizes_names_and_call_syntax(text):
    assert tool_leak(text, {"give_item", "set_intent", "give_quest"})


@pytest.mark.parametrize(
    "text",
    [
        "Дай мне функцию подумать.",
        "Это задание как раз по тебе.",
        "Отдам тебе булаву после боя.",
        "Xardas знает больше.",
        "Ладно, пошли.",
        "give_itemized preset_intent give_quests",
        "functional dysfunctional functions_extra",
    ],
)
def test_tool_leak_leaves_ordinary_words_and_name_substrings_alone(text):
    assert not tool_leak(text, {"give_item", "set_intent", "give_quest"})


def test_splitter_emits_whole_sentences_as_they_complete():
    s = SentenceSplitter(min_len=10)
    assert s.feed("Ты вернулся. Хоро") == ["Ты вернулся."]
    # "Хорошо!" is shorter than min_len: held back and merged with what follows
    assert s.feed("шо! Слушай") == []
    assert s.flush() == ["Хорошо! Слушай"]


def test_splitter_merges_short_sentences_and_keeps_order():
    s = SentenceSplitter(min_len=20)
    out = s.feed("Да. Нет. Возможно, когда-нибудь потом. ")
    assert out == ["Да. Нет. Возможно, когда-нибудь потом."]
    assert s.flush() == []


def test_splitter_flushes_tail_without_punctuation():
    s = SentenceSplitter()
    assert s.feed("Иди к лорду Хагену") == []
    assert s.flush() == ["Иди к лорду Хагену"]


def test_splitter_breaks_overlong_sentence_at_commas():
    s = SentenceSplitter(min_len=10, max_len=40)
    parts = s.feed("Драконы пришли с севера, орки идут с востока, паладины заперлись в городе. ")
    assert all(len(p) <= 40 for p in parts)
    assert " ".join(parts) == "Драконы пришли с севера, орки идут с востока, паладины заперлись в городе."


def _run(tags: MoodTags, *pieces: str) -> list[str | MoodTag]:
    out: list[str | MoodTag] = []
    for p in pieces:
        out += tags.feed(p)
    return out + tags.flush()


def test_mood_tags_come_out_of_the_text():
    out = _run(MoodTags(), "[angry] Опять ты? [neutral] Ладно, говори.")
    assert out == [MoodTag("angry"), " Опять ты? ", MoodTag("neutral"), " Ладно, говори."]


def test_mood_tag_split_across_pieces_waits_for_its_end():
    tags = MoodTags()
    assert tags.feed("[ang") == []
    assert tags.feed("ry] Оп") == [MoodTag("angry"), " Оп"]


def test_mood_tag_synonyms_and_stage_directions():
    assert mood_of(" Злость ") == "angry" and mood_of("испуг") == "afraid" and mood_of("HOSTILE") == "hostile"
    assert mood_of("задумчиво") is None
    # an unknown short tag is a stage direction: not said
    assert _run(MoodTags(), "[смеётся] Ха!") == [" Ха!"]


def test_long_brackets_and_unclosed_ones_are_text():
    long = "[" + "очень длинная ремарка, совсем не метка" + "]"
    assert "".join(p for p in _run(MoodTags(), long + " да") if isinstance(p, str)) == long + " да"
    assert _run(MoodTags(), "Цена [пять") == ["Цена ", "[пять"]


def test_game_text_drops_markdown_and_non_cp1251():
    assert to_game_text("**Ксардас** смотрит 🔥 на тебя…  «Иди»") == "Ксардас смотрит на тебя… «Иди»"


_WAV_ENTRY = re.compile(rb"\x01(..)([A-Za-z0-9_]+)\.WAV", re.DOTALL)
_MAX_TEXT = 4096


def parse_ou_bin(data: bytes, encoding: str = "cp1251") -> dict[str, str]:
    """The former production scanner, retained unchanged as an independent legacy oracle."""
    lines: dict[str, str] = {}
    for m in _WAV_ENTRY.finditer(data):
        name = m.group(2).decode("ascii")
        if int.from_bytes(m.group(1), "little") != len(name) + 4:
            continue
        hash_pos = m.start() - 5
        if hash_pos < 0 or data[hash_pos] != 0x12:
            continue
        text = _string_ending_at(data, hash_pos)
        if text is not None:
            lines[name.upper()] = text.decode(encoding, "replace").strip()
    return lines


def _string_ending_at(data: bytes, end: int) -> bytes | None:
    for start in range(end - 3, max(end - 3 - _MAX_TEXT, -1), -1):
        if data[start] == 0x01 and int.from_bytes(data[start + 1 : start + 3], "little") == end - start - 3:
            return data[start + 3 : end]
    return None


def _binsafe_line(name: str, text: str | bytes) -> bytes:
    raw = text.encode("cp1251") if isinstance(text, str) else text
    wav = f"{name}.WAV".encode()
    return (
        b"\x01\x05\x00ATOMB"
        + b"\x01"
        + len(raw).to_bytes(2, "little")
        + raw
        + b"\x12\x06\x00\x00\x00\x01"
        + len(wav).to_bytes(2, "little")
        + wav
    )


def test_parse_ou_bin_reads_subtitles_by_line_name():
    # Block names are independent; this view uses only the audio filename, in raw message order.
    messages = [
        (b"DIA_Xardas_Hello_14_01.WAV", "Первая версия.".encode("cp1251")),
        (b"DIA_Xardas_Hello_15_02.WAV", "Что случилось?".encode("cp1251")),
        (
            b"DIA_Xardas_Hello_14_01.WAV",
            b" \t\xa0" + "Наконец-то.".encode("cp1251") + b"\x98\xa0\r\n",
        ),
        (b"DIA_Xardas_Empty_14_03.WAV", b"\xa0 \t\r\n"),
        (b"DIA_Xardas_Lower_14_04.wav", b"ignored"),
    ]
    data = b"ZenGin Archive\nEND\n" + b"".join(
        _binsafe_line(name[:-4].decode("ascii"), text) if name.endswith(b".WAV") else b""
        for name, text in messages
    )
    lines = parse_ou_bin(data)
    assert lines_from_messages(messages) == lines == {
        "DIA_XARDAS_HELLO_14_01": "Наконец-то.\ufffd",
        "DIA_XARDAS_HELLO_15_02": "Что случилось?",
        "DIA_XARDAS_EMPTY_14_03": "",
    }
    # hero (voice 15) lines are not the npc's
    assert npc_lines(lines, "NONE_100_XARDAS") == [("DIA_XARDAS_HELLO_14_01", "Наконец-то.\ufffd")]


def test_native_text_is_not_reparsed_as_binsafe():
    # Import-time compatibility checks reject these structures; the runtime returns the actual text.
    name = b"DIA_Xardas_Hello_14_01.WAV"
    for raw, scanned in [(b"prefix\x01\x01\x00x", {"DIA_XARDAS_HELLO_14_01": "x"}), (b"x" * 4100, {})]:
        assert parse_ou_bin(_binsafe_line(name[:-4].decode("ascii"), raw)) == scanned
        assert lines_from_messages([(name, raw)]) == {"DIA_XARDAS_HELLO_14_01": raw.decode("cp1251")}


def _ou_export(messages):
    return {
        "format": "gothic-ou-messages",
        "version": 1,
        "source": {"path": "_work/Data/Scripts/CONTENT/CUTSCENE/OU.BIN", "sha256": "a" * 64},
        "native_sha256": "b" * 64,
        "messages": messages,
    }


def _ready_base(tmp_path, monkeypatch, files=None, fingerprint="a" * 64):
    directory = tmp_path / "base"
    directory.mkdir(exist_ok=True)
    entries = []
    for name, raw in (files or {"test.txt": b"payload"}).items():
        file = directory / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(raw)
        entries.append({"path": name, "size": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
    manifest = json.dumps({"format": "gothic-base-files", "version": 1, "files": entries}).encode()
    (directory / "base-files.json").write_bytes(manifest)
    (directory / "base.json").write_text(json.dumps({
        "format": "gothic-base", "version": 2, "fingerprint": fingerprint,
        "manifest": {"path": "base-files.json", "size": len(manifest),
                     "sha256": hashlib.sha256(manifest).hexdigest()},
    }))
    monkeypatch.setenv("OPENGOTHIC_BASE", str(directory))
    monkeypatch.setenv("OPENGOTHIC_BASE_CACHE", str(tmp_path / "cache"))
    return directory


@pytest.mark.parametrize("failure", ["missing", "foreign", "manifest", "size", "sha"])
def test_ready_base_contract_rejects_missing_foreign_or_corrupt_data(tmp_path, monkeypatch, failure):
    directory = _ready_base(tmp_path, monkeypatch)
    game = tmp_path / "game"
    game.mkdir()
    if failure in ("missing", "foreign"):
        monkeypatch.setenv("OPENGOTHIC_BASE", "")
        if failure == "foreign":
            pointer = tmp_path / "cache/current" / f"{hashlib.sha256(str(game).encode()).hexdigest()}.json"
            pointer.parent.mkdir(parents=True)
            pointer.write_text(json.dumps({
                "version": 1, "gothic": str(tmp_path / "other-game"),
                "base": {"path": str(directory), "fingerprint": "a" * 64},
            }))
    elif failure == "manifest":
        manifest = directory / "base-files.json"
        manifest.write_bytes(manifest.read_bytes().replace(b'"version": 1', b'"version": 2'))
    else:
        (directory / "test.txt").write_bytes(b"short" if failure == "size" else b"changed")
    with pytest.raises(RuntimeError, match="base"):
        with base.resolve(game) as selected:
            selected.read("test.txt")


def test_ready_base_pointer_works_without_installation_and_explicit_base_wins(tmp_path, monkeypatch):
    directory = _ready_base(tmp_path, monkeypatch)
    game = tmp_path / "game"
    game.mkdir()
    pointer = tmp_path / "cache/current" / f"{hashlib.sha256(str(game).encode()).hexdigest()}.json"
    pointer.parent.mkdir(parents=True)
    pointer.write_text(json.dumps({
        "version": 1, "gothic": str(game),
        "base": {"path": str(directory), "fingerprint": "a" * 64},
    }))
    monkeypatch.setenv("OPENGOTHIC_BASE", "")
    with base.resolve(game) as selected:
        assert selected.path == directory and selected.fingerprint == "a" * 64
        assert selected.read("test.txt") == b"payload"
    pointer.write_text("invalid")
    monkeypatch.setenv("OPENGOTHIC_BASE", str(directory))
    with base.resolve(game) as selected:
        assert selected.read("test.txt") == b"payload"


@pytest.fixture
def native_ou(tmp_path, monkeypatch):
    tool = tmp_path / "storygraph"
    tool.touch()
    monkeypatch.setenv("STORYGRAPH_TOOL", str(tool))
    _ready_base(tmp_path, monkeypatch)
    (tmp_path / "game").mkdir()
    return tmp_path / "game", tool


def test_native_ou_boundary_preserves_raw_bytes_and_empty_source(native_ou, monkeypatch):
    game, tool = native_ou
    raw = b"\x00\x98\xa0\xff"
    exports = [_ou_export([["DIA_X_14_01.WAV", raw.decode("latin1")]]), _ou_export([])]

    def read(cmd, **kwargs):
        assert cmd == [str(tool), str(game.parent / "base"), "--ou-messages"]
        assert kwargs["env"]["OPENGOTHIC_BASE"] == str(game.parent / "base")
        assert kwargs["check"] and kwargs["capture_output"] and kwargs["timeout"] == 1800
        return subprocess.CompletedProcess(cmd, 0, json.dumps(exports.pop(0)).encode(), b"progress on stderr")

    monkeypatch.setattr(ou.subprocess, "run", read)
    assert ou.native_messages(game) == [(b"DIA_X_14_01.WAV", raw)]
    assert ou.load_npc_lines(game, "NONE_100_XARDAS") == []


def test_native_ou_requires_ready_base_and_tool(tmp_path, monkeypatch):
    tool = tmp_path / "missing-storygraph"
    monkeypatch.setenv("STORYGRAPH_TOOL", str(tool))
    _ready_base(tmp_path, monkeypatch)
    with pytest.raises(RuntimeError, match="cmake --build.*STORYGRAPH_TOOL"):
        ou.load_npc_lines(tmp_path, "NONE_100_XARDAS")


@pytest.mark.parametrize("failure", ["child", "timeout", "json", "version", "unicode", "null"])
def test_native_ou_failures_are_explicit(native_ou, monkeypatch, failure):
    game, _ = native_ou

    def fail(cmd, **kwargs):
        if failure == "child":
            raise subprocess.CalledProcessError(1, cmd, stderr=b"broken native record")
        if failure == "timeout":
            raise subprocess.TimeoutExpired(cmd, 1800, stderr=b"import timeout")
        data = _ou_export([])
        if failure == "version":
            data["version"] = 2
        elif failure == "unicode":
            data["messages"] = [["DIA_X_14_01.WAV", "\u0100"]]
        elif failure == "null":
            data.update(source=None, native_sha256=None, messages=None)
        output = b"not JSON" if failure == "json" else json.dumps(data).encode()
        return subprocess.CompletedProcess(cmd, 0, output, b"native details")

    monkeypatch.setattr(ou.subprocess, "run", fail)
    match = {"child": "broken native record", "timeout": "import timeout", "null": "no server OU"}
    with pytest.raises(RuntimeError, match=match.get(failure, "invalid JSON.*")):
        ou.load_npc_lines(game, "NONE_100_XARDAS")


def test_npc_dialog_key():
    assert npc_dialog_key("NONE_100_XARDAS") == "XARDAS"
    assert npc_dialog_key("VLK_408_Jora") == "JORA"
    assert npc_dialog_key("PC_Hero") == "HERO"


def test_persona_prompt_contains_samples_and_is_deterministic():
    samples = pick_samples([f"Реплика номер {i}, достаточно длинная для образца." for i in range(100)], n=5)
    assert len(samples) == 5
    a = render_instructions("NONE_100_XARDAS", "Ксардас", samples)
    assert a == render_instructions("NONE_100_XARDAS", "Ксардас", samples)
    assert "некромант" in a and samples[0] in a


def test_first_line_can_go_out_alone():
    text = "Ты вернулся. Хорошо. Времени у нас мало, так что слушай внимательно. Драконы уже здесь."
    default = SentenceSplitter()
    assert default.feed(text + " ") == [
        "Ты вернулся. Хорошо. Времени у нас мало, так что слушай внимательно."
    ]
    voiced = SentenceSplitter(first_min_len=1)
    lines = voiced.feed(text + " ") + voiced.flush()
    assert lines == [
        "Ты вернулся.",
        "Хорошо. Времени у нас мало, так что слушай внимательно.",
        "Драконы уже здесь.",
    ]


def test_first_line_at_the_first_clause():
    def lines(text: str) -> list[str]:
        sp = SentenceSplitter(first_clause=True)
        out = []
        for i in range(0, len(text), 3):  # streamed in small deltas
            out += sp.feed(text[i : i + 3])
        return out + sp.flush()

    assert lines("Ты вернулся из храма, и это хорошо. Теперь слушай меня внимательно.") == [
        "Ты вернулся из храма,",
        "и это хорошо. Теперь слушай меня внимательно.",
    ]
    # a short first sentence goes alone, a comma too early does not cut
    assert lines("Ты вернулся. Хорошо, теперь слушай меня внимательно.")[0] == "Ты вернулся."
    assert lines("Да, конечно. Драконы уже здесь, и их много.")[0] == "Да, конечно."
    # no punctuation: cut after a few words, never right after a preposition
    first = lines("Когда ты пойдёшь в город через северные ворота мимо стражи найди там Лестера.")[0]
    assert first == "Когда ты пойдёшь в город через северные ворота мимо стражи"
    first = lines("Последний крик Спящего призвал их вместе со всей армией тьмы на этот остров.")[0]
    assert first == "Последний крик Спящего призвал их вместе со всей армией тьмы"
    first = lines("Я долго ждал тебя здесь и думал о том что делать с этим.")[0]
    assert first == "Я долго ждал тебя здесь и думал о том что делать"  # "с" is not left at the end


def test_a_quick_line_ends_at_its_first_blank_line():
    assert paragraph_end("[hostile] Держи его!\n\nset_intent(goal=fight)") == len("[hostile] Держи его!")
    assert paragraph_end("\n\nДержи его!") is None  # blank lines before any words do not end it
    assert paragraph_end("Держи его!\nИ живо!") is None


def test_the_answer_does_not_say_the_first_words_again():
    assert strip_said("Хм... Ты прав.", "Хм...") == "Ты прав."
    assert strip_said("Что?! Орки напали?", "Что?!") == "Орки напали?"
    assert strip_said("Что? Орки?", "Что?!") == "Орки?"
    assert strip_said("Ну-ну. Так уж и голыми?", "Ну-ну...") == "Так уж и голыми?"
    assert strip_said("Хм, ну ладно.", "Тьфу!") == "Ну ладно."  # an interjection after any first words
    assert strip_said("М-м... Пожалуй.", "Хм...") == "Пожалуй."
    assert strip_said("Так, слушай.", "Так...") == "Слушай."
    assert strip_said("Так вот оно что.", "Так...") == "Так вот оно что."  # not an interjection here
    assert strip_said("Что ты сказал?", "Что?!") == "Что ты сказал?"
    assert strip_said("Хмурый сегодня день.", "Хм...") == "Хмурый сегодня день."
    assert strip_said("Хм.", "Хм.") == ""


def test_the_quick_models_first_words_are_its_first_sentence_without_tags():
    assert first_words("[afraid] Что?! Лестер мёртв?") == ("Что?!", "afraid")
    assert first_words("[thoughtful] Хм... Дай подумать") == ("Хм...", None)  # not a mood: only dropped
    assert first_words("Тьфу, ну и наглец!\n\nset_intent(...)") == ("Тьфу, ну и наглец!", None)
    assert first_words("Ишь ты") == ("Ишь ты", None)  # no end: what it wrote
    assert first_words("[neutral] —") == ("", None)  # nothing to say first
    assert first_words("— Пошли тогда.") == ("Пошли тогда.", None)
    assert first_words("") == ("", None)
