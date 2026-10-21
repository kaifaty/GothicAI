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
    requires: tuple[tuple[str, str], ...] = ()
    about: str = ""
    about_name: str = ""
    direct_context: str = "ты раскрыл это сам"
    about_field: str = ""
    about_name_field: str = ""
    alternatives: tuple[Source, ...] = ()


SOURCES = {
    "gate_vouch_broken": Source(
        kind="gate_vouch_broken", subject="npc", observer="witnesses", key="id", context="",
        subject_name="name", observer_name="witness_names",
        text="{subject_name} поручился за преступника",
        warning_direct="{subject_name} поручился за преступника",
        warning_heard="ты слышал, что {subject_name} поручился за преступника; со слов: {source_name}",
        about=PLAYER, about_name="герой", direct_context="ты узнал об этом",
    ),
    "bounty_betrayal": Source(
        kind="bounty_betrayal", subject="npc", observer="witnesses", key="id", context="",
        subject_name="name", observer_name="witness_names",
        text="{subject_name} сдал Декстера герою",
        warning_direct="{subject_name} при тебе сдал Декстера герою",
        warning_heard="ты слышал, что {subject_name} сдал Декстера герою; со слов: {source_name}",
        about="BDT_1060_DEXTER", about_name="Декстер", direct_context="ты слышал это сам",
    ),
    "blamed": Source(
        kind="blamed", subject="culprit", observer="npc", key="id", context="",
        subject_name="culprit_name", observer_name="name",
        text="{title}",
        warning_direct="ты считаешь {subject_name} виновным; это убеждение, а не увиденное дело",
        warning_heard="{about_name} считает {subject_name} виновным; со слов: {source_name}",
        about_field="npc", about_name_field="name",
        direct_context="ты считаешь его виновным",
    ),
    "npc_accused": Source(
        kind="npc_suspected", subject="target", observer="witnesses", key="id", context="",
        subject_name="target_name", observer_name="witness_names",
        text="{about_name} думает, что {subject_name} украл(а) его вещь",
        warning_direct="{about_name} подозревает {subject_name} в краже; доказательств нет",
        warning_heard=(
            "ты слышал, что {about_name} подозревает {subject_name} в краже; со слов: {source_name}"
        ),
        requires=(("kind", "suspect"),), about_field="npc", about_name_field="name",
        direct_context="ты слышал обвинение; это подозрение, не доказательство",
        alternatives=(Source(
            kind="npc_accusation", subject="target", observer="witnesses", key="id", context="",
            subject_name="target_name", observer_name="witness_names",
            text="{about_name} обвиняет {subject_name} в краже своей вещи",
            warning_direct="{about_name} обвиняет {subject_name} в краже своей вещи",
            warning_heard="ты слышал обвинение {subject_name} в краже; со слов: {source_name}",
            requires=(("kind", "reclaim"),), about_field="npc", about_name_field="name",
            direct_context="ты слышал обвинение",
        ),),
    ),
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
    "npc_broken": Source(
        kind="npc_broken",
        subject="npc",
        observer="witnesses",
        key="deal",
        context="deal",
        subject_name="npc_name",
        observer_name="witness_names",
        text="{subject_name} не отдал(а) герою обещанное по сделке",
        warning_direct="{subject_name} при тебе не отдал(а) герою обещанное по сделке",
        warning_heard=(
            "ты слышал, что {subject_name} не отдал(а) герою обещанное по сделке; со слов: {source_name}"
        ),
        requires=(("how", "unpaid"),),
        about=PLAYER,
        about_name="герой",
        direct_context="ты слышал это сам",
    ),
    "npc_crime_seen": Source(
        kind="npc_theft",
        subject="culprit",
        observer="npc",
        key="crime",
        context="npc_crime",
        subject_name="culprit_name",
        observer_name="name",
        text="{subject_name} украл(а) чужую вещь у человека по имени {about_name}",
        warning_direct="{subject_name} при тебе украл(а) чужую вещь",
        warning_heard="ты слышал, что {subject_name} крал(а) чужое; со слов: {source_name}",
        requires=(("kind", "theft"),),
        about_field="victim",
        about_name_field="victim_name",
        direct_context="ты видел это сам",
        alternatives=(Source(
            kind="npc_attack",
            subject="culprit",
            observer="npc",
            key="crime",
            context="npc_crime",
            subject_name="culprit_name",
            observer_name="name",
            text="{subject_name} напал(а) на человека по имени {about_name}",
            warning_direct="{subject_name} при тебе напал(а) на другого человека",
            warning_heard="ты слышал, что {subject_name} нападал(а) на другого; со слов: {source_name}",
            requires=(("kind", "attack"),),
            about_field="victim",
            about_name_field="victim_name",
            direct_context="ты видел это сам",
            hops=3,
        ),),
    ),
}


def source_kind(kind: str) -> Source | None:
    return next(
        (s for root in SOURCES.values() for s in (root, *root.alternatives) if s.kind == kind), None,
    )


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


def source_events(fact: Fact, facts: list[Fact]) -> list[PersonKnowledge]:
    """A source can name one observer or several; all start the same piece of news."""
    source = SOURCES.get(fact.get("fact", ""))
    if source is None:
        return []
    context = next(
        (f for f in facts if f.get("fact") == source.context and f.get("id") == fact.get(source.key)),
        {},
    )
    values = {**context, **fact}
    source = next(
        (s for s in (source, *source.alternatives)
         if all(values.get(key) == value for key, value in s.requires)), None,
    )
    if source is None:
        return []
    subject = str(values.get(source.subject, "")).upper()
    if not subject or subject == PLAYER:
        return []
    observers = fact.get(source.observer, [])
    if isinstance(observers, str):
        observers = [observers]
    if not isinstance(observers, list):
        return []
    event_id = str(fact.get(source.key) or fact.get("by") or subject)
    subject_name = str(
        fact.get("subject_name") or fact.get(source.subject_name)
        or context.get(source.subject_name) or subject
    )
    names = fact.get(source.observer_name) or context.get(source.observer_name)
    entries = []
    for observer in dict.fromkeys(str(npc).upper() for npc in observers if npc):
        if observer in (PLAYER, subject):
            continue
        observer_name = str((names.get(observer) if isinstance(names, dict) else names) or observer)
        entries.append(PersonKnowledge(
            key=f"{fact['fact']}:{event_id}",
            kind=source.kind,
            subject=subject,
            subject_name=subject_name,
            about=source.about or str(values.get(source.about_field) or observer),
            about_name=(source.about_name or str(values.get(source.about_name_field)
                        or values.get(source.about_field) or observer_name)),
            title=str(fact.get("title") or ""),
            source=observer,
            source_name=observer_name,
            direct=True,
            day=int(values.get("day", int(values.get("minute", 0)) // DAY)),
        ))
    return entries


def source_event(fact: Fact, facts: list[Fact]) -> PersonKnowledge | None:
    """The first discovery, for callers that need only one source observer."""
    return next(iter(source_events(fact, facts)), None)


class PeopleKnowledge:
    """A read-only projection: a load rolls it back, with no cached state or new facts."""

    def __init__(self, chronicle: Chronicle) -> None:
        self.chronicle = chronicle

    def of(self, npc: str) -> list[PersonKnowledge]:
        npc = npc.upper()
        facts = self.chronicle.facts()
        dead = {f.get("npc") for f in facts if f.get("fact") == "died"}
        refuted = {f.get("blamed") for f in facts if f.get("fact") == "blame_refuted"}
        latest_belief = {
            (f.get("npc"), f.get("crime")): f.get("id")
            for f in facts if f.get("fact") == "blamed"
        }
        known: dict[tuple[str, str, str], PersonKnowledge] = {}
        for f in facts:
            if f.get("fact") == "blamed" and (
                f.get("id") in refuted
                or latest_belief.get((f.get("npc"), f.get("crime"))) != f.get("id")
            ):
                continue
            entry = next((e for e in source_events(f, facts) if e.source == npc), None)
            if entry is None and (
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
            elif entry is None:
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
            source = source_kind(entry.kind)
            how = (
                source.direct_context if entry.direct and source is not None
                else f"со слов: {entry.source_name}"
            )
            description = entry.description()
            if entry.kind == "blamed" and entry.direct and entry.about == npc.upper():
                prefix = f"{entry.about_name} считает, что "
                if description.startswith(prefix):
                    description = "ты считаешь, что " + description.removeprefix(prefix)
            lines.append(f"- {description} ({how})")
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
