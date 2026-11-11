# Автоматический ключ построителя переписи — 2026-11-11

Долг [#904](https://github.com/kaifaty/GothicAI/issues/904): код построения тел и правила переписи теперь входят в ключ `PlacedCensusCache` через SHA-256 исходников. Отпечаток генерируется при сборке `cmake -P`; список зависимостей — [`PlacedCensusBuilder.cmake`](../../../OpenGothic/cmake/PlacedCensusBuilder.cmake). `RulesVersion=3` обозначает формат файла с сохранённым отпечатком построителя. Формат `placedItemCensusInputs`, правила переписи и другие производные кэши не менялись.

## Проверка сборки и контракта

```sh
REMOTE_ENV='OPENGOTHIC_BASE_CACHE=/home/kaifaty/gothic-ai/codex-check/codex-census-builder-key-/base-cache OPENGOTHIC_BASE_STORE=/home/kaifaty/gothic-ai/codex-check/codex-census-builder-key-/base-store' \
  ~/.codex/checks/gothic/engine-targets.sh . gothic-placed-items-contract
```

Выход 0: контракт на Ryzen прошёл с первого запуска, игра на Mac собрана. `censusCache` проверил непустой SHA-256 в ключе и отдельный статус `Builder`, сохранив проверки `Absent`, `Hit`, `Inputs`, `Version`, `Corrupt`, регистра мира и выключенного кэша. `scripts/check.py plan` выбрал 0 Python-тестов.

Заголовки Mac и Ryzen побайтно совпали. Отпечатки:

```text
A: b3940108d57c73412f3d6913267ecbba2dab4eadb61be36f155c3b4e0513036f
B: fde4f696fa80a95a4209c25bd0f4e08a6e25df14aec015ab4018c9c551f28c6e
```

В B в конец `dynamicworld_items.cpp` добавлены только пустая строка и `// Census builder fingerprint invalidation probe.`. Обычный `cmake --build build --parallel 12 --target Gothic2Notr`, без конфигурации, перегенерировал заголовок и скомпилировал ровно два объекта: `dynamicworld_items.cpp.o` и включающий заголовок `placedcensuscache.cpp.o`, затем перелинковал игру. После проверки комментарий удалён; исходник и заголовок A восстановлены побайтно. Проверочный комментарий в git не попадал.

Пересборка A без изменений: `ninja: no work to do.`; mtime заголовка до и после — `2026-11-11 11:19:59.946582998 +0300`. Генератор использует только команды CMake, без bash/Python и платформенных утилит; сборка Windows отдельно не запускалась.

## Четыре загрузки NEWWORLD на Ryzen

Ryzen 3950X / RTX 3080, одна заявка `gq quiet` (`e316f69d9ba4`, 12 потоков, 10 ГиБ, GPU 2 ГиБ), выход 0. Внутри — четыре запуска `scripts/bench.sh loadprobe-idle` с `NO_BUILD=1`, `WORLD=NEWWORLD.ZEN`, `OPENGOTHIC_LOADPROBE=1`, `OPENGOTHIC_LOADPROBE_SERIAL=0`, `OPENGOTHIC_PLACED_CENSUS_CHECK=0`. Оригинальные меши и текстуры, без внешних кэшей fracture/convex-parts. Рендер в `TEMPEST_OFFSCREEN=1`, 800×600, без окна, звук выключен.

Рабочая папка — `/home/kaifaty/gothic-ai/codex-check/codex-census-builder-key-`; абсолютные OUT — её `probe-runs/A1`, `A2`, `B1`, `B2`. База `c885037e…dfd09e` скопирована в собственный `base-cache`, `OPENGOTHIC_BASE_STORE` указывал на собственный `base-store`. Перед A1 удалён только собственный `NEWWORLD.placed-census`; между A1/A2/B1/B2 кэш не удалялся. Сборка B меняла только комментарий. Во всех четырёх стартах — 2331 кандидат, 228 допущенных; по одному `session_end` и `rec_stop`.

| Старт | Причина/результат | `placed_items`, мс | Native-тела переписи | Запросы переписи | LA1 |
|---|---|---:|---:|---:|---:|
| A1 | miss (absent) | 849,4 | 2331 | 1 | 4,86 |
| A2 | hit | 120,3 | 0 | 0 | 4,67 |
| B1 | miss (builder) | 857,0 | 2331 | 1 | 4,85 |
| B2 | hit | 120,2 | 0 | 0 | 4,43 |

Точные строки журнала, по порядку A1, A2, B1, B2:

```text
placed item census cache: miss (absent), recomputed 2331 items
placed item census cache: hit, 2331 items
placed item census cache: miss (builder), recomputed 2331 items
placed item census cache: hit, 2331 items
```

На обоих попаданиях `physics_body count=0` и `placement_queries count=0`. `physics_prepare count=2559` — 2331 кандидат и 228 подготовок оболочки допущенных предметов после чтения решения; до решения тела и оболочки кандидатов не строятся. Тёплые стадии 120,3 и 120,2 мс не медленнее [прежних 124,0 мс (120,9–131,7)](../2026-11-03-streaming-optimisation/census-bodies.md#ryzen-новый-старт-newworld); дополнительная серия не запускалась.

Полные журналы и логи сборок перенесены `gq --archive` со сверкой в `/mnt/shared/gothic-ai/codex-check/codex-census-builder-key-/probe-runs` (42 658 508 байт). После проверки собственные `base-cache` и `base-store` удалены; общий кэш базы и данные `~/.local/share/gothic-ai` не изменялись.
