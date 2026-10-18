# Карта проекта

С этой страницы начинается любая работа в репозитории. Она говорит, где что лежит и как части связаны. Подробности — в документах систем, ссылки на них ниже. Правила работы — в [`AGENTS.md`](../AGENTS.md), у `server/`, `OpenGothic/ai_bridge/` и `shorts/` — ещё свои короткие `AGENTS.md`. Сборка и запуск — в [`README.md`](../README.md).

Карта описывает то, что есть сейчас. PR, который добавляет модуль, сервис, флаг, сценарий или документ, правит и эту страницу.

## Части и машины

```
 Mac                                         dev-ubuntu (RTX 3090)
 ┌───────────────────────────┐               ┌──────────────────────────────┐
 │ Gothic2Notr               │               │ Voxtral — распознавание речи │
 │  OpenGothic + ai_bridge   │               │ VoxCPM2 — голос (запасной)   │
 │        │ WebSocket JSON   │               │ Strands Decider 2B — решения │
 │        ▼ :47817           │   HTTP / WS   │ Gemma 4 12B — быстрые реплики│
 │ ИИ-сервер gothic-ai serve ├──────────────►└──────────────────────────────┘
 │  asr-service, tts-service │               Ryzen (3950X, RTX 3080)
 │  (клиенты к моделям)      ├──────────────►┌──────────────────────────────┐
 │ лаунчер                   │   TCP JSONL   │ body-service: политика боя   │
 └─────────────┬─────────────┘               │ P30 (ядра 0–1); тяжёлые      │
               │                             │ прогоны через scripts/gq     │
               ▼                             └──────────────────────────────┘
 подписка ChatGPT: gpt-6.1-sol (разговор), запасной ASR; Inworld TTS-2 (голос)
```

- **Игра — клиент.** Мост `OpenGothic/ai_bridge/` подключается к серверу по WebSocket, одно JSON-сообщение на кадр. Адрес по умолчанию задан в `aibridge.cpp` (`DefaultUrl`), меняется через `GOTHIC_AI_URL`, мост выключает `GOTHIC_AI=0`.
- **Версия протокола** — в трёх местах, они должны совпадать: `PROTOCOL_VERSION` в `server/src/gothic_ai/protocol.py`, `ProtocolVersion` в `OpenGothic/ai_bridge/aibridge.cpp`, заголовок [`protocol.md`](protocol.md). Новый номер берут через бронь: [`protocol.md`](protocol.md#бронь-номера).
- **Потоки движка.** WS-поток только кладёт сообщения в очередь. Разбор и вся работа с миром идут в главном потоке (`AiBridge::Impl::onTick`), входящие разбирает `AiBridge::Impl::handle`.
- **Сервер — один процесс asyncio.** `cli.ServerState` держит все подсистемы (около 45, все необязательные). Каждая получает `GameLink` из `game.py` и подписывается на сообщения игры через `link.listeners`.
- Где что считается и почему — [`compute.md`](compute.md). Адреса машин лежат вне git.

## Путь одной реплики героя

1. Герой держит клавишу разговора. `voice.py` пишет микрофон Mac и стримит звук в `asr-service`. Тот передаёт его Voxtral на dev-ubuntu, запасной вариант — подписка ChatGPT. Промежуточный текст сразу показывается в игре.
2. `talk.TalkDesk` выбирает адресата (`talk.addressee`).
3. Короткий приказ спутнику («За мной!», «Стой!») `commands.py` узнаёт по тексту, а `orders.py` исполняет без разговорной модели.
4. Быстрая реакция тела — `brain/reflex.Reflexes` через быстрый контур (около 0,1 с).
5. Ответ даёт `dialogue.DialogueManager`, правила хода — в `turn.py`. Модель — gpt-6.1-sol через `llm/openai_responses.py`. Контекст собирают `persona.py`, `memory.py`, `npc_tools.render_situation` и модули знания (ниже). В бою первые слова говорит Gemma (quick-модель), разговорная продолжает.
6. Инструменты модели — `npc_tools.TOOLS`. Их проверяет `executor.py`, а намерение превращается в действия для игры.
7. Текст режется на предложения с тегами настроения и озвучивается через `tts-service` (Inworld TTS-2, запасной — VoxCPM2). В игру уходят `say`, `say_audio`, `say_end`.
8. Движок двигает губы, лицо и руки по голосу (`common/graphics/mesh/lipsync.*`, `talkgestures.*`).
9. Ход пишется в трейс (`trace.py`, `~/.local/share/gothic-ai/traces/`). `replay.py` повторяет записанный ход на другой модели или с другим промптом без игры.

## Два контура NPC

| Контур | Модель | Что решает | Где подключён |
|---|---|---|---|
| Быстрый, поведенческий | Strands Decider 2B (`decider-service`), Gemma 4 12B для реплик в бою | реакции и позы, бегство, следующий час дня, кто вмешается в беседу, подойти ли к герою | `brain/` (`DeciderClient`, `NpcPlanner`, `Reflexes`, `GameApplier`), `--decider-url`, `--quick-url` |
| Медленный, говорящий | gpt-6.1-sol по подписке ChatGPT | что сказать, намерение, сделки, поручения, обещания | `dialogue.py`, `llm/`, `--llm`, `--model`; лимит самоинициативы — `budget.py` |

Боевое тело — третий слой: `bodypilot.py` берёт NPC в аренду (lease) у движка и ведёт его политикой P30 из `body-service`. Если сервис отвалился, бой возвращается к родному поведению движка. Подробно — [`npc-agent.md`](npc-agent.md), [`npc-body-control-plan.md`](npc-body-control-plan.md), [`npc-body-api.md`](npc-body-api.md).

## Правда и убеждения

Код владеет правдой, NPC — убеждениями. Ложь героя должна работать и иметь последствия.

- **Правда:** `chronicle.py` — факты ветки сохранения (умер, появился, квест, ложь, раскрытие, слух). Загрузка сейва откатывает их вместе с ним.
- **Убеждения NPC:** `knowledge.py` (что знает из диалогов и сюжета главы), `sightings.py` (видел и слышал сам, улики), `trust.py` (честность героя: lied, caught, kept), `social.py` (кто кого знает, слухи), `concerns.py` (заботы о герое из фактов).
- **Как дело становится известным:**
  1. `crimes.py` берёт деяние героя и настоящих свидетелей из движка.
  2. Свидетель запоминает его (`sightings.py`).
  3. Слух идёт по знакомым (`social.Rumors`); на сцене его ставит `gossip.py`, по поручению несёт `errands.py`.
  4. Закон общины считает дела из хроники ветки: законник и страж сообщают сразу, остальные свидетели — по таймеру (`crime_law.py`). Ванильные диалоги законников читают наш закон через патчи графа (`crime_law_graph.py`, `crime_law_runtime.py`). Слои дальше — в [`crime.md`](crime.md).
- **Ложь:** утверждение героя хранится вместе с правдой. Раскрыть её может улика (след, обломки), свидетель (`witnesses.ask_witness`) или сверка с фактами. Расплату ведут `amends.py` (сториллеты режиссёра) и `consequences.py` (ванильные диалоги обманутого).
- **Восприятие:** мост каждый ход шлёт `npc_view`. Из него `perception.py` делает смысл для обеих моделей, имена показываются только знакомым. Места берутся из книги мест `places.py`, ответы «где» и «знаю» дают `whereabouts.py` и `lore.py`.

## Сюжет и режиссёр

- **Граф сюжета:** конвертер `OpenGothic/ai_bridge/storygraph` поднимает `GOTHIC.DAT` в JSON. Кэш — `~/.local/share/gothic-ai/story.json`, загрузка — `gamedata/storygraph.py`. Ванильные узлы не меняются, `.DAT` не патчим.
- **Правки режиссёра** — журнал операций на ветку сохранения в SQLite-памяти (`story.StoryPatches`). Перед отправкой их проверяет `validator.py`: закрытый словарь эффектов, белый список переменных, лимиты. В игру они уходят как `graph_patch`, `story_program`, `story_npc`, `story_effect` и другие. Видимость реплик считает мост в C++.
- **Конвейер:** повод (`triggers.py`, `occasions.py`) → `showrunner.py` (бюджет, канон, хроника) → автор квеста `authoring.py` или цепочки `chains.py`, новый персонаж `casting.py` + `cast.py` → `questbook.py` (квест как объект) → `validator.py` → `story.py` → игра. Ход квестов скриптов ведёт `questlog.py`, квесты режиссёра — `quests.py`. Ремонт квестов после смерти — `director.py`. Чего инициатива NPC не вправе делать с сюжетом, решает `storyguard.py`.
- **Ваниль:** диалоги и темы живут в VM скриптов. Граф лишь выключает узел, добавляет условие или своё предложение (`vanilla.py`, `programs.py` — диалоговые функции как программы без VM).
- Подробно — [`story-graph.md`](story-graph.md), [`director.md`](director.md).

## Модули сервера

Все — в `server/src/gothic_ai/`.

| Группа | Модули |
|---|---|
| Вход и связь | `cli` (точка входа, `ServerState`), `game` (WebSocket), `protocol` (pydantic-схемы), `trace`, `replay`, `journal` (журнал для человека) |
| Разговор | `dialogue`, `talk`, `turn`, `voice`, `tts`, `persona`, `memory` (SQLite, ветвится с сейвами), `budget`, `npc_tools`, `executor`, `commands`, `orders`, `group` (общая беседа), `interject`, `llm/` (`chatgpt`, `openai`, `local`, `fake`) |
| Быстрый контур и тело | `brain/` (`decider`, `planner`, `reflex`, `day`, `apply`, `tiers`, `stimuli`, `incident`, `situation`), `incidents`, `daylife`, `initiative`, `bodies` (исход мягких действий), `bodypilot` (боевое тело P30), `beasts` (волки слышат тон), `wrath` (терпение к оскорблениям) |
| Восприятие и знание | `perception`, `sightings`, `places`, `lore`, `whereabouts`, `knowledge`, `social`, `gossip`, `trust`, `attitudes`, `concerns` |
| Правда, дела, ложь | `chronicle`, `crimes`, `crime_law`, `crime_law_graph`, `crime_law_runtime`, `witnesses`, `quests`, `vanilla`, `consequences`, `amends` |
| Сделки и поручения | `deals` (окно сделки, долги), `contracts` (опыт, обучение, платные задания), `chests`, `errands` (`plan`), `meetings` (`meet`), `guide` (`lead`) |
| Сюжет | `story`, `validator`, `storyguard`, `programs`, `director`, `questbook`, `questlog`, `triggers`, `occasions`, `showrunner`, `authoring`, `casting`, `cast`, `characters` (карточки характера), `chains` |
| Данные | `gamedata/`: `characters.json`, `lore.json`, `places.json`, `stash.json`, `ou.py` (реплики OU.BIN), `storygraph.py` |
| Сценарии e2e | `scenarios/`: общее в `common.py`, поддельный Responses WS/HTTP-сервер — в `responses_server.py`; модуль каждой области — в списке «Сценарии по областям» ниже; точки входа — в `__init__.py` |

## Движок: наши подсистемы

`OpenGothic/` — форк Try/OpenGothic. Сторонние библиотеки лежат в `lib/` обычными файлами ([`VENDORED.md`](../OpenGothic/lib/VENDORED.md)). Целиком наше: `ai_bridge/`, `game/bodyrunner.cpp`, `tests/*_contract.cpp`. В `common/` наши `physics/`, части `game/` и `graphics/mesh/`, `testbench.*`, `nav*`; остальное — апстрим с точечными правками. Эмуляция Ikarus/LeGo апстрима (`common/game/compatibility/`, кроме `phoenix.*`) удалена; при слиянии апстрима конфликт `modify/delete` в этой папке решается сохранением удаления.

Всё ниже включено по умолчанию. Выключатели `OPENGOTHIC_*=0` нужны для сравнений. Каждый выключатель с модулем и проверкой — в [`features.md`](features.md).

| Подсистема | Где | Выключатель | Документ |
|---|---|---|---|
| Мост к серверу | `ai_bridge/aibridge.cpp`; NPC — `npccontroller`, `npcview`, `npcchests`; сюжет — `story*`, `dialoggraph`; голос — `voicebuffer`, `linelibrary`; `beasts`, `necromancy` | `GOTHIC_AI=0` | [`protocol.md`](protocol.md) |
| Физика Jolt вместо Bullet: тела, рэгдоллы, CharacterVirtual, вода, разрушение | `common/physics/` (`collisionworld`, `dynamicworld`, `ragdoll`, `fracture`, `cloth`, `worldwind`), `common/world/movablevobs.*` | ini `[GAME]`: `ragdoll`, `movable_vobs`, `destruction`, `buoyancy`, `character_virtual` и другие; кэш разрушений `OPENGOTHIC_FRACTURE` | [`physics.md`](physics.md) |
| Ткань мира рядом с героем: mesh-vob и полотнища уровня | `common/physics/cloth.*`, `clothgeometry.*`, `worldcloth.*`, `common/graphics/clothvisual.*`, `common/world/portalrooms.h`; таблицы `data/cloth-vobs.csv`, `cloth-world.csv` | `[GAME] cloth=0`, Marvin `toggle cloth` | [Перепись и проверки](../reports/physics/2026-10-18-cloth/validation-notes.md#шаг-1) |
| Анимация: инерциальные переходы, foot IK, моргание, жесты, губы, взгляд, наклон, вздрагивание, натяжение лука, ходьба с прицелом | `common/graphics/mesh/` (`animblend`, `footik`, `eyeblink`, `talkgestures`, `lipsync`, `gaze`, `lean`, `flinch`, `bowdraw`, `aimwalk`, `aim`) | `OPENGOTHIC_ANIBLEND`, `_FOOTIK`, `_BLINK`, `_GESTURES`, `_LIPSYNC`, `_GAZE`, `_LEAN`, `_FLINCH`, `_BOWDRAW`, `_AIMWALK`, `_AIM` | — |
| Ручной прицел героя и лучники NPC | `common/game/archery.cpp`, `aimcamera.h`, `archerai.*` | `OPENGOTHIC_MANUALAIM`, `OPENGOTHIC_NPCAIM` (или ini `[GAME] manualAim`, `npcManualAim`) | [`manual-aim.md`](manual-aim.md) |
| Боевое тело NPC: аренда, наблюдения, арена, реплей | `common/game/bodycontroller.*`, `bodyobservations.*`, `bodyarena.*`, `bodyreplay.*` | без `--body-url` на сервере не работает | [`npc-body-api.md`](npc-body-api.md) |
| Headless-раннер для обучения тела | `game/bodyrunner.cpp` → `gothic-body-runner` (stdio RPC) | — | [`npc-body-headless-runner.md`](npc-body-headless-runner.md) |
| Навигация NPC: обучаемая — пока только на стенде; популяция N NPC, эпизоды без перезагрузки, выборка и награды | `common/nav*`, `common/game/nav*`; `scripts/nav_rl.py` — пары, сбор, финальный замер, проверки | `navdemos population`: `sample/argmax`, `RECORD=0` выключает запись; `navdemos physics cv` закрепляет CV для ученика и учителя (`nav_rl.py` по умолчанию); обычная игра не включает режим | [`navigation.md`](navigation.md) |
| HD-текстуры | `common/resources.cpp` | работают, если есть каталог `OPENGOTHIC_TEXTURES` | — |
| Тестовый стенд | `common/testbench.*`, `OPENGOTHIC_BENCH*` | — | [`scripts/bench/README.md`](../scripts/bench/README.md) |

Консоль для агентов: `ai status | attach <npc> | detach | dialogs <npc> | ptt start|stop|cancel` (в `aibridge.cpp`). Флаги командной строки игры — `common/commandline.cpp` (`-nointro`, `-save`, `-window`, …).

## Сервисы

| Сервис | Что | Где работает | Как сервер его находит |
|---|---|---|---|
| `asr-service/` | распознавание речи: Voxtral (audio.cpp), запасной — подписка ChatGPT | клиент на Mac, модель на dev-ubuntu | `--asr-url`, `GOTHIC_ASR_URL` |
| `tts-service/` | голос NPC: Inworld TTS-2, запасной — VoxCPM2 (audio.cpp) | клиент на Mac, VoxCPM2 на dev-ubuntu | `--tts-url`, `GOTHIC_TTS_URL` |
| `decider-service/` | быстрые решения NPC: Strands Decider 2B, полосы urgent и background | dev-ubuntu | `--decider-url`, `GOTHIC_DECIDER_URL` |
| `server/deploy/gemma/` | Gemma 4 12B (llama.cpp): реплики в бою, первые слова | dev-ubuntu | `--quick-url`, `GOTHIC_QUICK_LLM_URL` |
| `body-service/` | политика боя P30 (GRU), обучение BC и PPO | сервис — Ryzen, ядра 0–1; обучение — Ryzen и dev-ubuntu | `--body-url`, `GOTHIC_BODY_URL` |
| `launcher/` | `core/` поднимает сервисы и игру одной командой; `app/` — окно (Tauri, Svelte) | Mac | — |
| `libs/chatgpt-auth/` | вход через ChatGPT (OAuth PKCE), общее хранилище токенов | Mac | — |

Настройки сервисов — `.env` по образцу [`.env.example`](../.env.example). Развёртывание GPU-сервисов — `scripts/deploy-gpu.sh`.

## Флаги `gothic-ai serve`

Всё включено по умолчанию, флаг `--no-X` выключает. Полный список — `gothic-ai serve --help` или `cli.py` около строки 2200. Что выключает каждый флаг, где живёт фича и что её проверяет — в [`features.md`](features.md).

- **Голос и вывод:** `--no-voice`, `--no-tts`, `--no-console`, `--no-trace`.
- **Модели:** `--llm chatgpt|openai|local|fake`, `--model`, `--reasoning-effort`, `--no-websocket`, `--no-brain` (планировщик быстрого контура), `--no-quick`, `--no-hold-threats`.
- **Мир NPC:** `--no-wrath`, `--no-apply-threats`, `--no-beasts`, `--no-chests`, `--no-lead`, `--no-meet`, `--no-errands`, `--no-interject`, `--no-free-bandits`, `--no-apply-day`, `--no-knowledge`, `--no-crimes`.
- **Сюжет:** `--no-director`, `--no-questlog`, `--no-occasions`, `--no-showrunner`, `--story-graph PATH`.
- **Тело:** `--body-url`, `--no-neural-body`.
- **Сценарий:** `--scenario <имя>`, `--exit-after`.

## Тесты и прогоны

| Что | Как |
|---|---|
| pytest одного модуля | `cd server && uv run pytest tests/test_<модуль>.py -q`; на Ryzen — `scripts/ryzen.sh pytest tests/test_<модуль>.py` |
| Один сценарий e2e | `SCENARIO=<имя> scripts/ryzen.sh e2e`; на Mac — `SCENARIO=<имя> LLM=fake scripts/e2e.sh` |
| Бенч механики | `scripts/bench.sh <имя>` → `.bench/<имя>-<время>/` (видео, метрики, отчёт) |
| Очередь Ryzen | `scripts/gq status` |
| Сборка игры | `scripts/build-engine.sh` → `OpenGothic/build/opengothic/Gothic2Notr` |
| Контрактные тесты движка | цели `gothic-*-contract` в `OpenGothic/CMakeLists.txt`, исходники в `OpenGothic/tests/` |

Сценарий считается пройденным, когда все значения результата начинаются с `done`. Описание каждого — в шапке `scripts/e2e.sh`. Стенды ткани `cloth`, `cloth-orc`, `cloth-city`, `cloth-harbour` проверяют самостоятельные и извлечённые из уровня полотнища; `cloth-arrow-npc` проверяет пересечение ткани и NPC одним отрезком стрелы в обоих порядках; `cloth-visibility` проверяет заморозку вне камеры и плавный возврат; `cloth-on/off` измеряют плотное скопление, `cloth-reload` и `cloth-legacy-load` проверяют загрузку; приёмка включает извлечённые из уровня паруса, корабельные флаги и городские знамёна. Тесты сервисов лежат в их `tests/`, тесты скриптов — в `scripts/tests/`.

**Сценарии по областям** (модуль в `server/src/gothic_ai/scenarios/`):
- **основа** (`basics.py`): `m0`–`m6`, `m5b`, `m5c`, `m5e`, `tts`, `websocket`;
- **спутники и бой** (`companions.py`): `orders`, `companion`, `calm`, `wolf`, `rescue`, `rescue_self`, `neural`, `neural-companion`;
- **восприятие** (`perception.py`; `broken` — `broken.py`, `places` — прямо в `cli.py`): `see`, `seen`, `hearing`, `cave`, `ground`, `places`, `knows`, `remembers`, `trace`, `broken`;
- **жизнь NPC** (`npc_life.py`): `day`, `noise`, `initiative`, `lead`, `meet`, `meet_missed`, `errand`, `errand_fork`, `errand_far`, `errand_give`, `chests`, `chests_lobart`;
- **беседа** (`conversation.py`): `group`, `interject`, `gossip`, `gossip_demo`, `cards`;
- **сделки** (`deals.py`): `deal`, `deal_demo`, `debt`, `contract`, `haggle`;
- **ложь и дела** (`lies.py`; `crimes`, `crime_talk`, `crime_law`, `crime_director` — одноимённые модули): `lie`, `vlie`, `vgive`, `vcheck`, `rumor`, `witness`, `amends`, `amends_deal`, `crimes`, `crime_talk`, `crime_law`, `crime_director`, `wrath`;
- **стыки систем** (`trace_crime.py`, `quest_law.py`): `trace_crime` — свежие обломки и показание о краже в одном ходе, две поимки лжи, знание о деле и доверие; `quest_law` — квест книги на краденую вещь, сохранение ветки и независимый штраф города;
- **сюжет** (`story.py`; `occasions`, `questlog`, `showrunner*`, `vanilla_line*` — одноимённые модули): `graph`, `programs`, `repair`, `authored`, `own`, `own_repair`, `trigger`, `stages`, `requires`, `chain`, `cast`, `person`, `character`, `occasions`, `questlog`, `showrunner`, `showrunner_live`, `vanilla_line`, `vanilla_line_live`.

## Где лежат данные

- `~/.local/share/gothic-ai/` — рабочие данные:
  - `game/` — сохранения, `Gothic.ini`, `log.txt`;
  - `memory.sqlite3` — память NPC и правки сюжета;
  - `traces/`, `story.json`, `saves/original`, `hd-textures/`, `fracture/`, `voices/`, `music/`, `brand/`, `explain/`, `body/`.
- Логи e2e — в `LOG_DIR`, по умолчанию временная папка `gothic-e2e.*`.
- На Ryzen — `~/gothic-ai/<задача>/`.
- Отчёты прогонов — `reports/<область>/<дата-тема>/`.

## Остальные каталоги

- **`shorts/`** — шортсы из игры с живыми NPC: съёмка, монтаж, объясняющие ролики, сравнения «до и после», бренд ([`shorts/README.md`](../shorts/README.md)). Сравнения собирает `gothic_shorts.compare`, точка входа — `shorts/compare/cut.py`; `gothic-shorts dub` распознаёт их планы и берёт папку дублей из `takes` или `--takes DIR`. Обложки шаблона B — модуль `gothic_shorts.cover`, команда `cover` и поле `cover` плана; загрузка на YouTube с `--thumbnail`, отдельная команда `thumbnail`.
- **`site/`** — сайт и дневник на GitHub Pages ([`site/README.md`](../site/README.md)).
- **`evals/`** — эталоны: `npc-body/` (бой), `npc-decisions/` (выбор модели решений).
- **`reports/`** — итоговые исследования и отчёты прогонов.
- **`research_notes/`** — заметки исследований моделей и провайдеров.

## Документы

«Сейчас» в таблице — документ начинается с раздела «Как работает сейчас»: что система делает, модули, как выключить, чем проверяется, ограничения. Ниже в нём — «План и журнал».

| Документ | О чём | Что это |
|---|---|---|
| [`protocol.md`](protocol.md) | протокол игра ↔ сервер, версии и бронь номера | справочник |
| [`features.md`](features.md) | фичи, включённые по умолчанию: выключатели, модули, проверки, пробелы покрытия | реестр |
| [`compute.md`](compute.md), [`ryzen.md`](ryzen.md) | машины, очередь `gq`, прогоны на Ryzen | справочник |
| [`npc-agent.md`](npc-agent.md) | агент NPC: два контура, приказы, восприятие | сейчас + справочник с журналом |
| [`npc-tools.md`](npc-tools.md) | инструменты NPC: сделки, восприятие, карточки, беседа, поручения | сейчас + план с журналом |
| [`contracts.md`](contracts.md), [`beasts.md`](beasts.md), [`xardas-wrath.md`](xardas-wrath.md) | поручения и награды; волки; гнев Ксардаса | справочники |
| [`story-graph.md`](story-graph.md) | граф сюжета, ложь, слухи, ремонт квестов | сейчас + план с журналом |
| [`director.md`](director.md) | режиссёр-автор: квесты, персонажи, цепочки | сейчас + план с журналом, этапы 1–5 сделаны |
| [`crime.md`](crime.md) | преступления и репутация | сейчас + план, идёт: слои 1–3 и 5 сделаны |
| [`physics.md`](physics.md) | Jolt, рэгдоллы, разрушение, вода, ткань | сейчас + план с журналом |
| [`manual-aim.md`](manual-aim.md) | ручное прицеливание | сейчас + план с журналом |
| [`navigation.md`](navigation.md) | обучаемая навигация NPC; [популяционный стенд на CV и независимый набор 60 пар](../reports/navigation/2026-10-18-rl-env/README.md) | сейчас + план, идёт |
| [`npc-body-control-plan.md`](npc-body-control-plan.md) | боевое тело: план этапов 1–10 | сейчас + план |
| `npc-body-*.md` (`api`, `arena`, `bc`, `combat-contract`, `headless-runner`, `observations`, `trajectories`) | боевое тело по этапам | справочники |
| [`npc-body-combat-gate.md`](npc-body-combat-gate.md), [`npc-body-stage8-stop-report.md`](npc-body-stage8-stop-report.md) | этап 8: журнал до допуска и остановка 14.10 | исторические: gate пройден 15.10, итог в плане |
| [`npc-scheduler.md`](npc-scheduler.md) | планировщик решений NPC | проект от 11.10; шаги 1–3 сделаны, планировщик работает в `brain/` |
| [`launcher.md`](launcher.md) | лаунчер | этапы L1–L3 сделаны, лаунчером пользуются |
| [`MVP.md`](MVP.md) | первая спецификация MVP | исторический |
