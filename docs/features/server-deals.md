## Сервер: сделки и поручения

Часть реестра фич; [как читать столбцы](../features.md#как-читать-столбцы).

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Сделки: окно, торг, долги | нет | `deals.py` | e2e `deal`, `debt`, `haggle` (живой); бенч `deal` | [npc-tools.md](../npc-tools.md#сделки) |
| Помощь Ксардаса деньгами по просьбе: до 50 даром за ветку, до 200 неоплаченного долга | удалить запись NPC из таблицы `purse` в `gamedata/stash.json`, как у тайника | `purse.py`, `deals.py`, `gamedata/stash.json` | `test_purse.py`; e2e `xardas_money` (`LLM=fake`; живой режим — вручную) | [contracts.md](../contracts.md#как-работает-сейчас) |
| Опыт, обучение, платные задания, тайник | нет | `contracts.py`, `gamedata/stash.json` | e2e `contract` | [contracts.md](../contracts.md) |
| Сундуки NPC | `--no-chests` | `chests.py`; мост — `ai_bridge/npcchests*` | e2e `chests`, `chests_lobart` | [npc-tools.md](../npc-tools.md#сундуки) |
| Проводник: отвести героя | `--no-lead` | `guide.py` | e2e `lead` | [npc-tools.md](../npc-tools.md#сделано-проводник) |
| Встреча с героем в назначенный час | `--no-meet`; без проводника её нет | `meetings.py` | e2e `meet`, `meet_missed` | [npc-tools.md](../npc-tools.md#сделано-встречи) |
| Свои поручения NPC: рассказать, отнести | `--no-errands`; без проводника их нет | `errands.py` | e2e `errand`, `errand_fork`, `errand_far`, `errand_give` | [npc-tools.md](../npc-tools.md#сделано-поручения) |
