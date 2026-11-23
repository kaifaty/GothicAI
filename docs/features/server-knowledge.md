## Сервер: восприятие и знание

Часть реестра фич; [как читать столбцы](../features.md#как-читать-столбцы).

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Восприятие NPC, имена только знакомым | нет | `perception.py`; мост — `ai_bridge/npcview*` | e2e `see` | [npc-tools.md](../npc-tools.md#сделано-м1-вижу-сейчас) |
| Память увиденного, улики | нет | `sightings.py` | e2e `seen`, `remembers`, `trace`, `broken` | [npc-tools.md](../npc-tools.md#сделано-м2-видел-недавно) |
| Слышимость голоса героя в обход скал | нет | `talk.py`; мост — `npc_roster.heard` | e2e `hearing` | [npc-agent.md](../npc-agent.md#слух-и-защита-издалека) |
| Предметы на земле: NPC видит и поднимает | нет | `npc_tools.py`; мост | e2e `ground` | [protocol.md](../protocol.md#предметы-на-земле-v22) |
| Книга мест, ответ «где» | нет; без книги — только вейпоинты | `places.py`, `whereabouts.py`, `gamedata/places.json` | e2e `cave` (`places` только выгружает точки) | [npc-tools.md](../npc-tools.md#сделано-м3-карта-мест) |
| Кто что знает о мире по своим диалогам | нет | `lore.py`, `gamedata/lore.json` | e2e `knows` | [npc-tools.md](../npc-tools.md#сделано-м4-знаю) |
| Знание мира из скриптов по главе | `--no-knowledge` | `knowledge.py` | e2e `m6` | [npc-agent.md](../npc-agent.md#знания-о-мире-m6) |
