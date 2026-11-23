# Отличия удалённого режима Gothic 1

Режим Gothic 1 удалён 2026-11-07 решением пользователя «Да, убираем». Его ветки к тому времени были недостижимы: `detectGothicVersion` всегда задавал `game=2`, `patch=5`; на данных Gothic 1 эти ветки не проверялись. Старый код лежит в коммите базы задачи [`714f7f1d08518b273bdd133fb3f0e833e8f8dc9a`](https://github.com/kaifaty/GothicAI/tree/714f7f1d08518b273bdd133fb3f0e833e8f8dc9a); справка составлена только по нему (пути ниже от `OpenGothic/`, строки из этого коммита, «Gothic 2» — путь Gothic II NotR).

## Данные и скрипты

| Что в Gothic 1 | Что в Gothic 2 | Где было |
|---|---|---|
| Валюта `ItMiNugget`, множитель торговли `1`, время символа `550`, отмычка `itkelockpick`; `B_RefreshAtInsert` не загружалась | Валюта из `TRADE_CURRENCY_INSTANCE`, множитель из `TRADE_VALUE_MULTIPLIER` (по умолчанию `1`), время из `VIEW_TIME_PER_CHAR` (по умолчанию и при неположительном значении `550`), `ItKE_lockpick`, `B_RefreshAtInsert` | `common/game/gamescript.cpp:356` |
| При отсутствии `player_mob_missing_item`, `player_mob_missing_key`, `player_mob_another_is_using`, `player_mob_missing_key_or_lockpick`, `player_mob_missing_lockpick` игралась `T_DONTKNOW` | При отсутствии соответствующей функции — возврат без анимации; при наличии вызывается функция скрипта | `common/game/gamescript.cpp:1058`, `common/game/gamescript.cpp:1069`, `common/game/gamescript.cpp:1080`, `common/game/gamescript.cpp:1091`, `common/game/gamescript.cpp:1102` |
| `invokeRefreshAtInsert` возвращалась без вызова функции | Вызывает `B_RefreshAtInsert`, если символ найден | `common/game/gamescript.cpp:1250` |
| Коллизия заклинания: проверка `target_collect_type` через `isTargetableBySpell`, результат `COLL_DOEVERYTHING` или `COLL_DONOTHING` | Вызов `C_CanNpcCollideWithSpell`; при отсутствии функции — `COLL_DOEVERYTHING` | `common/game/gamescript.cpp:1257` |
| Если нет `player_hotkey_screen_map`, поиск предмета `itwrworldmap_orc`, затем `itwrworldmap`, использование найденного; иначе `-1` | При отсутствии функции — `-1`, иначе функция выбирает карту | `common/game/gamescript.cpp:1291`; удалённый помощник — `common/game/gamescript.cpp:1275` |
| Если нет `player_plunder_is_empty`, игралась `T_DONTKNOW` | При отсутствии функции — возврат без анимации | `common/game/gamescript.cpp:1485` |
| `AIV_PARTYMEMBER=36` в проверке дружественного огня | `AIV_PARTYMEMBER=15` | `common/game/gamescript.cpp:1534` |
| Если адресат `ai_output`, `ai_outputsvm`, `ai_outputsvm_overlay` не найден, подставлялся `global_other` | Без найденных отправителя и адресата действие не ставится в очередь | [`ai_output`](../OpenGothic/common/game/gamescript_externals.cpp#L424), [`ai_outputsvm`](../OpenGothic/common/game/gamescript_externals.cpp#L513), [`ai_outputsvm_overlay`](../OpenGothic/common/game/gamescript_externals.cpp#L523) |
| Для `PERC_ASSESSMAGIC` использовался `sendAssessMagic` выбранного эффекта коллизии (`emFXCollStat` либо `emFXCollDyn`) | Используется отдельный `emFXCollDynPerc` | `common/graphics/effect.cpp:311` |
| До 21 `T_DIALOGGESTURE_%02d`; всем жестам задавался класс `Lively` | До 11 жестов; классы первых десяти из `G2Kind`, для остальных `Lively`. Пропуск отсутствующих и повторных анимаций прежний | `common/graphics/mdlvisual.cpp:2354`, `common/graphics/mdlvisual.cpp:2364`, `common/graphics/mdlvisual.cpp:2380`, `common/graphics/mdlvisual.cpp:2560`; определение числа — `common/utils/versioninfo.h:11`, классификация — `common/graphics/mesh/talkgestures.cpp:134` |
| Блок кулаком `T_FISTPARADE_O` | `T_FISTPARADE_0` | `common/graphics/mesh/animationsolver.cpp:128` |
| Блок оружием `T_%sPARADE_O` | Сначала `std::rand()%3`, затем выбор `T_%sPARADE_0`, `_0_A2` или `_0_A3` с учётом стенда; при отсутствии выбранной — `_0` | `common/graphics/mesh/animationsolver.cpp:142` |
| По гильдии тонули скелеты `GIL_G1_SKELETON=32` и големы `GIL_G1_GOLEM=35` | По гильдии тонут `GIL_GOBBO_SKELETON`, `GIL_SUMMONED_GOBBO_SKELETON`, `GIL_SKELETON`, `GIL_SUMMONED_SKELETON`, `GIL_SKELETON_MAGE`, `GIL_SHADOWBEAST_SKELETON`, `GIL_STONEGOLEM`, `GIL_SUMMONED_GOLEM`, `GIL_Stoneguardian`, `GIL_SummonedGuardian`. Общая проверка имени модели и коэффициент `1.5` прежние | `common/physics/ragdoll.cpp:1300` |
| Расширение имени анимации не заменялось | Перед загрузкой замена `MDS` на `MSB` | `common/resources.cpp:2408` |
| Заклинание с `spellId=47` (`SPL_STORMFIST`) считалось снарядом принудительно | Только общая эвристика `target_collect_algo`: исключены `NONE`, `CASTER`, `FOCUS`, `ALL_FALLBACK_NONE` | `common/world/objects/item.cpp:368` |
| Исключение дракона при возврате мёртвого NPC к распорядку отключено | `guild()==GIL_DRAGON` сохраняет исключение из раннего возврата для мёртвых без миссионных предметов | [`Npc::resetPositionToTA`](../OpenGothic/common/world/objects/npc_motion.cpp) |
| Индекс члена партии в контактах клинка — `36` | Индекс `15`; сама проверка союзников прежняя | `common/world/objects/npc_melee_shadow.cpp:994` |
| Монстр: `16 < guild < 37`; человек: `guild < 16` | Монстр: `16 < guild < 58`; человек: `guild < 16` | [`isMonster`, `isHuman`](../OpenGothic/common/world/objects/npc_attributes.cpp); значения гильдий — `common/game/constants.h:8` и `common/game/constants.h:81` |
| Друг: тип `NpcType::G1_FRIEND` (число `3`) | Тип `NpcType::G2_FRIEND` (число `2`) | [`isFriend`](../OpenGothic/common/world/objects/npc_attributes.cpp); числа — `common/daedalus/classes.h:109` |
| Наличие блока в наблюдении тела проверялось по `T_1HPARADE_O` / `T_2HPARADE_O` | Проверяется суффикс `0`; этот просмотр не делает броска RNG | `common/world/objects/npc_observation.cpp` |
| Разрешённые состояния игрока включали `ZS_MAGICFEAR` | Вместо него `ZS_WHIRLWIND`; общие девять состояний: `ZS_DEAD`, `ZS_UNCONSCIOUS`, `ZS_MAGICFREEZE`, `ZS_PYRO`, `ZS_ASSESSMAGIC`, `ZS_ASSESSSTOPMAGIC`, `ZS_ZAPPED`, `ZS_SHORTZAPPED`, `ZS_MAGICSLEEP` | [`Npc::isPlayerEnabledState`](../OpenGothic/common/world/objects/npc_routine.cpp) |
| При отсутствии loop-функции состояния `loop=0` | `loop=1` для NotR; прежний помощник требовал `game==2 && patch>=5`, поэтому G2 classic тоже получал `0` | [`Npc::tickRoutine`](../OpenGothic/common/world/objects/npc_routine.cpp); помощник — `common/utils/versioninfo.h:10` |
| Для целей-орков `trueGuild > 37`; нежить — `GIL_G1_ZOMBIE`, `GIL_G1_UNDEADORC`, `GIL_G1_SKELETON` (30, 31, 32) | Орки: `trueGuild > 58`; нежить — `GIL_GOBBO_SKELETON`, `GIL_SUMMONED_GOBBO_SKELETON`, `GIL_SKELETON`, `GIL_SUMMONED_SKELETON`, `GIL_SKELETON_MAGE`, `GIL_SHADOWBEAST_SKELETON`, `GIL_ZOMBIE`. Общие проверки `ALL`, `NPCS`, людей прежние | `common/world/objects/npc.cpp:8070` |
| `invokeSpell` при финальной отправке заклинания пропускалась | Вызывается перед общей логикой снаряда/эффекта | `common/world/objects/npc_ranged.cpp:30` |
| Начало анимации заклинания пробовалось только при наличии `T_MAGRUN_2_<ani>CAST` | `startAnimSpell` вызывается без предварительного `hasAnim`; его результат обрабатывается как прежде | `common/world/objects/npc_ranged.cpp:169` |
| Признак дракона в правиле удаления тел всегда ложный | Признак `guild()==GIL_DRAGON` | `common/world/world.cpp:725` |

## Правила

| Что в Gothic 1 | Что в Gothic 2 | Где было |
|---|---|---|
| Ручное прицеливание NPC выключалось независимо от настройки | Работает при `enabled!=0` | `common/game/archerai.cpp:98` |
| Навык прицеливания — ловкость / 100 | `hitChance` лука либо арбалета / 100 | `common/game/archery.cpp:174` |
| Нет нижнего ограничения общего урона | Для состоявшегося попадания по неиммунной цели минимум `MinDamage` | `common/game/damagecalculator.cpp:37` |
| Обычная стрела: один `script.rand(100)` для попадания по пройденному пути; линейная интерполяция от 100% до `hitChance` на 2000 см, затем до нуля на 4500 см. После попадания по неиммунной цели — ещё один бросок критического урона, сравнение с `b.critChance()`, множитель `DAM_CRITICAL_MULTIPLIER` (по умолчанию 2) | Обычная стрела: ровно один `script.rand(100)` после `vanillaChance` по расстоянию между NPC и `b.hitChance()`. Предыдущая общая ветка точного выстрела остаётся без бросков. Второй `else if(!b.isSpell())` после выбора G2 целиком недостижим, включая локальный выбор опорной дальности 1500/2000 | `common/game/damagecalculator.cpp:98`, `common/game/damagecalculator.cpp:120` |
| Ближний бой: талант `talentValue` сравнивался с броском; критический удар умножал компонент урона оружия на `DAM_CRITICAL_MULTIPLIER`, затем прибавлялась сила и вычиталась защита | `hitchance` сравнивается с тем же заранее сделанным броском; неудачный критический удар даёт `(vd-1)/10`. Обычным монстрам без таланта ставится порог `-1`, но исходный бросок остаётся до этой проверки | `common/game/damagecalculator.cpp:224` |
| В урон стрелы ловкость не добавлялась | Ловкость добавляется в каждый выбранный тип урона | `common/game/damagecalculator.cpp:297` |
| Опорная дальность лука в боевом алгоритме — `ReferenceBowRangeG1=2000` | `ReferenceBowRangeG2=1500` | `common/game/fightalgo.cpp:382` |
| При управлении заклинанием отпускание движения вперёд **или** нулевая мана завершали каст | Только отпускание движения вперёд | `common/game/playercontrol.cpp:994` |
| При ошибке взлома порог поломки отмычки — `100 - talentValue(TALENT_PICKLOCK)` | Порог — ловкость; сравнение с `script.rand(100)` и последующие вызовы `G_PickLock` прежние | `common/game/playercontrol.cpp:1517` |
| На экране талантов 12 позиций, значение из `talentValue` | 22 позиции (`TALENT_MAX_G2`), значение из `hitChance` | `common/ui/gamemenu.cpp:1265` |
| Для взлома игроку хватало отмычек независимо от навыка | Нужны и отмычки, и ненулевой `talentSkill(TALENT_PICKLOCK)` | `common/world/objects/interactive.cpp:676` |
| После `invokeMana` движок отнимал единицу маны и для NPC при результате не `SPL_SENDCAST` проверял остаток `>0` | Этого дополнительного списания и assert нет | `common/world/objects/npc_ranged.cpp:199` |
| Шанс попадания стрелы — ловкость / 100; отдельный шанс критического удара — значение таланта лука/арбалета / 100 | Шанс попадания из `hitchance` лука/арбалета / 100; отдельного параметра критического удара нет | `common/world/objects/npc_ranged.cpp:324` |

## Интерфейс и старт

- Мир по умолчанию: G1 — `world.zen`, G2 — `newworld.zen` (`common/gothic.cpp:258`). Профиль уже безусловно задавался как `game=2`, `patch=5` (`common/gothic.cpp:919`); `-g1`, `-g2c`, `-g2` только выставляли невостребованные флаги (`common/commandline.cpp:93`), теперь удалены. `-g <путь>` остаётся.
- Заголовок окна: `Gothic` / `Gothic II` (`common/mainwindow.cpp:61`). В загрузочном экране G1 полоса занимала 50% ширины, 5% высоты и стояла на 75% высоты; G2 использует размер текстуры и положение около 92% ширины, 12% высоты (`common/mainwindow.cpp:232`). Отступы заполнения: G1 — 30/5, G2 — 75/10, умноженные на отношение размера полосы к размеру текстуры (`common/mainwindow.cpp:961`).
- Камера для лука/арбалета: G1 — `Normal`, G2 — `Ranged`; для мага G1 — `Melee`, G2 — `Ranged` (`common/mainwindow.cpp:1265`). В общем шаге симуляции выбор дальнобойной камеры тоже свёрнут в `Ranged` (`common/game/simulationstep.cpp:91`).
- Управление: G1 принудительно делал `g2Ctrl=false`; G2 читает `USEGOTHIC1CONTROLS`, настройка сохранена (`common/game/playercontrol.cpp:87`). При отсутствии секции пресета клавиш G1 выбирал встроенный обычный/альтернативный набор, G2 возвращается без изменения назначений (`common/utils/keycodec.cpp:179`).
- Отладочные `printdebug`, `printdebugch`, `printdebuginst`, `printdebuginstch`: G1 молчал, G2 пишет `Log::d` с `[zspy]` и при необходимости каналом (`common/gothic.cpp:1300`, `common/gothic.cpp:1305`, `common/gothic.cpp:1310`, `common/gothic.cpp:1315`). Наблюдение стенда за принятым блоком оружия раньше отбрасывало G1; остальные условия оставлены (`common/testbench.cpp:1667`).
- В заголовок сохранения раньше записывался номер `game`, теперь литерал `2` (`common/game/gamesession.cpp:245`); поле, порядок и формат не меняются. Версия сериализации (`Serialize::version()` / `fin.version()`) к профилю игры не относится и сохранена.
