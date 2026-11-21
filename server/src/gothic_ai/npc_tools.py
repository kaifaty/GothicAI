"""Tools of the conversational model and the situation it is shown.

The model does not drive the body (see executor.py): it sets intents and makes commitments. Checks run
before anything reaches the game and a rejection comes back within the same turn; what passes is carried
out after the npc's line, and its outcome is written into the conversation as a note.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import TYPE_CHECKING

from .contracts import GOLD
from .executor import ATTITUDES, GOALS, Intent, describe_targets
from .game import GameError, GameLink
from .llm import Tool, ToolCall
from .protocol import ActionResult, Snapshot

if TYPE_CHECKING:
    from .perception import Sight

log = logging.getLogger(__name__)

# gesture id -> human animation (the m2 scenario plays each; the model has no gesture tool)
GESTURES = {
    "nod": "T_YES",
    "shake_head": "T_NO",
    "come_here": "T_COMEOVERHERE",
    "shrug": "T_DONTKNOW",
    "greet": "T_GREETNOV",
    "dismiss": "T_FORGETIT",
}

END_CONVERSATION = "end_conversation"


def _object(properties: dict | None = None) -> dict:
    properties = properties or {}
    return {
        "type": "object",
        "properties": properties,
        "required": list(properties),
        "additionalProperties": False,
    }


SET_INTENT = "set_intent"
GIVE_ITEM = "give_item"
TAKE_ITEM = "take_item"
RECALL = "recall"
TAKE_S = 40.0  # the bridge gives a pick-up 30 s once the npc has said its lines

TOOLS = [
    Tool(
        SET_INTENT,
        "Изменить своё намерение: чего ты сейчас хочешь, как относишься к герою, чего хочешь от него. "
        "Тело и привычки выполнят его сами. Вызывай, только когда что-то меняется; текущее намерение "
        "есть в обстановке — передавай его целиком, неизменные поля повтори.",
        _object(
            {
                "goal": {"type": "string", "enum": list(GOALS), "description": "чего ты хочешь сейчас"},
                "target": {
                    "type": ["string", "null"],
                    "description": "на кого или куда: id из списка целей в обстановке "
                    "(PLAYER — герой); null — без цели",
                },
                "attitude": {"type": "string", "enum": list(ATTITUDES), "description": "отношение к герою"},
                "agenda": {
                    "type": ["string", "null"],
                    "description": "по-английски, до 15 слов: чего ты в долгую хочешь от героя или что "
                    "задумал; null — оставить как есть (мгновенные события его не меняют); "
                    "пустая строка — ничего",
                },
            }
        ),
    ),
    Tool(
        GIVE_ITEM,
        "Отдать герою предмет из СВОЕГО инвентаря (список — в обстановке). Тело подойдёт и отдаст после "
        "твоей реплики. Не раздавай ценное без причины.",
        _object(
            {
                "item": {
                    "type": "string",
                    "description": "идентификатор предмета из инвентаря, например ITPO_HEALTH_01",
                },
                "count": {"type": "integer", "description": "сколько штук, не меньше 1"},
            }
        ),
    ),
    Tool(
        TAKE_ITEM,
        "Поднять предмет, который лежит на земле и который ты видишь (строка «На земле» в обстановке): "
        "тело подойдёт и поднимет его после твоей реплики, и он станет твоим. Например, то, что герой "
        "бросил тебе под ноги.",
        _object(
            {
                "item": {
                    "type": "string",
                    "description": "идентификатор предмета из строки «На земле», например ITPL_BEET",
                },
            }
        ),
    ),
    Tool(
        RECALL,
        "Вспомнить подробности: прежние встречи с героем, которых нет в сводке твоей памяти (что он "
        "рассказывал, о чём просил, что ты обещал), и что ты знаешь о мире и своих делах — темы из "
        "«Что ты знаешь» целиком, люди, места, слухи. Перед вызовом скажи только «Хм…» или «Дай "
        "вспомнить…», без вопросов; ответишь, когда вспомнишь.",
        _object({"query": {"type": "string", "description": "о чём вспомнить, ключевые слова"}}),
    ),
    Tool(
        END_CONVERSATION,
        "Закончить разговор и вернуться к своим делам. Используй, когда сказать больше нечего "
        "или герой ведёт себя грубо. Перед этим попрощайся.",
        _object(),
    ),
]


@dataclass(frozen=True)
class Outcome:
    note: str  # for the conversation history, in the npc's perspective
    ok: bool


async def execute(link: GameLink, npc: str, call: ToolCall, snapshot: Snapshot | None) -> Outcome:
    """Run one tool call in the game: `give_item`, the one tool that acts (after the npc's line). Intents
    are carried out by executor.py and the behavioral model, `recall` and `end_conversation` by the
    dialogue itself."""
    args = call.arguments
    try:
        match call.name:
            case "give_item":
                item = str(args.get("item", "")).upper()
                count = max(1, int(args.get("count") or 1))
                name = _item_name(snapshot, item)
                res = await link.act(npc, "give_item", item=item, count=count)
                return _outcome(res, f"Ты отдал герою: {name} ×{count}", f"Не вышло отдать {name}")
            case "take_item":
                item = str(args.get("item", "")).upper()
                name = str(args.get("name") or item)
                res = await link.act(npc, "take_item", item=item, timeout=TAKE_S)
                return _outcome(res, f"Ты поднял с земли: {name}", f"Не вышло поднять {name}")
    except (GameError, TimeoutError, ValueError) as err:
        return Outcome(f"Действие {call.name} не выполнено: {err}", ok=False)
    return Outcome(f"Неизвестное действие {call.name}", ok=False)


def _outcome(res: ActionResult, done: str, failed: str) -> Outcome:
    if res.status == "done":
        return Outcome(done, ok=True)
    log.info("action %s %s: %s", res.id, res.status, res.detail)
    return Outcome(f"{failed} ({res.detail or res.status})", ok=False)


def _item_name(snapshot: Snapshot | None, item: str) -> str:
    if snapshot is not None:
        for it in snapshot.npc.inventory:
            if it.item == item:
                return it.name
    return item


# --- situation ------------------------------------------------------------------------

GUILDS = {
    0: "без гильдии",
    1: "паладин",
    2: "ополченец",
    3: "горожанин",
    4: "маг Огня",
    5: "послушник",
    6: "охотник на драконов",
    7: "наёмник",
    8: "крестьянин",
    9: "бандит",
    10: "каторжник",
    11: "ищущий",
    12: "житель окрестностей",
    13: "пират",
    14: "маг Воды",
}


def guild_name(guild: int) -> str:
    return GUILDS.get(guild, "существо")


def light(hour: int) -> str:
    """The part of the day, as one says it."""
    if 5 <= hour < 10:
        return "утро"
    if 10 <= hour < 18:
        return "день"
    return "вечер" if 18 <= hour < 22 else "ночь"


def _metres(cm: float) -> str:
    return f"{max(1, round(cm / 100))} м"


def render_situation(
    snap: Snapshot | None, intent: Intent | None = None, sight: Sight | None = None, *, brief: bool = False
) -> str:
    """What the npc perceives right now; sent with every turn, not kept in history. With `sight` (what it
    sees, perception.py) the place, the hero and the people around are as it perceives them; without, as
    the snapshot has them (everyone within 15 m, seen or not). `brief`: for a line in a fight (the quick
    model, dialogue.py), without the inventory and the targets of intents: only words, no acts."""
    if snap is None:
        if sight is None:
            return "[Обстановка неизвестна]" + (f"\nТвоё намерение: {intent.describe()}." if intent else "")
        # a free npc nobody asked a snapshot of: what it sees is all there is
        seen = [sight.place, *([sight.hero] if sight.hero else []), sight.people]
        seen += [sight.ground] if sight.ground else []
        seen += [sight.traces] if sight.traces else []
        intent_line = [f"Твоё намерение: {intent.describe()}."] if intent else []
        return "\n".join(["[Обстановка — видна только тебе, герою её не пересказывай]", *seen, *intent_line])
    w, npc, pl = snap.world, snap.npc, snap.player
    lines = [
        "[Обстановка — видна только тебе, герою её не пересказывай]",
        f"Время: день {w.day + 1}, {w.hour:02d}:{w.minute:02d}"
        + (f", {light(w.hour)}." if sight else ".")
        + ("" if sight else f" Ты у точки {npc.waypoint or npc.nearest_waypoint or 'неизвестно где'}."),
    ]
    if sight is not None:
        lines.append(sight.place)
        if sight.hero:
            lines.append(sight.hero)
    elif pl is not None:
        lines.append(
            f"Герой в {_metres(pl.distance)} от тебя. Гильдия героя: {guild_name(pl.guild)}. "
            f"Доспех: {pl.armor or 'нет'}. Оружие: {pl.melee or 'нет'}"
            f"{f', {pl.ranged}' if pl.ranged else ''}."
            + (f" Здоровье героя: {pl.hp}/{pl.hp_max}." if pl.hp is not None and pl.hp_max else "")
        )
    if not brief:
        items = [
            f"{it.name} ({it.item}) ×{it.count}"
            + (f" (цена за штуку — {it.value} зол.)" if it.item != GOLD and it.value is not None else "")
            for it in npc.inventory if not it.equipped
        ]
        lines.append("Твой инвентарь: " + ("; ".join(items) if items else "пусто") + ".")
    if sight is not None:
        lines.append(sight.people)
        if sight.ground:
            lines.append(sight.ground)
        if sight.traces:
            lines.append(sight.traces)
    else:
        near = [f"{o.name} ({_metres(o.distance)}{', мёртв' if o.dead else ''})" for o in snap.nearby_npcs]
        lines.append("Рядом: " + (", ".join(near) if near else "никого") + ".")
    lines.append("Сейчас ты идёшь за героем." if npc.following else "Сейчас ты стоишь на месте.")
    if intent is not None:
        lines.append(f"Твоё намерение: {intent.describe()}.")
        if not brief:
            targets = sight.targets if sight is not None and sight.targets else None
            lines.append(f"Цели для намерения (target): {describe_targets(snap, targets)}.")
    return "\n".join(lines)
