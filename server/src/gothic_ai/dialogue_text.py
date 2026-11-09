"""Text shaping, sentence splitting and mood tags for dialogue."""

from __future__ import annotations

import re
from collections.abc import Collection
from dataclasses import dataclass

from . import commands
from .dialogue_constants import REACTION_WORDS_MAX, SVM_EVENTS, SVM_HINT
from .protocol import MOODS, Mood, Snapshot

_MARKDOWN = re.compile(r"[*_#`~]+")
_SPACES = re.compile(r"\s+")
_TOOL_SYNTAX = re.compile(
    r"\b(?:functions?|protoc)\b|to\s*=\s*tools\.|[\"']arguments[\"']|\bcall_id\b|"
    r"</?tool_call\b|\{\s*[\"']name[\"']",
    re.IGNORECASE,
)


def tool_leak(text: str, names: Collection[str]) -> bool:
    """Whether text contains a tool name or a call written out as speech."""
    known = {name.casefold() for name in names}
    return bool(_TOOL_SYNTAX.search(text)) or any(
        word.casefold() in known for word in re.findall(r"\w+", text)
    )


def to_game_text(text: str) -> str:
    """Plain spoken text the game's 8-bit fonts can show (cp1251): no markdown, no emoji."""
    text = _MARKDOWN.sub("", text)
    out = []
    for ch in text:
        try:
            ch.encode("cp1251")
            out.append(ch)
        except UnicodeEncodeError:
            out.append(" ")
    return _SPACES.sub(" ", "".join(out)).strip()


def paragraph_end(text: str) -> int | None:
    """Where the first paragraph of `text` ends: the first blank line after some words; None if none."""
    start = len(text) - len(text.lstrip())
    end = text.find("\n\n", start)
    return end if end > start else None


_LETTERS = re.compile(r"[а-яёa-z]+", re.IGNORECASE)
# what a model's answer may start with that the npc said already ("Хм... Хм, ты прав.")
_INTERJECTION = re.compile(
    r"^\s*(?:(?:(?:хм+|м+)(?:-м+)*|э+)(?:[.,!?…]+|(?=\s))|(?:ну|так|угу|ага)[.,!?…]+)\s*", re.IGNORECASE
)
_FIRST_SENTENCE = re.compile(r"[.!?…]+[\"»)]*\s")
_NOTHING = re.compile(r"^[\s—–\-.…]*$")  # the quick model's "—": nothing to say first
_PURE_SOUNDS = frozenset(
    "хм гм ха ой ли ого ага угу эх ох ах ух тьфу фу ишь ну э м о а что чего".split()
)


def is_pure_sound(text: str) -> bool:
    """Only interjections, including stretched letters ("хммм", "ааа")."""
    words = _LETTERS.findall(text.casefold())
    return bool(words) and all(re.sub(r"(.)\1+", r"\1", word) in _PURE_SOUNDS for word in words)


def first_words_outcome(text: str, previous: str = "") -> str:
    """Filters shared by the game and the trace replay, after `first_words` parsing."""
    if not text:
        return "nothing"
    words = _LETTERS.findall(text.casefold())
    if len(words) > REACTION_WORDS_MAX:
        return "too_long"
    if words == _LETTERS.findall(previous.casefold()):
        return "same"
    return "said"


def strip_said(line: str, said: str) -> str:
    """The first line of an answer without what the npc said at once: the same words again ("Что?! Что?!
    Орки?") or another "Хм..." ("Хм... Хм, ты прав.")."""
    if words := _LETTERS.findall(said):
        again = r"^\s*" + r"[\s,-]*".join(map(re.escape, words)) + r"[.,!?…]+\s*"
        line = re.sub(again, "", line, count=1, flags=re.IGNORECASE)
    rest = _INTERJECTION.sub("", line, count=1)
    return rest[:1].upper() + rest[1:] if rest else rest


def first_words(text: str) -> tuple[str, Mood | None]:
    """The quick model's first words, as said: up to the end of the first sentence (or blank line), the
    mood of its tag, the tags out; "" for its "—" (nothing to say first)."""
    tags = MoodTags()
    pieces = [*tags.feed(text), *tags.flush()]
    mood = next((p.mood for p in pieces if isinstance(p, MoodTag)), None)
    words = "".join(p for p in pieces if isinstance(p, str)).strip()
    if (end := paragraph_end(words)) is not None:
        words = words[:end]
    if (m := _FIRST_SENTENCE.search(words + " ")) is not None:
        words = words[: m.end()].strip()
    if _NOTHING.match(words):
        return "", None
    return words.lstrip("—–- "), mood  # "— Пошли тогда.": the dash of a written reply is not said


def svm_text(text: str) -> str:
    """An SVM line without parenthesized stage directions."""
    return re.sub(r"\([^)]*\)", "", text).strip()


def svm_hint(
    svm: str, text: str, *, target_name: str | None = None, self_target: bool = False,
    hint: str = SVM_HINT,
) -> str:
    event = SVM_EVENTS.get(svm.upper(), {}).get("event", "").format(
        target=f" — {target_name}" if target_name else "",
    )
    if not event:
        hint = hint.replace("Сейчас: {event}. ", "")
    return hint.format(
        event=event, text=svm_text(text) or text.replace("(", "").replace(")", "").strip(),
        to=f"обращаясь к {target_name}" if target_name and not self_target else "вслух",
    )


def line_duration(text: str) -> float:
    """Seconds the game gives a line (mirrors LineLibrary::durationFor in ai_bridge)."""
    return min(max(0.8 + 0.065 * len(text.encode("cp1251", "replace")), 1.5), 12.0)


_FOLLOW_DELAY = re.compile(
    r"\b(?:если|когда|пока|сначала|потом|позже|попозже|завтра|послезавтра|подожди|погоди|"
    r"бы|останусь|остаюсь|никуда|ли|наверное|возможно|может|вряд)\b"
)
_FOLLOW_REFUSAL = re.compile(
    r"\bне\s+(?:иду|пойду|идем|пойдем|пошли|собираюсь|стану|могу|хочу|готов|следую|последую|"
    r"побегу|думаю|уверен)\b"
)
_FOLLOW_REPORTED = re.compile(r"\b(?:сказал|сказали|говорит|говорят|спросил|крикнул)\b")
_FOLLOW_LEADING = re.compile(r"\b(?:за мной|со мной|я поведу|я веду|я отведу|я покажу дорогу)\b")
_FOLLOW_ACCEPTED = re.compile(
    r"(?:^|[.!?;:—–,]\s*)(?:(?:да|ладно|хорошо|тогда|ну)\b[,\s]*)*"
    r"(?:пойдем(?:те)?|пошли|идем(?:те)?)\b|"
    r"\b(?:я\s+)?(?:иду|пойду|следую|отправляюсь)\s+(?:за тобой|с тобой)\b|"
    r"\bя\s+за тобой\b"
)
_FOLLOW_SHORT = re.compile(
    r"^(?:(?:да|ладно|хорошо|ну|ага|угу)\b[\s,.!—–-]*)*(?:я\s+)?(?:иду|пойду)\s*(?:$|[,!.…])"
)
_FOLLOW_LEAD = re.compile(
    r"(?:(?:да|ладно|хорошо|ну|ага|угу|тогда|давай)\b[\s,]*)*"
    r"(?:веди|ведите)(?:\s+(?:уж|же))?\s*[.!…]*"
)


def follow_agreement(heard: str, reply: str, name: str = "") -> bool:
    """An invitation and an unconditional agreement to go, in these two lines specifically.

    A model's incidental "пойдём разберёмся" does not invite itself into the hero's party: the hero
    must have asked it to go. Refusals, conditions, later promises and somebody else's words do not
    commit the body either. Unclear pairs stay the conversational model's responsibility.
    """
    names = frozenset(commands.words(name))
    if not commands.follow_request(heard, names.__contains__):
        return False
    answer = reply.lower().replace("ё", "е").strip(" —–-")
    if (
        _FOLLOW_DELAY.search(answer)
        or _FOLLOW_REFUSAL.search(answer)
        or _FOLLOW_REPORTED.search(answer)
        or _FOLLOW_LEADING.search(answer)
        or re.search(
            r"\b(?:не сейчас|не с тобой|не за тобой|без меня|без тебя|без вас|иди сам|идите сами)\b", answer
        )
        or any(mark in answer for mark in ('"', "«", "»"))
        or re.match(r"нет\b(?!\s+проблем\b)", answer)
    ):
        return False
    sentences = re.split(r"(?<=[.!?…])\s+", answer)
    return any(
        bool(
            _FOLLOW_ACCEPTED.search(line)
            or _FOLLOW_SHORT.match(line)
            or _FOLLOW_LEAD.fullmatch(line)
        )
        for line in sentences
        if not line.endswith("?")
    )


_SENTENCE_END = re.compile(r"(?<=[.!?…])[\"»)]*\s+")
_CLAUSE_END = re.compile(r"(?<=[,;:—–])\s+")
_WORD = re.compile(r"\S+\s+")
FIRST_CLAUSE_MIN = 12  # a first line cut at a comma is at least this long
FIRST_WORDS_MAX = 10  # ...or cut after this many words when there is no punctuation
# a cut never leaves one of these at the end of the first line ("вместе с" | "армией")
_FUNCTION_WORDS = frozenset(
    "а и но да или что чтобы как когда если не же ли бы "
    "в во на с со к ко по о об от до из за для у при без над под".split()
)


class SentenceSplitter:
    """Cuts streamed text into subtitle lines: whole sentences, merged if short, split if too long.
    `first_min_len` lets the first line go out sooner (with TTS it is voiced before it is heard);
    `first_clause` goes further: the first line ends at the first clause (a comma) or after a few
    words, whichever comes first."""

    def __init__(
        self,
        min_len: int = 24,
        max_len: int = 200,
        first_min_len: int | None = None,
        first_clause: bool = False,
    ) -> None:
        self.min_len = min_len
        self.max_len = max_len
        self.first_min_len = 1 if first_clause else min_len if first_min_len is None else first_min_len
        self.first_clause = first_clause
        self._emitted = False
        self._buf = ""

    def _first_cut(self) -> int | None:
        cuts = []
        if m := _SENTENCE_END.search(self._buf):
            cuts.append(m.end())
        for m in _CLAUSE_END.finditer(self._buf):
            if len(self._buf[: m.end()].strip()) >= FIRST_CLAUSE_MIN:
                cuts.append(m.end())
                break
        words = list(_WORD.finditer(self._buf))
        n = FIRST_WORDS_MAX
        while n <= len(words) and words[n - 1].group().strip().lower() in _FUNCTION_WORDS:
            n += 1
        if n <= len(words):
            cuts.append(words[n - 1].end())
        return min(cuts) if cuts else None

    def feed(self, text: str) -> list[str]:
        self._buf += text
        out: list[str] = []
        if self.first_clause and not self._emitted and (cut := self._first_cut()) is not None:
            out.append(self._buf[:cut].strip())
            self._buf = self._buf[cut:]
            self._emitted = True
        start = 0
        pending = ""
        for m in _SENTENCE_END.finditer(self._buf):
            pending += self._buf[start : m.end()]
            start = m.end()
            if len(pending.strip()) >= (self.min_len if self._emitted or out else self.first_min_len):
                out.extend(self._limit(pending))
                pending = ""
        self._buf = pending + self._buf[start:]
        self._emitted = self._emitted or bool(out)
        return out

    def flush(self) -> list[str]:
        rest, self._buf = self._buf, ""
        return self._limit(rest) if rest.strip() else []

    def _limit(self, sentence: str) -> list[str]:
        sentence = sentence.strip()
        parts = []
        while len(sentence) > self.max_len:
            cut = sentence.rfind(", ", 0, self.max_len)
            cut = cut + 1 if cut > self.min_len else sentence.rfind(" ", 0, self.max_len)
            if cut <= 0:
                cut = self.max_len
            parts.append(sentence[:cut].strip())
            sentence = sentence[cut:].strip()
        if sentence:
            parts.append(sentence)
        return parts


# The model starts a reply (or a sentence where the mood changes) with a tag: "[angry] Опять ты?". Russian
# words and near synonyms are taken too: the model sometimes writes them.
MOOD_TAG_MAX = 24  # longer text in square brackets is not a tag
_MOOD_ALIASES: dict[str, Mood] = {
    "calm": "neutral",
    "спокойно": "neutral",
    "спокойствие": "neutral",
    "нейтрально": "neutral",
    "happy": "friendly",
    "smile": "friendly",
    "дружелюбно": "friendly",
    "приветливо": "friendly",
    "радость": "friendly",
    "улыбка": "friendly",
    "anger": "angry",
    "злость": "angry",
    "гнев": "angry",
    "сердито": "angry",
    "раздражение": "angry",
    "враждебно": "hostile",
    "угроза": "hostile",
    "презрение": "hostile",
    "fear": "afraid",
    "scared": "afraid",
    "страх": "afraid",
    "испуг": "afraid",
    "испуганно": "afraid",
    "hurt": "pain",
    "боль": "pain",
}


def mood_of(tag: str) -> Mood | None:
    t = tag.strip().lower()
    for mood in MOODS:
        if t == mood:
            return mood
    return _MOOD_ALIASES.get(t)


@dataclass(frozen=True)
class MoodTag:
    mood: Mood


class MoodTags:
    """Takes mood tags out of the model's streamed text: the text goes on to the chat and the voice, a tag
    becomes the mood of the lines after it. Other short text in square brackets is a stage direction and
    is not said either. A tag split between two pieces of the stream waits for its end."""

    def __init__(self) -> None:
        self._buf = ""

    def feed(self, text: str) -> list[str | MoodTag]:
        self._buf += text
        out: list[str | MoodTag] = []
        while self._buf:
            i = self._buf.find("[")
            if i < 0:
                out.append(self._buf)
                self._buf = ""
                break
            if i > 0:
                out.append(self._buf[:i])
                self._buf = self._buf[i:]
            j = self._buf.find("]")
            if j < 0:
                if len(self._buf) <= MOOD_TAG_MAX + 1:
                    break  # the rest of the tag has not come yet
                out.append("[")  # too long for a tag: plain text
                self._buf = self._buf[1:]
                continue
            tag, self._buf = self._buf[1:j], self._buf[j + 1 :]
            if len(tag) > MOOD_TAG_MAX:
                out.append(f"[{tag}]")
            elif (mood := mood_of(tag)) is not None:
                out.append(MoodTag(mood))
        return out

    def flush(self) -> list[str | MoodTag]:
        rest, self._buf = self._buf, ""
        return [rest] if rest else []


# --- conversation -------------------------------------------------------------------


def game_time(snap: Snapshot | None) -> str | None:
    if snap is None:
        return None
    w = snap.world
    return f"день {w.day + 1}, {w.hour:02d}:{w.minute:02d}"
