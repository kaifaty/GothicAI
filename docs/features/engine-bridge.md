## Движок: мост

Часть реестра фич; [как читать столбцы](../features.md#как-читать-столбцы).

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Мост к ИИ-серверу | `GOTHIC_AI=0` | `ai_bridge/aibridge.cpp` | все e2e | [protocol.md](../protocol.md) |
| Изменение списка `body_watch` под lease или с заявкой на захват (v45) | нет; без владельцев остаётся полный сброс | `ai_bridge/aibridge.cpp`, `common/game/bodyobservations.*`, `common/game/bodycontroller.*` | `gothic-body-watch-test`; e2e `neural` | [npc-body-api.md](../npc-body-api.md), [npc-body-observations.md](../npc-body-observations.md) |
| Диалоговые функции скриптов как программы без VM | `GOTHIC_AI_STORYEXEC=0` | `ai_bridge/dialoggraph.cpp` | e2e `graph`, `programs` | [story-graph.md](../story-graph.md) |
