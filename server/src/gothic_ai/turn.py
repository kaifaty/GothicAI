"""Rules of one turn of the conversational model, shared by the dialogue and by `replay`.

A turn is up to `MAX_STEPS` model calls. After each step the tool calls are handled at once; the turn
goes on only when the model has to see a result before it is done:
- a read (`recall`) brought something back;
- a check rejected a call (no such item...);
- an accepted amendment made quest completion available in this turn;
- the model committed to something but has not said a word in the whole turn.

The last step offers the tools with `tool_choice="none"`: the request (and the provider's prompt cache)
stays the same, and the model can only talk.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from typing import Literal

from .llm import ToolChoice

MAX_STEPS = 3

OutcomeStatus = Literal["accepted", "rejected", "read", "ignored"]


@dataclass(frozen=True)
class ToolOutcome:
    """What a tool call gives the turn at once (its effect in the game may come later)."""

    output: str  # what the model is told if the turn goes on
    status: OutcomeStatus
    continue_turn: bool = False  # an accepted call opened a next action for this turn

    @property
    def needs_reply(self) -> bool:
        return self.continue_turn or self.status in ("rejected", "read")


def tool_choice(step: int) -> ToolChoice | None:
    return "none" if step == MAX_STEPS - 1 else None


def tool_output(outcome: ToolOutcome, said: str) -> str:
    """A rejection may correct spoken words, but does not start the hero's reply over."""
    if outcome.status != "rejected" or not said:
        return outcome.output
    return (
        f"{outcome.output} Ты уже сказал вслух: «{said}». "
        "Если отказ этого не меняет — ничего не говори. "
        "Если меняет — поправься одной короткой фразой, не повторяя сказанного."
    )


def go_on(outcomes: Sequence[ToolOutcome], said_anything: bool) -> bool:
    if any(o.needs_reply for o in outcomes):
        return True
    return not said_anything and any(o.status == "accepted" for o in outcomes)
