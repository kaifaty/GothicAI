# Тонкие клинки: самая глубокая точка контакта Item

2026-10-31. **BLOCKED: регрессия полного `gothic-physics-contract`.** Запись матрицы исправлена на `matrix.set(c,r,value)`, контракт собран на Mac с `--parallel 2`. Новые проверки обеих шпаг проходят ON/OFF в двух запусках. Однако полный контракт **дважды завершился с exit 1** на существующей проверке monster CV: `neither a light nor heavy native dynamic hull may be crossed by a walking monster` (`physics_contract.cpp:3620`). Чистая `origin/main` **5040e23bece5a11286491edde70a6a603f5ae11d** прошла тот же полный контракт с **exit 0**. Это регрессия ветки, не исключение для фонового падения; по правилу двух неудач дальнейшие проверки остановлены. Патч Jolt и флаг не менялись. Временный worktree чистой main удалён.

Логи: `.bench/thin-blades/contract-build.log`, `contract-physics.log`, `contract-physics-repeat.log`, `baseline-physics.log`. Игровые диагностики пока **0/8**; доказательство в настоящих сценах ещё не получено. Ожидание занятой очереди Ryzen само по себе ошибкой не считается.

## Причина и изменение

В готовом диагнозе Claude EPA находил глубокую пару точек правильно. Затем Jolt выбирал поддерживающую грань оболочки по направлению нормали. У длинного тонкого тела эта грань может находиться у одного конца, хотя самая глубокая точка уже на другом. Если обрезка грани по треугольнику давала хотя бы одну точку, исходная пара EPA терялась. Решатель держал один конец и позволял другому повернуться сквозь опору; после прохода COM срабатывал `recoverTunneledItems`.

Первый патч вендорного Jolt 5.6.0 сохраняет эту пару, когда в непустом результате обрезки нет точки ближе 1 мм к `inContactPoint1` и в обоих массивах есть место. `PhysicsSettings::mDeepestContactLayers` по умолчанию равен 0; все три вызова, включая CCD LinearCast, получают `false` при нулевой маске. Движок включает только `1u << C_Item`. `[GAME] item_deepest_contact=1` включён по умолчанию, Marvin `toggle item_deepest_contact` меняет режим следующего нативного шага, стенды принимают `ITEM_DEEPEST_CONTACT=0|1`.

Оболочки, возврат, слоп, speculative distance, остальные слои, правила переписи и reader не менялись. Из текущего `known-exceptions.csv` убраны OLDWORLD 1644 и ADDONWORLD 6863; остался флакон NEWWORLD 18261. Новая перепись ещё не подтверждает этот список.

## Диагностики ON/OFF

| Мир / ZEN-ID | Вариант | ON: шаги / максимум, см / возвраты | OFF: шаги / максимум, см / возвраты |
|---|---|---|---|
| OLDWORLD / 1644 | scene | не запущен: остановка после двух падений полного контракта | не запущен |
| OLDWORLD / 1644 | control | не запущен | не запущен |
| ADDONWORLD / 6863 | scene | не запущен | не запущен |
| ADDONWORLD / 6863 | control | не запущен | не запущен |

Команда первой диагностики:

```bash
WORLD=OLDWORLD.ZEN PLACED_ITEMS=1 ITEM_DEEPEST_CONTACT=1 \
  BENCH_HOST=ryzen RYZEN_LOCK_WAIT=3600 RYZEN_MAX=75m \
  RYZEN_CLEAN_IDLE_HOURS=0 scripts/bench.sh placed-items-diagnose-1644 \
  "$PWD/.bench/thin-blades/OLDWORLD-1644-scene-on"
```

После решения по регрессии — восемь последовательных запусков через очередь Ryzen:

```bash
set -e
for deepest in 1 0; do
  if [[ "$deepest" == 1 ]]; then mode=on; else mode=off; fi
  for id in 1644 6863; do
    if [[ "$id" == 1644 ]]; then world=OLDWORLD; else world=ADDONWORLD; fi
    for variant in scene control; do
      scenario="placed-items-diagnose-$id"
      if [[ "$variant" == control ]]; then scenario+="-control"; fi
      WORLD="$world.ZEN" PLACED_ITEMS=1 ITEM_DEEPEST_CONTACT="$deepest" \
        BENCH_HOST=ryzen RYZEN_LOCK_WAIT=3600 RYZEN_MAX=75m \
        RYZEN_CLEAN_IDLE_HOURS=0 scripts/bench.sh "$scenario" \
        "$PWD/.bench/thin-blades/$world-$id-$variant-$mode"
    done
  done
done
```

Если за 60 минут допуска нет или стенд вернул 75, разрешён `BENCH_HOST=local`. Перед каждой игрой проверить `LC_ALL=C pgrep -fl '[G]othic2Notr' || echo absent`, дождаться завершения чужой игры и запускать строго по одной. Совпадения с командами сборки/агентов отличать от игрового процесса дополнительным `LC_ALL=C pgrep -x Gothic2Notr`.

Числа нужно читать из `placed-items-diagnose.jsonl` и `game.log`: максимум расстояния `position_cm` от исходной `origin_cm` в census плюс позы `from` каждого `item recovered`, как в reader отчёта #604. Итоговый event сам по себе может скрыть сброс внутри шага. ON требует 300 нативных шагов, максимум <5 см и 0 возвратов на каждом запуске; OFF — ровно 1 возврат на каждом из тех же четырёх запусков. Максимальный разрыв нативных состояний должен быть 1. Текущих чисел пока нет.

## Нативный контракт и проверки

В существующий `OpenGothic/tests/physics_contract.cpp` добавлен `testItemDeepestContact`: обе готовые оболочки (75 и 92 вершины в рамке тела, метры), кватернионы, позиции и масса `7800 × исходный объём`. Все **167/167 вершин** сверены с выгрузками без изменений. Объёмы трости / рапиры — **0,00205267756 / 0,00332153379 м³**. Опора каждой — один исходный горизонтальный треугольник: `C_Object` для полки трости и `C_Landscape` для рапиры. Используется настоящий `CollisionWorld` с его настройками и нативным шагом 1/60 с; форма не строится заново из игровых ассетов.

ON оставляет штатный возврат включённым, считает сообщения `item recovered`, проверяет 300 шагов, максимум <5 см и COM над плоскостью на каждом шаге. OFF выключает только страховочный возврат пересечений через существующий `setItemWakeEnabled(false)`, чтобы увидеть собственную динамику Jolt: за 20 шагов COM должен пройти под плоскость. Параметры решателя в OFF остаются штатными.

Числа нового случая совпали в обоих запусках полного контракта:

| Оболочка | Режим | Шаги | Максимум сдвига, см | Минимальный COM над опорой, см | Шаг прохода COM | Возвраты |
|---|---|---:|---:|---:|---:|---:|
| OLDWORLD 1644 | ON | 300 | 0,656105 | +1,554932 | нет | 0 |
| OLDWORLD 1644 | OFF | 20 | 35,202251 | −41,043945 | 7 | 0, страховка выключена |
| ADDONWORLD 6863 | ON | 300 | 1,241524 | +3,203125 | нет | 0 |
| ADDONWORLD 6863 | OFF | 20 | 53,966980 | −42,909668 | 7 | 0, страховка выключена |

| Проверка | Результат |
|---|---|
| `bash -n scripts/bench.sh scripts/ryzen.sh` | PASS, exit 0 |
| `git diff --check` | PASS, exit 0 |
| Сверка вершин контракта с двумя dump | PASS, 75/75 и 92/92 |
| Сборка `gothic-physics-contract` на Mac, `--parallel 2` | PASS, exit 0 |
| Новый случай шпаг внутри `gothic-physics-contract` | PASS в обоих запусках: 2 оболочки × 2 режима × 2 запуска |
| Полный `gothic-physics-contract`, ветка | FAIL, 2/2 exit 1 на одной проверке monster CV |
| Полный `gothic-physics-contract`, чистая main 5040e23 | PASS, 1/1 exit 0 |
| Сборка игры и 8 диагностик | NOT RUN: остановка после двух падений полного контракта |
| `gothic-ragdoll-contract` | NOT RUN: остановка после двух падений полного контракта |
| `gothic-ragdoll-save-contract` | NOT RUN: остановка после двух падений полного контракта |
| `gothic-placed-items-contract` | NOT RUN: остановка после двух падений полного контракта |

Нативный ON/OFF подтверждён, но общий набор проверок и игровые критерии готовности не пройдены. Исправление регрессии monster CV требует отдельного решения: пользователь запретил менять проверенный патч Jolt и проводку флага.

Если контракты падают на отпечатке базы после #621, выполнить `scripts/ryzen.sh setup` и повторить. Это подготовка стенда, а не попытка проверки исправленной механики.

## Офлайн-стенд

[bladerepro/](bladerepro/) скопирован из готового диагноза. Пути CMake привязаны к этому репозиторию; аргумент `manifoldMode` 0/1 теперь задаёт маску движущегося слоя через `mDeepestContactLayers`. Исходный `experiment-manifold.patch` сохранён как история эксперимента; рабочая сборка использует вендорный Jolt из этого PR.

Команда сборки из корня репозитория (на Ryzen — внутри своей заявки `gq`, с абсолютным каталогом сборки):

```bash
cmake -S reports/physics/2026-10-31-thin-blades/bladerepro \
  -B "$PWD/.bench/thin-blades/build-bladerepro" -G Ninja \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build "$PWD/.bench/thin-blades/build-bladerepro" --parallel 4
```

Стенд читает исходные `Data/*.VDF` из `GOTHIC_DIR`. Координаты аргументов — сантиметры; после треугольника идут режим 0/1, вариант оболочки 0, число подробных шагов, 300 шагов и трение:

```bash
"$PWD/.bench/thin-blades/build-bladerepro/bladerepro" \
  "$GOTHIC_DIR" OLDWORLD.ZEN ITMW_065_1H_SWORDCANE_02.MRM \
  -12604.3779 2955.90576 -33644.2891 \
  -12662.5645 2952.96582 -33685.8516 \
  -12550.6016 2952.96582 -33566.0703 \
  -12533.5527 2952.96582 -33597.7812 1 0 10 300 0.5
"$PWD/.bench/thin-blades/build-bladerepro/bladerepro" \
  "$GOTHIC_DIR" ADDONWORLD.ZEN ITMW_050_1H_SWORD_RAPIER_01.MRM \
  4271.98438 -3254.3125 25873.4824 \
  4245.82031 -3258.76465 25742.9688 \
  4087.59204 -3258.76465 26070.0645 \
  4358.33301 -3258.76465 25964.2266 1 0 10 300 0.5
```

Офлайн-стенд в этой задаче не запускался: диагноз принят готовым.

## Перепись

**ожидает** — перепись и пробу четырёх миров выполняет Claude после PR. В этой задаче они не запускались.
