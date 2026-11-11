"""Timing, prompts and limits shared by the dialogue modules."""

import json
from pathlib import Path

CHAT_TEXT_EVERY = 0.06  # seconds between chat_text updates while a reply streams in
TTS_PAUSE = 30.0  # after a failure, lines go out silent for a while instead of each waiting to fail
PREPARE_S = 3.0  # what an extension asks the game before a turn
KILLERS = frozenset({9})  # GIL_BDT: bandits attack the hero to kill, as their scripts do
# an intent's attitude -> the scripts' attitude to the hero of one that called off its fight (effect calm)
CALM_ATTITUDES = {
    "devoted": "ATT_FRIENDLY",
    "friendly": "ATT_FRIENDLY",
    "neutral": "ATT_NEUTRAL",
    "wary": "ATT_NEUTRAL",
}
FIGHTS_HERO = (
    "Ты дерёшься с героем. Прекратить драку — set_intent с целью none или keep_routine и отношением не "
    "hostile; продолжать — намерение не меняй. Решай по своему характеру и по тому, что он говорит."
)
# the hero's phrase: the behavioral model's gesture in the pause is waited for no longer than this (~50 ms)
FIRST_REACTION_S = 0.25
# ...and the quick model's first words (~0.3 s warm, ~1 s cold): the answer starts after them and goes on
# from them, so a late one is not waited for
REACTION_S = 1.2
# first words longer than this are an answer of their own (Gemma does it now and then): not said, else the
# conversational model's answer would follow a second one
REACTION_WORDS_MAX = 8
# a model call that goes quiet: no answer this long after the request, or no more of it this long; such a
# step is asked again once per turn if the npc has said nothing of it yet (a stalled stream would otherwise
# leave the npc "thinking" for the 10 minutes of the HTTP read timeout)
LLM_FIRST_S, LLM_IDLE_S, STEP_RETRIES = 15.0, 15.0, 1
# the quick model of a fight: a line not started this long after the request (~1.2 s with a cold cache)
# is the conversational model's after all
QUICK_FIRST_S, QUICK_IDLE_S = 3.0, 2.0
QUICK_HINT = "Сейчас горячо: ответь сразу, одной-двумя короткими фразами."
SURRENDER_CLASSIFIER = (
    "Ты классифицируешь реплики игрока в бою в ролевой игре. Ответь «да», если игрок сам сдаётся, "
    "признаёт поражение или просит пощады/не бить его. Ответь «нет», если он угрожает, "
    "требует сдаться противника, спрашивает или говорит что-то другое."
)
# the quick model's first words to the hero's phrase; "—": nothing (the answer is quick itself)
REACT_HINT = (
    "Герой только что это сказал. Отреагируй вслух сразу, одной короткой фразой до восьми слов, "
    "как вырвалось бы само. Зацепись за главное в его словах: переспроси это, оцени или покажи, "
    "как ты к этому относишься. Ответ по существу скажешь следом: не начинай его и ничего не обещай. "
    "На приветствие и прощание, или если сразу сказать нечего, напиши только «—»."
)
ACK_HINT = (
    "Ты слышишь приказ героя (выше) и уже выполняешь его. Откликнись вслух на ходу одним-двумя словами "
    "(«Есть!», «Иду!», «Стою.»)."
)
SOUND_SAID = "\nТы уже сказал вслух: «{}» — ответ продолжает это, не повторяй его."
PARALLEL_ACT_HINT = (
    "Твою реплику уже произносят вслух. Не пиши текст: если нужно действие, сразу вызови инструмент; "
    "если делать ничего не нужно, ответь пустым сообщением."
)
PREWARM_DELAY_S = 0.1  # notes come in bursts: the quick model's cache is filled once they are in
TALK_WARM_S = 300.0  # an npc the hero spoke to this recently is kept warm in the quick model's cache
PREWARM_CM = 800.0  # a new npc the hero looks at this close is warmed before he speaks
PREWARM_SLOTS = 3  # the quick model's concurrent prompt-cache slots
# a server slot past those, when the quick server has one, holds only the surrender classifier's prompt:
# switching a slot from an npc's prompt makes llama.cpp copy its state to RAM first, up to 2 s
CLASSIFIER_SLOT = PREWARM_SLOTS
SVM_REPLY_S = 2.0  # from svm_line arrival to the first complete sentence; late lines stay silent
SVM_WORDS_MAX = 12
SVM_QUIET_S = 3.0
SVM_RECOVER_S = 30.0
SVM_HINT = (
    "[Сейчас: {event}. В этот момент обычно говорят: «{text}» — эти слова не повторяй. "
    "Скажи то же по смыслу по-своему, одной короткой фразой — как сказал бы ты, {to}.]"
)
SVM_EVENTS = json.loads((Path(__file__).parent / "gamedata" / "svm_events.json").read_text(encoding="utf-8"))
SUMMARY_FIRST_S = 60.0  # the summary is not waited for by anyone: more patience
MAX_HISTORY = 40  # messages in the prompt; older ones are folded into a summary
KEEP_RECENT = 16  # messages left verbatim after folding
# two npc's talking to each other (play_scene): the gap between their voices (the bridge breathes 250 ms
# after a line itself), and how long a sent line may take to start (a turn, a walk, a dialog; the game
# never starts one far from the hero) before the scene ends
LINE_LOST_S = 20.0  # a line sent this long ago that has not started is not coming
SCENE_GAP_S = 0.35
SCENE_START_S = 8.0
QUIET_STYLE = "say quietly, in a low hushed voice, as if not to be overheard"

SUMMARY_INSTRUCTIONS = (
    "Ты ведёшь память персонажа {name} из игры Gothic 2. Тебе дают прежнюю сводку и новые записи: его "
    "разговоры с героем и то, что он слышал о герое от других. Напиши обновлённую сводку от второго лица "
    '("ты" — это {name}), не длиннее 1200 символов: кто такой герой и что он о себе рассказал, о чём '
    "просил, что ты обещал и что отдал, как ты к нему относишься, важные события и когда они были. Только "
    "факты из записей, ничего не выдумывай. Выведи только текст сводки."
)
