import pytest

from gothic_ai.commands import FORMS, follow_request, parse


def kind(text: str, names: tuple[str, ...] = ()) -> str | None:
    order = parse(text, lambda w: w in names)
    return order.kind if order else None


def test_the_forms_of_an_order_in_what_the_hero_says_around_them():
    assert kind("Стой!") == "halt"
    assert kind("Погоди.") == "halt" and kind("Подожди здесь.") == "wait"
    assert kind("Погоди, я спросить хотел.") is None  # words
    assert kind("Стой здесь.") == "wait"
    assert kind("За мной!") == "follow"
    assert kind("Горн, за мной!", ("горн",)) == "follow"
    assert kind("Эй, ребята, отступаем, живо!") == "retreat"
    assert kind("В атаку!") == "attack"
    assert kind("Ко мне!") == "come"
    assert kind("Оружие к бою!") == "draw"
    assert kind("Убери оружие.") == "sheathe"
    assert kind("Свободна.") == "dismiss"
    # said again and again
    assert kind("Стой, стой!") == "halt"
    assert kind("Назад! Назад! Назад!") == "retreat"
    assert kind("Бей его, бей!") == "attack"
    assert kind("Давай, бей его!") == "attack"
    assert kind("Кавалорн, на помощь!", ("кавалорн",)) == "attack"
    assert kind("Помоги!") == "attack"  # orders.py: an order only with someone to attack
    # as speech recognition may write it: run together, a letter off in a long form
    assert kind("Замной!") == "follow"
    assert kind("Оружия к бою!") == "draw"
    assert kind("Иди по своим делом.") == "dismiss"


def test_words_of_an_order_that_are_not_one():
    for text in (
        "Не стой столбом, помоги мне.",
        "Сколько это стоит?",
        "Сюда?",  # a question
        "Он крикнул мне: стой!",
        "Убил его.",
        "Стою.",
        "Иду.",
        "Стой, стой, а где Лестер?",
        "Ну блин, давай бей его, сделай что-нибудь.",  # the conversational model's
        "Помоги мне найти Лестера.",
        "Горн, за мной!",  # Горн is nobody around: a word too many
        "",
    ):
        assert kind(text) is None, text


@pytest.mark.parametrize(
    "text",
    [
        "Пойдем, бодитов, пиздеть.",
        "Ну ты идешь, нет?",
        "Ты идёшь?",
        "Давай за мной.",
        "Пошли к бандитам, разберемся с ними вместе.",
        "Иди за мной в город.",
        "Пойдёшь со мной к пещере?",
        "Кавалорн, давай, иди за мной.",
        "Ну, ты, если что, не туда идешь, ты в стену куда-то идешь. Иди за мной.",
        "Ну, ты, если что, не туда идёшь, ты в стену куда-то идёшь. Иди за мной.",
        "Ты куда пошёл? За мной!",
        "Горн, ты не туда идёшь. Пойдём.",
        "Ты не туда идёшь! Горн, за мной!",
        "Ты не туда идёшь… Ну, Горн, ты идёшь?",
        "За мной, я сказал!",
        "Пошли, говорю!",
        "Ты не туда идёшь. За мной, я сказал!",
        "Иди за мной, сказал же.",
        "За мной, я сказала!",
        "Иди за мной, я же сказал.",
        "Пошли, я же сказала!",
        "Иди за мной, сказала же.",
    ],
)
def test_follow_requests_can_have_more_words_or_be_questions(text):
    names = ("кавалорн", "горн")
    assert follow_request(text, names.__contains__)
    order = parse(text, names.__contains__)
    assert order is not None and order.kind == "follow" and order.heard == text
    assert order.form == "замной"


@pytest.mark.parametrize(
    "text",
    [
        "Куда ты идешь?",
        "Ты идешь в город?",
        "Он сказал: пойдем к бандитам.",
        "Не пойдем туда.",
        "Если пойдем, прикрой мне спину.",
        "Пойдем туда позже.",
        "Мы пойдём туда завтра.",
        "Кто пойдет со мной?",
        "Я за тобой иду.",
        "Пошли слухи, что бандиты вернулись.",
        "Пойдём ли мы завтра к бандитам?",
        "Он мне сказал. Иди за мной.",
        "Иди за мной, сказал он.",
        "Вчера Диего говорит. Пошли.",
        "Ты в стену идешь. Иди за мной, если хочешь.",
        "Иди за мной. Если что, прикрою.",
        "Мы шли по лесу, и он крикнул: за мной. А потом пропал.",
        "Я ему сказал. Пошли.",
        "Он сказал, за мной.",
        "Я ему сказала. Пошли.",
    ],
)
def test_follow_words_in_a_story_or_route_question_are_not_requests(text):
    assert not follow_request(text)
    assert kind(text) is None


def test_every_form_is_its_order():
    for k, forms in FORMS.items():
        for form in forms:
            assert kind(form[0].upper() + form[1:] + "!") == k, form
