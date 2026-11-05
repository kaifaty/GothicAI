# `common/society/` — ядро стенда общества NPC

Ядро `society-core` стенда общества ([`docs/society-sim.md`](../../../docs/society-sim.md)): мир событий по выгрузке
`society-world` (v1 и v2), распорядок по правилам движка, журнал, снимки.

**Правило папки: ничего из движка.** Здесь включают только стандартную библиотеку C++ и rapidjson — ни `Tempest`, ни
`world/`, ни `game/`, ни другие файлы `common/`. Так стенд собирает ядро отдельной библиотекой, а движок потом
соберёт то же ядро у себя. Сборка следит за правилом сама: у цели `society-core` в путях включения есть только эта
папка и rapidjson, а общий список исходников движка (`OpenGothic`) эту папку пропускает.

- `society.h` — весь открытый интерфейс: мир, состояние, события, правила движка (`StartDelay`, `SettleDelay`,
  `StartStateSeconds`, `NearPointCm`, `FollowStates`), `loadWorld`, `makeSim`, `reset`, `advance`, `save`/`restore`,
  `drainEvents`, `stateEndAt`, `startStateAt`, перевод длины пути во время;
- `world.cpp` — чтение выгрузки v1/v2, дневная лента распорядка каждого NPC, конец состояния по `Npc::endTime`,
  расписание монстров;
- `sim.cpp` — очередь событий, правила «только распорядок», путь по рёбрам и точки пути для позиций в отметках;
- `journal.h`, `journal.cpp` — двоичный журнал v2 и пятиминутные отметки CSV с позициями.

Утилита — `game/societyday.cpp` (`gothic-society-day`), проверка — `tests/society_core_contract.cpp`
(`gothic-society-core-contract`), сверка с родным днём движка — [`scripts/society_parity.py`](../../../scripts/society_parity.py).
