# Система частиц ZenGin (C_PARTICLEFX / zCParticleFX) и как моддеры делают дым

Примечание о источниках. Скрипты Gothic II цитируются по зеркалу исходников в репозитории `auronen/Gothic-2-localization` (номера строк по файлу), класс `C_PARTICLEFX` — по `dosinabox/g2nr_unofficial_update`. Поведение OpenGothic проверено по форку движка в этом репозитории (`OpenGothic/` рядом с этой папкой; ссылки относительные). С апстримом `Try/OpenGothic` построчно не сверялось. Значения скриптов приведены как есть. Если поле в инстансе не задано, движок берёт 0 или пустую строку. Например, у `FIRE_SMOKE` нет `visalphaend`, значит там 0.

## 1. Что значат поля C_PARTICLEFX и как они складываются

### Takeaway
Время во всех полях — в миллисекундах, расстояния — в сантиметрах, углы — в градусах. Скорость `velAvg` — в см/мс: 0,04 = 40 см/с. Гравитация `flyGravity_S` — в см/мс²: 0,0001 = 1 м/с². Альфа задаётся числом от 0 до 255. Размер, цвет и альфа частицы меняются за её жизнь линейно, от значения «Start» к значению «End». В OpenGothic поля `m_bIsAmbientPFX`, `timeStartEnd_S` и `flockMode` читаются, но нигде не применяются.

### Cited Findings
**Полный список полей класса** в Gothic II (порядок как в скрипте):
`ppsvalue, ppsscalekeys_s, ppsislooping, ppsissmooth, ppsfps, ppscreateem_s, ppscreateemdelay, shptype_s, shpfor_s, shpoffsetvec_s, shpdistribtype_s, shpdistribwalkspeed, shpisvolume, shpdim_s, shpmesh_s, shpmeshrender_b, shpscalekeys_s, shpscaleislooping, shpscaleissmooth, shpscalefps, dirmode_s, dirfor_s, dirmodetargetfor_s, dirmodetargetpos_s, diranglehead, dirangleheadvar, dirangleelev, dirangleelevvar, velavg, velvar, lsppartavg, lsppartvar, flygravity_s, flycolldet_b, visname_s, visorientation_s, vistexisquadpoly, vistexanifps, vistexaniislooping, vistexcolorstart_s, vistexcolorend_s, vissizestart_s, vissizeendscale, visalphafunc_s, visalphastart, visalphaend, trlfadespeed, trltexture_s, trlwidth, mrkfadespeed, mrktexture_s, mrksize, flockmode, flockstrength, useemittersfor, timestartend_s, m_bisambientpfx`. Поля `m_bDontKillPFXWhenDone` в классе G2 нет. — [ParticleFX.d (g2nr_unofficial_update)](https://github.com/dosinabox/g2nr_unofficial_update/blob/HEAD/PrjPARTICLEFX/_Intern/ParticleFX.d)

**Единицы и смысл полей по вики OpenGothic** (пересказ таблицы):
- **Выпуск.** `ppsValue` — частиц в секунду. `ppsScaleKeys_S` — ряд множителей к `ppsValue`, который проходится за время работы эффекта. `ppsIsLooping` — после последнего ключа ряд начинается заново. `ppsIsSmooth` — между ключами плавный переход, а не скачок. `ppsFPS` — сколько ключей проходится за секунду. — [OpenGothic wiki: Particles](https://github.com/Try/OpenGothic/wiki/Particles)
- **Форма излучателя.** `shpType_S`: POINT, LINE, CIRCLE, BOX, SPHERE или MESH. `shpFOR_S`: WORLD или OBJECT. `shpOffsetVec_S` — смещение в сантиметрах. `shpDim_S` — диаметр, длина или три ребра, в сантиметрах. `shpIsVolume` — для BOX и SPHERE частицы рождаются во всём объёме, а не только на поверхности. `shpDistribType_S` — для LINE и CIRCLE: WALK (равномерный обход фигуры) или RAND. — [OpenGothic wiki](https://github.com/Try/OpenGothic/wiki/Particles)
- **Направление и скорость.** `dirMode_S`:
  - DIR — направление задано углами;
  - RAND — случайное;
  - TARGET — на точку.

  `dirAngleHead` — горизонтальный угол, плюс влево. `dirAngleElev` — вертикальный угол, плюс вверх. К обоим `…Var` добавляет разброс ±. `velAvg` и `velVar` — в см/мс. — [OpenGothic wiki](https://github.com/Try/OpenGothic/wiki/Particles)
- **Время жизни.** `lspPartAvg` и `lspPartVar` — в миллисекундах, разброс ±. — [OpenGothic wiki](https://github.com/Try/OpenGothic/wiki/Particles)
- **Полёт.** `flyGravity_S` — тройка в см/мс² по мировым осям X, Y, Z, без выбора системы координат. `flyCollDet_B`: 0 — без столкновений, 1 — замедлить и отразить, 2 — ускорить и отразить, 3 — остановить, 4 — удалить. — [OpenGothic wiki](https://github.com/Try/OpenGothic/wiki/Particles)
- **Вид частицы.**
  - `visName_S` — текстура.
  - `visOrientation_S`: NONE — всегда лицом к камере; VELO и VELO3D — вытянута по скорости.
  - `visTexAniFPS` — кадров в секунду для анимированной текстуры с кадрами `_A0`, `_A1`…
  - `visTexColorStart_S` и `visTexColorEnd_S` — множитель RGB от 0 до 255; 255 255 255 не меняет текстуру. Цвет переходит от начального к конечному линейно.
  - `visSizeStart_S` — размер в сантиметрах. При NONE это экранные X и Y, иначе вторая величина идёт вдоль полёта.
  - `visSizeEndScale` — множитель размера к концу жизни, растёт линейно.
  - `visAlphaFunc_S`: NONE, BLEND, ADD или MUL.
  - `visAlphaStart` и `visAlphaEnd` — от 0 до 255.

  — [OpenGothic wiki](https://github.com/Try/OpenGothic/wiki/Particles)
- **След и метки.** След включается при `trlFadeSpeed > 0`; ширина `trlWidth` — в сантиметрах. Метки (`mrk*`) остаются на геометрии при столкновении, если `flyCollDet_B ≠ 0`. — [OpenGothic wiki](https://github.com/Try/OpenGothic/wiki/Particles)
- **Прочее.** `useEmittersFOR` — частицы движутся вместе с излучателем. `timeStartEnd_S` — часы, когда эффект виден, например только ночью. `m_bIsAmbientPFX` — эмбиентный эффект, игрок может выключить его в настройках (gothic.ini). — [OpenGothic wiki](https://github.com/Try/OpenGothic/wiki/Particles); [Gothic Modding Community: first-effect.md](https://github.com/Sefaris/gothic-modding-wiki/blob/HEAD/docs/modding-basics-daedalus/first-effect.md)
- **Сборка эффектов.** Эффекты собираются не через `Gothic.src`, а через отдельный `ParticleFX.src` (`_intern\ParticleFx.d`, `Pfx\PfxInstEngine.d`, `Pfx\PfxInst.d`, `Pfx\PfxInstMagic.d`). Свой файл моддер дописывает в конец списка. — [first-effect.md](https://github.com/Sefaris/gothic-modding-wiki/blob/HEAD/docs/modding-basics-daedalus/first-effect.md); [GothicFreeAim wiki](https://github.com/szapp/GothicFreeAim/wiki/Installation-Steps-for-Gothic-2/bc8e175ae03566cf1cae26e0dd5b392afc4cbd1a)

**Как OpenGothic считает частицу** (код форка):
- **Движение.** Каждый кадр `pos += dir*dt; dir += flyGravity*dt`, где `dt` — в миллисекундах. Это подтверждает единицы см/мс и см/мс². — [pfxbucket.cpp, tick, стр. ~456–461](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp)
- **Время жизни** хранится в `uint16_t`: больше 65 535 мс (~65 с) записать нельзя, значение переполнится. — [pfxbucket.cpp, init, стр. ~275](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp)
- **Разброс** `randf(base,var)` равномерный в пределах `base ± var`. — [pfxbucket.cpp, стр. 263–264](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp)
- **Направление DIR.** Курс считается как `head = 90 + dirAngleHead ± Var`, подъём — как `elev = dirAngleElev ± Var`. Вектор: `(cos e·cos h, sin e, cos e·sin h)`. При `dirFOR_S = OBJECT` он поворачивается осями излучателя, при WORLD (по умолчанию) — нет. Хак: если `dirAngleHeadVar` или `dirAngleElevVar` ≥ 180, разброс обнуляется (так сделано ради STARGATE_PARTICLES). — [pfxbucket.cpp, стр. 374–404](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp); значение по умолчанию — [particlefx.cpp, стр. 28](../../OpenGothic/common/graphics/pfx/particlefx.cpp)
- **Скорость.** Вектор направления нормируется и умножается на `velAvg ± velVar`. — [pfxbucket.cpp, стр. ~432–436](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp)
- **Ключи выпуска.** Индекс ключа считается как `fps*time/1000`. При `smooth` значение интерполируется линейно, иначе берётся ближайший ключ. При `loop` индекс идёт по модулю. Для `ppsFPS ≤ 0` подставляется 1. — [particlefx.cpp, fetchScaleKey, стр. 292–315; стр. 45](../../OpenGothic/common/graphics/pfx/particlefx.cpp)
- **Цвет, размер и альфа за жизнь.** Цвет `= start·(1−a) + end·a`. Масштаб `= 1·(1−a) + a·visSizeEndScale`. Альфа берёт `visAlphaStart/255` и `visAlphaEnd/255` с плавным появлением и исчезанием на первых и последних 10 % жизни. Автор оставил комментарий: в оригинале частицы и с резким переходом будто бы немного проявляются и гаснут. — [pfxbucket.cpp, buildSsbo, стр. 609–655](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp); [particlefx.cpp, стр. 90–91](../../OpenGothic/common/graphics/pfx/particlefx.cpp)
- **Режимы смешивания.** `NONE` → AlphaTest, `BLEND` → Transparent, `ADD` → AdditiveLight, `MUL` → Multiply. — [parser.cpp, loadAlpha, стр. 44–53](../../OpenGothic/common/utils/parser.cpp)
  - Transparent: `SrcAlpha / OneMinusSrcAlpha`, без записи глубины, отдельный шейдер `_f`.
  - AdditiveLight: `SrcAlpha / One`; цвет заранее умножается на альфу.
  - Multiply: `DstColor / SrcColor`, то есть результат = 2·src·dst. ADD и MUL рисуются шейдером `_e` (mainEmissive) и освещения не получают.

  — [shaders.cpp, стр. 519–534 и 590–613](../../OpenGothic/common/graphics/shaders.cpp); [pfxbucket.cpp, стр. 648–660](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp)
- **Прочитанные, но неиспользуемые поля.** `m_bIsAmbientPFX` и `timeStartEnd_S` загружаются ([particlefx.cpp, стр. 106–107](../../OpenGothic/common/graphics/pfx/particlefx.cpp)), но поиск по `common/` других обращений к ним не нашёл. `flockMode` и `flockStrength` регистрируются в разборе скриптов ([definitiontypes.cpp, стр. 129–131](../../OpenGothic/common/game/definitions/definitiontypes.cpp)), но в `ParticleFx` не переносятся. Ранее в проекте записано: снос ветром (`flock_mode=WIND`) движок читает, но не применяет. — [harbour-motion README](../../reports/content/remaster/harbour-motion/README.md)
- **Распределение по форме** (`shpDistribType` и `shpDistribWalkSpeed`) в OpenGothic помечено TODO, то есть WALK не реализован. — [pfxbucket.cpp, стр. ~279](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp)

### Inferences
- **Число живых частиц** у одного излучателя ≈ `ppsValue × множитель ключа × lspPartAvg/1000`. У `LIGHTSMOKE` это 5 × 8 = 40, у `FIRE_SMOKE` — 10 × 5,5 = 55 (расчёт).
- **Высота столба без гравитации** ≈ `velAvg × lspPartAvg`. У `FIRE_SMOKE` и `LIGHTSMOKE` скорость 0,04 см/мс, то есть 40 см/с. За 5,5 с столб поднимается на 2,2 м, за 8 с — на 3,2 м (расчёт).
- **Гравитация в рецептах сильнее, чем кажется.** `flyGravity_S = "0 0.0001 0"` — это 1 м/с² вверх. В рецепте вики подъём за 2,5 с от «гравитации» ≈ ½·1·2,5² ≈ 3,1 м, а от начальной скорости 0,02 см/мс — всего 0,5 м. Подписанное там «чуть вверх» на деле главный двигатель столба (расчёт).
- **Вертикальный DIR даёт веер, а не конус.** При `dirAngleElev = 90` и `dirAngleElevVar = 20` угол подъёма лежит в 70°–110°, и при переходе за 90° горизонтальная составляющая меняет знак. Вместе с `dirAngleHeadVar = 50` частицы разлетаются в двух встречных секторах по 100°, а не в круговом конусе. Шлейф от такого сплющен по одной горизонтальной оси, повёрнутой осями излучателя. Это вывод из формулы OpenGothic; повторяет ли его ZenGin, не проверено.
- **Рецепт вики в OpenGothic теряет разброс по курсу.** Там `dirAngleHeadVar = 180`, а хак OpenGothic обнуляет такой разброс. Для кругового конуса в OpenGothic надо ставить 179 (вывод из кода).
- **Кадры анимации.** Скорее всего, `visTexAniFPS` действует только на текстуры с суффиксом `_A0`, например `SMK_16BIT_A0.TGA`. У `FIRESMOKE.TGA` суффикса нет, и в `FIRE_SMOKE` строка `vistexanifps = 5` вероятно ничего не делает. Это догадка по соглашению об именах, не проверена.

### Gaps
- Как исходный ZenGin (zCParticleFX, DX7) считает альфу (линейно ли, есть ли та же подложка 10 %), освещает ли частицы BLEND и как ведёт себя WALK, первичными источниками не подтверждено. Исходников ZenGin нет, нашлись только слова автора OpenGothic (раздел 5).
- Документацию по `flockMode = "WIND"` (как ZenGin смешивает силу и ветер) не нашёл.

## 2. Как оригинальные скрипты Gothic II делают дым и есть ли в игре трубы

### Takeaway
В Gothic II все «бытовые» дымы — копии одного шаблона: точка или круг, подъём 90° ±20°, 40 см/с, жизнь 3–8 с, серый 140–150 → белый, `BLEND`. Различаются они размером, ростом частицы и текстурой. Дыма из труб в стандартных PFX нет. В порту, по переписи проекта, на кровлях нет ни одной трубы: огонь очагов дымит внутри домов (`FIRE_SMOKE`).

### Cited Findings
Значения ниже — из [PfxInst.d Gothic II](https://github.com/auronen/Gothic-2-localization/blob/HEAD/Scripts/System/PFX/PfxInst.d); поля с нулём и пустые не перечислены.

- **`FIRE_SMOKE`** (стр. 3859) — дым очага.
  - Выпуск: `ppsvalue = 10`, `ppsscalekeys_s = "1.0"`.
  - Форма: `shptype_s = "CIRCLE"`, `shpfor_s = "object"`, `shpoffsetvec_s = "0 1 0"`, `shpdistribtype_s = "RAND"`, `shpisvolume = 1`, `shpdim_s = "25"`.
  - Направление: `dirmode_s = "DIR"`, `dirangleheadvar = 50`, `dirangleelev = 90`, `dirangleelevvar = 20`, `velavg = 0.04`.
  - Жизнь: `lsppartavg = 5500`, `lsppartvar = 400`.
  - Вид: `visname_s = "FIRESMOKE.TGA"`, `vistexisquadpoly = 1`, `vistexanifps = 5`, `vistexcolorstart_s = "140 140 140"`, `vistexcolorend_s = "255 255 255"`, `vissizestart_s = "20 20"`, `vissizeendscale = 2`, `visalphafunc_s = "BLEND"`, `visalphastart = 150`. `visalphaend` не задан, то есть 0.
  - `flygravity_s = "0 0 0"`, `visorientation_s = "NONE"`.
  - Перед инстансом стоят немецкие комментарии разработчиков: `FIRE_SMOKE` пока совпадает с `LIGHTSMOKE`, а для дыма огня предлагается `SMK_FIRE.TGA`.
- **`TORCH_SMOKE`** (стр. 3933) — дым факела.
  - Выпуск: `ppsvalue = 8`, ключи `"0.2 1 0.4 2 0.6 2.4 1.2 3 0.5"` при `ppsfps = 4`, плавно, по кругу. Выпуск пульсирует от 1,6 до 24 частиц в секунду.
  - Форма: `POINT`, `shpdim_s = "10"`.
  - Направление: подъём 90 ±20, курс ±50, `velavg = 0.04`.
  - Жизнь: 3000 ±400.
  - Вид: `SMK_FIRE.TGA`, цвет 150 → 255, размер `"5 5"` ×5, `BLEND`, `visalphastart = 80`.
- **`LIGHTSMOKE`** (стр. 4450) — «лёгкий дым».
  - Выпуск: `ppsvalue = 5`.
  - Форма: `POINT`, `shpdim_s = "10"`, `shpoffsetvec_s = "0 1 0"`.
  - Направление: `dirfor_s = "object"`, подъём 90 ±20, курс ±50, `velavg = 0.04`.
  - Жизнь: 8000 ±400.
  - Вид: **`visname_s = "GROUNDFOG.TGA"`**, анимации нет, цвет 150 → 255, размер `"10 10"`, **`vissizeendscale = 30`**, `BLEND`, `visalphastart = 255`.
  - **`flockmode = "WIND"`, `flockstrength = 0.05`.**
- **`WATERVAPOUR`** (стр. 4411) — пар.
  - Выпуск: `ppsvalue = 20`.
  - Форма: `CIRCLE`, `shpdim_s = "10"`.
  - Скорость: `velavg = 0.04`, `velvar = 0.01`.
  - Жизнь: 2000 ±400.
  - Вид: **`SMK_16BIT_A0.TGA`** — анимированный, 5 кадров/с, по кругу. Цвет 150 → 255, размер `"10 10"` ×2, `BLEND`, `visalphastart = 255`, `visorientation_s = "NONE"`.
- **`MAGICPOTIONSMOKE`** (стр. 4491).
  - Выпуск: 5 частиц/с. Форма: `CIRCLE` 15.
  - Жизнь: 5500. Вид: `SMK_16BIT_A0.TGA`, размер `"2 2"` ×30, `BLEND`, α 150.
  - Комментарий: тоже пока копия `LIGHTSMOKE`.
- **`PSILAB_SMOKE`** (стр. 2966).
  - Жизнь: 4000. Вид: `LABSMOKE.TGA`, размер `"10 10"` ×10, **`ADD`**, α 70.
  - Единственный «бытовой» дым с аддитивным смешиванием: светящийся дым лаборатории.
- **`GreenSmoke`** (стр. 3973).
  - Выпуск: 1 частица/с с ключами `"0.1 0.2 0.1 0 0.3 0.2"`. Форма: `CIRCLE` 200.
  - Скорость: `velavg = 0.02`. Жизнь: 8000.
  - Вид: `GREENSMOKE.TGA`, размер 2 ×40, `BLEND`, α 150.
- **`AMBIENTFOG`** (стр. 4011).
  - Выпуск: 20 частиц/с × ключ 5 = 100 частиц/с.
  - Форма: `SPHERE` 2000 см, в объёме.
  - Скорость: 0,02. Жизнь: 8000.
  - Вид: `SMK_16BIT_A0.TGA`, размер `"400 400"` ×3, `BLEND`, α 200.
- **`SMOKE1`** ([PfxInstMagic.d, стр. 10293](https://github.com/auronen/Gothic-2-localization/blob/HEAD/Scripts/System/PFX/PfxInstMagic.d#L10293)) — магический «пуф», а не бытовой дым.
  - Форма: `BOX` 50×50×50, `shpfor_s = "world"`.
  - Ключи выпуска и формы пульсируют.
  - Жизнь: 800 ±200. Гравитация `"0 0.001 0"`, то есть 10 м/с² вверх.
  - Вид: `PUFF5.TGA`, размер `"10 20"` ×10, **`MUL`**.
- **Текстуры бытового дыма**, по поиску `visname_s` по файлу: `FIRESMOKE.TGA` (огонь, лава и все `GROUNDFOG*`), `SMK_FIRE.TGA`, `SMK_16BIT_A0.TGA`, `GROUNDFOG.TGA`, `LABSMOKE.TGA`, `GREENSMOKE.TGA`, `PUFF5.TGA`. — [PfxInst.d](https://github.com/auronen/Gothic-2-localization/blob/HEAD/Scripts/System/PFX/PfxInst.d)
- **`flockmode = "WIND"`** есть у 14 инстансов: `LIGHTSMOKE` (0,05), `LEAVES` (0,5), `BUTTERFLY`, `BUTTERFLY_2`, `FOCUS_HIGHLIGHT2`, а также у всех `GROUNDFOG*_OUTDOOR_*` и `GROUNDFOGNIGHT_OUTDOOR_*` (0,01–0,025). — [PfxInst.d](https://github.com/auronen/Gothic-2-localization/blob/HEAD/Scripts/System/PFX/PfxInst.d)
- **`m_bIsAmbientPFX = 1`** у 26 инстансов. Среди них `GROUNDFOG*`, `LAVAFOG_*`, `LEAVES`, `BUTTERFLY*`, `WOOD_LIGHT*`, `PFX_WATERLIGHT`, `DRAGONLOCATION_*`. У дымов огня, факелов и `LIGHTSMOKE` этого флага нет. **`timeStartEnd_S`** задан у `GROUNDFOGNIGHT_OUTDOOR_*` (`"23 9"` — туман только ночью), `ICE_BLINK_*` (`"11 16"`), `PFX_WATERSPLASH_SEA` (`"8 18"`). — [PfxInst.d](https://github.com/auronen/Gothic-2-localization/blob/HEAD/Scripts/System/PFX/PfxInst.d)
- **Gothic I** тоже имеет `LightSmoke` и `SMOKE1` (в `PFX.d` и `PfxMagic.d`), то есть шаблон перешёл из первой части. — [gothic-1-community-patch PFX.d](https://github.com/AmProsius/gothic-1-community-patch/blob/HEAD/scriptbase/_work/Data/Scripts/System/PFX/PFX.d)
- **Трубы в порту Хориниса.** По переписи проекта (порт, NEWWORLD) труб на кровлях нет: над всеми 17 очагами и печами квартала сразу кровля в 1,6–4,3 м. 13 горящих очагов дымят внутри домов (`FIRE_SMOKE`), девять жаровен `OC_LOB_LIGHTER_HUGE3` горят круглые сутки. — [harbour-motion intent.md](../../reports/content/remaster/harbour-motion/intent.md), [README](../../reports/content/remaster/harbour-motion/README.md)

### Inferences
- **Разработчики не делали отдельного «дыма на расстоянии».** Все бытовые дымы — копия-паста `LIGHTSMOKE` (так сказано прямо в комментариях скрипта), рассчитанная на взгляд с 2–5 м. Отсюда вывод проекта: с 11–15 м `LIGHTSMOKE` почти не виден, а `FIRE_SMOKE` не виден вовсе.
- **Дым вдали держат две вещи:** лёгкое ветровое смещение (`flock WIND` 0,01–0,05) и анимированная текстура `SMK_16BIT_A0`.

### Gaps
- Есть ли дымящиеся трубы где-то ещё в Gothic II / Ночи Ворона (верхний квартал, ферма Онара, Яркендар), не проверено. Перепись проекта охватывает только порт. Надёжного источника «в G2 нет труб с дымом» не нашёл.
- Прямых описаний текстур (размер, кадры `SMK_16BIT_A0…An`, альфа-канал) в открытых источниках не нашёл. Их надо смотреть в `Textures.vdf` игры.

## 3. Знания сообщества моддеров Gothic о дыме

### Takeaway
В открытом доступе есть по сути один подробный рецепт дыма — учебник Gothic Modding Community: `BLEND`, большой `visSizeEndScale`, `visAlphaEnd = 0`, медленная скорость и лёгкая подъёмная «гравитация». Обсуждений труб и дыма из Returning, L'Hiver, Odyssey и Chronicles of Myrtana найти не удалось.

### Cited Findings
- **Рецепт «Simple Smoke»** из учебника GMC.
  - Выпуск: `ppsValue = 30`, `ppsScaleKeys_S = "1"`, `ppsIsLooping = 1`.
  - Форма: `shpType_S = "POINT"`, `shpFOR_S = "OBJECT"`.
  - Направление: `dirMode_S = "DIR"`, `dirFOR_S = "OBJECT"`, `dirAngleElev = 90`, `dirAngleElevVar = 15`, `dirAngleHeadVar = 180`, `velAvg = 0.02`, `velVar = 0.01`.
  - Жизнь: `lspPartAvg = 2500`, `lspPartVar = 500`. Подъём: `flyGravity_S = "0 0.0001 0"`.
  - Вид: `visName_S = "SMOKE1.TGA"`, `visOrientation_S = "NONE"`, цвет `"150 150 150"` → `"80 80 80"` (темнеет к концу), размер `"10 10"`, `visSizeEndScale = 5`, `visAlphaFunc_S = "BLEND"`, `visAlphaStart = 180`, `visAlphaEnd = 0`.

  — [first-effect.md (Gothic Modding Community)](https://github.com/Sefaris/gothic-modding-wiki/blob/HEAD/docs/modding-basics-daedalus/first-effect.md)
- **Сводка того же учебника.**
  - Дым: BLEND, большой `visSizeEndScale`, `visAlphaEnd = 0`.
  - Огонь: ADD, анимированная текстура, короткая жизнь.
  - Режимы: BLEND — для дыма, тумана, пыли; ADD — для огня, искр, магии; MUL — для теней и затемнения.

  — [first-effect.md](https://github.com/Sefaris/gothic-modding-wiki/blob/HEAD/docs/modding-basics-daedalus/first-effect.md)
- **Грабли из учебника.**
  - Больше `ppsValue` — больше нагрузки.
  - `flyCollDet_B` при множестве частиц сильно грузит CPU; самое дорогое сочетание — с `useEmittersFOR = 1`.
  - `ppsCreateEm_S` создаёт новый излучатель на каждую частицу.
  - Если эффекта не видно, проверь `visAlphaStart > 0`, не слишком ли мал `visSizeStart_S` и есть ли текстура.
  - Эффекты с `m_bIsAmbientPFX = 1` игрок может выключить.

  — [first-effect.md](https://github.com/Sefaris/gothic-modding-wiki/blob/HEAD/docs/modding-basics-daedalus/first-effect.md)
- **Новые PFX моддеры добавляют отдельным файлом** в `PFX.src` или `ParticleFX.src` (пример — `PfxInstGFA_G2.d` в GothicFreeAim). — [GothicFreeAim wiki](https://github.com/szapp/GothicFreeAim/wiki/Installation-Steps-for-Gothic-2/bc8e175ae03566cf1cae26e0dd5b392afc4cbd1a)
- **Неофициальное обновление G2NR и польские скрипты** правят `PfxInst.d` и `PfxInstMagic.d`: в них лежат те же `DRAGON_BREATH_SMOKE`, `TORCH_SMOKE`, `SMOKE1`. Отдельного «дыма из труб» поиск по коду GitHub не нашёл. — поиск `gh search code "C_PARTICLEFX" "SMOKE"`: [g2nr_unofficial_update](https://github.com/dosinabox/g2nr_unofficial_update), [G2MDK-PolishScripts](https://github.com/Fabiosek/G2MDK-PolishScripts)
- **Gothic II Remaster на Nexus** заявляет переработанные и дополненные скрипты частиц, звуков и визуальных эффектов. Подробностей о дыме на странице не видно (по сниппету поиска). — [Nexus Gothic 2 mod 46](https://www.nexusmods.com/gothic2/mods/46)

### Inferences
- Рецепт GMC с темнеющим к концу цветом (150 → 80) ближе к тому, что проект вывел замерами, — «цвет темнее неба». Оригинальные дымы, наоборот, светлеют от 140–150 к 255. На светлом небе такой дым растворяется.
- Значения GMC (30 частиц/с, 2,5 с, 10 → 50 см) — это дым у костра, видимый с близи. Чтобы дым трубы читался с 15–50 м, их надо масштабировать: дольше жизнь, крупнее частица, темнее цвет.

### Gaps
- На форумах World of Gothic, в Returning, L'Hiver, Odyssey и Chronicles of Myrtana (Archolos) поиск не нашёл ни тредов, ни конфигов про дым из труб. Есть ли там трубы с дымом и какие у них параметры — неизвестно. Можно распаковать `.mod` этих модов и сделать grep по `PfxInst*.d` или скомпилированному `ParticleFX.dat`.
- Сайт gothic-modding-community.github.io напрямую не открывал. Цитаты взяты из исходника вики на GitHub (Sefaris/gothic-modding-wiki), которая, судя по всему, и есть эта вики.

## 4. Как ремастеры и моды игр той же эпохи добавляли дым из труб

### Takeaway
Типовой путь в модах Morrowind и Skyrim такой: поставить на крыши меши труб и прицепить к ним готовый излучатель дыма. Где трубы нет, дым пускают из проёма в кровле (Вайтран в Skyrim). Отдельные моды делают ванильный дым тише и прозрачнее или заставляют его реагировать на ветер и погоду. OpenMW даёт «мягкие частицы» по глубине. Готовых решений именно для ночи не нашёл.

### Cited Findings
- **OpenMW, мягкие частицы.** Включаются через `soft particles = true` в `[Shaders]` и автоматически применяются ко всем системам частиц. Документация приводит примеры: пар над горячим источником и низкий туман в подземельях. `size` (по умолчанию 45) задаёт мягкость перехода, больше значение — мягче край. `falloff` гасит геометрию, когда камера подходит близко; `falloffDepth` по умолчанию 300 единиц. — [OpenMW docs: Custom Shader Effects](https://openmw.readthedocs.io/en/latest/reference/modding/custom-shader-effects.html)
- **OpenMW, баг с цветом частиц.** Из-за ошибки в `NiParticleColorModifier` дымоподобные частицы у `furn_6th_ashstatue` были заметно плотнее, чем в оригинальном Morrowind, где эффект тоньше. Это пример того, как порт движка меняет вид дыма. — [OpenMW issue #3676](https://gitlab.com/OpenMW/openmw/-/issues/3676)
- **Morrowind: Subtle Smoke (wazabear)** делает многие дымовые эффекты «спокойнее и мягче для глаз». Технических деталей на странице нет. — [modding-openmw.com: Subtle Smoke](https://modding-openmw.com/mods/subtle-smoke)
- **Morrowind: OpenMW Dynamic Candle Smoke** вешает дым на свечи, порождая VFX-объекты скриптом, без ESP. — [modding-openmw.com](https://modding-openmw.com/mods/openmw-dynamic-candle-smoke)
- **Morrowind: дымовые трубы в Кальдере** (Nexus 45766) добавляют меши труб. Даёт ли мод дым, на странице не сказано (по сниппету). — [Nexus Morrowind 45766](https://www.nexusmods.com/morrowind/mods/45766)
- **Skyrim SE, мод 78495.** Ванильный дым реагирует на направление и силу ветра, а значит и на погоду. Яркость дыма подстраивается через ENB particle intensity (по сниппету поиска; Nexus отдал 403). — [Nexus Skyrim SE 78495](https://nexusmods.com/skyrimspecialedition/mods/78495)
- **Skyrim SE, мод 84271.** Добавляет трубы с дымом на ванильные фермы и трубу домам Солитьюда. У домов Вайтрана с очагом дым идёт из проёма в крыше (по сниппету). — [Nexus Skyrim SE 84271](https://nexusmods.com/skyrimspecialedition/mods/84271)
- **Skyrim SE, Farmhouse Chimney (8766).** Добавлен вариант меша трубы без дыма, чтобы дым можно было выбрать отдельно (по сниппету). — [Nexus Skyrim SE 8766](https://nexusmods.com/skyrimspecialedition/mods/8766)
- **Skyrim, мод 58424.** Обратное направление: делает дым большинства источников огня прозрачным (по сниппету). — [Nexus Skyrim 58424](https://www.nexusmods.com/skyrim/mods/58424)
- **Skyrim, мод 113228.** Ретекстур дыма, углей и пепла в высоком разрешении с вариантами для слабых ПК (по сниппету). — [Nexus Skyrim 113228](https://www.nexusmods.com/skyrim/mods/113228)
- **GD3D11** (DX11-рендерер для Gothic): поиск не нашёл ни мягких частиц, ни правок дыма. В найденных заметках о частицах упомянут только перевод частиц дождя на compute-шейдеры. — [GD3D11](https://github.com/degenerated1123/GD3D11)

### Inferences
- **Решение Вайтрана совпадает с решением проекта.** Дым из проёма кровли вместо новой трубы — это и есть выбор проекта для хижин порта без труб.
- **Моды на ванильный дым** чаще делают его тише (Subtle Smoke, 58424), чем громче. Повод не перегружать: заметность лучше добирать формой и цветом (тёмный столб, ветер), а не непрозрачностью и числом частиц.
- **Мягкие частицы по глубине** (OpenMW `soft_effect`) — стандартный приём против резких краёв дыма о кровлю и стены. В OpenGothic его нет (раздел 5), и он мог бы скрыть линию, где шлейф режется коньком.

### Gaps
- Не нашёл, как моды Oblivion добавляли дым из труб.
- Технических деталей модов Skyrim и Morrowind (NIF-излучатели, флаги освещения частиц, ночь) получить не удалось: Nexus отдаёт 403. Сведения выше — из сниппетов поиска.
- Не нашёл мода, который отдельно меняет дым ночью (подсветка от очага, светящийся низ столба).

## 5. Особенности OpenGothic: мягкие частицы, освещение, смешивание, ветер, отличия от оригинала

### Takeaway
В OpenGothic частицы `BLEND` освещаются: солнцем через фазовую функцию Хеньи — Гринстейна, тенями от солнца и окружающим светом. Поэтому днём против солнца плотный дым становится серо-синим, а ночью — тёмно-серым. `ADD` и `MUL` не освещаются. Мягких частиц нет. Ветер частиц, режим по часам и флаг эмбиентности не работают. `MUL` (`SMOKE1`) рисуется чёрными прямоугольниками. Магический туман на меш-излучателе не реализован.

### Cited Findings
- **Освещение частиц BLEND.** Они рисуются в прямом проходе (`mainForward`): `color = albedo × diffuseLight(alpha) × exposure`. Для частиц (`T_PFX`) свет солнца берётся по фазовой функции `henyeyGreenstein(-dot(view, sunDir), a*0.63)`, где `a` — альфа частицы. Сверху умножаются тень солнца `calcShadow`, `scene.ambient` и `NightAmbient`. — [main.frag, стр. 105–130 и 443–458](../../OpenGothic/shader/materials/main.frag)
- **Мягких частиц нет.** В `mainForward` нет выборки глубины сцены. Буфер глубины читается только в `mainGhost`. — [main.frag, стр. 443–510](../../OpenGothic/shader/materials/main.frag)
- **Слова автора OpenGothic (Try) в issue #906.** Игра настраивает частицы как alpha-blended, поэтому в OpenGothic они получают тень от горы и зависят от направления света. Ночью они ближе к оригиналу на DX7, но совпадения 1:1 не будет. Короткая цитата: «they do receive shadowing, from the mountain and affected by light direction». — [OpenGothic issue #906](https://github.com/Try/OpenGothic/issues/906)
- **Issue #871.** Магический туман вокруг предметов — система частиц с меш-излучателем. Её не реализовали из-за сомнений в производительности: частицы считаются на CPU, а при большой дальности обзора их много. Там же: у огней мира нет единиц яркости, и рендер обходит экспозицию хаками. — [OpenGothic issue #871](https://github.com/Try/OpenGothic/issues/871)
- **Готовые PFX в порту, замеры проекта.**
  - `LIGHTSMOKE` с 11–15 м почти не виден: серая дымка за коньком. `FIRE_SMOKE` с 15 м не виден вовсе. Ночью дым тёмно-серый и не светится.
  - `SMOKE1` (`MUL`) движок рисует непрозрачными чёрными прямоугольниками.
  - Днём против солнца плотный дым сходится к серо-синему, RGB ≈ 50–60, 55–75, 55–95, а это почти яркость неба. Рост числа излучателей с 13 до 40 поднял контраст к небу только с +5 до +7,5 уровней.
  - Выводы для своего PFX: серый 60–90 вместо 150 → 255; рост частицы в 4–6 раз вместо 30; жизнь больше 8 с.
  - Поворот излучателя `LIGHTSMOKE` (у него `dirFOR = object`) наклоняет шлейф по ветру мира.

  — [harbour-motion README, раздел «Дым»](../../reports/content/remaster/harbour-motion/README.md)
- **Свои PFX без пересборки программ.** В проекте уже есть механизм `particles.d/*.json`: свой эффект наследует `base` и переопределяет исходные поля `C_PARTICLEFX` через `set`. Бенч проверен на `OWN_VOB_LAYER_SMOKE` от `FIRE_SMOKE`. — [docs/world-detail-plan.md, Д2б](../../docs/world-detail-plan.md)
- **Режимы смешивания и размер в OpenGothic** — раздел 1: `BLEND` = SrcAlpha/1−SrcAlpha; `ADD` = SrcAlpha/One, неосвещённый; `MUL` = DstColor/SrcColor, неосвещённый. — [shaders.cpp](../../OpenGothic/common/graphics/shaders.cpp)

### Inferences
- **Почему ночной дым в OpenGothic тёмный, а в оригинале, вероятно, нет.** Если, как намекает Try, DX7-оригинал не освещал BLEND-частицы солнцем, то оригинальный дым ночью оставался «текстура × visTexColor», то есть светло-серым. В OpenGothic он тонет в ночном окружающем свете. Это догадка: прямого подтверждения поведения ZenGin нет.
- **Как читаемость дыма ночью получают в OpenGothic** (варианты проекта, не проверены):
  - заметный дым ночью — подсветка снизу: отдельный слабый `ADD`-слой у проёма, «отсвет очага»;
  - либо принять, что ночью дым почти не виден, как в жизни.
- **Без мягких частиц** край шлейфа режется о кровлю и конёк. Рождать частицы стоит чуть выше проёма или делать их мелкими у основания (`visSizeStart` мал, рост ×4–6), чтобы пересечение с кровлей было незаметно.
- **Снос ветром** (`flockMode WIND`) не работает, поэтому дым по ветру делается наклоном излучателя (`dirFOR_S = OBJECT` + поворот vob) или небольшой горизонтальной составляющей `flyGravity_S`. Гравитация задаётся в мировых осях и одинаково сносит все излучатели (вывод из кода и вики).

### Gaps
- Отличается ли код частиц и шейдер в апстриме `Try/OpenGothic` от форка проекта, не сверялось.
- Других issue OpenGothic о том, что дым отличается от оригинала, поиск (`smoke`, `particle`) не нашёл. Есть только #906 (освещение частиц) и #871 (магический туман).
