# Статичные меши vob четырёх миров Gothic II + Ночь Ворона

Офлайн-перепись для этапа 2 [плана своего контента](../../../docs/own-content.md), 2026-10-20. В репозитории только имена ресурсов и агрегированные числа; вершины, UV, пиксели и меши не экспортируются.

Повторить на Mac из корня репозитория:

```sh
scripts/content-meshes.sh --self-test
scripts/content-meshes.sh
scripts/content-meshes.sh reports/content/2026-10-world-meshes
```

`GOTHIC_DIR` задаёт установленную игру (по умолчанию `/Volumes/DevSSD/Dev/games/Gothic II`); необязательный аргумент — каталог отчёта. Скрипт использует готовую библиотеку ZenKit или собирает только цель `zenkit`; бинарник — `OpenGothic/build/meshes/world-meshes`. Игра, GPU и модели ИИ не запускаются.

## Состав и метод

- Смонтированы `Meshes.vdf`, `Meshes_Addon.vdf`, `Worlds.vdf`, `Worlds_Addon.vdf` именно в этом порядке с `VfsOverwriteBehavior::ALL`: аддон перекрывает базу независимо от временных меток. Loose-файлы и моды не подключаются.
- Обход и геометрия повторяют `world-textures.cpp`: все видимые vob с `.3DS` → `.MRM`, `.MRM` или `.MSH`, полная детализация. Флаг `vob_static` не фильтрует объекты: входят и лежащие предметы `oCItem`. Дочерние vob обходятся независимо от родителя. Ключ визуала — basename в верхнем регистре без расширения, как в `meshes.json`.
- Меш мира не входит. Исключены NPC, предметы в руках и инвентарях NPC, скрытые визуалы, декали, частицы, `.MDS`/`.MDL` и другие анимированные/морф-модели; сундуки и двери на `.MDS` сюда не попадают. Отсутствующий меш, коллизия ключей разных форматов или ошибка ZenKit прерывает прогон.
- Площадь — сумма площадей треугольников (см² / 10 000), для каждого экземпляра со своим мировым трансформом. Вложенные vob уже имеют object-to-world трансформ; родитель повторно не применяется. Это площадь всех поверхностей, не земли и не видимых пикселей; прозрачность не вычитается. `.MSH` триангулируется веером с тем же исключением portal/ghost occluder/outdoor, что в переписи текстур.
- Локальный bbox вычислен по позициям вершин исходного меша; размеры переведены из см в м. Локальная площадь и число треугольников относятся к одному экземпляру без трансформа. Текстуры — непустые имена материалов, basename в верхнем регистре, без повторов, по алфавиту.
- `movable` и `destructible` — совпадение ключа визуала с глобом первого столбца `OpenGothic/data/movable-vobs.csv` и `destructible-vobs.csv` (без регистра, `*` — любая строка, `?` — один символ). Это признаки таблиц движка, не вывод из имени семейства или типа vob.

## Семейства и районы

Сначала применяется первая подходящая строка явной таблицы визуал-глоб → семейство ниже. Иначе имя без расширения разбивается по `_` и просматривается с хвоста: пропускаются числа и варианты (`01`, `3`, `01B`, `V1`, `V2`, одиночная буква), коды размера `\d+X\d+(X\d+)?M?`, коды полигонов `\d+P`/`\d+PP` и перечисленные модификаторы. После каждого снятого хвоста правило повторяется: `STONE_02_BIG_124P` → `STONE`. У токена отрезаются хвостовые цифры, затем снова проверяется модификатор (`M02` → `M` снимается, `BARREL2` → `BARREL` остаётся). Первое оставшееся существительное становится семейством с явной нормализацией из второй таблицы; если ничего не осталось, используется полное имя.

Полный список модификаторов: `ALONE`, `BEND`, `BENT`, `BIG`, `BIGFLAT`, `BIGHIGH`, `BL`, `BR`, `BROKEN`, `CLOSED`, `CORNER`, `DEFECT`, `DESTROYED`, `EMPTY`, `FILLED`, `GROUND`, `HALF`, `HEAVY`, `HIGH`, `HUGE`, `IN`, `INDOOR`, `L`, `LARGE`, `LEFT`, `LONG`, `LOW`, `M`, `MED`, `MEDIUM`, `MIDDLE`, `MIS`, `ML`, `MS`, `NEW`, `NICE`, `NORMAL`, `OLD`, `OPEN`, `OUT`, `PART`, `PEASANT`, `POOR`, `R`, `RED`, `RICH`, `RIGHT`, `ROUND`, `SHORT`, `SMALL`, `STRAIGHT`, `TL`, `TOP`, `TR`, `VERYHIGH`, `WAR`, `WHITE`, `XL`.

Явная таблица (регистр не учитывается, `*` и `?` — глоб):

| Визуал-глоб | Семейство |
|---|---|
| `NW_CITY_WINDOW_*` | WINDOW |
| `NW_CITY_BEAMS_*` | BEAMS |
| `NW_CITY_SIGN_*` | SIGN |
| `NW_CITY_MARKETSTALL_*` | MARKETSTALL |
| `NW_NATURE_STONE_CULT_TOP_*` | STONE |
| `NW_NATURE_STONE_CULT_STAND_*` | STONE |
| `NW_NATURE_STONE_CULT_PART_*` | STONE |
| `NW_NATURE_STONE_CULT_SCENE_*` | STONE |
| `OC_DECORATE_V?` | DECORATION |
| `NC_LOB_NICE*` | DECORATION |
| `ORC_E_DEKO_*` | DECORATION |
| `NW_MISC_*_STANDY` | DISPLAY |
| `OC_BARRELS_REIHE` | BARRELROW |
| `OC_BARRELS_STRUBBELIG` | BARRELPILE |
| `NW_MISC_STRAW_GROUP_*` | STRAWGROUP |
| `NW_NATURE_GREAT_WEED_XGROUP` | WEEDGROUP |
| `OW_LOB_BUSH_WATER_*` | BUSH |
| `ADDON_STONES_CRYSTAL_ROSE_*` | CRYSTAL |
| `OW_SHIPWRECK_*` | SHIPWRECK |
| `NW_DRAGONISLE_TORTURE_*` | TORTUREDEVICE |
| `EVT_SPAWN_*` | SPAWNPOINT |
| `NW_MISC_INNOS_GROUND_*` | STATUE |
| `NW_MISC_DECO_INNOS_*` | STATUE |
| `NW_HARBOUR_IRONROPEHOLDER_WROPE_*` | IRONROPEHOLDER |
| `DT_SKELETON_V??_HEAD` | SKULL |
| `NW_DRAGONISLE_BIGDOOR_HEAD_*` | BIGDOOR |
| `OC_LOB_BOWTRAIN` | TRAININGTARGET |

Явные эквивалентные существительные; автоматического выделения частей слитного слова и единственного числа нет:

| Остаток имени | Семейство |
|---|---|
| BUSHES | BUSH |
| CACTUSBIG | CACTUS |
| CACTUSLONG | CACTUS |
| CRATEBROKEN | CRATE |
| FISHINGNETBIG | FISHINGNET |
| FISHINGNETSMALL | FISHINGNET |
| HOHETANNEN | HOHETANNE |

`CRATEPILE` и `BARRELGROUP` сохраняются отдельно: это группы, другая модель. `STRAWGROUP`, `WEEDGROUP`, `BARRELROW` и `BARRELPILE` также обозначают целые группы. Для непрозрачных имён `DECORATE`, `NICE`, `DEKO` выбрано общее существительное `DECORATION`, без предположения о точном виде украшения. По именам просмотрены все 533 визуала и все 221 получившихся семейств: чисел, размерных кодов, прилагательных и позиций среди семейств нет.

`kind` определяется **только явным списком семейств nature**: `BAUMSTAMM`, `BAUMSTUMPF`, `BIGTREE`, `BLAETTERDACH`, `BROKENSTONE`, `BUSH`, `CACTUS`, `CAVEWEBS`, `COUPLESTONES`, `CRYSTAL`, `DEADTREE`, `DUCKWEED`, `FARN`, `FARNTEPPICH`, `GRASSGROUP`, `HANGPLANTS`, `HOHETANNE`, `JUNGLETREE`, `LIANA`, `MANYSTONES`, `MUSHROOM`, `OLDTREE`, `PALM`, `PLANT`, `REED`, `ROOT`, `ROOTS`, `SIDEPLANT`, `SMALLPALM`, `SMALLTREE`, `SMALLWEED`, `SPIDERWEBS`, `STALAG`, `STONE`, `TANNE`, `THINTREE`, `TREE`, `TREEGROUP`, `WALLFERN`, `WATERGRASS`, `WATERLILI`, `WEEDGROUP`, `WOODPIECE`. Всё остальное — `object`, в том числе кости, черепа, факелы и заборы; `NW_NATURE_FENCE_01` имеет `kind=object`. Слово `NATURE` в имени визуала не используется для классификации.

Каждому учтённому vob назначается ближайший waypoint своего `world.way_net->points`, исключая `free_point`, в радиусе **30 м** по 3D-расстоянию от позиции vob. На равном расстоянии выбирается первое имя по алфавиту. При отсутствии точки — `<NONE>`. У имени снимаются хвостовые числовые токены, после чего остаются **первые три токена** по `_`: гавань `NW_CITY_HABOUR_*` (написание оригинала), рынок `NW_CITY_MERCHANT_*`, верхний квартал `NW_CITY_UPTOWN_*` и казармы `NW_CITY_KASERN_*` остаются разными районами. `NW_CITY_TO` объединяет переходы от города. Это приближение по навигационным именам, не географические границы: большие объекты приписываются по началу координат, не по покрываемой территории.

## Файлы и столбцы

[Все миры](all-worlds.csv), [Хоринис](newworld.csv), [Долина рудников](oldworld.csv), [Яркендар](addonworld.csv), [Остров драконов](dragonisland.csv): строка на визуал, сортировка по экземплярам в области файла, затем по площади (оба по убыванию), затем по имени.

- `world`: имя мира или `ALL`; `visual`, `format`: ключ и `MRM`/`MSH` (`.3DS` считается `MRM`).
- `instances`, `item_instances`, `area_total_m2`: экземпляры, из них `oCItem`, сумма мировых площадей в области файла. `instances_newworld`, `instances_oldworld`, `instances_addonworld`, `instances_dragonisland`, `instances_total` в каждом файле дают глобальные числа этого визуала по четырём мирам.
- `triangles`, `bbox_x_m`, `bbox_y_m`, `bbox_z_m`, `area_per_instance_m2`: полная локальная геометрия одного экземпляра. `textures`: имена через `;`; `movable`, `destructible`: 0/1; `family`: семейство; `kind`: `nature` или `object`.
- `cum_instances_fraction`, `cum_area_fraction`: накопленные доли 0–1 в порядке строк, с итогами области файла в знаменателе.

[families.csv](families.csv): семейство, его `kind`, число разных визуалов, экземпляры по мирам и всего, суммарная площадь, число подвижных визуалов, до пяти самых частых примеров; сортировка по экземплярам, площади и имени.

[areas.csv](areas.csv): `world`, `area`, `instances`, `area_m2`, число разных `visuals` и `families`, `visuals_for_80pct_instances` (минимум визуалов для ≥80 % экземпляров района при сортировке по экземплярам), `movable_instances`, `object_instances`, `object_visuals`, `object_visuals_for_80pct` (экземпляры и разные визуалы только `object`, затем минимум этих визуалов для ≥80 % экземпляров `object` района), `top_visuals` (до пяти `ИМЯ:число` через `;`). Сортировка по имени мира, затем экземплярам, площади и имени района.

## Сверка сумм

Независимая сумма по всем треугольникам считается до разнесения по визуалам. Прогон падает, если сумма экземпляров не равна числу учтённых vob или площадь расходится больше чем на 0,1 %. Районные суммы проверяются отдельно. Для сравнения взяты опубликованные числа [переписи текстур](../2026-10-world-textures/README.md), округлённые до 0,001 м².

| Мир | Учтено vob | Визуалов | Семейств | Сумма CSV, м² | По треугольникам, м² | Ошибка, % | Vob / площадь отчёта текстур | Совпало |
|---|---:|---:|---:|---:|---:|---:|---|---|
| NEWWORLD.ZEN | 11546 | 312 | 145 | 2533385.016 | 2533385.016 | 0.000000000 | 11546 / 2533385.016 | да |
| OLDWORLD.ZEN | 4438 | 203 | 107 | 1781506.359 | 1781506.359 | 0.000000000 | 4438 / 1781506.359 | да |
| ADDONWORLD.ZEN | 4718 | 248 | 120 | 1316101.720 | 1316101.720 | 0.000000000 | 4718 / 1316101.720 | да |
| DRAGONISLAND.ZEN | 499 | 91 | 54 | 13138.775 | 13138.775 | 0.000000000 | 499 / 13138.775 | да |
| Все миры | 21201 | 533 | 221 | 5644131.870 | 5644131.870 | 0.000000000 | — | — |

Число vob и площадь совпали во всех четырёх мирах в точности опубликованного округления.

| Область | Вид | Экземпляров | Доля экземпляров, % | Площадь, м² | Доля площади, % |
|---|---|---:|---:|---:|---:|
| Все миры | nature | 13508 | 63.714 | 5529548.222 | 97.970 |
| Все миры | object | 7693 | 36.286 | 114583.649 | 2.030 |
| NEWWORLD.ZEN | nature | 7126 | 61.718 | 2477824.577 | 97.807 |
| NEWWORLD.ZEN | object | 4420 | 38.282 | 55560.440 | 2.193 |

В этих vob встречаются 533 визуалов MRM и 0 MSH. Это использованные визуалы четырёх миров, а не все 1 355 MRM в архивах из плана замены: неиспользованные ресурсы и другие классы визуалов не перечисляются. Учтённых `oCItem`: 0; `item_instances` считает только явно заданные подходящие визуалы vob из ZEN, без восстановления визуалов предметов по скриптам.

| Мир | Скрытых статичных визуалов | Прочих исключённых непустых визуалов |
|---|---:|---:|
| NEWWORLD.ZEN | 18 | 2986 |
| OLDWORLD.ZEN | 4 | 734 |
| ADDONWORLD.ZEN | 23 | 1177 |
| DRAGONISLAND.ZEN | 7 | 363 |

## Топ-50 визуалов всех миров по экземплярам

NW / OW / AW / DI — экземпляры в Хоринисе, Долине рудников, Яркендаре и на Острове драконов. M / D — movable / destructible; bbox и площадь одного экземпляра локальные.

| Визуал | Семейство | NW | OW | AW | DI | Всего | Треугольников | Bbox X × Y × Z, м | Площадь одного, м² | Всего, м² | M / D |
|---|---|---:|---:|---:|---:|---:|---:|---|---:|---:|---|
| `NW_NATURE_GRASSGROUP_01` | GRASSGROUP | 688 | 63 | 174 | 0 | 925 | 40 | 7.290 × 2.200 × 6.879 | 189.788 | 175553.993 | 0 / 0 |
| `NW_NATURE_BUSH_120P` | BUSH | 690 | 0 | 116 | 0 | 806 | 120 | 3.707 × 4.229 × 7.431 | 213.520 | 172096.932 | 0 / 0 |
| `NW_NATURE_FARNTEPPICH_306P` | FARNTEPPICH | 565 | 0 | 211 | 0 | 776 | 306 | 11.888 × 3.028 × 15.177 | 447.698 | 347413.639 | 0 / 0 |
| `NW_CAVEWEBS_V201` | CAVEWEBS | 455 | 84 | 209 | 9 | 757 | 20 | 7.146 × 5.392 × 7.316 | 263.424 | 199411.978 | 0 / 0 |
| `OW_NATURE_SMALLTREE_01` | SMALLTREE | 286 | 192 | 55 | 2 | 535 | 191 | 1.786 × 5.332 × 2.901 | 25.311 | 13541.248 | 0 / 0 |
| `NW_NATURE_TANNE_75P` | TANNE | 389 | 143 | 0 | 0 | 532 | 75 | 11.064 × 11.981 × 11.272 | 695.488 | 369999.795 | 0 / 0 |
| `NW_NATURE_FARN_102P` | FARN | 101 | 114 | 303 | 0 | 518 | 102 | 4.358 × 1.644 × 4.237 | 59.965 | 31061.960 | 0 / 0 |
| `MIN_LOB_COUPLESTONES` | COUPLESTONES | 236 | 152 | 52 | 1 | 441 | 261 | 10.656 × 1.291 × 8.381 | 33.493 | 14770.594 | 0 / 0 |
| `OW_MISC_WALL_TORCH_01` | TORCH | 145 | 21 | 170 | 88 | 424 | 134 | 0.162 × 0.759 × 0.455 | 0.362 | 153.648 | 0 / 0 |
| `OW_NATURE_BUSH_02` | BUSH | 104 | 229 | 51 | 0 | 384 | 400 | 6.063 × 1.430 × 4.879 | 151.446 | 58155.125 | 0 / 0 |
| `OW_MUSHROOM_V2` | MUSHROOM | 133 | 52 | 122 | 19 | 326 | 660 | 1.978 × 2.165 × 3.717 | 5.109 | 1665.614 | 0 / 0 |
| `MIN_LOB_STONE_V2_30` | STONE | 2 | 298 | 0 | 0 | 300 | 52 | 6.831 × 4.461 × 5.292 | 96.978 | 29093.280 | 0 / 0 |
| `NW_CITY_OILLAMP_01` | OILLAMP | 223 | 0 | 0 | 19 | 242 | 138 | 0.291 × 0.485 × 0.410 | 0.560 | 135.539 | 0 / 0 |
| `NW_NATURE_FARN_SMALL_102P` | FARN | 121 | 0 | 120 | 0 | 241 | 102 | 4.358 × 1.492 × 4.237 | 53.471 | 12886.600 | 0 / 0 |
| `OC_BARREL_V1_DESTROYED` | BARREL | 94 | 46 | 88 | 2 | 230 | 84 | 0.738 × 1.108 × 0.738 | 4.520 | 1039.620 | 0 / 0 |
| `OW_NATURE_BUSH_01` | BUSH | 92 | 131 | 0 | 0 | 223 | 20 | 2.007 × 2.561 × 2.521 | 32.143 | 7167.838 | 0 / 0 |
| `NW_NATURE_LONG_BUSH_360P` | BUSH | 156 | 0 | 63 | 0 | 219 | 360 | 6.571 × 4.229 × 16.233 | 640.559 | 140282.492 | 0 / 0 |
| `NW_MISC_SPIDERWEBS_01` | SPIDERWEBS | 140 | 4 | 74 | 0 | 218 | 256 | 5.192 × 2.828 × 5.189 | 124.036 | 27039.867 | 0 / 0 |
| `NW_NATURE_HANGPLANTS_02` | HANGPLANTS | 44 | 0 | 173 | 0 | 217 | 128 | 5.446 × 5.692 × 5.231 | 100.635 | 21837.789 | 0 / 0 |
| `NW_NATURE_FARN_BIG_01` | FARN | 91 | 0 | 119 | 0 | 210 | 216 | 4.857 × 2.330 × 5.040 | 87.459 | 18366.484 | 0 / 0 |
| `NW_NATURE_TANNE_100P` | TANNE | 209 | 0 | 0 | 0 | 209 | 99 | 11.064 × 11.981 × 11.272 | 866.061 | 181006.683 | 0 / 0 |
| `NW_NATURE_SIDEPLANT_01` | SIDEPLANT | 202 | 0 | 1 | 0 | 203 | 10 | 2.102 × 0.980 × 0.745 | 6.205 | 1259.680 | 0 / 0 |
| `NW_NATURE_FENCE_01` | FENCE | 167 | 1 | 20 | 0 | 188 | 60 | 3.202 × 1.808 × 0.487 | 5.302 | 996.749 | 1 / 1 |
| `NW_CITY_CANDLE_01` | CANDLE | 157 | 0 | 18 | 7 | 182 | 124 | 0.400 × 0.322 × 0.490 | 0.445 | 81.004 | 1 / 0 |
| `NW_HARBOUR_BARREL_01` | BARREL | 150 | 0 | 27 | 3 | 180 | 60 | 0.798 × 1.108 × 0.798 | 3.187 | 573.718 | 1 / 1 |
| `NW_NATURE_BUSH_25P` | BUSH | 168 | 5 | 4 | 0 | 177 | 26 | 4.844 × 5.380 × 3.551 | 98.714 | 17472.303 | 0 / 0 |
| `OW_LOB_BUSH_V5` | BUSH | 39 | 132 | 0 | 0 | 171 | 48 | 1.348 × 1.857 × 1.348 | 13.329 | 2279.289 | 0 / 0 |
| `NW_NATURE_BUSH_56P` | BUSH | 164 | 0 | 4 | 0 | 168 | 56 | 2.432 × 1.992 × 3.356 | 38.966 | 6546.219 | 0 / 0 |
| `OW_NATURE_BIG_TREE_02_380P` | TREE | 11 | 153 | 0 | 0 | 164 | 375 | 26.686 × 31.024 × 26.687 | 7476.365 | 1226123.577 | 0 / 0 |
| `NW_HARBOUR_CRATEPILE_01` | CRATEPILE | 132 | 0 | 8 | 24 | 164 | 135 | 3.068 × 2.102 × 2.870 | 37.518 | 6152.969 | 0 / 0 |
| `OW_LOB_WOODPLANKS_V2` | WOODPLANKS | 28 | 125 | 10 | 0 | 163 | 12 | 0.250 × 6.500 × 0.550 | 10.506 | 1712.426 | 0 / 0 |
| `NW_NATURE_SIDEPLANT_BIG_01` | SIDEPLANT | 97 | 0 | 56 | 0 | 153 | 10 | 3.738 × 1.743 × 1.324 | 19.617 | 3001.368 | 0 / 0 |
| `OC_BARREL_V1` | BARREL | 8 | 87 | 54 | 0 | 149 | 60 | 0.798 × 1.108 × 0.798 | 3.187 | 474.910 | 1 / 1 |
| `ORC_SKELETON_V3` | SKELETON | 98 | 25 | 12 | 6 | 141 | 816 | 0.787 × 0.238 × 2.362 | 1.608 | 226.707 | 0 / 0 |
| `NW_NATURE_HOHETANNE_59P` | HOHETANNE | 88 | 47 | 0 | 0 | 135 | 59 | 5.354 × 14.066 × 5.097 | 224.265 | 30275.709 | 0 / 0 |
| `NW_NATURE_SMALLTREE_79P` | SMALLTREE | 130 | 0 | 1 | 0 | 131 | 79 | 6.322 × 12.005 × 3.937 | 247.425 | 32412.606 | 0 / 0 |
| `NW_NATURE_BUSH_300P` | BUSH | 115 | 12 | 3 | 0 | 130 | 320 | 3.149 × 1.472 × 2.767 | 40.625 | 5281.291 | 0 / 0 |
| `NW_NATURE_HANGPLANTS_01` | HANGPLANTS | 95 | 0 | 34 | 0 | 129 | 192 | 5.028 × 3.639 × 3.514 | 71.165 | 9180.247 | 0 / 0 |
| `OW_LOB_TREE_V11` | TREE | 31 | 97 | 0 | 0 | 128 | 159 | 19.208 × 20.021 × 19.078 | 1517.248 | 194202.107 | 0 / 0 |
| `AW_CRATE_01` | CRATE | 1 | 104 | 22 | 0 | 127 | 12 | 1.200 × 1.000 × 1.000 | 6.800 | 863.600 | 1 / 1 |
| `NW_HARBOUR_BARRELGROUP_02` | BARRELGROUP | 97 | 6 | 13 | 6 | 122 | 420 | 4.338 × 1.359 × 1.790 | 22.311 | 2721.969 | 0 / 0 |
| `OC_SAECKE_01` | SAECKE | 98 | 11 | 12 | 0 | 121 | 252 | 1.956 × 0.908 × 1.589 | 8.176 | 989.281 | 0 / 0 |
| `NW_HARBOUR_CRATEBROKEN_01` | CRATE | 92 | 1 | 27 | 0 | 120 | 152 | 1.200 × 1.000 × 1.000 | 6.704 | 804.431 | 0 / 0 |
| `NW_HARBOUR_CRATE_01` | CRATE | 105 | 0 | 5 | 9 | 119 | 12 | 1.200 × 1.000 × 1.000 | 6.800 | 809.200 | 1 / 1 |
| `ORC_SKULLSONFLOOR` | SKULLSONFLOOR | 107 | 3 | 6 | 1 | 117 | 232 | 0.854 × 0.316 × 0.811 | 1.216 | 142.227 | 0 / 0 |
| `OW_LOB_BUSH_V7` | BUSH | 0 | 112 | 0 | 0 | 112 | 98 | 3.670 × 1.751 × 3.848 | 17.644 | 1976.155 | 0 / 0 |
| `NW_NATURE_BAUMSTAMM_01_30P` | BAUMSTAMM | 104 | 0 | 7 | 0 | 111 | 30 | 5.926 × 0.885 × 1.014 | 13.342 | 1480.980 | 0 / 0 |
| `OW_LOB_FENCE_V1` | FENCE | 0 | 106 | 0 | 0 | 106 | 60 | 3.202 × 1.808 × 0.487 | 5.302 | 561.997 | 1 / 1 |
| `NW_NATURE_STONE_BIGHIGH_01` | STONE | 85 | 1 | 19 | 0 | 105 | 40 | 6.509 × 2.941 × 6.408 | 53.288 | 5595.273 | 0 / 0 |
| `ADDON_DUNGEON_BROKENSTONE_03` | BROKENSTONE | 29 | 0 | 74 | 0 | 103 | 70 | 4.094 × 0.168 × 3.352 | 4.941 | 508.964 | 0 / 0 |

## Топ-20 семейств по экземплярам

| Семейство | Визуалов | Экземпляров | Площадь, м² | Подвижных визуалов | Примеры |
|---|---:|---:|---:|---:|---|
| BUSH | 21 | 2789 | 451601.024 | 0 | NW_NATURE_BUSH_120P; OW_NATURE_BUSH_02; OW_NATURE_BUSH_01; NW_NATURE_LONG_BUSH_360P; NW_NATURE_BUSH_25P |
| STONE | 27 | 1071 | 70383.444 | 0 | MIN_LOB_STONE_V2_30; NW_NATURE_STONE_BIGHIGH_01; MIN_MOB_STONE_V2_20; MIN_LOB_STONE_V3_30; ADDON_STONES_STONE_01_84P |
| FARN | 3 | 969 | 62315.044 | 0 | NW_NATURE_FARN_102P; NW_NATURE_FARN_SMALL_102P; NW_NATURE_FARN_BIG_01 |
| GRASSGROUP | 1 | 925 | 175553.993 | 0 | NW_NATURE_GRASSGROUP_01 |
| FARNTEPPICH | 2 | 866 | 399144.461 | 0 | NW_NATURE_FARNTEPPICH_306P; NW_NATURE_FARNTEPPICH_612P |
| CAVEWEBS | 3 | 779 | 205960.786 | 0 | NW_CAVEWEBS_V201; OW_CAVEWEBS_V1; OW_CAVEWEBS_V2 |
| TANNE | 2 | 741 | 551006.477 | 0 | NW_NATURE_TANNE_75P; NW_NATURE_TANNE_100P |
| SMALLTREE | 3 | 711 | 59475.300 | 0 | OW_NATURE_SMALLTREE_01; NW_NATURE_SMALLTREE_79P; NW_NATURE_SMALLTREE_103P |
| TREE | 21 | 610 | 2010849.812 | 0 | OW_NATURE_BIG_TREE_02_380P; OW_LOB_TREE_V11; ADDON_PLANTS_TREE_05_239P; ADDON_PLANTS_TREE_01_353P; ADDON_VALLEYPLANT_TREE_01_L_357P |
| BARREL | 6 | 608 | 2295.520 | 4 | OC_BARREL_V1_DESTROYED; NW_HARBOUR_BARREL_01; OC_BARREL_V1; NW_CITY_WEAPON_BARREL_01; OC_BARREL_V2 |
| COUPLESTONES | 2 | 501 | 15981.786 | 0 | MIN_LOB_COUPLESTONES; ADDON_STONES_COUPLESTONES_01_230P |
| SKELETON | 10 | 445 | 713.494 | 0 | ORC_SKELETON_V3; ORC_SKELETON_V4; ORC_SKELETON_V2; ORC_SKELETON_V1; NW_MISC_SKELETON_02 |
| TORCH | 2 | 429 | 154.622 | 0 | OW_MISC_WALL_TORCH_01; DT_TORCH_V1 |
| HANGPLANTS | 4 | 403 | 82353.177 | 0 | NW_NATURE_HANGPLANTS_02; NW_NATURE_HANGPLANTS_01; ADDON_PLANTS_HANGPLANTS_02_494P; ADDON_PLANTS_HANGPLANTS_01_374P |
| SIDEPLANT | 3 | 393 | 5001.682 | 0 | NW_NATURE_SIDEPLANT_01; NW_NATURE_SIDEPLANT_BIG_01; NW_NATURE_SIDEPLANT_CORNER_01 |
| FENCE | 4 | 385 | 1790.251 | 4 | NW_NATURE_FENCE_01; OW_LOB_FENCE_V1; OW_LOB_FENCE_V3; OW_LOB_FENCE_V2 |
| CRATE | 3 | 366 | 2477.232 | 2 | AW_CRATE_01; NW_HARBOUR_CRATEBROKEN_01; NW_HARBOUR_CRATE_01 |
| MUSHROOM | 2 | 355 | 1837.445 | 0 | OW_MUSHROOM_V2; OW_MUSHROOM_V1 |
| HOHETANNE | 6 | 329 | 254539.120 | 0 | NW_NATURE_HOHETANNE_59P; NW_NATURE_HOHETANNE_91P; NW_NATURE_HOHETANNEN_01_531P; NW_NATURE_HOHETANNEN_02_531P; NW_NATURE_HOHETANNE_VERYHIGH_59P |
| WOODPLANKS | 5 | 284 | 4084.833 | 0 | OW_LOB_WOODPLANKS_V2; OW_LOB_WOODPLANKS_V1; OW_LOB_WOODPLANKS_V3; OW_LOB_WOODPLANKS_V4; OW_LOB_WOODPLANKS_V5 |

## Топ-20 семейств по площади

| Семейство | Визуалов | Экземпляров | Площадь, м² | Подвижных визуалов | Примеры |
|---|---:|---:|---:|---:|---|
| TREE | 21 | 610 | 2010849.812 | 0 | OW_NATURE_BIG_TREE_02_380P; OW_LOB_TREE_V11; ADDON_PLANTS_TREE_05_239P; ADDON_PLANTS_TREE_01_353P; ADDON_VALLEYPLANT_TREE_01_L_357P |
| BIGTREE | 1 | 100 | 614841.704 | 0 | NW_NATURE_BIGTREE_356P |
| TANNE | 2 | 741 | 551006.477 | 0 | NW_NATURE_TANNE_75P; NW_NATURE_TANNE_100P |
| BUSH | 21 | 2789 | 451601.024 | 0 | NW_NATURE_BUSH_120P; OW_NATURE_BUSH_02; OW_NATURE_BUSH_01; NW_NATURE_LONG_BUSH_360P; NW_NATURE_BUSH_25P |
| FARNTEPPICH | 2 | 866 | 399144.461 | 0 | NW_NATURE_FARNTEPPICH_306P; NW_NATURE_FARNTEPPICH_612P |
| HOHETANNE | 6 | 329 | 254539.120 | 0 | NW_NATURE_HOHETANNE_59P; NW_NATURE_HOHETANNE_91P; NW_NATURE_HOHETANNEN_01_531P; NW_NATURE_HOHETANNEN_02_531P; NW_NATURE_HOHETANNE_VERYHIGH_59P |
| CAVEWEBS | 3 | 779 | 205960.786 | 0 | NW_CAVEWEBS_V201; OW_CAVEWEBS_V1; OW_CAVEWEBS_V2 |
| WATERGRASS | 2 | 144 | 181557.386 | 0 | NW_NATURE_WATERGRASS_56P; NW_NATURE_WATERGRASS_02_100P |
| GRASSGROUP | 1 | 925 | 175553.993 | 0 | NW_NATURE_GRASSGROUP_01 |
| OLDTREE | 1 | 59 | 152970.184 | 0 | OW_NATURE_OLDTREE_02 |
| HANGPLANTS | 4 | 403 | 82353.177 | 0 | NW_NATURE_HANGPLANTS_02; NW_NATURE_HANGPLANTS_01; ADDON_PLANTS_HANGPLANTS_02_494P; ADDON_PLANTS_HANGPLANTS_01_374P |
| STONE | 27 | 1071 | 70383.444 | 0 | MIN_LOB_STONE_V2_30; NW_NATURE_STONE_BIGHIGH_01; MIN_MOB_STONE_V2_20; MIN_LOB_STONE_V3_30; ADDON_STONES_STONE_01_84P |
| FARN | 3 | 969 | 62315.044 | 0 | NW_NATURE_FARN_102P; NW_NATURE_FARN_SMALL_102P; NW_NATURE_FARN_BIG_01 |
| SMALLTREE | 3 | 711 | 59475.300 | 0 | OW_NATURE_SMALLTREE_01; NW_NATURE_SMALLTREE_79P; NW_NATURE_SMALLTREE_103P |
| PALM | 8 | 155 | 45775.543 | 0 | ADDON_PLANTS_PALM_SHORT_BENT_01_170P; ADDON_PLANTS_PALM_STRAIGHT_01_151P; ADDON_PLANTS_PALM_BEND_01_199P; ADDON_PLANTS_PALM_LONG_STRAIGHT_01_200P; ADDON_PLANTS_PALM_LONG_BENT_02_200P |
| SPIDERWEBS | 1 | 218 | 27039.867 | 0 | NW_MISC_SPIDERWEBS_01 |
| JUNGLETREE | 3 | 118 | 25142.876 | 0 | ADDON_PLANTS_JUNGLETREE_02_163P; ADDON_PLANTS_JUNGLETREE_03_163P; ADDON_PLANTS_JUNGLETREE_04_163P |
| BLAETTERDACH | 1 | 11 | 22313.780 | 0 | ADDON_PLANTS_BLAETTERDACH_02_6P |
| DEADTREE | 2 | 37 | 19368.076 | 0 | OW_LOB_DEADTREE_04; OW_LOB_DEADTREE_07 |
| DUCKWEED | 2 | 52 | 16917.936 | 0 | NW_NATURE_DUCKWEED_72P; NW_NATURE_DUCKWEED_144P |

## Топ-20 семейств object по экземплярам

| Семейство | Визуалов | Экземпляров | Площадь, м² | Подвижных визуалов | Примеры |
|---|---:|---:|---:|---:|---|
| BARREL | 6 | 608 | 2295.520 | 4 | OC_BARREL_V1_DESTROYED; NW_HARBOUR_BARREL_01; OC_BARREL_V1; NW_CITY_WEAPON_BARREL_01; OC_BARREL_V2 |
| SKELETON | 10 | 445 | 713.494 | 0 | ORC_SKELETON_V3; ORC_SKELETON_V4; ORC_SKELETON_V2; ORC_SKELETON_V1; NW_MISC_SKELETON_02 |
| TORCH | 2 | 429 | 154.622 | 0 | OW_MISC_WALL_TORCH_01; DT_TORCH_V1 |
| FENCE | 4 | 385 | 1790.251 | 4 | NW_NATURE_FENCE_01; OW_LOB_FENCE_V1; OW_LOB_FENCE_V3; OW_LOB_FENCE_V2 |
| CRATE | 3 | 366 | 2477.232 | 2 | AW_CRATE_01; NW_HARBOUR_CRATEBROKEN_01; NW_HARBOUR_CRATE_01 |
| WOODPLANKS | 5 | 284 | 4084.833 | 0 | OW_LOB_WOODPLANKS_V2; OW_LOB_WOODPLANKS_V1; OW_LOB_WOODPLANKS_V3; OW_LOB_WOODPLANKS_V4; OW_LOB_WOODPLANKS_V5 |
| CUPBOARD | 4 | 258 | 3578.282 | 2 | NW_CITY_CUPBOARD_POOR_02; NW_CITY_CUPBOARD_RICH_02; NW_CITY_CUPBOARD_POOR_01; NW_CITY_CUPBOARD_RICH_01 |
| OILLAMP | 1 | 242 | 135.539 | 0 | NW_CITY_OILLAMP_01 |
| WINDOW | 4 | 210 | 1327.007 | 0 | NW_CITY_WINDOW_WOOD_IN_01; NW_CITY_WINDOW_WOOD_IN_02; NW_CITY_WINDOW_WOOD_OUT_01; NW_CITY_WINDOW_STONE_OUT_01 |
| FIREPLACE | 12 | 206 | 2627.869 | 0 | NW_MISC_FIREPLACE_01; NW_CITY_FIREPLACE_NORMAL_01; OC_FIREPLACE_V5; ORC_FIREPLACE_V3; NW_CITY_FIREPLACE_LARGE_01 |
| CRATEPILE | 2 | 195 | 7316.030 | 0 | NW_HARBOUR_CRATEPILE_01; AW_CRATEPILE_01 |
| BARRELGROUP | 2 | 183 | 4860.659 | 0 | NW_HARBOUR_BARRELGROUP_02; NW_HARBOUR_BARRELGROUP_01 |
| CANDLE | 1 | 182 | 81.004 | 1 | NW_CITY_CANDLE_01 |
| TABLE | 18 | 181 | 1948.022 | 17 | NW_CITY_TABLE_PEASANT_01; NW_CITY_TABLE_RICH_02; NW_CITY_TABLE_NORMAL_01; OC_TABLE_V3; DT_TABLE_V3 |
| CARPET | 4 | 163 | 1839.131 | 0 | NW_CITY_CARPET_01; NW_CITY_CARPET_02; NW_CITY_CARPET_03; DT_CARPET_ROUND_01 |
| SAECKE | 2 | 124 | 999.733 | 0 | OC_SAECKE_01; OC_SAECKE_02 |
| WOODPILLAR | 7 | 124 | 739.186 | 0 | ADDON_MISC_WOODPILLAR_M02; ADDON_MISC_WOODPILLAR_L01; ADDON_MISC_WOODPILLAR_M03; ADDON_MISC_WOODPILLAR_M01; ADDON_MISC_WOODPILLAR_L04 |
| SKULLSONFLOOR | 1 | 117 | 142.227 | 0 | ORC_SKULLSONFLOOR |
| SHELF | 10 | 115 | 1233.718 | 0 | DT_SHELF_V1; OC_SHELF_V3; ADDON_DUNGEON_SHELF_01; OC_WEAPON_SHELF_EMPTY_V1; ADDON_DUNGEON_SHELF_03 |
| BOARDS | 6 | 110 | 1521.829 | 0 | NW_MISC_BOARDS_01B; NW_MISC_BOARDS_02B; NW_MISC_BOARDS_01C; NW_MISC_BOARDS_01A; NW_MISC_BOARDS_02C |

## Топ-20 семейств object по площади

| Семейство | Визуалов | Экземпляров | Площадь, м² | Подвижных визуалов | Примеры |
|---|---:|---:|---:|---:|---|
| ORCTENT | 2 | 23 | 13397.851 | 0 | OW_ORCTENT_LARGE_01; OW_ORCTENT_SMALL_01 |
| CRATEPILE | 2 | 195 | 7316.030 | 0 | NW_HARBOUR_CRATEPILE_01; AW_CRATEPILE_01 |
| BOOKSHELF | 3 | 104 | 5240.053 | 0 | NW_CITY_BOOKSHELF_01; DT_BOOKSHELF_V2; DT_BOOKSHELF_V1 |
| BARRELGROUP | 2 | 183 | 4860.659 | 0 | NW_HARBOUR_BARRELGROUP_02; NW_HARBOUR_BARRELGROUP_01 |
| BURNEDHUT | 1 | 13 | 4709.313 | 0 | OW_MISC_BURNEDHUT_01 |
| WOODPLANKS | 5 | 284 | 4084.833 | 0 | OW_LOB_WOODPLANKS_V2; OW_LOB_WOODPLANKS_V1; OW_LOB_WOODPLANKS_V3; OW_LOB_WOODPLANKS_V4; OW_LOB_WOODPLANKS_V5 |
| SHIPWRECK | 2 | 4 | 3708.111 | 0 | OW_SHIPWRECK_TAIL; OW_SHIPWRECK_BUG |
| CUPBOARD | 4 | 258 | 3578.282 | 2 | NW_CITY_CUPBOARD_POOR_02; NW_CITY_CUPBOARD_RICH_02; NW_CITY_CUPBOARD_POOR_01; NW_CITY_CUPBOARD_RICH_01 |
| TENT | 2 | 16 | 3021.728 | 0 | NW_MISC_BIG_TENT_430P; OW_MISC_TENT_01 |
| FIREPLACE | 12 | 206 | 2627.869 | 0 | NW_MISC_FIREPLACE_01; NW_CITY_FIREPLACE_NORMAL_01; OC_FIREPLACE_V5; ORC_FIREPLACE_V3; NW_CITY_FIREPLACE_LARGE_01 |
| FISHINGNET | 3 | 106 | 2618.985 | 0 | NW_HARBOUR_FISHINGNETBIG; NW_HARBOUR_FISHINGNETSMALL; NW_HARBOUR_FISHINGNET_01 |
| METALTHORNS | 2 | 63 | 2477.337 | 0 | EVT_TPL_METALTHORNS_2X2X05M; EVT_TPL_METALTHORNS_4X4X4M |
| CRATE | 3 | 366 | 2477.232 | 2 | AW_CRATE_01; NW_HARBOUR_CRATEBROKEN_01; NW_HARBOUR_CRATE_01 |
| BARREL | 6 | 608 | 2295.520 | 4 | OC_BARREL_V1_DESTROYED; NW_HARBOUR_BARREL_01; OC_BARREL_V1; NW_CITY_WEAPON_BARREL_01; OC_BARREL_V2 |
| BEAMS | 8 | 39 | 2095.488 | 0 | NW_CITY_BEAMS_01; NW_CITY_BEAMS_FARM_01; NW_CITY_BEAMS_FARM_02; NW_CITY_BEAMS_PUB_01; NW_CITY_BEAMS_MIXSHOP_01 |
| GATE | 6 | 35 | 2007.210 | 0 | OC_LOB_GATE_SMALL3; OC_LOB_GATE_BIG; OC_LOB_GATE_SMALL; EVT_GATE_LARGE_01; EVT_GATE_SMALL_01 |
| PALISSADE | 1 | 33 | 1967.171 | 0 | OW_PALISSADE |
| TABLE | 18 | 181 | 1948.022 | 17 | NW_CITY_TABLE_PEASANT_01; NW_CITY_TABLE_RICH_02; NW_CITY_TABLE_NORMAL_01; OC_TABLE_V3; DT_TABLE_V3 |
| CARPET | 4 | 163 | 1839.131 | 0 | NW_CITY_CARPET_01; NW_CITY_CARPET_02; NW_CITY_CARPET_03; DT_CARPET_ROUND_01 |
| FENCE | 4 | 385 | 1790.251 | 4 | NW_NATURE_FENCE_01; OW_LOB_FENCE_V1; OW_LOB_FENCE_V3; OW_LOB_FENCE_V2 |

## Покрытие

Число визуалов и семейств до первого достижения ≥50/80/90 % выбранной меры. Для каждой меры своя сортировка по убыванию; семейства ранжируются по сумме всех своих визуалов.

| Область | Мера | 50 %: визуалов | 50 %: семейств | 80 %: визуалов | 80 %: семейств | 90 %: визуалов | 90 %: семейств |
|---|---|---:|---:|---:|---:|---:|---:|
| Все миры, все виды | Экземпляры | 32 | 12 | 117 | 37 | 190 | 63 |
| Все миры, все виды | Площадь | 5 | 3 | 17 | 8 | 35 | 11 |
| NEWWORLD.ZEN, все виды | Экземпляры | 23 | 11 | 72 | 31 | 111 | 47 |
| NEWWORLD.ZEN, все виды | Площадь | 4 | 3 | 11 | 6 | 19 | 8 |
| Все миры, object | Экземпляры | 30 | 13 | 93 | 40 | 143 | 65 |
| Все миры, object | Площадь | 23 | 12 | 78 | 35 | 129 | 56 |
| NEWWORLD.ZEN, object | Экземпляры | 21 | 12 | 58 | 29 | 88 | 44 |
| NEWWORLD.ZEN, object | Площадь | 17 | 9 | 51 | 23 | 83 | 34 |

## Топ-15 районов NEWWORLD.ZEN по экземплярам

| Район | Экземпляров | Площадь, м² | Визуалов | Семейств | Визуалов для 80 % | Подвижных экземпляров | Топ визуалов |
|---|---:|---:|---:|---:|---:|---:|---|
| `NW_FOREST_PATH` | 934 | 230842.437 | 46 | 23 | 14 | 0 | NW_NATURE_FARNTEPPICH_306P:176;NW_NATURE_BUSH_25P:77;NW_NATURE_BUSH_120P:73;NW_NATURE_BAUMSTAMM_01_30P:63;NW_NATURE_FARN_SMALL_102P:58 |
| `NW_CITY_HABOUR` | 582 | 10210.857 | 98 | 63 | 31 | 85 | NW_NATURE_SIDEPLANT_01:98;NW_NATURE_BUSH_60P:36;NW_CITY_OILLAMP_01:29;NW_HARBOUR_CRATEPILE_01:27;NW_CITY_BEAMS_01:25 |
| `NW_TROLLAREA_PATH` | 515 | 177776.494 | 40 | 25 | 15 | 13 | NW_NATURE_BUSH_120P:78;NW_NATURE_GRASSGROUP_01:72;NW_NATURE_TANNE_75P:55;NW_NATURE_FARNTEPPICH_306P:32;NW_NATURE_FARN_102P:30 |
| `<NONE>` | 498 | 163763.417 | 65 | 44 | 25 | 25 | OW_NATURE_SMALLTREE_01:57;NW_CAVEWEBS_V201:35;NW_NATURE_LONG_BUSH_360P:32;NW_NATURE_HOHETANNE_59P:25;NW_HARBOUR_CRATEBROKEN_01:23 |
| `NW_XARDAS_TOWER` | 490 | 73763.008 | 75 | 52 | 30 | 29 | NW_NATURE_BUSH_300P:77;NW_CAVEWEBS_V201:40;NW_NATURE_GRASSGROUP_01:36;NW_NATURE_BUSH_120P:25;OW_MISC_WALL_TORCH_01:21 |
| `NW_CITY_TO` | 364 | 72987.436 | 49 | 32 | 18 | 8 | NW_NATURE_GRASSGROUP_01:57;NW_NATURE_BUSH_120P:41;NW_NATURE_BUSH_56P:32;NW_NATURE_FARN_SMALL_102P:23;NW_NATURE_FARNTEPPICH_306P:17 |
| `NW_FARM1_PATH` | 325 | 74947.083 | 40 | 28 | 13 | 24 | NW_NATURE_GRASSGROUP_01:53;NW_NATURE_BUSH_120P:47;NW_NATURE_BUSH_56P:29;NW_NATURE_FENCE_01:24;NW_NATURE_TANNE_100P:20 |
| `NW_TROLLAREA_RUINS` | 277 | 61940.035 | 55 | 39 | 21 | 7 | NW_CAVEWEBS_V201:51;NW_NATURE_STONE_CULT_STAND_01:33;NW_NATURE_BUSH_120P:27;NW_NATURE_STONE_CULT_TOP_01:18;NW_NATURE_SMALLTREE_79P:13 |
| `NW_FARM4_WOOD` | 244 | 182449.482 | 26 | 22 | 9 | 3 | NW_NATURE_BUSH_120P:48;NW_NATURE_TANNE_75P:34;NW_NATURE_FARNTEPPICH_306P:32;NW_NATURE_FARNTEPPICH_612P:17;NW_NATURE_BIGTREE_356P:16 |
| `NW_CITY_KANAL` | 239 | 5214.700 | 45 | 29 | 18 | 33 | OC_BARREL_V1_DESTROYED:38;OW_LOB_WOODPLANKS_V3:32;OC_SAECKE_01:21;NW_HARBOUR_CRATE_01:21;OC_LOB_GATE_SMALL3:12 |
| `NW_CITY_UPTOWN` | 194 | 5927.754 | 45 | 31 | 22 | 29 | NW_CITY_WINDOW_STONE_OUT_01:23;NW_CITY_WINDOW_WOOD_IN_02:21;NW_NATURE_BUSH_120P:11;NW_CITY_CUPBOARD_RICH_02:10;NW_HARBOUR_BARREL_01:10 |
| `NW_CITY_MERCHANT` | 185 | 5867.381 | 53 | 37 | 25 | 71 | NW_HARBOUR_BARREL_01:28;NW_CITY_WEAPON_BARREL_01:15;NW_HARBOUR_CRATE_01:13;NW_MISC_SIGN_01:11;NW_CITY_OILLAMP_01:9 |
| `NW_FARM1_CITYWALL` | 155 | 94185.941 | 23 | 15 | 10 | 4 | NW_NATURE_TANNE_75P:29;NW_NATURE_FARNTEPPICH_306P:29;NW_NATURE_BUSH_120P:12;NW_NATURE_GRASSGROUP_01:12;NW_NATURE_TANNE_100P:9 |
| `NW_TROLLAREA_RITUALFOREST` | 155 | 75255.907 | 28 | 21 | 12 | 2 | NW_NATURE_TANNE_75P:20;NW_NATURE_GRASSGROUP_01:19;NW_NATURE_HOHETANNE_59P:18;NW_NATURE_FARNTEPPICH_306P:13;NW_NATURE_HANGPLANTS_01:12 |
| `NW_PASS` | 133 | 5150.493 | 11 | 8 | 4 | 0 | OW_NATURE_SMALLTREE_01:42;OW_NATURE_BUSH_01:37;ORC_SKELETON_V4:27;ORC_SKELETON_V3:16;MIN_LOB_COUPLESTONES:5 |

## Топ-15 районов NEWWORLD.ZEN по object_instances

| Район | Экземпляров object | Площадь object, м² | Визуалов object | Визуалов object для 80 % | Подвижных экземпляров object | Топ визуалов object |
|---|---:|---:|---:|---:|---:|---|
| `NW_CITY_HABOUR` | 389 | 4439.753 | 83 | 33 | 85 | NW_CITY_OILLAMP_01:29;NW_HARBOUR_CRATEPILE_01:27;NW_CITY_BEAMS_01:25;NW_CITY_WINDOW_WOOD_OUT_01:23;NW_CITY_CANDLE_01:17 |
| `NW_CITY_KANAL` | 226 | 2568.132 | 41 | 16 | 33 | OC_BARREL_V1_DESTROYED:38;OW_LOB_WOODPLANKS_V3:32;OC_SAECKE_01:21;NW_HARBOUR_CRATE_01:21;OC_LOB_GATE_SMALL3:12 |
| `NW_CITY_MERCHANT` | 170 | 1585.340 | 47 | 21 | 71 | NW_HARBOUR_BARREL_01:28;NW_CITY_WEAPON_BARREL_01:15;NW_HARBOUR_CRATE_01:13;NW_MISC_SIGN_01:11;NW_CITY_OILLAMP_01:9 |
| `NW_CITY_UPTOWN` | 165 | 1474.914 | 37 | 18 | 29 | NW_CITY_WINDOW_STONE_OUT_01:23;NW_CITY_WINDOW_WOOD_IN_02:21;NW_CITY_CUPBOARD_RICH_02:10;NW_HARBOUR_BARREL_01:10;NW_CITY_CARPET_01:9 |
| `NW_XARDAS_TOWER` | 157 | 1481.914 | 44 | 20 | 29 | OW_MISC_WALL_TORCH_01:21;ORC_SKULLSONFLOOR:13;OC_BARREL_V1_DESTROYED:11;NW_HARBOUR_BARRELGROUP_02:10;NW_CITY_CANDLE_01:8 |
| `<NONE>` | 141 | 5797.234 | 33 | 16 | 25 | NW_HARBOUR_CRATEBROKEN_01:23;OW_MISC_BURNED_POLE_01:17;NW_HARBOUR_CRATE_01:9;NW_HARBOUR_BARRELGROUP_02:8;NW_NATURE_FENCE_01:8 |
| `SHIP_IN` | 91 | 931.608 | 23 | 11 | 15 | NW_CITY_OILLAMP_01:18;NW_HARBOUR_CANONBALLS_01:9;NW_HARBOUR_FISHINGNETBIG:8;NW_HARBOUR_CRATEPILE_01:7;NW_CITY_DECO_WHEEL_01:6 |
| `NW_BIGFARM_HOUSE` | 84 | 779.446 | 19 | 10 | 27 | NW_CITY_WINDOW_WOOD_IN_01:17;NW_CITY_CUPBOARD_POOR_02:9;NW_CITY_OILLAMP_01:8;NW_CITY_DECO_SHADOWHEAD_01:7;NW_CITY_CUPBOARD_POOR_01:6 |
| `NW_CITY_WAY` | 77 | 1489.244 | 18 | 9 | 1 | OC_SAECKE_01:14;NW_MISC_BOARDS_01B:10;NW_HARBOUR_IRONROPEHOLDER_01:10;NW_HARBOUR_CRATEBROKEN_01:8;NW_HARBOUR_ROPE_01:6 |
| `SHIP_DECK` | 69 | 1125.653 | 20 | 10 | 6 | NW_HARBOUR_CRATEPILE_01:16;NW_HARBOUR_ROPE_01:9;NW_HARBOUR_BARRELGROUP_02:7;NW_HARBOUR_CANONBALLS_01:6;NW_HARBOUR_CANON_01:5 |
| `NW_TROLLAREA_RUINS` | 67 | 1176.885 | 32 | 19 | 7 | OC_SAECKE_01:5;NW_NATURE_FENCE_01:5;NW_MISC_FIREPLACE_01:4;NW_HARBOUR_CRATEBROKEN_01:4;OC_SACK_V03:4 |
| `NW_FARM1_PATH` | 66 | 1245.444 | 17 | 8 | 24 | NW_NATURE_FENCE_01:24;OC_SAECKE_01:7;NW_HARBOUR_FISHINGNETBIG:6;NW_HARBOUR_CRATEPILE_01:4;NW_HARBOUR_CRATEBROKEN_01:4 |
| `NW_TROLLAREA_PORTALTEMPEL` | 64 | 337.196 | 10 | 2 | 0 | OW_MISC_WALL_TORCH_01:44;OC_LOB_LIGHTER_HUGE3:8;ADDON_DUNGEON_PILLAR_02:3;ADDON_DUNGEON_PILLAR_01:3;EVT_GATE_LARGE_01:1 |
| `NW_CITY_TO` | 60 | 873.829 | 22 | 10 | 8 | NW_MISC_SIGN_01:12;NW_HARBOUR_CRATEBROKEN_01:8;NW_CITY_OILLAMP_01:6;OC_BARREL_V1_DESTROYED:5;NW_HARBOUR_BARRELGROUP_02:4 |
| `NW_FOREST_PATH` | 58 | 209.357 | 8 | 5 | 0 | ORC_SKELETON_V3:19;ORC_SKULLSONFLOOR:11;NW_HARBOUR_CRATEBROKEN_01:7;OW_MISC_BURNED_POLE_01:7;ORC_SKELETON_V4:7 |

## С чего начать

Растительность, деревья и грибы дают 10744 экземпляров (50.677 %) и 5201105.654 м² (92.151 %) во всех мирах. Сюда включены пни и стволы; это `nature` без семейств `BROKENSTONE`, `CAVEWEBS`, `COUPLESTONES`, `CRYSTAL`, `MANYSTONES`, `SPIDERWEBS`, `STALAG`, `STONE`. Полная категория `nature` в сверке включает также камни, кристаллы и паутину. Растительность требует отдельного вида ручной работы; для первого набора утвари и построек ниже используются только `object`.

Первое семейство `object` по экземплярам — **BARREL**: 608 экземпляров, 6 разных визуалов и 2295.520 м². Его визуалы: `OC_BARREL_V1_DESTROYED`, `NW_HARBOUR_BARREL_01`, `OC_BARREL_V1`, `NW_CITY_WEAPON_BARREL_01`, `OC_BARREL_V2`, `OC_BARREL_V2_DESTROYED`.

Для первого цельного городского набора сравниваются четыре района. В 80 %-набор входят самые частые визуалы `object` до первого достижения ≥80 % экземпляров `object` района; доля показана по фактически закрытым экземплярам. Критерий выбора — наибольшее число закрытых экземпляров на один визуал этого набора.

| Район | Экземпляров object | Разных визуалов object | Визуалов для 80 % | Закрыто экземпляров | Доля, % | Из выбранных в movable-vobs.csv |
|---|---:|---:|---:|---:|---:|---:|
| `NW_CITY_HABOUR` | 389 | 83 | 33 | 312 | 80.206 | 8 |
| `NW_CITY_MERCHANT` | 170 | 47 | 21 | 136 | 80.000 | 6 |
| `NW_CITY_KANAL` | 226 | 41 | 16 | 183 | 80.973 | 2 |
| `NW_CITY_UPTOWN` | 165 | 37 | 18 | 134 | 81.212 | 4 |

По этому критерию начинать стоит с **NW_CITY_KANAL**: полный набор содержит 41 разных визуалов `object`, а сокращённый — **16** и закрывает 183 из 226 экземпляров `object` (80.973 %), 11.438 экземпляров на визуал. Из этих 16 визуалов **2** уже перечислены в `movable-vobs.csv`. Это вывод из таблиц частот, без решений о геймплее.
