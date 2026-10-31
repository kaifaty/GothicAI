"""Orders the hero gives by voice: a short fixed phrase ("За мной!", "Стой!", "В атаку!").

The conversational model takes seconds to answer, and in a fight that is too long for "Стой!". An order
is told apart by code from the transcript in no time (orders.py carries it out at once). Follow
requests may include a destination or a question ("Пошли к пещере", "Ну ты идёшь, нет?"): an existing
companion obeys, while anyone else decides in conversation. Other orders stay short fixed phrases;
"Не стой столбом" and "Он сказал: стой" go to the conversational model as before.
If the whole phrase is not a follow request, its last sentence is checked by the same rules.
Reported speech anywhere in the phrase or quotation marks rule out a follow request.

A phrase is an order when, without words of address ("эй", "ребята", the npc's name) and of hurry
("быстро", "живо"), what is left is one of the forms of the order, said once or repeated ("Стой, стой!",
"Бей его, бей!").
Speech recognition may run words together or mishear a letter in a long form ("оружия к бою"): a form of
nine letters or more is matched with one letter off, of six to eight with an alike letter only.

Pure: no game, no models.
"""

from __future__ import annotations

import re
from collections.abc import Callable
from dataclasses import dataclass

# the forms of each order, as speech recognition writes them (no "ё"); plural forms for a group
FORMS: dict[str, tuple[str, ...]] = {
    "follow": (
        "за мной",
        "иди за мной",
        "идите за мной",
        "следуй за мной",
        "следуйте за мной",
        "пошли",
        "пойдем",
        "идем",
    ),
    "come": ("ко мне", "сюда", "иди сюда", "идите сюда", "подойди", "подойдите"),
    # «Погоди.» to one sent ahead (2026-10-14: the model took it for «I no longer need you»)
    "halt": (
        "стой",
        "стойте",
        "стоять",
        "замри",
        "замрите",
        "погоди",
        "погодите",
        "подожди",
        "подождите",
        "постой",
        "постойте",
    ),
    "wait": (
        "жди здесь",
        "ждите здесь",
        "подожди здесь",
        "подождите здесь",
        "оставайся здесь",
        "оставайтесь здесь",
        "стой здесь",
        "стойте здесь",
        "жди тут",
        "стой тут",
    ),
    # with nobody to attack (no foe, the hero looks at no one) these are words: "Помоги!" asks for help
    "attack": (
        "в атаку",
        "атакуй",
        "атакуйте",
        "атакуй его",
        "атакуйте его",
        "бей",
        "бейте",
        "бей его",
        "бейте его",
        "бей их",
        "бейте их",
        "убей",
        "убейте",
        "убей его",
        "убейте его",
        "убей их",
        "мочи его",
        "руби его",
        "взять его",
        "помоги",
        "помоги мне",
        "помогите",
        "на помощь",
        "защищай меня",
        "защити меня",
    ),
    "retreat": ("отступай", "отступайте", "отступаем", "назад", "беги", "бегите", "уходим", "отходим"),
    "draw": ("к бою", "к оружию", "оружие к бою", "доставай оружие", "достань оружие", "обнажить оружие"),
    "sheathe": ("убери оружие", "уберите оружие", "спрячь оружие", "отбой"),
    "dismiss": ("свободен", "свободна", "свободны", "можешь идти", "можете идти", "иди по своим делам"),
}
KINDS = tuple(FORMS)

# what the conversational model is told the hero ordered (orders.py)
SAID = {
    "follow": "идти за ним",
    "come": "подойти к нему",
    "halt": "остановиться",
    "wait": "ждать его здесь",
    "attack": "атаковать",
    "retreat": "отступить к нему",
    "draw": "взять оружие наизготовку",
    "sheathe": "убрать оружие",
    "dismiss": "больше не идти за ним, заниматься своими делами",
}

# words of address and of hurry around an order: "Эй, ребята, за мной, живо!"
EXTRA = frozenset(
    "эй ну ка а давай давайте быстро быстрее живо живее скорее скорей сейчас же немедленно все ребята "
    "парни мужики друг дружище так".split()
)

_WORD = re.compile(r"[a-zа-я]+")


def words(text: str) -> list[str]:
    return _WORD.findall(text.lower().replace("ё", "е"))


# letters speech recognition easily takes for each other (talk.py has the same for names)
_ALIKE = frozenset(
    frozenset(p) for p in ("ао", "еи", "еэ", "иы", "уо", "ая", "ую", "бп", "вф", "гк", "дт", "жш", "зс")
)


def _off(a: str, b: str, limit: float) -> float:
    """Letters to change, put in or take out to make `a` of `b` (an alike letter changes for half)."""
    if abs(len(a) - len(b)) > limit:
        return limit + 1
    prev = [float(j) for j in range(len(b) + 1)]
    for i in range(1, len(a) + 1):
        row = [float(i)] + [0.0] * len(b)
        for j in range(1, len(b) + 1):
            change = (
                0.0 if a[i - 1] == b[j - 1] else 0.5 if frozenset((a[i - 1], b[j - 1])) in _ALIKE else 1.0
            )
            row[j] = min(prev[j] + 1, row[j - 1] + 1, prev[j - 1] + change)
        prev = row
    return prev[-1]


def _slack(form: str) -> float:
    return 1.0 if len(form) >= 9 else 0.5 if len(form) >= 6 else 0.0


# a form without spaces -> its order ("замной" -> follow)
_JOINED = {"".join(form.split()): kind for kind, forms in FORMS.items() for form in forms}


@dataclass(frozen=True)
class Order:
    kind: str  # one of KINDS
    form: str  # the form heard, without spaces ("замной")
    heard: str  # the hero's words


def match(rest: str) -> str | None:
    """The form `rest` (words run together) is, if any: exactly, else the nearest within its slack."""
    if rest in _JOINED:
        return rest
    near = [(d, form) for form in _JOINED if (d := _off(rest, form, _slack(form))) <= _slack(form)]
    return min(near)[1] if near else None


def follow_request(text: str, is_name: Callable[[str], bool] = lambda w: False) -> bool:
    """A direct invitation to go with the speaker, including its destination or a reminder question.

    Only the opening may be words of address; if the whole phrase is not a request, its last sentence
    is checked by the same rules. Reported speech anywhere or quotation marks reject the whole phrase.
    """
    rest = words(text)
    if any(mark in text for mark in ('"', "«", "»")) or any(
        w in (
            "сказал",
            "сказала",
            "сказали",
            "говорит",
            "говорят",
            "говорил",
            "говорила",
            "крикнул",
            "крикнула",
            "кричит",
            "велел",
            "велела",
            "спросил",
            "спросила",
            "ответил",
        )
        and not (
            w in {"сказал", "сказала"}
            and (
                rest[i - 1 : i] == ["я"]
                or rest[i - 2 : i] == ["я", "же"]
                or rest[i + 1 : i + 2] == ["же"]
            )
        )
        for i, w in enumerate(rest)
    ):
        return False
    if _follow_words(rest, is_name):
        return True
    sentences = re.split(r"(?<=[.!?…])\s+", text.strip())
    return len(sentences) > 1 and _follow_words(words(sentences[-1]), is_name)


def _follow_words(rest: list[str], is_name: Callable[[str], bool]) -> bool:
    while rest and (rest[0] in EXTRA or is_name(rest[0])):
        rest.pop(0)
    if not rest or any(
        w in rest
        for w in (
            "не",
            "если",
            "бы",
            "когда",
            "потом",
            "позже",
            "завтра",
            "послезавтра",
            "сначала",
            "ли",
        )
    ):
        return False
    if rest[:2] in (["пошли", "слухи"], ["пошли", "разговоры"]):
        return False
    if rest[0] in {"пойдем", "пойдемте", "пошли", "пошлите", "идем", "идемте"}:
        return True
    if rest[:2] == ["за", "мной"]:
        return True
    if rest[0] in {"иди", "идите", "следуй", "следуйте"}:
        return rest[1:3] in (["за", "мной"], ["со", "мной"])
    # "Ну ты идешь, нет?" is an invitation/reminder; "Ты идешь в город?" asks about a route.
    if rest[0] in {"ты", "вы"}:
        rest = rest[1:]
    if rest in (["со", "мной"], ["за", "мной"]):
        return True
    if not rest or rest[0] not in {"идешь", "идете", "пойдешь", "пойдете"}:
        return False
    tail = rest[1:]
    return tail in ([], ["нет"], ["или", "нет"]) or tail[:2] in (
        ["со", "мной"],
        ["за", "мной"],
    )


def parse(text: str, is_name: Callable[[str], bool] = lambda w: False) -> Order | None:
    """The order in the hero's phrase, if it is one (see the module's doc). `is_name`: whether a word is
    the name of someone around, said to call him ("Горн, за мной!"). Only follow requests may be
    questions; other questions ("Сюда?") are not orders."""
    if follow_request(text, is_name):
        return Order("follow", "замной", text)
    if text.rstrip().endswith("?"):
        return None
    rest = [w for w in words(text) if w not in EXTRA and not is_name(w)]
    if not rest:
        return None
    joined = "".join(rest)
    # "стой стой", "назад назад назад": the form said again and again
    for n in range(1, len(rest) + 1):
        if len(rest) % n == 0 and rest[:n] * (len(rest) // n) == rest:
            joined = "".join(rest[:n])
            break
    form = match(joined)
    if form is None and len(set(rest)) < len(rest):  # "бей его, бей": its words, some said twice
        form = match("".join(dict.fromkeys(rest)))
    return Order(_JOINED[form], form, text) if form is not None else None
