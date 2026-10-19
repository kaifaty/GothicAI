"""What each NPC knows against other people, projected from the save branch's facts.

Sources describe both the original discovery and the rumor it starts. Adding a source needs one
entry in SOURCES; the rumor desk, the situation and witness warnings use that same description.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import TYPE_CHECKING

from .chronicle import Chronicle, Fact
from .llm import Tool, ToolCall
from .protocol import Snapshot
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .dialogue import Conversation, DialogueManager

PLAYER = "PLAYER"
DAY = 24 * 60
SHOWN = 3


def rumor_subject(fact: Fact) -> str:
    """Old rumor facts concern the hero, including an omitted or empty subject."""
    return str(fact.get("subject") or PLAYER).upper()


@dataclass(frozen=True)
class Source:
    kind: str
    subject: str
    observer: str
    key: str
    context: str
    subject_name: str
    observer_name: str
    text: str
    warning_direct: str
    warning_heard: str
    hops: int = 2


SOURCES = {
    "exposed": Source(
        kind="cover",
        subject="liar",
        observer="npc",
        key="testimony",
        context="testimony",
        subject_name="witness_name",
        observer_name="name",
        text="{subject_name} покрывал(а) героя и лгал(а) человеку по имени {about_name}",
        warning_direct="{subject_name} уже лгал(а) тебе, покрывая героя",
        warning_heard="ты слышал, что {subject_name} лгал(а), покрывая героя; со слов: {source_name}",
    ),
}


def source_kind(kind: str) -> Source | None:
    return next((s for s in SOURCES.values() if s.kind == kind), None)


@dataclass(frozen=True)
class PersonKnowledge:
    key: str
    kind: str
    subject: str
    subject_name: str
    about: str
    about_name: str
    title: str
    source: str  # the discoverer, or the person who retold it
    source_name: str
    direct: bool
    day: int = 0

    def description(self) -> str:
        source = source_kind(self.kind)
        return source.text.format(**vars(self)) if source is not None else self.title


def source_event(fact: Fact, facts: list[Fact]) -> PersonKnowledge | None:
    """The source's observer is the first to know; context supplies old facts' missing names/time."""
    source = SOURCES.get(fact.get("fact", ""))
    if source is None:
        return None
    subject = str(fact.get(source.subject, "")).upper()
    observer = str(fact.get(source.observer, "")).upper()
    if not subject or not observer or subject == PLAYER:
        return None
    event_id = str(fact.get(source.key) or fact.get("by") or subject)
    context = next(
        (f for f in facts if f.get("fact") == source.context and f.get("id") == fact.get(source.key)),
        {},
    )
    subject_name = str(fact.get("subject_name") or context.get(source.subject_name) or subject)
    observer_name = str(fact.get("name") or context.get(source.observer_name) or observer)
    return PersonKnowledge(
        key=f"{fact['fact']}:{event_id}",
        kind=source.kind,
        subject=subject,
        subject_name=subject_name,
        about=observer,
        about_name=observer_name,
        title=str(fact.get("title") or ""),
        source=observer,
        source_name=observer_name,
        direct=True,
        day=int(fact.get("day", int(fact.get("minute", context.get("minute", 0))) // DAY)),
    )


class PeopleKnowledge:
    """A read-only projection: a load rolls it back, with no cached state or new facts."""

    def __init__(self, chronicle: Chronicle) -> None:
        self.chronicle = chronicle

    def of(self, npc: str) -> list[PersonKnowledge]:
        npc = npc.upper()
        facts = self.chronicle.facts()
        dead = {f.get("npc") for f in facts if f.get("fact") == "died"}
        known: dict[tuple[str, str, str], PersonKnowledge] = {}
        for f in facts:
            entry = source_event(f, facts)
            if entry is not None:
                if entry.source != npc:
                    continue
            elif (
                f.get("fact") == "rumor" and f.get("to") == npc
                and rumor_subject(f) != PLAYER and source_kind(f.get("kind", "")) is not None
            ):
                entry = PersonKnowledge(
                    key=str(f.get("quest", "")),
                    kind=str(f["kind"]),
                    subject=rumor_subject(f),
                    subject_name=str(f.get("subject_name") or rumor_subject(f)),
                    about=str(f.get("about", "")),
                    about_name=str(f.get("about_name") or f.get("about", "")),
                    title=str(f.get("title", "")),
                    source=str(f.get("from", "")),
                    source_name=str(f.get("from_name") or f.get("from", "")),
                    direct=False,
                    day=int(f.get("day", 0)),
                )
            else:
                continue
            if entry.subject == npc or entry.subject in dead:
                continue
            key = (entry.kind, entry.subject, entry.key)
            # Replayed facts never multiply the knowledge, and hearsay cannot replace a discovery.
            old = known.get(key)
            if old is not None and (old.direct or not entry.direct):
                continue
            known.pop(key, None)
            known[key] = entry
        return list(reversed(known.values()))

    def context(self, npc: str) -> str | None:
        lines = []
        for entry in self.of(npc)[:SHOWN]:
            how = "ты раскрыл это сам" if entry.direct else f"со слов: {entry.source_name}"
            lines.append(f"- {entry.description()} ({how})")
        return "Что ты знаешь о людях вокруг:\n" + "\n".join(lines) if lines else None

    def warning(self, npc: str, subject: str) -> str | None:
        entry = next((e for e in self.of(npc) if e.subject == subject.upper()), None)
        if entry is None or (source := source_kind(entry.kind)) is None:
            return None
        text = source.warning_direct if entry.direct else source.warning_heard
        return f"({text.format(**vars(entry))})"

    async def prepare(self, npc: str) -> None:
        pass

    def situation(self, npc: str) -> str | None:
        return self.context(npc)

    all_tools: tuple[Tool, ...] = ()

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None
