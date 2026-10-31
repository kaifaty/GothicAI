# OpenGothic/ai_bridge/ — мост игры к ИИ-серверу

Что где в мосте — [карта проекта](../../docs/README.md#движок-наши-подсистемы), сообщения — [`docs/protocol.md`](../../docs/protocol.md), граф сюжета — [`docs/story-graph.md`](../../docs/story-graph.md). Общие правила — корневой [`AGENTS.md`](../../AGENTS.md).

- **После правки:** сборка `scripts/build-engine.sh` и один сценарий e2e, который задевает правку (`SCENARIO=<имя> scripts/ryzen.sh e2e`; на Ryzen игра собирается под Linux тем же кодом). Конвертер `storygraph/` собирается отдельно, команды и режимы `--check` — в `docs/story-graph.md`.
- **Предупреждения — ошибки:** `AiBridge` собирается с `-Wall -Wconversion -Werror`. Apple clang на Mac и gcc на Ryzen ловят разное: код, собранный на одной машине, может не собраться на другой.
- **Потоки:** поток WebSocket только кладёт сообщения в очередь. Разбор и вся работа с миром — в главном потоке, в `Impl::onTick`, входящие разбирает `Impl::handle`. Мир из потока WebSocket не трогай.
- **Ответы идут только в тике мира.** Во время видео и паузы мост молчит: сервер не получит ответа, пока игра не вернётся в мир.
- **Тексты игры — Windows-1251, протокол — UTF-8.** Переводит `textcodec`. Ё, тире и кавычки-ёлочки шрифт игры не рисует: `fromUtf8` подставляет похожие знаки.
- **Правка протокола поднимает версию в трёх местах:** `ProtocolVersion` в `aibridge.cpp`, `PROTOCOL_VERSION` в `server/src/gothic_ai/protocol.py`, заголовок `docs/protocol.md`. Новый номер сначала забронируй ([бронь номера](../../docs/protocol.md#бронь-номера)). Схему сообщения правь там же, в `protocol.py`.
- **Отладка в игре:** консоль `ai status | attach <npc> | detach | dialogs <npc> | skip <npc> | ptt start|stop|cancel`.
