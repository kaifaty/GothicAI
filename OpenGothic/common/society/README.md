# `common/society/` — ядро стенда общества NPC

Ядро `society-core` стенда общества ([`docs/society-sim.md`](../../../docs/society-sim.md)): мир событий по выгрузке
`society-world` (v1, v2 и v3), распорядок по правилам движка, журнал, снимки; с файлом настройки
([`data/society/config.json`](../../../data/society/config.json)) — нужды людей, а с его разделом `decisions` — выбор
занятий: меню, маски, учитель-полезность и случайная политика.

**Правило папки: ничего из движка.** Здесь включают только стандартную библиотеку C++ и rapidjson — ни `Tempest`, ни
`world/`, ни `game/`, ни другие файлы `common/`. Так стенд собирает ядро отдельной библиотекой, а движок потом
соберёт то же ядро у себя. Сборка следит за правилом сама: у цели `society-core` в путях включения есть только эта
папка и rapidjson, а общий список исходников движка (`OpenGothic`) эту папку пропускает.

- `society.h` — весь открытый интерфейс: мир, состояние, события, правила движка (`StartDelay`, `SettleDelay`,
  `StartStateSeconds`, `NearPointCm`, `FollowStates`), `loadWorld`, `makeSim`, `reset`, `advance`, `save`/`restore`,
  `drainEvents`, `stateEndAt`, `startStateAt`, перевод длины пути во время;
- `world.cpp` — чтение выгрузки v1/v2/v3, комнат и владельцев и файла настройки (нужды, таблица `ZS_*`, виды предметов,
  характер по `characters.json`), дневная лента распорядка каждого NPC, конец состояния по `Npc::endTime`, расписание
  монстров;
- `sim.cpp` — очередь событий, правила «только распорядок», нужды (отрезки, пересечения порогов в очереди), путь по
  рёбрам и точки пути для позиций в отметках;
- `decide.cpp` — решения: точки решения, меню и маски, места вокруг точки (`Nearby`), брони, места распорядка
  (`routinePlace`) и выселение с них выбранных занятий, выбранные занятия и их конец, учитель (`teacherScores`,
  `teacher`) и `randomPolicy`;
- `reach.cpp` — что раздел `decisions` даёт миру при загрузке: места вариантов по точкам сети с их видами (в том
  числе своих комнат и мест `places.json`), места распорядка у каждой точки (`World::spots`: mobsi точки и свободные
  точки в 8 м), доступ по виду, нужды в деле и список «нечем утолить»;
- `core.h` — внутренности правил, общие для `sim.cpp`, `decide.cpp` и `reach.cpp` (не интерфейс);
- `journal.h`, `journal.cpp` — двоичный журнал v2 (v3 с нуждами, v5 с решениями) и пятиминутные отметки CSV с позициями,
  уровнями нужд и источником и целью занятия.

Утилита — `game/societyday.cpp` (`gothic-society-day`), проверка — `tests/society_core_contract.cpp`
(`gothic-society-core-contract`), сверка с родным днём движка — [`scripts/society_parity.py`](../../../scripts/society_parity.py), карта нужд под распорядком —
[`scripts/society_needs_report.py`](../../../scripts/society_needs_report.py), отчёт суток с оградами —
[`scripts/society_day_report.py`](../../../scripts/society_day_report.py).
