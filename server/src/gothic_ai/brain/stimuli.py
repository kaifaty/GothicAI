"""What an npc noticed, how urgent it is and how long it matters.

Hero threats and crimes are context only: the eval scored the decision model at ~45-55%.
CrimeReact rules settle held deeds; the scripts handle the remaining perceptions.
"""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class EventPolicy:
    text: str  # English, for the state; may use {other}, {detail}, {dist}
    ttl_s: float  # how long it stays relevant
    urgent: bool = False  # the player is right there and expects a reaction now
    salient: bool = False  # worth a decision for MID npc's too (NEAR ones react to any event)
    scripted: bool = False  # scripts or CrimeReact rules react; context only
    threat: bool = (
        False  # danger from someone else (not the hero): the behavioral model reacts, `other` is it
    )


POLICIES: dict[str, EventPolicy] = {
    "player_near": EventPolicy("The hero came close to you ({dist} away).", 15),
    "player_speech": EventPolicy('The hero said nearby: "{detail}"', 20, urgent=True, salient=True),
    "enter_room": EventPolicy("The hero walked into your house.", 30, urgent=True, salient=True),
    "item_given": EventPolicy("The hero gave you {detail}.", 60, urgent=True, salient=True),
    "item_dropped": EventPolicy("The hero dropped {detail} on the ground near you.", 30, salient=True),
    "noise": EventPolicy(
        "You heard an impact {dist} away {place}: {source} hit something (loudness {loudness}). "
        "You have already turned toward the sound. {witness}",
        15,
        salient=True,
    ),
    "player_sleeps": EventPolicy("The hero lay down to sleep nearby.", 60),
    "player_sits": EventPolicy("The hero sat down nearby.", 30),
    "weapon_drawn": EventPolicy("The hero drew a weapon near you.", 20, scripted=True),
    "threatened": EventPolicy("The hero aims a weapon at you.", 10, scripted=True),
    "theft": EventPolicy("The hero stole {detail}.", 300, scripted=True),
    "attack": EventPolicy("The hero attacked {detail}.", 300, scripted=True),
    "magic": EventPolicy("The hero cast a spell nearby.", 30, scripted=True),
    "murder": EventPolicy("The hero killed {detail}.", 600, scripted=True),
    "sheep": EventPolicy("The hero killed someone's sheep.", 600, scripted=True),
    # threats from others (v6): the behavioral model reacts (fight, flee, call for help), worded as in the
    # scenes it was trained on (decider-service/tuning); the bridge's kinds are mapped by `normalize`
    "monster_near": EventPolicy("{other} is coming at you.", 20, urgent=True, salient=True, threat=True),
    "npc_attack": EventPolicy("{other} attacked you.", 60, urgent=True, salient=True, threat=True),
    "npc_threat": EventPolicy(
        "{other} is hostile and running toward you with a drawn weapon.",
        10,
        urgent=True,
        salient=True,
        threat=True,
    ),
    "fight_nearby": EventPolicy("A fight broke out nearby: {detail}.", 30, salient=True, threat=True),
    "cry_for_help": EventPolicy("{other} is shouting for help nearby.", 30, salient=True, threat=True),
    # the end of such a danger: the monster is dead (the threats from it are dropped, see `NpcPlanner`)
    "monster_killed": EventPolicy(
        "{other} killed {detail}{context}; the danger is over.", 30, urgent=True, salient=True
    ),
}
DEFAULT_POLICY = EventPolicy("Something happened: {kind} ({detail}).", 15)


def policy(kind: str) -> EventPolicy:
    return POLICIES.get(kind, DEFAULT_POLICY)


# monster_killed: what the dead monster had been doing, by the threats from it the npc had in mind
THREAT_CONTEXT = {
    "npc_attack": ", which attacked you",
    "npc_threat": ", which ran at you with a drawn weapon",
    "monster_near": ", which was coming at you",
    "fight_nearby": ", which was attacking someone nearby",
}


def normalize(kind: str, other: str, other_name: str | None, detail: str | None) -> tuple[str, str | None]:
    """The bridge's threat events (protocol v6) as the kinds the behavioral model was trained on: an attack
    on the npc itself is `npc_attack`, an attack on someone else (the hero too) is `fight_nearby`."""
    who = other_name or other
    if kind == "monster_attack":
        if detail == "you":
            return "npc_attack", None
        victim = "the hero" if detail == "PLAYER" else (detail or "someone")
        return "fight_nearby", f"{who} attacks {victim}"
    if kind == "hero_attacked":
        return "fight_nearby", f"{who} attacks the hero"
    return kind, detail


@dataclass(frozen=True)
class Stimulus:
    kind: str
    at: float  # monotonic time it arrived
    other: str = ""  # never infer the cause of a sound; legacy event defaults are set by the planner
    distance_cm: float | None = None
    detail: str | None = None
    other_name: str | None = None  # shown name of `other` (not the player)
    context: str = ""  # what it was about, e.g. the threat a killed monster was to the npc
    position: tuple[float, float, float] | None = None  # noise: where the sound came from, cm
    place: str | None = None
    loudness: float | None = None
    source: str | None = None

    @property
    def policy(self) -> EventPolicy:
        return policy(self.kind)

    def alive(self, now: float) -> bool:
        return now - self.at <= self.policy.ttl_s

    def describe(self, now: float) -> str:
        other = "the hero" if self.other == "PLAYER" else (self.other_name or self.other)
        dist = f"{self.distance_cm / 100:.0f} m" if self.distance_cm is not None else "a few metres"
        detail = {"you": "you", "PLAYER": "the hero"}.get(self.detail or "", self.detail or "something")
        text = self.policy.text.format(
            other=other,
            detail=detail,
            dist=dist,
            kind=self.kind,
            context=self.context,
            place=self.place or "nearby",
            source={"table": "a table", "barrel": "a barrel", "cup": "a cup", "body": "a body"}.get(
                self.source or "", "an object"
            ),
            loudness=f"{self.loudness:.2f}" if self.loudness is not None else "unknown",
            witness=f"You saw {other} cause it." if self.other else "You did not see who caused it.",
        )
        text = text[:1].upper() + text[1:]  # "{other} ..." may start with "the hero"
        ago = now - self.at
        return text if ago < 3 else f"{text} ({ago:.0f} s ago)"


def merge(stimuli: list[Stimulus], new: Stimulus, now: float, keep: int = 5) -> list[Stimulus]:
    """Alive stimuli plus the new one; the same kind from the same source is replaced, not repeated."""
    alive = [s for s in stimuli if s.alive(now) and not (s.kind == new.kind and s.other == new.other)]
    alive.append(new)
    # most urgent first, then the newest
    alive.sort(key=lambda s: (not s.policy.urgent, -s.at))
    return alive[:keep]
