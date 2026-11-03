"""Write one hero-known ending for the current save branch and return its final card."""

from __future__ import annotations

import asyncio
import json
import logging
import re
from pathlib import Path

from pydantic import BaseModel, ConfigDict, ValidationError

from .chronicle import Chronicle, Fact
from .fates import _feats
from .feat_art import DEFAULT_ROOT
from .feats import CACHE_KEY, _foreign_names, _names, hero_view
from .game import GameError, GameLink
from .llm import LLMBackend, Message, Tool, ToolCall
from .protocol import ChronicleFeat, FateCardCmd, FateEnd, GameMessage

log = logging.getLogger(__name__)

FALLBACK = {
    "death": ("Конец пути", "Здесь путь героя оборвался."),
    "wrath": ("Вечная служба", "Ксардас не стерпел дерзости, и герой стал его слугой."),
    "finale": ("Путь пройден", "Герой одолел зло и покинул эти земли."),
}
INSTRUCTIONS = (
    "Ты летописец. Дай итог всей судьбы героя по показанным сведениям: как он прошёл этот путь, "
    "а не только как погиб или чем кончилась игра. Ничего не придумывай и не используй знания, "
    "которых нет у героя. Название судьбы — до четырёх слов. Текст — одна-две фразы до 220 знаков, "
    "в прошедшем времени и третьем лице: «герой», «он». Выбери до трёх лучших достижений этой "
    "ветки по их id, можно выбрать меньше. Ответ — ровно один вызов write_fate."
)
WRITE_FATE = Tool(
    "write_fate", "Записать итог судьбы героя и выбрать лучшие достижения.",
    {
        "type": "object",
        "properties": {
            "title": {"type": "string"}, "text": {"type": "string"},
            "best": {"type": "array", "items": {"type": "string"}, "maxItems": 3},
        },
        "required": ["title", "text", "best"], "additionalProperties": False,
    },
)


class Fate(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True)

    title: str
    text: str
    best: list[str]


def canon(fate: Fate, feats: list[ChronicleFeat], known: set[str], allowed: set[str]) -> list[str]:
    errors = []
    if not fate.title.strip() or len(fate.title.split()) > 4:
        errors.append("The title must contain one to four words")
    if not fate.text.strip() or len(fate.text) > 220:
        errors.append("The text must contain one or two sentences up to 220 characters")
    words = fate.title + " " + fate.text
    if any(c in words for c in "\r\n\u2028\u2029"):
        errors.append("The title and text must each be a single line")
    ids = {feat.id for feat in feats}
    if len(fate.best) > 3 or len(set(fate.best)) != len(fate.best) or any(i not in ids for i in fate.best):
        errors.append("Select at most three distinct feat ids from this branch")
    if re.search(
        r"\b(?:я|меня|мне|мной|мною|мо[йяёиею]|мо[её][мю]|моего|моему|моим|моих|моими|моей|"
        r"ты|тебя|тебе|тобой|тобою|тво[йяёиею]|тво[её][мю]|твоего|твоему|твоим|твоих|твоими|твоей|"
        r"мы|нас|нам|нами|наш\w*|вы|вас|вам|вами|ваш\w*|i|me|my|mine|we|us|our|ours|you|your|yours)\b",
        words, flags=re.IGNORECASE,
    ):
        errors.append("Write in the third person, without first or second person")
    return errors + _foreign_names(words, allowed, known)


class FateCards:
    def __init__(
        self, link: GameLink, chronicle: Chronicle, llm: LLMBackend, *, enabled: bool = True,
        art_root: Path = DEFAULT_ROOT,
    ) -> None:
        self.link, self.chronicle, self.llm = link, chronicle, llm
        self.enabled, self.art_root = enabled, art_root
        self._pending: dict[str, asyncio.Task[Fact]] = {}
        self._sending: set[asyncio.Task[None]] = set()
        link.listeners.append(self._on_message)

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, FateEnd):
            # Capture the branch now; a load must never attach this ending to its new branch.
            task = asyncio.create_task(self._answer(message, self.chronicle.branch))
            self._sending.add(task)
            task.add_done_callback(self._sending.discard)

    def _entries(self, branch: str | None) -> list[tuple[int, Fact]]:
        if branch is None:
            return []
        return [(seq, json.loads(raw)) for seq, raw in self.chronicle.memory.facts_since(branch, 0)]

    def _deeds(self, entries: list[tuple[int, Fact]]) -> list[ChronicleFeat]:
        return [feat for _, _, feat in _feats(
            [(seq, 0, fact) for seq, fact in entries], self.art_root, set(),
        )]

    async def _answer(self, message: FateEnd, branch: str | None) -> None:
        response = FateCardCmd(req=message.req, off=not self.enabled)
        if self.enabled:
            deeds = []
            chapter, day = 1, 0
            try:
                entries = self._entries(branch)
                deeds = self._deeds(entries)
                if branch is not None:
                    chapter, day = self.chronicle.memory.fact_clock(
                        branch, self.chronicle.memory.head(branch),
                    )
                ending = next((f for _, f in entries if f.get("fact") == "fate_end"), None)
                if ending is None:
                    if branch is None:
                        ending = self._fallback(message, deeds, chapter, day)
                    else:
                        if branch not in self._pending:
                            self._pending[branch] = asyncio.create_task(
                                self._write(message, branch, entries, deeds, chapter, day),
                            )
                        work = self._pending[branch]
                        try:
                            ending = await asyncio.shield(work)
                        finally:
                            if work.done() and self._pending.get(branch) is work:
                                del self._pending[branch]
                response = self._card(message.req, ending, deeds)
            except Exception:
                log.exception("fate card: request failed")
                ending = self._fallback(message, deeds, chapter, day)
                response = self._card(message.req, ending, deeds)
        try:
            await self.link.send(response)
        except GameError:
            pass  # A repeated request after reconnect reads the recorded ending.

    @staticmethod
    def _fallback(message: FateEnd, deeds: list[ChronicleFeat], chapter: int, day: int) -> Fact:
        title, text = FALLBACK[message.kind]
        return {
            "fact": "fate_end", "kind": message.kind, "killer": message.killer, "name": message.name,
            "title": title, "text": text, "best": [f.id for f in deeds[-3:]],
            "chapter": chapter, "day": day, "fallback": True,
        }

    @staticmethod
    def _card(req: str, ending: Fact, deeds: list[ChronicleFeat]) -> FateCardCmd:
        return FateCardCmd(
            req=req, title=ending["title"], text=ending["text"],
            feats=[feat for feat in deeds if feat.id in ending["best"]][:3],
            chapter=ending["chapter"], day=ending["day"], count=len(deeds),
        )

    async def _write(
        self, message: FateEnd, branch: str, entries: list[tuple[int, Fact]],
        deeds: list[ChronicleFeat], chapter: int, day: int,
    ) -> Fact:
        ending = self._fallback(message, deeds, chapter, day)
        try:
            # One budget includes the clock query and both model attempts.
            async with asyncio.timeout(20):
                try:
                    state = await self.link.story_state(vars=["KAPITEL"], timeout=1)
                    if self.chronicle.branch == branch:
                        ending["chapter"] = int(state.get("vars", {}).get("KAPITEL") or chapter)
                        ending["day"] = int(state.get("day", day))
                except (GameError, TimeoutError):
                    pass
                if self.llm.name not in {"none", "off"}:
                    fate = await self._propose(message, entries, deeds, ending["chapter"], ending["day"])
                    if fate is not None:
                        ending.update(fate.model_dump(), fallback=False)
        except Exception:
            log.exception("fate card: using fallback ending")
        if self.chronicle.branch == branch:
            self.chronicle.record(ending)
        else:
            self.chronicle.memory.add_fact(branch, json.dumps(ending, ensure_ascii=False))
        return ending

    async def _propose(
        self, message: FateEnd, entries: list[tuple[int, Fact]], deeds: list[ChronicleFeat],
        chapter: int, day: int,
    ) -> Fate | None:
        visible = [(seq, view) for seq, fact in entries if (view := hero_view(seq, fact)) is not None]
        shown = {f["feat"] for _, f in entries if f.get("fact") == "feat_epilogue_shown"}
        revealed = [(seq, f) for seq, f in entries
                    if f.get("fact") == "feat_epilogue" and f["feat"] in shown]
        allowed = _names(visible + revealed) | {message.name.casefold()}
        brief = json.dumps({
            "kind": message.kind, "name": message.name, "chapter": chapter, "day": day,
            "feats": [{key: f.model_dump()[key] for key in ("id", "title", "text", "epilogue")}
                      for f in deeds],
            "facts": [f for _, f in visible[-40:]],
        }, ensure_ascii=False)
        messages = [Message("user", brief)]
        for _ in range(2):
            calls = []
            async for event in self.llm.stream(
                instructions=INSTRUCTIONS, messages=messages, tools=[WRITE_FATE], cache_key=CACHE_KEY,
            ):
                if isinstance(event, ToolCall) and event.name == WRITE_FATE.name:
                    calls.append(event)
            raw = calls[0].arguments if len(calls) == 1 else {}
            try:
                fate = Fate.model_validate(raw)
                errors = canon(fate, deeds, _names(entries), allowed)
                if not errors:
                    return fate
            except ValidationError:
                errors = ["Invalid write_fate fields"]
            messages.append(Message(
                "user", "Предложение: " + json.dumps(raw, ensure_ascii=False)
                + "\nОтклонено:\n" + "\n".join(errors) + "\nИсправь его.",
            ))
        return None

    async def close(self) -> None:
        self.link.listeners.remove(self._on_message)
        tasks = {*self._sending, *self._pending.values()}
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
