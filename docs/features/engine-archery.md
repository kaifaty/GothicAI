## Движок: стрельба

Часть реестра фич; [как читать столбцы](../features.md#как-читать-столбцы).

| Фича | Как выключить | Модуль | Проверка | Документ |
|---|---|---|---|---|
| Ручной прицел героя | Выключить нельзя: единственный режим с 2026-11-08 | `common/game/archery.cpp`, `aimcamera.h` | бенчи `manual-aim`, `aim-cant`, `aim-corner`, `aim-over-edge`, `aim-stance`, `aim-steep-wall`, `crossbow`, `edge-shot`, `bow-pole`, `hero-balance`, `archery-smoke` | [manual-aim.md](../manual-aim.md) |
| Натяжение лука от силы: скорость, ровный предел и рывок | `OPENGOTHIC_BOWSTRENGTH=0` или `[GAME] bowDrawByStrength=0` (по умолчанию 1); Marvin `toggle bowstrength`, стенд `bowstrength on\|off` | `common/game/archery.*`, `common/world/objects/archer.*`, `common/game/archerai.cpp` | контракт `gothic-archery-contract` | [manual-aim.md](../manual-aim.md) |
| Ручной прицел лучников NPC | `OPENGOTHIC_NPCAIM=0` или `[GAME] npcManualAim=0` | `common/game/archerai.*` | бенчи `npc-mech`, `npc-fight`, `npc-lead`; `npc-today` — кости, для сравнения | [manual-aim.md](../manual-aim.md) |
