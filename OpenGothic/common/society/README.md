# `common/society/` — ядро стенда общества NPC

Ядро `society-core` стенда общества ([`docs/society-sim.md`](../../../docs/society-sim.md)): мир событий по выгрузке
`society-world` v1, распорядок, журнал, снимки.

**Правило папки: ничего из движка.** Здесь включают только стандартную библиотеку C++ и rapidjson — ни `Tempest`, ни
`world/`, ни `game/`, ни другие файлы `common/`. Так стенд собирает ядро отдельной библиотекой, а движок потом
соберёт то же ядро у себя. Сборка следит за правилом сама: у цели `society-core` в путях включения есть только эта
папка и rapidjson, а общий список исходников движка (`OpenGothic`) эту папку пропускает.

- `society.h` — весь открытый интерфейс: мир, состояние, события, `loadWorld`, `makeSim`, `reset`, `advance`,
  `save`/`restore`, `drainEvents`, перевод длины пути во время;
- `world.cpp` — чтение выгрузки v1 и дневная лента распорядка каждого NPC;
- `sim.cpp` — очередь событий, правила «только распорядок», путь по рёбрам;
- `journal.h`, `journal.cpp` — двоичный журнал и пятиминутные отметки CSV.

Утилита — `game/societyday.cpp` (`gothic-society-day`), проверка — `tests/society_core_contract.cpp`
(`gothic-society-core-contract`).
