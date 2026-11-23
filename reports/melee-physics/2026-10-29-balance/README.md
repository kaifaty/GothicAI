# Инструменты оценки баланса арены — проверка 29.10.2026

Проверены инструменты на 24 дуэлях текущей ветки. **Полная матрица и выводы — в [results.md](results.md).**

## Полная матрица одной командой

Из корня актуального checkout:

```sh
scripts/melee-balance-remote.sh
```

Команда синхронизирует исходник, собирает `gothic-body-runner` и `Gothic2Notr`, запускает шесть серий через отдельные `gq quiet`, затем строит отчёт. По умолчанию: **64 дуэли на серию, seed 100000–100063, предел 60000 игровых мс**. Серии: `native-vanilla-1h`, `native-posture-on-1h`, `native-vanilla-2h`, `native-posture-on-2h`, `p30-vanilla-1h`, `p30-posture-on-1h`.

Параметры: `--episodes N`, `--seed N`, `--timeout-ms N`, `--series <имена через запятую>`, `--run-name <новое имя>`. Действия `sync`, `build`, `run`, `report` позволяют выполнять шаги отдельно с одним `--run-name`. Для повторения короткой проверки в новой папке:

```sh
scripts/melee-balance-remote.sh --episodes 4 --seed 100000 --timeout-ms 60000 --run-name smoke-repeat
```

Рабочая папка — `~/gothic-ai/melee-balance/`; кэш базы — `base-cache/`, хранилище базы — `/mnt/shared/gothic-ai/melee-balance/base/`. `gq --archive` получает локальную папку серии и переносит её со сверкой на NAS. Результат печатается как абсолютный путь; новая команда по умолчанию выбирает новое имя по UTC. Используется существующий Python `~/gothic-ai/body/.venv/bin/python`; `MELEE_BALANCE_PYTHON` позволяет указать другой установленный интерпретатор с torch. Постоянные сервисы и обучение не запускаются.

Сам отчёт можно пересчитать из одной или нескольких папок, содержащих `bench.json`:

```sh
python3 scripts/melee_balance_report.py <папки-серий-с-bench.json> --output <папка-отчёта>
```

Выход: `balance.json`, `balance.md`, таблицы в stdout. `--policy-only` исключает повторные native-дуэли из заявок P30; обычное поведение `--checkpoint` сохранено. `--checkpoint --weapon 2h` отклоняется до запуска runner.

## Проверенный исходник и входы

- Коммит исходника runner: `10b698d7b670462d5af685e5c588e0c230be699a` (engine-изменения — `453e4d832`). Отчёт пересчитан текущим `melee_balance_report.py`; его точный diff сохранён в `report/provenance/source.patch`.
- Runner: `~/gothic-ai/melee-balance/build/opengothic/gothic-body-runner`, SHA256 **`f8809af2d5ac846d46b0c14caf4c182ed36e77ca81900c351f7c3b4285eff04d`**.
- Assets: `~/gothic-ai/body-baseline/assets`; save: `~/gothic-ai/body-baseline/runs/melee-reference-stage1-v4/run/save_slot_1.sav`; reference: `~/gothic-ai/body-baseline/runs/melee-reference-stage1-v4`; сценарий — `scripts/bench/body-arena.bench` из этой ветки.
- P30: `~/gothic-ai/body/models/working/best.pt`, SHA256 `edcb77c36358a2d2646e78621c529d5efb818537c9f6bcbede476d9a4d67b97a`, temperature 0, fallback **0 / 0**.
- По 4 дуэли, сиды `100000–100003`, стороны `0/1/0/1`, предел 60 с. Native начинает с 400 см; P30 — с `250/250/300/300` см. Внутри каждого управления входы `vanilla` и `posture-on` одинаковы.
- `vanilla`: `melee_blade_shadow=0`, `melee_physics_hits=0`, `melee_posture=0`. `posture-on`: все три равны 1. Окружение, trace/watch и fork-reset/prefix одинаковы.

## Проверки

- Mac: `CMAKE_BUILD_PARALLEL_LEVEL=3 scripts/build-engine.sh` — exit **0**.
- Ryzen: `gq quick 06a7fe7ce896` собрал игру и runner; повтор `7ae4721e7211` — exit **0**, `ninja: no work to do`. Первая оболочка вернула 2 уже после успешной сборки из-за редактирования выполняемого запускателя; повтор проверил исправный запуск оболочки без пересборки.
- Контракт `scripts/body_setup_probe.py::weapon_contract` на реальном runner: `gq quick 1d7fa7ed6e82`, exit **0**, **4/4** (`default`, `1h`, `2h`, `unknown`). Default и `1h` совпали с прежним профилем; `2h` — топор, W2H (enum 3), навык 60/master 2, валидация `is2H()` пройдена; unknown отклонён.
- Все 6 заявок дуэлей — exit **0**, **24/24** завершились с победителем, таймаутов **0**. В `posture-on 2h` — **6 нокдаунов / 4 дуэли**. Во всех 12 `vanilla` — **0 записей нокдауна**; метрики выключенной подсистемы в отчёте — `null` с причиной.
- Финальный reporter: `gq quick 74b0226d0478`, exit **0**, **6 таблиц серий / 24 дуэли**, все числовые метрики конечны, **0** значений `null` без причины. В **6/6** сериях `swings_started = damaging_hits + blocks + misses`; для ON покрытие журналом клинка полное.
- `python3 -m py_compile scripts/body_sim_bench.py scripts/melee_balance_report.py` и остальных затронутых Python-файлов, `bash -n scripts/melee-balance-remote.sh`, `git diff --check` — exit **0**.

## Архивы

Корень NAS: **`/mnt/shared/gothic-ai/melee-balance/runs/smoke-20261029/`**. В каждой строке ниже `bench/` содержит `bench.json`, эпизоды, события и журналы; рядом — `launcher.log`, исходный коммит/patch и SHA runner.

| Папка серии | gq | Дуэлей | Архив, байт |
|---|---|---:|---:|
| `native-vanilla-1h/` | `075ac65efbfe` | 4 | 18404042 |
| `native-posture-on-1h/` | `02d84d4bffa2` | 4 | 22478202 |
| `native-vanilla-2h/` | `ee8e8e6184ae` | 4 | 27295584 |
| `native-posture-on-2h/` | `97f59cb58d05` | 4 | 36963298 |
| `p30-vanilla-1h/` | `8f6cf593eee4` | 4 | 12752361 |
| `p30-posture-on-1h/` | `651d96ffd3ca` | 4 | 37652319 |

Финальные `balance.json` и `balance.md` — `report/` (**405730 байт** вместе с provenance). В `report/provenance/` лежат source commit/patch, runner SHA, configure/build и Mac build logs. Контракт reset — `/mnt/shared/gothic-ai/melee-balance/checks/weapon-contract/` (**224724 байт**), `weapon-contract.json` и реальные журналы четырёх запросов.

## Определения метрик

Замах — уникальный слой атаки/начало/combo при принятом изменении позы. Сдвиг часов при ускорении завершения `T_1HSFINISH` / `T_2HSFINISH` не создаёт новый замах. Промах — начатый замах без повреждающего попадания и блока противника до конца эпизода, включая прерванный, уклон и стену. Блоки в колонке бойца — выполненные им блоки. Урон — фактическая потеря HP по разности `hp_before − hp_after`.

Время лежания считается от knockdown до `recover`/`cancel` или конца дуэли и включает переход подъёма, пока управление заблокировано. Повтор — новый нокдаун того же бойца не позднее 10 с после `recover`. Доля поражения первого упавшего использует дуэли с нокдауном и победителем. Доли попаданий и DPS считаются из сумм, p90 — линейная интерполяция `(n−1)×0,9`. JSON содержит исходы и метрики каждого эпизода, суммы серии и причины недоступности. `null` в выключенных подсистемах не заменяется нулём.

## Таблицы короткой проверки

| Variant | Controller | Weapon | Duels | W/L/T | Duration mean / median / p90 (s) | KD/duel | Wall/duel |
|---|---|---|---:|---|---|---:|---:|
| vanilla | native | 1h | 4 | 4/0/0 | 15.142 / 14.017 / 20.113 | null | null |
| posture-on | native | 1h | 4 | 3/1/0 | 16.975 / 16.934 / 21.490 | 1.000 | 0.000 |
| vanilla | native | 2h | 4 | 0/4/0 | 22.409 / 19.733 / 31.050 | null | null |
| posture-on | native | 2h | 4 | 2/2/0 | 28.475 / 25.100 / 37.110 | 1.500 | 0.000 |
| vanilla | P30 | 1h | 4 | 4/0/0 | 10.742 / 10.100 / 11.963 | null | null |
| posture-on | P30 | 1h | 4 | 4/0/0 | 29.833 / 33.050 / 35.893 | 1.500 | 0.000 |

### vanilla / native / 1h

| Metric | Total | Both / duel | Primary / duel | Opponent / duel |
|---|---:|---:|---:|---:|
| swings_started | 48 | 12.000 | 8.500 | 3.500 |
| damaging_hits | 37 | 9.250 | 7.250 | 2.000 |
| blocks | 0 | 0.000 | 0.000 | 0.000 |
| misses | 11 | 2.750 | 1.250 | 1.500 |
| damage | 1440 | 360.000 | 300.000 | 60.000 |
| knockdowns | null | null | null | null |
| repeat_knockdowns_within_10s_after_getup | null | null | null | null |
| prone_hits | null | null | null | null |
| prone_damage | null | null | null | null |
| overhead_started | 0 | 0.000 | 0.000 | 0.000 |
| overhead_hits | 0 | 0.000 | 0.000 | 0.000 |
| hit_fraction | — | 0.771 | 0.853 | 0.571 |
| damage_per_hit | — | 38.919 | 41.379 | 30.000 |
| damage_per_s | — | 23.776 | 19.813 | 3.963 |
| prone_damage_fraction | — | null | null | null |
| down_time_fraction | — | — | null | null |

| KD in duel | 0 | 1 | 2 | 3+ |
|---|---:|---:|---:|---:|
| Duel fraction | null | null | null | null |

First knocked-down fighter lost: null (0 decisive duels with knockdowns).
Observed knockdown journal rows: 0.

Unavailable measurements:

- Zero denominator.
- melee_blade_shadow and melee_physics_hits are disabled.
- melee_posture is disabled; live knockdown state is not measured.
- melee_posture is disabled; no knockdown telemetry in this variant.

### posture-on / native / 1h

| Metric | Total | Both / duel | Primary / duel | Opponent / duel |
|---|---:|---:|---:|---:|
| swings_started | 62 | 15.500 | 10.750 | 4.750 |
| damaging_hits | 39 | 9.750 | 6.750 | 3.000 |
| blocks | 1 | 0.250 | 0.000 | 0.250 |
| misses | 22 | 5.500 | 3.750 | 1.750 |
| damage | 1305 | 326.250 | 225.000 | 101.250 |
| knockdowns | 4 | 1.000 | 0.250 | 0.750 |
| repeat_knockdowns_within_10s_after_getup | 0 | 0.000 | 0.000 | 0.000 |
| prone_hits | 7 | 1.750 | 1.250 | 0.500 |
| prone_damage | 173 | 43.250 | 33.750 | 9.500 |
| overhead_started | 1 | 0.250 | 0.000 | 0.250 |
| overhead_hits | 1 | 0.250 | 0.000 | 0.250 |
| hit_fraction | — | 0.629 | 0.628 | 0.632 |
| damage_per_hit | — | 33.462 | 33.333 | 33.750 |
| damage_per_s | — | 19.219 | 13.255 | 5.965 |
| prone_damage_fraction | — | 0.133 | 0.150 | 0.094 |
| down_time_fraction | — | — | 0.098 | 0.221 |

| KD in duel | 0 | 1 | 2 | 3+ |
|---|---:|---:|---:|---:|
| Duel fraction | 0.000 | 1.000 | 0.000 | 0.000 |

First knocked-down fighter lost: 1.000 (4 decisive duels with knockdowns).
Observed knockdown journal rows: 4.

Unavailable measurements:

- Zero denominator.

### vanilla / native / 2h

| Metric | Total | Both / duel | Primary / duel | Opponent / duel |
|---|---:|---:|---:|---:|
| swings_started | 49 | 12.250 | 4.000 | 8.250 |
| damaging_hits | 41 | 10.250 | 2.500 | 7.750 |
| blocks | 2 | 0.500 | 0.000 | 0.500 |
| misses | 6 | 1.500 | 1.000 | 0.500 |
| damage | 1562 | 390.500 | 90.500 | 300.000 |
| knockdowns | null | null | null | null |
| repeat_knockdowns_within_10s_after_getup | null | null | null | null |
| prone_hits | null | null | null | null |
| prone_damage | null | null | null | null |
| overhead_started | 0 | 0.000 | 0.000 | 0.000 |
| overhead_hits | 0 | 0.000 | 0.000 | 0.000 |
| hit_fraction | — | 0.837 | 0.625 | 0.939 |
| damage_per_hit | — | 38.098 | 36.200 | 38.710 |
| damage_per_s | — | 17.426 | 4.039 | 13.388 |
| prone_damage_fraction | — | null | null | null |
| down_time_fraction | — | — | null | null |

| KD in duel | 0 | 1 | 2 | 3+ |
|---|---:|---:|---:|---:|
| Duel fraction | null | null | null | null |

First knocked-down fighter lost: null (0 decisive duels with knockdowns).
Observed knockdown journal rows: 0.

Unavailable measurements:

- Zero denominator.
- melee_blade_shadow and melee_physics_hits are disabled.
- melee_posture is disabled; live knockdown state is not measured.
- melee_posture is disabled; no knockdown telemetry in this variant.

### posture-on / native / 2h

| Metric | Total | Both / duel | Primary / duel | Opponent / duel |
|---|---:|---:|---:|---:|
| swings_started | 85 | 21.250 | 12.750 | 8.500 |
| damaging_hits | 50 | 12.500 | 7.750 | 4.750 |
| blocks | 2 | 0.500 | 0.000 | 0.500 |
| misses | 33 | 8.250 | 4.500 | 3.750 |
| damage | 1580 | 395.000 | 232.500 | 162.500 |
| knockdowns | 6 | 1.500 | 0.750 | 0.750 |
| repeat_knockdowns_within_10s_after_getup | 1 | 0.250 | 0.250 | 0.000 |
| prone_hits | 5 | 1.250 | 0.750 | 0.500 |
| prone_damage | 104 | 26.000 | 23.250 | 2.750 |
| overhead_started | 1 | 0.250 | 0.250 | 0.000 |
| overhead_hits | 0 | 0.000 | 0.000 | 0.000 |
| hit_fraction | — | 0.588 | 0.608 | 0.559 |
| damage_per_hit | — | 31.600 | 30.000 | 34.211 |
| damage_per_s | — | 13.872 | 8.165 | 5.707 |
| prone_damage_fraction | — | 0.066 | 0.100 | 0.017 |
| down_time_fraction | — | — | 0.152 | 0.173 |

| KD in duel | 0 | 1 | 2 | 3+ |
|---|---:|---:|---:|---:|
| Duel fraction | 0.000 | 0.500 | 0.500 | 0.000 |

First knocked-down fighter lost: 0.750 (4 decisive duels with knockdowns).
Observed knockdown journal rows: 6.

Unavailable measurements:

- Zero denominator.

### vanilla / P30 / 1h

| Metric | Total | Both / duel | Primary / duel | Opponent / duel |
|---|---:|---:|---:|---:|
| swings_started | 36 | 9.000 | 8.750 | 0.250 |
| damaging_hits | 35 | 8.750 | 8.500 | 0.250 |
| blocks | 0 | 0.000 | 0.000 | 0.000 |
| misses | 1 | 0.250 | 0.250 | 0.000 |
| damage | 1206 | 301.500 | 300.000 | 1.500 |
| knockdowns | null | null | null | null |
| repeat_knockdowns_within_10s_after_getup | null | null | null | null |
| prone_hits | null | null | null | null |
| prone_damage | null | null | null | null |
| overhead_started | 0 | 0.000 | 0.000 | 0.000 |
| overhead_hits | 0 | 0.000 | 0.000 | 0.000 |
| hit_fraction | — | 0.972 | 0.971 | 1.000 |
| damage_per_hit | — | 34.457 | 35.294 | 6.000 |
| damage_per_s | — | 28.069 | 27.929 | 0.140 |
| prone_damage_fraction | — | null | null | null |
| down_time_fraction | — | — | null | null |

| KD in duel | 0 | 1 | 2 | 3+ |
|---|---:|---:|---:|---:|
| Duel fraction | null | null | null | null |

First knocked-down fighter lost: null (0 decisive duels with knockdowns).
Observed knockdown journal rows: 0.

Unavailable measurements:

- Zero denominator.
- melee_blade_shadow and melee_physics_hits are disabled.
- melee_posture is disabled; live knockdown state is not measured.
- melee_posture is disabled; no knockdown telemetry in this variant.

### posture-on / P30 / 1h

| Metric | Total | Both / duel | Primary / duel | Opponent / duel |
|---|---:|---:|---:|---:|
| swings_started | 125 | 31.250 | 22.500 | 8.750 |
| damaging_hits | 60 | 15.000 | 10.500 | 4.500 |
| blocks | 3 | 0.750 | 0.500 | 0.250 |
| misses | 62 | 15.500 | 11.750 | 3.750 |
| damage | 1797 | 449.250 | 300.000 | 149.250 |
| knockdowns | 6 | 1.500 | 0.250 | 1.250 |
| repeat_knockdowns_within_10s_after_getup | 2 | 0.500 | 0.000 | 0.500 |
| prone_hits | 8 | 2.000 | 1.500 | 0.500 |
| prone_damage | 231 | 57.750 | 40.250 | 17.500 |
| overhead_started | 1 | 0.250 | 0.250 | 0.000 |
| overhead_hits | 0 | 0.000 | 0.000 | 0.000 |
| hit_fraction | — | 0.480 | 0.467 | 0.514 |
| damage_per_hit | — | 29.950 | 28.571 | 33.167 |
| damage_per_s | — | 15.059 | 10.056 | 5.003 |
| prone_damage_fraction | — | 0.129 | 0.134 | 0.117 |
| down_time_fraction | — | — | 0.052 | 0.255 |

| KD in duel | 0 | 1 | 2 | 3+ |
|---|---:|---:|---:|---:|
| Duel fraction | 0.250 | 0.250 | 0.250 | 0.250 |

First knocked-down fighter lost: 1.000 (3 decisive duels with knockdowns).
Observed knockdown journal rows: 6.

Unavailable measurements:

- No knockdown in this duel.
