# Архитектурное исследование и проект модкита GothicAI

> **Срез кодовой базы:** `main` (`518fef195`, 2026-11-28, после PR #1786 `native-v3-cutover`, PR #1789 `native-v3-contracts`, PR #1853–#1866).  
> **Решения пользователя (2026-11-28):**
> - Оформить исследование и план в репозитории перед реализацией.
> - Начать с **Phase 0**: подключить Luau в CMake и измерить стоимость границы, `interrupt` и память на `lua_State` через `gothic-mod-bench` на Mac и Ryzen.
> - Для UI крафта в v0.1 сразу делать **отдельный минимальный нативный `CraftingMenu`** со списком рецептов и ингредиентов (не перегружая `DialogMenu::isMobsiDialog()`).

---

## 0. Главный вывод

Для GothicAI оптимальна **двухфазная архитектура модов в духе Factorio / Alan Wake 2 (Northlight)**:

1. **Data Stage (декларативный слой):** текстовые JSON-манифесты и определения контента (`items`, `recipes`, `stations`, `loot`, `trades`, `dialogs`, `quests`, визуалы поверх `OPENGOTHIC_CONTENT`) с пространствами имён `mod_id:name`, графом зависимостей и явными патчами.
2. **Runtime Stage (императивное поведение):** **upstream Luau** (встроенный C++17, режим интерпретатора на старте) с **изолированным `lua_State` на каждый мод** (модель Factorio, а не общий VM как в Roblox), жёсткими бюджетами CPU (`lua_Callbacks::interrupt`) и памяти (`lua_Alloc`), **транзакционным Command API** (`world.transaction` + ключи идемпотентности) и **явным версионированным состоянием без сериализации корутин**.
3. **Граница с ИИ-сервером:** вся детерминированная игровая логика модов исполняется **внутри C++ движка (`ModHost` в слое `rpg`)** и на 100% работает при `GOTHIC_AI=0`; в Python-сервер `gothic-ai` проецируются только декларативные метаданные (описания предметов/квестов для LLM) и структурированные факты (`mod_catalog` / `mod_fact`).

### 4 критических факта из `main`, определяющих архитектуру

| Факт в коде (`518fef195`) | Следствие для модкита |
|---|---|
| **1. Daedalus VM уже вырезан из игры** (PR #1786, PR #1789; [`OpenGothic/CMakeLists.txt`](../../../OpenGothic/CMakeLists.txt)) | `GOTHIC_NATIVE_V3=1` включён в `Gothic2Notr` и `gothic-body-runner`, `common/daedalus` — `EXCLUDE_FROM_ALL`, `BaseStore` отклоняет старые `.DAT`-моды. Модкит строится чисто поверх v3-каталога и `GameRules`. |
| **2. Строгое разделение слоёв `platform` → `rpg` → `gothic` → `bridge`** ([`scripts/engine_layers.py`](../../../scripts/engine_layers.py), [`OpenGothic/common/game/gamerules.h`](../../../OpenGothic/common/game/gamerules.h)) | `ModHost` должен жить в слое `rpg` (или подслое, зависящем только от `platform` + `rpg`), чтобы работать для Хориниса, самостоятельного «Острова» (`island/`) и безоконного `gothic-mod-runner`. |
| **3. Два контура состояния: C++ движок и Python `gothic-ai`** ([`OpenGothic/ai_bridge/aibridge.cpp`](../../../OpenGothic/ai_bridge/aibridge.cpp), [`server/src/gothic_ai/story.py`](../../../server/src/gothic_ai/story.py)) | ИИ-сервер ведёт ветвящуюся SQLite-память (`memory.sqlite3`: хроника, убеждения, преступления, режиссёр), связанную с сейвом через ZIP-секцию `ai/memory`. Нужна жёсткая матрица единственного владельца каждого домена. |
| **4. 10k-симуляция `society-core` изолирована в чистом C++** ([`OpenGothic/common/society/README.md`](../../../OpenGothic/common/society/README.md)) | Нельзя вызывать Luau на каждый тик 10 000 дальних NPC. Моды влияют на дальнюю симуляцию через декларативные правила и редкие доменные события. |

---

## 1. Карта актуальных подсистем (существующие возможности и пробелы)

> В этом разделе описано только **текущее состояние кода** на срезе `518fef195`.

| Подсистема | Файлы в коде | Что уже расширяемо данными | Пробелы для модкита |
|---|---|---|---|
| **Визуальные ассеты** | `OPENGOTHIC_CONTENT`, [`docs/own-content.md`](../../../docs/own-content.md), [`docs/content-delivery.md`](../../../docs/content-delivery.md) | Подмена текстур, GLB-мешей, голов, VOB-слоёв поверх базы (`materials.json`, `meshes.json`, `characters.json`, `vobs.d/*.json`) | Плоский каталог; нет стека модов, приоритетов, namespace и регистрации новых архетипов |
| **Определения предметов и базовые эффекты** | [`itemdefinitions.h`](../../../OpenGothic/common/itemdefinitions.h), [`itemdefinitionruntime.h`](../../../OpenGothic/common/game/itemdefinitionruntime.h), [`gamescript_items.cpp`](../../../OpenGothic/common/game/gamescript_items.cpp), [`gamescript_itemroutines.cpp`](../../../OpenGothic/common/game/gamescript_itemroutines.cpp) | `items.json` + таблицы `restoreEffects`, `fullRestoreEffects`, `permanentAttributeEffects`, `armorSetEffects`, `fightSkillEffects`, `equipmentStatEffects`, `documents` | Привязаны к единой v3-базе и SHA `stockProgram`; особые предметы зашиты в `applyQuestEquipmentEffect`; неизвестный `callback` бросает `std::runtime_error` ([`gamescript.cpp`](../../../OpenGothic/common/game/gamescript.cpp)) |
| **Экземпляр предмета и инвентарь** | [`itemrecord.h`](../../../OpenGothic/common/world/records/itemrecord.h), [`item.h`](../../../OpenGothic/common/world/objects/item.h), [`item.cpp`](../../../OpenGothic/common/world/objects/item.cpp), [`inventory.cpp`](../../../OpenGothic/common/game/inventory.cpp) | — | **Нет свойств экземпляра** (качество, износ, температура). `Inventory::addItem` всегда сливает стопки по `findByClass(cls)`. Все операции (`delItem`, `transfer`, `use`, `equip`, `buyItem`, `sellItem`) и `InventoryMenu` адресуют предметы по `size_t cls`, а не по экземпляру |
| **Крафт и MOBSI** | [`interactive.h`](../../../OpenGothic/common/world/objects/interactive.h), [`interactive.cpp`](../../../OpenGothic/common/world/objects/interactive.cpp), [`dialogmenu.cpp`](../../../OpenGothic/common/ui/dialogmenu.cpp) | `Interactive::invokeStateFunc` → `useInteractive` → `StoryGraph` + `DialogMenu::isMobsiDialog()` | Нет реестра рецептов, транзакций крафта и специализированного UI станций (`CraftingMenu`) |
| **Торговля и трофеи** | [`npc_inventory.cpp`](../../../OpenGothic/common/world/objects/npc_inventory.cpp), [`inventorymenu.cpp`](../../../OpenGothic/common/ui/inventorymenu.cpp), [`tradeinventorydata.h`](../../../OpenGothic/common/game/definitions/tradeinventorydata.h) | Таблицы стока (`tradeinventorydata`, `deathtrophydefinitions`) | `buyItem`/`sellItem` используют фиксированный `tradeValMult` без хуков цены/запрета и без атомарного отката |
| **Сюжет и диалоги** | [`nativeexec.h`](../../../OpenGothic/ai_bridge/storygraph/nativeexec.h), [`gamescript_storyhost.cpp`](../../../OpenGothic/common/game/gamescript_storyhost.cpp), [`storyruntime.cpp`](../../../OpenGothic/ai_bridge/storyruntime.cpp), [`storyeffects.cpp`](../../../OpenGothic/ai_bridge/storyeffects.cpp) | `NativeInterpreter` исполняет 11 047 стоковых программ и патчи AI-режиссёра | Закрытый словарь `StoryEffect` (17 видов), лимит `directorCapacity = 1024`, плоский `NativeValue` без структур/таблиц/модулей |
| **Сохранения (v57)** | [`savearchive.h`](../../../OpenGothic/common/savearchive.h), [`gamesession.cpp`](../../../OpenGothic/common/game/gamesession.cpp), [`scriptvars.h`](../../../OpenGothic/common/game/definitions/scriptvars.h) | ZIP (miniz) с именованными секциями + `saveExtFn` | `WorldObjects::npcId/itmId/mobsiId` ([`worldobjects.cpp`](../../../OpenGothic/common/world/worldobjects.cpp)) — линейный поиск по индексу вектора; нет секций `mods/` и `registry_map` |
| **Реакции NPC, преступления, бой** | [`gamescript_rules.cpp`](../../../OpenGothic/common/game/gamescript_rules.cpp) (172 КБ), [`damagecalculator.cpp`](../../../OpenGothic/common/game/damagecalculator.cpp), серверные `crimes*.py` | — | Зашито в C++ и Python-сервере |

---

## 2. Выбор среды исполнения

| Критерий | **Upstream Luau** *(выбран)* | **Rust → Wasm → Wasmtime** | **AssemblyScript → Wasmtime** | **SLua (Second Life)** |
|---|---|---|---|---|
| **Сборка CMake (macOS/Linux/Win)** | Чистый C++17 (`Luau.VM`, `Luau.Compiler`, `Luau.Ast`), ~1–2.5 МБ release, 0 новых тулчейнов | Тянет Rust/Cargo в CMake, `libwasmtime.a` ~15–30 МБ release (100–250 МБ debug), OS-специфичные системные либы | Как Wasmtime + Node/npm у моддера | Сторонний форк с собственным diff (`Ares`/Eris) |
| **Цикл человека и кодинг-агента** | Текстовые `.luau` в Git, без шага сборки, `--!strict` + `luau-analyze` + `luau-lsp` по `.d.luau`, reload <50 мс | `cargo build` на правку, бинарные `.wasm`, borrow checker | «Ложный TS»: нет полноценных замыканий/динамики/npm, постоянные ошибки агентов | Как Luau, но привязка к форку |
| **Граница API** | Прямой C-стек, нативные `vector` и `buffer`, нет линейно-памятного маршалинга строк | ~30–50 нс базовый переход + маршалинг строк/структур через линейную память (C++ WIT всё ещё сырой) | Ручной ABI + свой GC внутри линейной памяти Wasm | Как Luau |
| **Изоляция и бюджеты** | `luaL_sandbox`, запрет сырого байткода, `lua_Callbacks::interrupt` на back-edges, лимит RAM через `lua_Alloc` | Линейная память + детерминированный `Fuel` | Как Wasmtime, но паузы внутреннего GC | Как Luau + обход кучи |
| **Сохранения и обновления** | Явное версионированное состояние + миграции (модель Factorio) | То же | То же | Сериализация корутин хрупка при обновлении кода мода |
| **Горячие вычисления** | Интерпретатор (~5–15× медленнее C++); есть опциональный Luau CodeGen (x64/ARM64) | Близко к C++ (Cranelift JIT) | Быстро на скалярах, медленнее на аллокациях | Как Luau + JIT |

### Зафиксированные решения по рантайму
- **Один рантайм для v1:** upstream **Luau** (режим интерпретатора в v0.1; без генерации исполняемого кода `W^X`, с опциональным бенчмарком `Luau.CodeGen` в Phase 0 как справочной точкой для Phase 4).
- **Изоляция: 1 `lua_State` = 1 мод** (модель Factorio): отдельный `lua_Alloc` (жёсткий потолок RAM на мод), отдельный GC, чистая выгрузка/перезагрузка одного мода, межмодовое общение только через типизированные события и интерфейсы `ModHost`.
- **Без сериализации корутин (SLua не берём):** моды хранят только явные сериализуемые данные (`state`) и декларативные C++-таймеры; обновления версий проходят через явные функции `migrations`.

---

## 3. Архитектура ModHost и Mod SDK (предложение)

### 3.1. Слои и владение состоянием

- **Слой размещения:** `ModHost`, `ContentRegistry`, `EntityId/ItemUid`, `Transactional Command Bus` и `Gameplay Event Bus` размещаются в слое **`rpg`** (не зависят от `gothic` и `bridge`).
- **Единственные владельцы состояния:**
  - `ContentRegistry` (C++ Data Stage) — архетипы предметов, рецептов, станций, лута, торговцев (read-only в Runtime Stage).
  - Движок + `ModHost` — мир, `EntityId`, `ItemUid`, `ItemInstanceProps`, `ModState`, таймеры, ключи идемпотентности (сохраняются в ZIP сейва v58).
  - `GameScript` / `StoryRuntime` — сток-состояние Gothic II (`C_INFO`, `ScriptVars`, `QuestLog`) до постепенного переноса в `base_gothic`.
  - Python `gothic-ai` (`memory.sqlite3`) — LLM-диалоги, убеждения, слухи, доверие, хроника, AI-режиссёр; получает от модов `mod_catalog` и `mod_fact` через `AiBridge`.
  - `common/society/` — дальняя 10k-симуляция (только декларативные данные в Data Stage и редкие события).

### 3.2. Жизненный цикл, порядок загрузки и перезагрузка

1. **Discovery & Topo-Sort:** `base_gothic` → `dependencies` → `load_after` → лексикографический порядок по `mod_id`. Проверка `sdk_api` и `conflicts` до загрузки мира.
2. **Data Stage (2 прохода):**
   - Проход 1 (`definitions`): каждый мод добавляет объекты только в свой namespace (`<mod_id>:<name>`).
   - Проход 2 (`patches`): декларативные JSON-патчи чужих/базовых объектов (`merge`, `replace`, `add_ingredient`, `remove_loot`). Конфликты одного поля логируются явно; побеждает последний по порядку загрузки.
   - Заморозка `ContentRegistry` и назначение компактных сессионных `uint32_t` ID.
   - Визуальные замены (`materials.json`, `meshes.json`, `vobs.d/*.json`) монтируются стеком поверх `OPENGOTHIC_CONTENT` с соблюдением правил [поставки контента](../../../docs/content-delivery.md) (никаких файлов оригинала).
3. **Runtime Stage:** per-mod `lua_State`, компиляция только из `.luau` исходников, вызов `on_init` / `migrate` + `on_load` / `on_mod_config_changed` / `on_ready`.
4. **Безопасная перезагрузка:**
   - `mod reload` (Runtime-only, цель `<50 мс`): пауза вне активной транзакции → закрытие модальных UI перезагружаемого мода (`CraftingMenu`/`DialogMenu`) без поломки C++ анимаций и `AiQueue` → снятие подписок → сохранение `state` в C++ → пересоздание `lua_State` → `on_load(state)` → восстановление persistent-таймеров (хранятся в C++ по имени обработчика + JSON payload). При ошибке компиляции — откат к предыдущему `lua_State` или изоляция мода в `faulted`.
   - `mod reload --full` (Data + Runtime): быстрый snapshot сейва в память → пересборка `ContentRegistry` → восстановление сессии.

### 3.3. Контракты данных, атомарность и UI

- **Экземпляры предметов (`ItemInstanceProps`):** опциональный `std::unique_ptr<ItemInstanceProps>` на `Item` (`quality`, `durability`, `max_durability`, `temperature` + namespaced `custom`). У ванильных предметов `nullptr` (0 оверхеда). `Inventory` объединяет стопки только при совпадении `(archetype_id, unique == false, stack_key)`. Все операции инвентаря и `InventoryMenu` переводятся на `ItemUid`.
- **Атомарность и идемпотентность:**
  - Декларативные рецепты исполняются в C++ неделимо: проверка станции, инструмента, ингредиентов и read-only `can_craft` → атомарное списание/создание с `compute_props` → только после коммита событие `crafting.completed`.
  - Императивные мутации идут через `world.transaction(fn)`: сбор типизированного `TxPlan` → атомарный коммит в C++ только после успешного завершения `fn` без ошибок и превышения CPU-бюджета.
  - Защита от повторной выдачи: `tx:once(key)` проверяет и записывает ключ атомарно в рамках той же транзакции; ключи сохраняются в `mods/idempotency.json`.
  - Торговля: read-only цепочка `trade.quote` (бюджет `≤0.25 мс`, без мутаций) → атомарный обмен `ItemUid` и золота в C++ → событие `trade.completed`.
- **Изоляция сбоев:** каждый вызов в `lua_pcall`; при ошибке или превышении бюджета CPU/RAM отбрасывается незакоммиченный `TxPlan`, пишется структурированная ошибка (`mod_id`, `event`, `file:line`, `stack`, `budget_us`), остальные моды продолжают работу; circuit breaker ставит на паузу систематически падающий обработчик.
- **Нативный UI в v0.1:**
  - **Отдельный минимальный нативный `CraftingMenu`** (решение пользователя 2026-11-28): открывается на станциях крафта (например `BSANVIL`, `BSFIRE`, алхимический стол), показывает список доступных рецептов из `ContentRegistry`, требуемые/имеющиеся ингредиенты, превью результата и статус `can_craft`, вызывает атомарную транзакцию крафта.
  - Динамические строки тултипа в `InventoryMenu::drawInfo` из `ItemInstanceProps` + read-only `on_item_tooltip`.
  - Нативные тосты/уведомления (`ui.notify`, `ui.toast`), записи дневника (`journal.*`), варианты диалогов (`DialogGraph`).

### 3.4. Сохранения v58

В ZIP-архиве (`SaveArchive::Current = 58`) добавляются:
- `mods/manifest.json` — активные моды (`id`, `version`, `state_schema`) + `registry_map` (`string_id ↔ save_numeric_id`, защита сейвов от смены порядка загрузки).
- `mods/<mod_id>/state.json` — явное сериализуемое состояние мода (без функций/корутин/userdata).
- `mods/timers.json` и `mods/idempotency.json`.
- **Обновление мода:** цепочка миграторов `migrations[v -> v+1]` до `on_load`; при падении мигратора сейв не перезаписывается.
- **Удаление мода:** предупреждение и подтверждение → перенос старого `state` в `mods/_orphaned/`, сброс таймеров, мягкая очистка пропавших архетипов без падения загрузки.

---

## 4. Инструментарий для человека и кодинг-агента

- **Единая схема SDK (`sdk/schema/v1/`):** генератор `scripts/modsdk/gen_sdk.py` строит из одной схемы C++ привязки, `.d.luau` типы для `luau-analyze` / `luau-lsp`, JSON Schemas данных и Markdown-справочник API.
- **CLI `gothic-mod`:**
  - `init` — шаблон мода (`--!strict`, `.luaurc`, `mod.json`, тест-сценарий).
  - `check` — валидация схем, ссылок, namespace, прав и `luau-analyze --defs gothic_sdk.d.luau` (<0.5 с).
  - `test` — безоконный `gothic-mod-runner` на синтетическом мини-мире (без GPU и без установки Gothic II) либо на v3-базе Хориниса; пишет `mod_trace.jsonl` (события, транзакции, ошибки, время в мкс, пик RAM).
  - `inspect-save` — дамп `mods/` из `.sav`.
  - `pack` — детерминированный `.gmod` (ZIP + SHA-256) с отклонением файлов оригинальной базы.

---

## 5. Поэтапный план внедрения и критерии приёмки прототипа

| Фаза | Содержание | Критерий завершения |
|---|---|---|
| **Phase 0: Спайк и замеры** | Вендоринг upstream Luau (`OpenGothic/lib/luau/`) + CMake + `gothic-mod-bench` на Mac и Ryzen (стоимость вызовов, `interrupt`, память на `lua_State`, hot reload, интерпретатор vs CodeGen) | Подтверждённые числа и бюджеты в разделе 6 ниже; чистый прогон `engine_layers.py` |
| **Phase 1: Фундамент ядра** | `EntityId` + `O(1)` lookup; `ItemUid` + `ItemInstanceProps` + `stack_key` в `Inventory`/`InventoryMenu`; `ContentRegistry` (Data Stage + патчи + `registry_map`); сейв v58 | C++ контракты: два клинка с разными свойствами не сливаются, переживают v58 save/load и смену порядка модов |
| **Phase 2: ModHost + SDK v1 + `CraftingMenu` + сквозной прототип** | `ModHost` (per-mod `lua_State`, sandbox, бюджеты, `world.transaction`, `tx:once`, таймеры, hot reload), генератор SDK, `gothic-mod` CLI, `gothic-mod-runner`, нативный `CraftingMenu`, хуки `use`/`trade.quote`/`loot` | **Сквозной прототип** (`ember_smithing` + `guild_renown` + `faulty_listener`) реализован и проверен агентом при **0 правках C++** |
| **Phase 3: Сюжет, квесты, репутация, ИИ-мост** | Квестовый/диалоговый SDK поверх `DialogGraph`/`QuestLog`, репутация/распорядки, проекция `mod_catalog`/`mod_fact` в Python `gothic-ai`, старт переноса стока в `base_gothic` | Агент собирает квест-кампанию; AI-NPC знает о выкованном предмете и квесте мода; работает и при `GOTHIC_AI=0` |
| **Phase 4: Бой, урон, заклинания, расширенный UI** | Контракты синхронных хуков (`damage.calculate`, `spell.cast`) с бюджетом `≤0.25 мс`, оценка включения `Luau.CodeGen` по замерам, декларативные панели UI | Нет регрессии P95 времени кадра на боевом стенде |

### Критерии приёмки сквозного прототипа (Phase 2)
1. Кодинг-агент по публичному SDK (`api-v1.md`, `gothic_sdk.d.luau`, `gothic-mod`) создаёт и проверяет мод `ember_smithing` без изменений C++.
2. Полный цикл в `gothic-mod-runner` и `Gothic2Notr`: новый предмет (`ember_blade` с `quality/temperature`) → рецепт на кузнице (`BSANVIL` через `CraftingMenu`) → атомарное списание ингредиентов и `tx:once` XP → применение / продажа с `trade.quote` → сохранение v58 и загрузка → обновление `state_schema` v1→v2 через `migrations.luau`.
3. Взаимодействие двух модов: `guild_renown` слушает `ember_smithing:blade_forged` и добавляет скидку в `trade.quote`.
4. Отказоустойчивость: третий мод падает исключением и зацикливается в том же событии — `ModHost` обрывает его по CPU-бюджету, откатывает его транзакцию и не мешает работе первых двух модов и крафта.
5. `mod reload` обновляет формулу в `scripts/main.luau` без перезапуска мира и потери `ModState`.

### Ограничения первого выпуска (v0.1)
- Нет конструктора произвольных GUI-окон (только `CraftingMenu`, тултип, тосты, дневник, диалоги).
- Нет синхронных хуков боя/урона/заклинаний и по-кадровых колбэков 10k NPC.
- Горячая перезагрузка новых архетипов/мешей требует `mod reload --full`.
- Нет файлового/сетевого I/O, FFI, мультиплеера и сериализации корутин.

---

## 6. Замеры Phase 0 (подтверждение бюджетов на Mac и Ryzen)

Стенд: [`OpenGothic/tests/mod_bench.cpp`](../../../OpenGothic/tests/mod_bench.cpp) (цель CMake `gothic-mod-bench`, сборка `RelWithDebInfo`, вендоринг upstream **Luau 0.748** `3d91c9d5c280c95bfc4be63aac46012cf861d394` в [`OpenGothic/lib/luau/`](../../../OpenGothic/lib/luau/) — только `Common`, `Ast`, `Bytecode`, `Compiler`, `VM`, всего `2,6 МБ` исходников).

- **Mac:** Apple M1 Pro, macOS, AppleClang 21.0 (`QOS_CLASS_USER_INTERACTIVE`), бинарник `1,3 МБ` (`918 КБ` stripped).
- **Ryzen:** AMD Ryzen 9 3950X, Ubuntu Linux 7.0, GCC 15.2 (через `scripts/gq run --class quiet`), бинарник `12 МБ` (`1,1 МБ` stripped).

| Метрика | Цель (гипотеза) | Apple M1 Pro (Mac) | Ryzen 9 3950X (Linux) | Вывод |
|---|---:|---:|---:|---|
| **Размер бинарника (stripped, VM+Compiler+Ast)** | <3 МБ | **918 КБ** | **1,1 МБ** | В 15–25× легче Wasmtime; 0 новых тулчейнов |
| **Память 1 изолированного `lua_State` (`lua_Alloc`)** | <500 КиБ | **443,0 КиБ** live / **458,9 КиБ** peak | **443,0 КиБ** live / **458,9 КиБ** peak | Побайтно совпадает; модель **1 `lua_State` = 1 мод** подтверждена |
| **RSS-дельта 50 / 100 изолированных модов** | <25 МБ на 50 модов | **22,42 МБ** / **44,36 МБ** | **9,81 МБ** / **19,89 МБ** | Запас кратный на обеих ОС |
| **Жёсткий лимит RAM (`512 КиБ` vs `memory_bomb`)** | `LUA_ERRMEM`, сосед жив | **caught=yes, sibling=yes** | **caught=yes, sibling=yes** | Утечка/OOM одного мода изолирована полностью |
| **Граница C++↔Luau: простой query (`ping + host_get_attr`)** | <100 нс | **45,9–49,7 нс** | **60,9–64,6 нс** | В 1,5–2× быстрее целевого порога |
| **Граница C++↔Luau: `trade.quote` (таблица in/out)** | <500 нс | **299,6–323,1 нс** | **344,8–352,3 нс** | Укладывается с запасом |
| **Граница C++↔Luau: крафт `TxPlan` (4 host-ops + commit)** | <1000 нс | **192,2–205,9 нс** | **257,4–258,2 нс** | ~0,25 мкс на полную 4-шаговую транзакцию |
| **Оверхед `interrupt` на плотном цикле (5M итераций):** | <5% | | | |
| — *inline polling (`cb.interrupt` всегда задан, stride=64)* | — | **+36,9…+48,0%** | **+28,5…+29,2%** | **Ловушка:** держит `LUAU_UNLIKELY` slow-path на каждом back-edge |
| — *внешний watchdog (`cb.interrupt == nullptr` на горячем пути)* | <5% | **+0,22%** *(повтор без прогрева +9,2%)* | **+0,19…+0,53%** | **Рабочий паттерн:** практически нулевой оверхед |
| **Отсечение `while true do end` (250 мкс / 2000 мкс):** | +100 мкс | | | |
| — *inline polling (stride=64)* | — | med **278 мкс** / **2037 мкс** | med **253 мкс** / **2003 мкс** | Точность <35 мкс на обеих ОС |
| — *watchdog (`cv.wait_until`)* | — | med **1170–1200 мкс** / **7,1–9,0 мс** | med **308–320 мкс** / **2061–2075 мкс** | На Linux точность `+60…+75 мкс`; на macOS фоновый поток попал под timer coalescing (нужен `QOS_CLASS_USER_INTERACTIVE` / `dispatch_source` в `ModHost`) |
| **Hot reload (1 мод / 10 модов × ~500 LOC Luau)** | <50 мс на 10 модов | **0,97–1,01 мс** / **9,83–9,94 мс** | **0,95–0,97 мс** / **9,22–9,23 мс** (P95 9,7–9,9 мс) | В 5× быстрее целевых 50 мс |
| **Боевая формула `swordDamage + meleeMultiplier` (интерпретатор)** | <50 мкс/удар | **437 нс** (**0,437 мкс/удар**) | **428–439 нс** (**0,43 мкс/удар**) | **В 100× быстрее порога**, идентичный checksum `10140000`; **`Luau.CodeGen` (JIT) не нужен даже для Phase 4** |

### Архитектурные выводы Phase 0 (подтверждены замерами)

1. **Изоляция `1 lua_State = 1 мод` принята окончательно:** `443 КиБ` на мод и чистый перехват OOM без влияния на соседние моды.
2. **Контроль CPU — внешний watchdog, а не постоянный `cb.interrupt`:** постоянная установка `cb.interrupt` даёт `+26…48%` замедления циклов (бьёт в `LUAU_UNLIKELY(!!interrupt)` в `VM_INTERRUPT()`). Внешний watchdog держит `cb.interrupt == nullptr` в штатной работе (`~0,2–0,5%` оверхеда) и выставляет `watchdogAbortHandler` только по истечении дедлайна. В `ModHost` на macOS поток watchdog должен получать `QOS_CLASS_USER_INTERACTIVE` (или `mach_wait_until` / `DISPATCH_TIMER_STRICT`), чтобы избежать timer coalescing ядра macOS.
3. **Интерпретатор Luau достаточен на весь дорожный план (включая Phase 4):** полная формула ближнего боя с множителями скорости/угла и свойствами предмета считается за `0,44 мкс` (`50` ударов в кадр = `0,022 мс`). `Luau.CodeGen` не вендорится — экономим `1,7 МБ` исходников и избегаем `W^X`/JIT-осложнений.
4. **Слои движка не нарушены:** `scripts/engine_layers.py check --base origin/main` — `0` новых нарушений включений и `0` новых чужих определений.

