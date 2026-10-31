"""What an npc knows of the hero's word (docs/story-graph.md, stage 5), from the facts of the save branch.

A lie is a mechanic the player can read, not the model's mood: what the npc sees for itself is in the
situation of its quests (vanilla.py), and what it knows of the hero's honesty is here.

- lied: it caught him lying (`caught`) or found a lie of his out later (`found_out`). It takes his word
  no more: a quest is accepted only on what it sees for itself (quests.py and vanilla.py enforce it);
- heard: rumors that he lied to someone else (`rumor`): told before it, or retold by whom (social.py);
- kept: claims of his to it that were true (`claim` with `truth`);
- amended: lies he made amends for, to it or as it asked (amends.py), or it forgave in a deal (deals.py): a
  lie amended does not break his word;
- broke: debts to it he did not pay back by their day (deals.py), a meeting he did not come to
  (meetings.py): a broken word, like a lie.

The last two the conversational model weighs in character.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING

from .chronicle import Chronicle, Fact
from .llm import Tool, ToolCall
from .people_knowledge import rumor_subject
from .protocol import Snapshot
from .turn import ToolOutcome

if TYPE_CHECKING:
    from .crimes import Crimes
    from .dialogue import Conversation, DialogueManager


def is_meeting(key: str) -> bool:
    """A broken word's key is a meeting's id (meetings.py), not a debt's."""
    return str(key).startswith("meet-")


WORD_BROKEN = "На слово ты ему больше не веришь: засчитаешь только то, что увидишь сам."


@dataclass
class Record:
    """An npc's knowledge of the hero's word."""

    lied: list[str] = field(default_factory=list)  # lies to it it knows of: what and how it learned
    heard: list[str] = field(default_factory=list)  # rumors: whom he lied to, about what
    kept: list[str] = field(default_factory=list)  # true claims to it
    amended: list[str] = field(default_factory=list)  # lies he made amends for, with it
    broke: list[str] = field(default_factory=list)  # debts to it not paid back by their day
    crimes: list[str] = field(default_factory=list)  # proved deeds, or a victim's learned grievance

    @property
    def broken(self) -> bool:
        return bool(self.lied or self.broke or self.crimes)

    def broken_reason(self) -> str:
        """Why this npc no longer takes the hero's word."""
        reasons = []
        if self.lied:
            reasons.append("он уже лгал тебе")
        if self.broke:
            reasons.append("он не сдержал слово")
        if self.crimes:
            reasons.append(f"его дела подорвали твоё доверие ({'; '.join(self.crimes)})")
        return "; ".join(reasons)


class Trust:
    """The hero's word in the eyes of each npc: a TurnExtension with situation lines only."""

    def __init__(self, chronicle: Chronicle, *, crimes: Crimes | None = None) -> None:
        self.chronicle = chronicle
        self.crimes = crimes

    def of(self, npc: str) -> Record:
        npc = npc.upper()
        facts = [
            f for f in self.chronicle.facts()
            if f.get("fact") != "rumor" or rumor_subject(f) == "PLAYER"
        ]
        titles = _titles(facts)
        r = Record()
        amended = {
            (f.get("quest"), f.get("about"))
            for f in facts
            if f.get("fact") == "amends" and not f.get("crime") and npc in (f.get("npc"), f.get("to"))
        }
        amended |= {
            _lie(key)
            for f in facts
            if f.get("fact") == "deal" and f.get("npc") == npc
            for key in f.get("forgive", [])
        }
        for f in facts:
            quest = f.get("quest", "")
            title = f.get("title") or titles.get(quest) or quest
            match f.get("fact"):
                case "found_out" | "caught" if f.get("npc") == npc and (quest, npc) not in amended:
                    how = f.get("how", "")
                    r.lied.append(f"«{title}»{f' — {how}' if how else ''}")
                case "amends" if not f.get("crime") and npc in (f.get("npc"), f.get("to")):
                    gold = int(f.get("gold", 0))
                    r.amended.append(f"«{title}»{f' — заплатил {gold} золотых' if gold else ''}")
                case "deal" if f.get("npc") == npc and f.get("forgive"):
                    gold = int(f.get("gold", 0))
                    for key in f["forgive"]:
                        if str(key).startswith("crime:"):
                            continue
                        what = titles.get(_lie(key)[0]) or _lie(key)[0]
                        r.amended.append(f"«{what}»{f' — заплатил {gold} золотых' if gold else ''}")
                case "broken" if f.get("npc") == npc and f.get("promise") == "meet":
                    r.broke.append(f"обещал прийти на встречу ({f.get('what', '')}), а не пришёл")
                case "broken" if f.get("npc") == npc:
                    r.broke.append(f"обещал вернуть {f.get('what', '')}, а не вернул")
                case "gate_vouch_broken" if f.get("npc") == npc:
                    kind = f.get("crime_kind") or next(
                        (deed["kind"] for deed in facts
                         if deed.get("fact") == "crime" and deed.get("id") == f.get("crime")),
                        "",
                    )
                    r.crimes.append(f"ты поручился за него, а он совершил преступление: «{kind}»")
                case "rumor" if f.get("to") == npc and f.get("kind") == "broken":
                    about = f.get("about_name") or f.get("about", "")
                    teller = f.get("from_name") if int(f.get("hops", 1)) > 1 else None
                    told = f", со слов: {teller}" if teller else ""
                    what = "не пришёл на встречу" if is_meeting(f.get("quest", "")) else "не вернул долг"
                    r.heard.append(f"{about} ({what}: {f.get('title', '')}{told})")
                case "rumor" if f.get("to") == npc and f.get("kind") in {"caught", "found_out"}:
                    about = f.get("about_name") or f.get("name") or f.get("from", "")
                    teller = f.get("from_name") if int(f.get("hops", 1)) > 1 else None
                    r.heard.append(f"{about} («{title}»{f', со слов: {teller}' if teller else ''})")
                case "claim" if f.get("npc") == npc and f.get("truth"):
                    r.kept.append(f"«{title}»")
        r.heard = list(dict.fromkeys(r.heard))
        r.amended = list(dict.fromkeys(r.amended))
        if self.crimes is not None:
            r.crimes.extend(self.crimes.distrust(npc))
        return r

    def broken(self, npc: str) -> bool:
        return self.of(npc).broken

    # --- the conversation (TurnExtension) ------------------------------------------------------

    async def prepare(self, npc: str) -> None:
        pass  # the chronicle is here already

    def situation(self, npc: str) -> str | None:
        r = self.of(npc)
        lines = []
        if r.lied:
            lines.append(f"- он уже лгал тебе: {'; '.join(r.lied)}. {WORD_BROKEN}")
        if r.broke:
            lines.append(f"- он не сдержал слово: {'; '.join(r.broke)}.{'' if r.lied else ' ' + WORD_BROKEN}")
        if r.crimes:
            lines.append(f"- его дела подорвали твоё доверие: {'; '.join(r.crimes)}. {WORD_BROKEN}")
        if r.heard:
            lines.append(f"- ты слышал, что он обманул других: {', '.join(r.heard)}")
        if r.kept:
            lines.append(f"- он уже говорил тебе правду: {', '.join(r.kept)}")
        if r.amended:
            lines.append(f"- он загладил вину за ложь: {'; '.join(r.amended)}")
        return "Что ты знаешь о честности героя:\n" + "\n".join(lines) if lines else None

    all_tools: tuple[Tool, ...] = ()

    def tools(self, npc: str) -> list[Tool]:
        return []

    async def handle(
        self, dialogue: DialogueManager, conv: Conversation, call: ToolCall, snap: Snapshot | None
    ) -> ToolOutcome | None:
        return None


def _lie(key: str) -> tuple[str, str]:
    """A lie's key (kind:about:quest, as amends.Lie.key) -> (quest, about)."""
    _, about, quest = (key.split(":", 2) + ["", ""])[:3]
    return quest, about


def _titles(facts: list[Fact]) -> dict[str, str]:
    """Quest -> its title, from the facts that name it (the director's quests, the lies)."""
    out: dict[str, str] = {}
    for f in facts:
        quest = f.get("quest") or (f.get("id") if f.get("fact") == "quest" else None)
        if quest and f.get("title"):
            out.setdefault(quest, f["title"])
    return out
