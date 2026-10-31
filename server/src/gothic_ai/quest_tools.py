"""Shared tools for changing a quest giver's terms."""

from .llm import Tool

AMEND = "amend_quest"
DROP = "drop_quest"

_QUEST_ARG = {
    "type": "object",
    "properties": {
        "quest": {"type": "string", "description": "id из обстановки, например MIS_LOBART_RUEBEN"}
    },
    "required": ["quest"],
    "additionalProperties": False,
}
AMEND_TOOL = Tool(
    AMEND,
    "Изменить свой уговор с героем: снять пункт, который тебе больше не нужен, — пощадил того, кого "
    "просил убить; договорились; он ушёл; хватит и того, что есть. Вызывай сразу, как решил, не жди, пока "
    "герой придёт сдавать дело. Снимать можно только пункты, которые перечислены в обстановке. Мир от "
    "этого не меняется: пощажённый остаётся жив.",
    {
        **_QUEST_ARG,
        "properties": {
            **_QUEST_ARG["properties"],
            "drop": {"type": "array", "items": {"type": "string"}, "minItems": 1},
            "why": {"type": "string", "description": "Почему ты снимаешь эти пункты"},
        },
        "required": ["quest", "drop", "why"],
    },
)
DROP_TOOL = Tool(
    DROP,
    "Отменить своё поручение, когда дело потеряло смысл: ты передумал или нужда отпала сама. Квест "
    "закроется без награды. Не используй это, чтобы наказать героя: для этого есть твоё отношение к нему.",
    {
        **_QUEST_ARG,
        "properties": {
            **_QUEST_ARG["properties"],
            "why": {"type": "string", "description": "Почему дело потеряло смысл"},
        },
        "required": ["quest", "why"],
    },
)
