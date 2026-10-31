# Перепись ткани

Утилита читает штатные VDF через ZenKit. Игра и рендер не запускаются.
Имена визуалов, флаги vob, положения и материалы берутся из оригинальных
`NEWWORLD.ZEN`, `OLDWORLD.ZEN`, `ADDONWORLD.ZEN`, `DRAGONISLAND.ZEN`.
Архивы монтируются с `VfsOverwriteBehavior::OLDER`, как в движке
(`Resources::loadVdfs`): новые ресурсы дополнения заменяют старые.

```sh
cmake -S scripts/cloth -B /tmp/gothic-cloth-census -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/gothic-cloth-census --target cloth-census --parallel 3
/tmp/gothic-cloth-census/cloth-census "$GAME_ROOT" > /tmp/cloth-vobs.csv
/tmp/gothic-cloth-census/cloth-census "$GAME_ROOT" --world-materials > /tmp/cloth-world-materials.csv
/tmp/gothic-cloth-census/cloth-census "$GAME_ROOT" OC_LOB_FLAG_SMALL.3DS ADD_PIRATEFLAG.MMS NW_MORPH_ORC_STANDART_01.MMS > /tmp/cloth-geometry.csv
/tmp/gothic-cloth-census/cloth-census "$GAME_ROOT" --texture OBJ_CITY_SIGNSALL_01.TGA > /tmp/cloth-signs.ppm
```

Без дополнительных аргументов выводится CSV всех vob с непустым визуалом;
полное количество vob каждого мира идёт в stderr. `visual_type` использует
значения ZenKit: 0 — decal, 1 — mesh, 2 — MRM, 3 — PFX, 5 — model,
6 — morph mesh. `anim_mode`: 0 — NONE, 1 — WIND, 2 — WIND_ALT.
Ненулевой `anim_mode` при нулевом `anim_strength` не включает качание меша.

`--world-materials` выводит записи `worldmaterial` с именем мира, индексом,
именем и текстурой материала, `wave_mode`, амплитудой и числом треугольников.
Для материалов с именами ткани, флагов, парусов и вывесок добавляет записи
`worldface` с исходными индексами и координатами треугольников. Это позволяет
различить самостоятельные vob и ткань, вшитую в уровень, и посчитать связные
острова. Дополнительные имена визуалов выводят MRM/MMB-геометрию и индексы,
которые двигает исходная morph-анимация. `--texture` декодирует оригинальную
текстуру в PPM для проверки материала.

Выход с оригинальной геометрией и изображениями хранить локально, в git
его не добавлять. Итоги переписи и координаты креплений — в
`reports/physics/2026-10-18-cloth/validation-notes.md`.

`OpenGothic/data/cloth-vobs.csv` содержит четыре основных поля без заголовка:
визуал без расширения, крепление, жёсткость 0–1, glob материала ткани.
`x:min:1.5` закрепляет вершины в 1,5 см от минимальной локальной X-границы;
`y:max:22` — в 22 см от максимальной Y-границы. Граница считается только
по выбранному материалу. Остальные материалы смешанного меша остаются
жёсткими. В таблице нет правил по догадкам из имён: каждое правило проверено
на исходных вершинах и материалах.

Необязательное пятое поле — капсула собственного крепления:
`fromX:fromY:fromZ;toX:toY:toZ;radius`, локальные сантиметры. Она удерживает
полотно орочьего штандарта снаружи измеренного стержня без запросов коллизии
мира; закреплённые точки остаются неподвижными.

`OpenGothic/data/cloth-world.csv` задаёт материал, геометрию, крепление,
жёсткость, фильтр острова и линейное затухание в 1/с. Для высокого главного
корабельного флага используется 0,5/с, для остальных полотнищ — 1,5/с:
избыточное затухание замедляет падение длинного полотна в висячий покой.
