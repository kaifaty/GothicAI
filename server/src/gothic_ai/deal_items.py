"""Item quantities and script prices for a negotiated deal."""

from dataclasses import dataclass
from typing import Any

from .contracts import GOLD, MAX_COUNT, MAX_TERMS, gold_text
from .game import GameError, GameLink
from .llm import Tool
from .turn import ToolOutcome

ITEM_LIST = {
    "type": "array",
    "items": {
        "type": "object",
        "properties": {
            "item": {"type": "string", "description": "идентификатор предмета, золото — ITMI_GOLD"},
            "count": {"type": "integer", "description": "сколько, не меньше 1"},
        },
        "required": ["item", "count"],
        "additionalProperties": False,
    },
}

APPRAISE_TOOL = Tool(
    "appraise_deal",
    "Узнать игровую цену названных вещей до торга, в том числе вещей, которые предлагает герой. "
    "Укажи предметы и количества с каждой стороны. Получишь цену за штуку и сумму в золоте. "
    "Оценка доступна независимо от того, у кого есть вещь.",
    {
        "type": "object",
        "properties": {
            "npc_gives": {**ITEM_LIST, "description": "предметы с твоей стороны"},
            "hero_gives": {**ITEM_LIST, "description": "предметы со стороны героя"},
        },
        "required": ["npc_gives", "hero_gives"],
        "additionalProperties": False,
    },
)


def parse_items(raw: Any) -> tuple[dict[str, int] | None, str]:
    """[{item, count}] -> {ITEM: count}, the same item added up; or why not."""
    if raw is None:
        return {}, ""
    if not isinstance(raw, list):
        return None, "нужен список предметов."
    out: dict[str, int] = {}
    for x in raw:
        if not isinstance(x, dict):
            return None, "каждый предмет — {item, count}."
        item = str(x.get("item") or "").strip().upper()
        try:
            count = int(x.get("count") or 0)
        except (TypeError, ValueError):
            count = 0
        if not item or count < 1:
            return None, f"неверный предмет: {x}."
        out[item] = out.get(item, 0) + count
    if len(out) > MAX_TERMS:
        return None, f"не больше {MAX_TERMS} разных предметов с одной стороны."
    if any(c > MAX_COUNT for c in out.values()):
        return None, f"не больше {MAX_COUNT} штук."
    return out, ""


async def appraise_deal(link: GameLink, args: dict[str, Any]) -> ToolOutcome:
    npc, error = parse_items(args.get("npc_gives"))
    if npc is None:
        return ToolOutcome(error, "rejected")
    hero, error = parse_items(args.get("hero_gives"))
    if hero is None:
        return ToolOutcome(error, "rejected")
    if not (npc or hero):
        return ToolOutcome("Укажи хотя бы один предмет для оценки.", "rejected")

    items = sorted((npc.keys() | hero.keys()) - {GOLD})
    try:
        state = await link.story_state(items=items) if items else {}
    except (GameError, TimeoutError) as err:
        return ToolOutcome(f"Не удалось узнать игровые цены ({err}).", "rejected")

    appraisal = format_appraisal(npc, hero, state.get("items", {}), state.get("item_values", {}))
    return ToolOutcome(appraisal, "read")


@dataclass
class DealItemLookup:
    names: dict[str, str]
    values: dict[str, int]
    swaps: dict[str, str]
    day: int


def format_appraisal(
    npc: dict[str, int], hero: dict[str, int], names: dict[str, str], values: dict[str, int]
) -> str:
    lines = []
    for side, quantities in (("От тебя", npc), ("От героя", hero)):
        if not quantities:
            continue
        terms = []
        total = 0
        complete = True
        for item, count in quantities.items():
            if item == GOLD:
                terms.append(gold_text(count))
                total += count
                continue
            name = names.get(item) or item
            value = values.get(item)
            if type(value) is not int:
                terms.append(f"{name} ×{count} (цена неизвестна)")
                complete = False
                continue
            amount = value * count
            total += amount
            terms.append(f"{name} ×{count} по {value} зол. = {amount} зол.")
        subtotal = gold_text(total) if complete else "сумма неизвестна"
        lines.append(f"{side} — {', '.join(terms)}. Итого — {subtotal}.")
    return "\n".join(lines)
