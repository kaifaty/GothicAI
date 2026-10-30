"""The director's hidden line conditions, decided only by the save branch's facts."""

from __future__ import annotations

import asyncio
from collections.abc import Awaitable, Callable, Iterable
from uuid import uuid4

from .chronicle import Chronicle, Fact
from .protocol import GameMessage, Loading, Session
from .questbook import QuestBook
from .social import Social, _chance

KINDS = ("no_blood", "no_reward", "secret_kept", "mercy")
CLOSED = {"done", "failed", "obsolete", "closed"}
TERMINAL = {f"story_quest_{state}" for state in CLOSED}
WATCHED = TERMINAL | {"challenge", "died", "quest_outcome", "lie", "found_out", "caught", "surrender"}


def line_people(book: QuestBook, quests: Iterable[str], giver: str, characters: Iterable[str]) -> list[str]:
    """Freeze the line's people, including roles that have no quest goal of their own."""
    ids = set(quests)
    people = {giver, *characters}
    for id in ids:
        quest = book.all()[id]
        people.add(quest.giver)
        for stage in quest.stages:
            for goal in stage.goals:
                people.update((goal.npc, *goal.npcs))
    for chain in book.chronicle.facts("chain"):
        if ids.intersection(chain.get("quests", [])):
            for members in chain.get("roles", {}).values():
                people.update(members)
    return sorted({npc.upper() for npc in people if npc})


class Challenges:
    def __init__(
        self, chronicle: Chronicle, book: QuestBook, *, social: Callable[[], Awaitable[Social]],
    ) -> None:
        self.chronicle, self.book = chronicle, book
        self.social = social
        self._pending: asyncio.Handle | None = None
        self._hints: set[asyncio.Task[None]] = set()
        self._loading = False
        chronicle.on_entry.append(self._on_entry)
        chronicle.link.listeners.append(self._on_message)
        self._schedule()

    def close(self) -> None:
        for task in self._hints:
            task.cancel()
        if self._pending is not None:
            self._pending.cancel()
            self._pending = None
        self.chronicle.on_entry.remove(self._on_entry)
        self.chronicle.link.listeners.remove(self._on_message)

    def _on_entry(self, seq: int, fact: Fact) -> None:
        if not self._loading and fact.get("fact") == "challenge":
            task = asyncio.create_task(self._hint(seq, fact))
            self._hints.add(task)
            task.add_done_callback(self._hints.discard)
        if not self._loading and fact.get("fact") in WATCHED:
            self._schedule()

    async def _hint(self, seq: int, challenge: Fact) -> None:
        if _chance("challenge_hint", str(seq)) >= 0.5:
            return
        if any(f.get("challenge") == seq for f in self.chronicle.facts("challenge_hint")):
            return
        line = next((f for n, f in self.chronicle.entries("line") if n == challenge["line"]), None)
        if line is None or not (giver := line.get("giver")):
            return
        candidates = sorted(set(challenge["people"]) - {giver, "PLAYER"})
        if not candidates:
            return
        branch = self.chronicle.branch
        social = await self.social()
        if self._loading or self.chronicle.branch != branch:
            return
        if any(f.get("challenge") == seq for f in self.chronicle.facts("challenge_hint")):
            return
        known = social.ties.get(giver, {})
        source = next((npc for npc in candidates if (tie := known.get(npc)) is not None
                       and (tie.meets or tie.deals or tie.bonds)), candidates[0])
        names = social.names
        self.chronicle.record({
            "fact": "challenge_hint", "id": uuid4().hex, "challenge": seq, "kind": challenge["kind"],
            "npc": giver, "name": names.get(giver, giver),
            "witnesses": [source], "witness_names": {source: names.get(source, source)},
            **{key: challenge[key] for key in ("chapter", "day", "hour", "minute")},
        })

    def _on_message(self, message: GameMessage) -> None:
        if isinstance(message, (Loading, Session)):
            self._loading = isinstance(message, Loading)
            for task in self._hints:
                task.cancel()
            if self._pending is not None:
                self._pending.cancel()
                self._pending = None
            if isinstance(message, Session):
                self._schedule()

    def _schedule(self) -> None:
        # QuestBook writes the closing state and its payment outcome in the same handler.
        if self._pending is None:
            self._pending = asyncio.get_running_loop().call_soon(self._check)

    def _check(self) -> None:
        self._pending = None
        entries = self.chronicle.entries()
        finished = {
            fact["challenge"] for _, fact in entries
            if fact.get("fact") in {"challenge_met", "challenge_failed"}
        }
        for seq, challenge in entries:
            if challenge.get("fact") == "challenge" and seq not in finished:
                result = self._result(seq, challenge, entries)
                if result is not None:
                    self.chronicle.record(result)

    def _result(self, seq: int, challenge: Fact, entries: list[tuple[int, Fact]]) -> Fact | None:
        kind, quests = challenge["kind"], set(challenge["quests"])
        people = set(challenge["people"])
        history = [(n, f) for n, f in entries if n > seq]
        closing: dict[str, int] = {}
        closed_at = None
        for n, fact in history:
            if fact.get("id") in quests and fact.get("fact") in TERMINAL:
                closing[fact["id"]] = n
                if closing.keys() == quests:
                    closed_at = n
                    break
        before_close = [(n, f) for n, f in history if closed_at is None or n <= closed_at]
        outcomes = [(n, f) for n, f in history
                    if f.get("fact") == "quest_outcome" and f.get("quest") in quests]
        violation = next((
            (n, f) for n, f in before_close
            if (kind == "no_blood" and f.get("fact") == "died"
                and f.get("killer") == "PLAYER" and f.get("npc") in people)
            or (kind == "secret_kept" and f.get("fact") in {"found_out", "caught"}
                and f.get("quest") in quests)
        ), None)
        if kind == "no_reward":
            violation = next(((n, f) for n, f in outcomes if f.get("pay") == "full"), None)
        base = {"challenge": seq, "kind": kind}
        if violation is not None:
            n, fact = violation
            return {"fact": "challenge_failed", **base, "why": fact["fact"], "evidence": [n]}
        if closed_at is None or any(self.book.progress(q).state not in CLOSED for q in quests):
            return None
        evidence = [*closing.values(), *(n for n, _ in outcomes)]
        why = None
        if kind == "no_reward":
            executed = [f for _, f in outcomes if f.get("how") != "dropped"]
            if not executed:
                why = "no_outcome"
            elif any(f.get("pay") != "none" for f in executed):
                why = "no_reward_refusal"
        elif kind == "secret_kept":
            lies = [n for n, f in before_close if f.get("fact") == "lie" and f.get("quest") in quests]
            evidence.extend(lies)
            if not lies:
                why = "no_lie"
        elif kind == "mercy":
            spared = [n for n, f in before_close if f.get("fact") == "surrender"
                      and f.get("status") == "spared" and f.get("npc") in people]
            evidence.extend(spared)
            if not spared:
                why = "no_mercy"
        evidence = sorted(set(evidence))
        if why is not None:
            return {"fact": "challenge_failed", **base, "why": why, "evidence": evidence}
        return {"fact": "challenge_met", **base, "line": challenge["line"],
                "people": challenge["people"], "evidence": evidence}
