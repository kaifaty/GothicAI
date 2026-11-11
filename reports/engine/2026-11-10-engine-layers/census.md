# Перепись слоёв движка

Снимок на `e790cadc3`, создан нынешним `scripts/engine_layers.py` (тогда `census.py` в этой папке); правила слоёв — в нём же.

## Слои

| Слой | Файлов | Строк | Включают VM скриптов прямо | Достают до VM через включения | ZenKit |
|---|---:|---:|---:|---:|---:|
| Платформа (`platform`) | 255 | 78233 | 3 | 113 | 0 |
| Каркас RPG (`rpg`) | 166 | 46278 | 3 | 81 | 0 |
| Готика (`gothic`) | 156 | 31652 | 20 | 80 | 0 |
| Импорт Готики (`import`) | 22 | 9194 | 0 | 0 | 9 |
| Мост ИИ (`bridge`) | 62 | 16059 | 4 | 27 | 0 |
| Стенды и инструменты (`tools`) | 114 | 49007 | 15 | 69 | 4 |

## Связи между слоями

Строка — кто включает, столбец — кого. Жирное — запрещено правилом.

| | `platform` | `rpg` | `gothic` | `import` | `bridge` | `tools` |
|---|---:|---:|---:|---:|---:|---:|
| `platform` | 609 | **77** | **67** | **7** | 0 | **6** |
| `rpg` | 186 | 395 | **105** | 0 | 0 | **42** |
| `gothic` | 113 | 81 | 259 | **2** | **1** | **7** |
| `import` | 26 | 0 | 13 | 16 | 0 | 0 |
| `bridge` | 26 | 92 | 39 | 0 | 117 | **6** |
| `tools` | 189 | 308 | 128 | 2 | 13 | 216 |

Всего нарушений: **320**.

## Платформа: куда тянутся нарушения

157 включений из 75 файлов.

| Включаемый файл | Его слой | Сколько файлов слоя его включают |
|---|---|---:|
| `common/gothic.h` | `gothic` | 23 |
| `common/world/world.h` | `rpg` | 22 |
| `common/world/objects/npc.h` | `rpg` | 16 |
| `common/game/constants.h` | `gothic` | 10 |
| `common/gothicglb.h` | `gothic` | 9 |
| `common/game/serialize.h` | `rpg` | 9 |
| `common/testbench.h` | `tools` | 6 |
| `common/game/definitions/definitiontypes.h` | `gothic` | 5 |
| `common/baseimport.h` | `import` | 4 |
| `common/world/objects/interactive.h` | `rpg` | 4 |
| `common/world/objects/item.h` | `rpg` | 4 |
| `common/world/chunkresidency.h` | `rpg` | 4 |
| `common/world/objects/pfxemitter.h` | `rpg` | 3 |
| `common/daedalus/classes.h` | `gothic` | 3 |
| `common/contentcensus.h` | `import` | 3 |

| Файл слоя с нарушениями | Запрещённых включений |
|---|---:|
| `common/camera.cpp` | 7 |
| `common/physics/dynamicworld.cpp` | 7 |
| `common/graphics/mdlvisual.cpp` | 6 |
| `common/physics/arrowtrace.h` | 5 |
| `common/physics/collisionworld.cpp` | 5 |
| `common/resources.cpp` | 5 |
| `common/graphics/effect.h` | 4 |
| `common/graphics/mdlvisual_quiver.cpp` | 4 |
| `common/graphics/mesh/animationsolver.cpp` | 4 |
| `common/graphics/mesh/pose.cpp` | 4 |
| `common/graphics/visualfx.h` | 4 |
| `common/musicdata.h` | 4 |
| `common/physics/ragdoll.cpp` | 4 |
| `common/graphics/effect.cpp` | 3 |
| `common/graphics/lightgroup.cpp` | 3 |

## Каркас RPG: куда тянутся нарушения

147 включений из 64 файлов.

| Включаемый файл | Его слой | Сколько файлов слоя его включают |
|---|---|---:|
| `common/gothic.h` | `gothic` | 33 |
| `common/testbench.h` | `tools` | 19 |
| `common/game/gamescript.h` | `gothic` | 16 |
| `common/game/constants.h` | `gothic` | 10 |
| `common/navdemos.h` | `tools` | 7 |
| `common/game/damagecalculator.h` | `gothic` | 6 |
| `common/game/bodyarena.h` | `tools` | 6 |
| `common/world/triggers/abstracttrigger.h` | `gothic` | 5 |
| `common/game/bodytrajectory.h` | `tools` | 4 |
| `common/daedalus/classes.h` | `gothic` | 3 |
| `common/npcdayjournal.h` | `tools` | 3 |
| `common/world/triggers/cscamera.h` | `gothic` | 3 |
| `common/game/fightalgo.h` | `gothic` | 2 |
| `common/game/savegameheader.h` | `gothic` | 2 |
| `common/game/globaleffects.h` | `gothic` | 2 |

| Файл слоя с нарушениями | Запрещённых включений |
|---|---:|
| `common/world/objects/vob.cpp` | 13 |
| `common/world/objects/npc.cpp` | 8 |
| `common/world/worldobjects.cpp` | 8 |
| `common/world/world.cpp` | 7 |
| `common/world/worldsound.cpp` | 5 |
| `common/game/simulationstep.cpp` | 4 |
| `common/world/objects/npc_attack.cpp` | 4 |
| `common/world/objects/npc_damage.cpp` | 4 |
| `common/world/objects/npc_melee_shadow.cpp` | 4 |
| `common/game/archerai.cpp` | 3 |
| `common/game/movealgo.cpp` | 3 |
| `common/game/playercontrol.cpp` | 3 |
| `common/game/serialize.h` | 3 |
| `common/world/bloodspawn.cpp` | 3 |
| `common/world/objects/npc.h` | 3 |

## Готика: куда тянутся нарушения

10 включений из 9 файлов.

| Включаемый файл | Его слой | Сколько файлов слоя его включают |
|---|---|---:|
| `common/testbench.h` | `tools` | 6 |
| `common/baseimport.h` | `import` | 2 |
| `common/navdemos.h` | `tools` | 1 |
| `ai_bridge/aibridge.h` | `bridge` | 1 |

| Файл слоя с нарушениями | Запрещённых включений |
|---|---:|
| `common/game/gamescript.cpp` | 2 |
| `common/game/damagecalculator.cpp` | 1 |
| `common/game/fightalgo.cpp` | 1 |
| `common/gamemusic.cpp` | 1 |
| `common/mainwindow.cpp` | 1 |
| `common/outputunits.h` | 1 |
| `common/programdata.h` | 1 |
| `common/ui/inventorymenu.cpp` | 1 |
| `game/main.cpp` | 1 |

## Мост ИИ: куда тянутся нарушения

6 включений из 6 файлов.

| Включаемый файл | Его слой | Сколько файлов слоя его включают |
|---|---|---:|
| `common/testbench.h` | `tools` | 6 |

| Файл слоя с нарушениями | Запрещённых включений |
|---|---:|
| `ai_bridge/aibridge.cpp` | 1 |
| `ai_bridge/aibridge_debug.cpp` | 1 |
| `ai_bridge/fateend.cpp` | 1 |
| `ai_bridge/necromancy.cpp` | 1 |
| `ai_bridge/npcobserver.cpp` | 1 |
| `ai_bridge/stucklogger.cpp` | 1 |

