# server/ — ИИ-сервер

Модули и их связи — [карта проекта](../docs/README.md#модули-сервера), сообщения игры — [`docs/protocol.md`](../docs/protocol.md). Общие правила — корневой [`AGENTS.md`](../AGENTS.md).

- **После правки:** `uv run pytest tests/test_<модуль>.py -q` по тестам изменённого модуля и `uv run ruff check <файлы>`. Полный `pytest` — один раз перед PR, на Ryzen: `scripts/ryzen.sh pytest`.
- **Один сценарий e2e на правку:** `SCENARIO=<имя> scripts/ryzen.sh e2e`, на Mac — `SCENARIO=<имя> LLM=fake scripts/e2e.sh`. Сценарий пройден, когда все значения результата начинаются с `done`. `LLM=chatgpt` тратит квоту подписки: только когда шаг этого требует.
- **Новая подсистема** включена по умолчанию, выключает её флаг `--no-X` в `cli.py`. Сообщения игры она получает через `link.listeners` своего `GameLink`.
- **Сообщение протокола** живёт в трёх местах: схема в `protocol.py`, мост `OpenGothic/ai_bridge/`, `docs/protocol.md`. Правка протокола поднимает версию во всех трёх: `PROTOCOL_VERSION`, `ProtocolVersion` в `aibridge.cpp`, заголовок документа. Новый номер сначала забронируй ([бронь номера](../docs/protocol.md#бронь-номера)). Иначе сервер только пишет в лог `protocol mismatch`.
- **Новый сценарий e2e:** код в модуле своей области `scenarios/<область>.py` (общее — в `scenarios/common.py`), точку входа, которую `cli.py` зовёт как `scenarios.<имя>`, добавить в `scenarios/__init__.py`, ветка и имя в `cli.py`, строка в шапке `scripts/e2e.sh` и в списке сценариев карты.
- **Память NPC и правки сюжета ветвятся с сохранениями** (`memory.py`, `story.StoryPatches`): загрузка сейва откатывает их. Новое состояние, которое должно жить в ветке сейва, кладётся туда же.
- `scripts/ryzen.sh pytest` требует `scripts/ryzen.sh setup` и работает от `game-settings` и готовой базы без установки. Тесты `test_storygraph.py` выбирают базу через `GOTHIC_DIR` и указатель либо явный `OPENGOTHIC_BASE`; только две проверки физических байтов OU требуют `GOTHIC_INSTALL=<каталог установки>` и без него пропускаются. На Ryzen его не задавай; на Mac эталон — `/Volumes/DevSSD/Dev/games/Gothic II`.
- Часть тестов читает граф сюжета игры (`story.json` в `GOTHIC_AI_DATA`) и без него пропускается: `skipped` — не провал.
