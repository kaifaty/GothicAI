# Рендер частиц нашего OpenGothic: что он может и чего не может для дыма

Источник — только локальный код worktree `elastic-shirley-52e9ff` на `main` (`c2169a5b7`). Ссылки — `путь:строка` относительно корня репозитория (ссылки в скобках ведут туда же из этой папки). Всё, что выведено рассуждением или арифметикой, а не прочитано прямо в коде, стоит в разделах «Inferences».

Короткие обозначения файлов:
- `pfxbucket.cpp` = `OpenGothic/common/graphics/pfx/pfxbucket.cpp`; `particlefx.*` = `OpenGothic/common/graphics/pfx/particlefx.*`; `pfxobjects.*` = `OpenGothic/common/graphics/pfx/pfxobjects.*`
- `main.frag`, `pfx.vert`, `materials_common.glsl` = `OpenGothic/shader/materials/…`
- `common.glsl` = `OpenGothic/shader/common.glsl`; `tonemapping.glsl`, `ambient_light.frag`, `sky_exposure.comp` = `OpenGothic/shader/lighting/…`
- `shaders.cpp`, `material.cpp`, `renderer.cpp`, `sceneglobals.*`, `objvisual.cpp` = `OpenGothic/common/graphics/…`; `sky.cpp` = `OpenGothic/common/graphics/sky/sky.cpp`
- `resources.cpp` = `OpenGothic/common/resources.cpp`; `parser.cpp` = `OpenGothic/common/utils/parser.cpp`
- `particlecontent.cpp`, `particlesdefinitions.cpp` = `OpenGothic/common/game/definitions/…`; `pfxemitter.cpp` = `OpenGothic/common/world/objects/pfxemitter.cpp`; `voblayer.cpp` = `OpenGothic/common/world/voblayer.cpp`; `worldwind.h` = `OpenGothic/common/physics/worldwind.h`

## 1. Освещение: как затеняется цвет частицы и почему серый дым ночью фиолетовый и ярче неба

### Takeaway
`BLEND`-частицы рисуются отдельным forward-шейдером без точечных источников, без небесной освещённости и без AO: свет = солнце × фаза Хеньи-Гринстейна × тень + **удвоенная** окружающая составляющая, в которую входит константа `NightAmbient = (0.3, 0.26, 1)·0.36` — сине-фиолетовая. Ночью солнце обнулено, и частица получает только `2 × NightAmbient`, тогда как мир получает его один раз (плюс тёплый свет костров через отложенное освещение), а ночное небо — лишь долю `NightAmbient` через рассеяние атмосферы. Отсюда фиолетовый дым, который светится ярче неба и стен. `ADD` и `MUL` вообще не освещаются (эмиссив) и не зависят от экспозиции.

### Cited Findings
- Альфа-функция частицы берётся из строки `visAlphaFunc_S` без эвристик текстуры: `NONE`→`AlphaTest`, `BLEND`→`Transparent`, `ADD`→`AdditiveLight`, `MUL`→`Multiply`, всё остальное → `Solid` — [material.cpp:88-94](../../OpenGothic/common/graphics/material.cpp#L88), [parser.cpp:44-54](../../OpenGothic/common/utils/parser.cpp#L44).
- Шейдеры по альфа-функции: `Transparent` → `_f` (FORWARD), `Multiply`/`Multiply2`/`AdditiveLight` → `_e` (EMISSIVE), `Solid`/`AlphaTest` → `_g`/`_g_at` (запись в G-буфер) — [shaders.cpp:580-620](../../OpenGothic/common/graphics/shaders.cpp#L580); варианты для частиц собираются с `-DMESH_TYPE=4` — [OpenGothic/shader/CMakeLists.txt:211-229](../../OpenGothic/shader/CMakeLists.txt#L211).
- Forward-путь (`BLEND`): `color = textureAlbedo(rgb); color *= diffuseLight(alpha); color *= scene.exposure` — [main.frag:443-456](../../OpenGothic/shader/materials/main.frag#L443).
- `diffuseLight` для частиц (`MESH_TYPE==T_PFX`): вместо Ламберта — фаза Хеньи-Гринстейна `henyeyGreenstein(-dot(view,sunDir), a*0.63)`, где `a` — альфа фрагмента; солнце `sunColor*light*shadow`; окружающая `ambient = scene.ambient + (norm.y*0.25+0.75)*NightAmbient*Fd_Lambert`; `sky = vec3(0); // TODO: irradiance`; возврат `lcolor + ambient + ambient` — [main.frag:117-132](../../OpenGothic/shader/materials/main.frag#L117).
- Нормаль billboard-а — `-depth` камеры (`v.normal = -depth`), т. е. одна на весь спрайт и зависит только от наклона камеры — [pfx.vert:92-102](../../OpenGothic/shader/materials/pfx.vert#L92).
- Константы: `GMoonIntensity = 0.36`, `NightAmbient = vec3(0.3, 0.26, 1)*GMoonIntensity` (комментарий: «in vanilla game night time attenuated as rgb{18 16 60}»), `Fd_Lambert = 1/π` — [common.glsl:18-23](../../OpenGothic/shader/common.glsl#L18).
- `textureAlbedo(rgb) = acesTonemapInv(srgbDecode(rgb)*0.78+0.001)*5.0` («HACK: need to preserve look-and-fill of original graphics») — [tonemapping.glsl:43-56](../../OpenGothic/shader/lighting/tonemapping.glsl#L43); `srgbDecode = pow(c, 2.2)` — [common.glsl:94-96](../../OpenGothic/shader/common.glsl#L94).
- Цвет вершины умножается на текстуру **до** `textureAlbedo`: `t *= shInp.color` — [main.frag:589-591](../../OpenGothic/shader/materials/main.frag#L589).
- Ночью прямой свет обнуляется: `scene.sunColor = sunColor * clouds * push.sunOcclusion`, `sunOcclusion = smoothstep(0, 0.01, sunDir.y)` — [sky_exposure.comp:106-117](../../OpenGothic/shader/lighting/sky_exposure.comp#L106), [renderer.cpp:2575](../../OpenGothic/common/graphics/renderer.cpp#L2575).
- `scene.ambient` на CPU: `DirectSunLux/π·0.3·sunOcclude + DirectMoonLux/π·0.3`, затем `×0.68 ×0.5 ×2π ×1/π ×0.3`, `DirectMoonLux = 0.32` — [sky.cpp:31-32](../../OpenGothic/common/graphics/sky/sky.cpp#L31), [sky.cpp:133-149](../../OpenGothic/common/graphics/sky/sky.cpp#L133); в шейдере ещё `× transmittance(sunDir) × clouds` — [sky_exposure.comp:98-117](../../OpenGothic/shader/lighting/sky_exposure.comp#L98).
- Экспозиция: `exposure = 1/max(lum*1.25 + baseL, …)`, `lum = max(objLum*2.5, skyLum*clouds*1.5)` — [sky_exposure.comp:109-118](../../OpenGothic/shader/lighting/sky_exposure.comp#L109); `baseL = 0 + add`, `add = 1.5` — [renderer.cpp:2570-2581](../../OpenGothic/common/graphics/renderer.cpp#L2570).
- Мир (G-буфер → ambient-проход): `ambient + sky*0.8 + (norm.y*0.25+0.75)*NightAmbient*Fd_Lambert` — `NightAmbient` один раз и плюс небесная освещённость — [ambient_light.frag:69-82](../../OpenGothic/shader/lighting/ambient_light.frag#L69); затем `color *= ao; color *= scene.exposure` — [ambient_light.frag:119-134](../../OpenGothic/shader/lighting/ambient_light.frag#L119); точечные источники — отдельный отложенный проход `drawLights` по G-буферу, до прозрачных — [renderer.cpp:722-739](../../OpenGothic/common/graphics/renderer.cpp#L722).
- В forward-шейдере частиц нет ни цикла по точечным источникам, ни чтения кластеров/дерева света: подключены только тени солнца (`L_Shadow0/1`) и, для воды/призраков, цвет и глубина сцены — [materials_common.glsl:143-151](../../OpenGothic/shader/materials/materials_common.glsl#L143), [pfxbucket.cpp:838-846](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L838).
- Тени солнца принимает только `BLEND`: `isShadowmapRequired` = forward и не `AdditiveLight`/`Multiply`/`Multiply2`/`Water` — [material.cpp:140-144](../../OpenGothic/common/graphics/material.cpp#L140); сам `BLEND` тень не отбрасывает — [shaders.cpp:485-487](../../OpenGothic/common/graphics/shaders.cpp#L485), [pfxbucket.cpp:781-787](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L781).
- Ночное небо: свет луны в LUT неба = `raymarchScattering(…, normalize(vec3(-1,1,0)), …) * scene.isNight * NightAmbient/DirectSunLux` — [OpenGothic/shader/sky/sky_view_lut.frag:62-94](../../OpenGothic/shader/sky/sky_view_lut.frag#L62); итог неба `(sky − maxFog) → облака → ×GSunIntensity ×exposure` — [OpenGothic/shader/sky/sky.frag:98-117](../../OpenGothic/shader/sky/sky.frag#L98).
- Эмиссив (`ADD`, `MUL`): `outColor = vec4(textureEmmisive(t.rgb), t.a)` без умножения на экспозицию и без освещения — [main.frag:459-463](../../OpenGothic/shader/materials/main.frag#L459); `textureEmmisive = acesTonemapInv(srgbDecode(rgb))*3` — [tonemapping.glsl:38-41](../../OpenGothic/shader/lighting/tonemapping.glsl#L38).
- `NONE` (`AlphaTest`) частицы пишутся в G-буфер (`drawGBuffer` для `Solid`/`AlphaTest`) и, значит, проходят обычное отложенное освещение; они же рисуются в карту теней — [pfxbucket.cpp:774-787](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L774), [main.frag:366-372](../../OpenGothic/shader/materials/main.frag#L366).
- Туман накладывается после всех прозрачных по глубине **непрозрачной** сцены: `drawTranslucent` → … → `drawFog` — [renderer.cpp:739-763](../../OpenGothic/common/graphics/renderer.cpp#L739); туман берёт `depth` пикселя и смешивает `lum + dst·tr` (`One, OneMinusSrcAlpha`, `alpha = 1−tr`) — [OpenGothic/shader/sky/fog.frag:182-204](../../OpenGothic/shader/sky/fog.frag#L182), [shaders.cpp:726-735](../../OpenGothic/common/graphics/shaders.cpp#L726).
- Фазовая функция неба и тумана — `miePhase(dot(rayDir, sunDir))` (пик при взгляде на солнце) — [OpenGothic/shader/sky/sky_view_lut.frag:22-26](../../OpenGothic/shader/sky/sky_view_lut.frag#L22), [OpenGothic/shader/sky/sky_common.glsl:23-30](../../OpenGothic/shader/sky/sky_common.glsl#L23); у частиц аргумент со знаком минус — `-dot(view, sunDir)` — [main.frag:120-121](../../OpenGothic/shader/materials/main.frag#L120).

### Inferences
- **Цифры ночи (арифметика по константам выше).** `NightAmbient·Fd_Lambert = (0.034, 0.030, 0.115)`. Частица при горизонтальном взгляде (`norm.y≈0` → множитель 0.75) получает `2·0.75 = 1.5×` этого: `(0.052, 0.045, 0.172)`. Земля того же альбедо (`norm.y=1`) — `1.0×`, стена (`norm.y=0`) — `0.75×`, и обе ещё ослаблены SSAO. Итого `BLEND`-частица ночью ≈ в 1,5 раза светлее земли и ≈ в 2 раза светлее стены того же цвета. Знак `norm.y` зависит от наклона камеры, множитель гуляет в пределах 0.5–1.0.
- **Откуда фиолетовый.** Ночью `scene.sunColor = 0` (sunOcclusion), а `scene.ambient` уже на CPU около 0.006 и дополнительно умножается на пропускание атмосферы в сторону солнца под горизонтом (вероятно, ≈0 — LUT пропускания не читал). Значит, единственный свет частицы ночью — `NightAmbient` с отношением R:G:B = 0.30:0.26:1.0: синий втрое сильнее красного, а красный больше зелёного — сине-фиолетовый оттенок. Серая текстура этот оттенок только повторяет. Мир вокруг очага дополнительно освещён тёплыми точечными источниками (отложенный проход), а дым — нет, поэтому контраст «оранжевая стена / фиолетовый дым» максимален именно у огня.
- **Почему ярче неба.** Небо ночью = доля рассеяния атмосферы от «луны» × `NightAmbient`, и из него ещё вычитается `maxFog`. Дневное соотношение в комментарии `sky.cpp:20-27` (небо ~6000–20000 лк против солнца 128000) подсказывает, что эта доля порядка 0.05–0.15. Частица же ≈ `0.48·A·NightAmbient` (A — альбедо после `textureAlbedo`; для серого 128 это ≈0.63, для белого 255 ≈4.5 — вычислено по формуле ACES-инверсии). Поэтому серая частица ≈ в 2–6 раз ярче ночного неба за ней. Цифры для неба — оценка, не замер.
- **Экспозиция ночью** ≈ `1/(1.25·lum + 1.5) ≤ 0.67`: `baseL = 1.5` доминирует при малом `lum` и не даёт глазу «привыкать» дальше. Это одинаково для мира и частиц, на соотношение яркостей не влияет.
- **Днём** у частиц нет `sky*0.8` (небесная освещённость не подключена), зато `scene.ambient` удвоен. Солнце даёт им фазу Хеньи-Гринстейна с аргументом `-dot(view,sunDir)`. При соглашении `sunDir` = направление на солнце (оно же в `lambert`) пик приходится на взгляд **от** солнца: дым ярче всего, когда солнце за спиной, и темнее всего в контровом свете. У настоящего дыма и у тумана в этом движке всё наоборот. Вероятно, знак инвертирован; это вывод из сравнения с `miePhase` неба и тумана, а не проверка в кадре.
- **Данными** освещение исправить нельзя: можно только темнить `visTexColorStart_S`/`End_S`. Цвет вершины умножается до `pow 2.2` и ACES-инверсии, так что уменьшение цвета вдвое уменьшает яркость примерно в 4–5 раз. Но это темнит и днём, а ночной сдвиг оттенка тёплым цветом исказит дневной вид. Нужна работа в движке: убрать удвоение `ambient + ambient`, добавить `skyIrradiance`, подать точечные источники в forward-шейдер (или освещать частицы через буфер освещённости/кластеры) и, по-видимому, исправить знак фазы.
- `NONE` (`AlphaTest`) — единственный путь «данными» к частицам, которые освещают костры и которые сами отбрасывают тень, но у них жёсткий срез по `alpha < 0.5` и нет полупрозрачности. Для дыма это годится разве что на плотные хлопья сажи.

### Gaps
- Значение LUT пропускания (`transLut`) при солнце под горизонтом не прочитано: что `scene.ambient` ночью строго ≈0 — вывод.
- Абсолютную яркость ночного неба (интеграл `raymarchScattering` и ночные текстуры облаков `textureNightL0/L1`) без замера в кадре не оценить. Сравнение «дым/небо» — порядок величины.
- Почему у частиц `ambient + ambient` и знак фазы — намеренно (подгонка под ванильный вид) или ошибка, в коде не объяснено.

## 2. Смешивание: какие visAlphaFunc поддержаны и почему MUL рисует чёрные квадраты

### Takeaway
Поддержаны `BLEND` (обычное «over», освещённое), `ADD` (аддитивное, эмиссив, без освещения), `MUL` (умножение, эмиссив) и `NONE` (альфа-тест в G-буфер). `MUL2`, `SUB` и любые другие строки молча становятся непрозрачным `Solid`. `MUL` сломан для обычных текстур: смешивание `DstColor·src + SrcColor·dst = 2·src·dst` игнорирует альфу, а `src` проходит через ACES-инверсию ×3. Чёрный фон текстуры даёт чёрный квадрат, белый — пересвет примерно в 40 раз; «нейтральный» серый ≈ 134/255.

### Cited Findings
- Строки альфа-функции частиц: только `NONE`, `BLEND`, `ADD`, `MUL`; иначе `Solid` — [parser.cpp:44-54](../../OpenGothic/common/utils/parser.cpp#L44). `Multiply2` и `Ghost` из описания PFX получить нельзя (для мешей `MULTIPLY_ALT` → `Multiply2` есть, но это другой загрузчик — [material.cpp:174-179](../../OpenGothic/common/graphics/material.cpp#L174)).
- Состояния смешивания: `Transparent` → `SrcAlpha, OneMinusSrcAlpha`, без записи глубины (комментарий «premultiply in shader», но в шейдере предумножения нет); `AdditiveLight` → `SrcAlpha, One`; `Multiply`/`Multiply2` → `DstColor, SrcColor`; глубина везде проверяется `LEqual` и не пишется — [shaders.cpp:500-535](../../OpenGothic/common/graphics/shaders.cpp#L500).
- `Multiply` и `AdditiveLight` используют эмиссивный фрагментный шейдер `_e` — [shaders.cpp:603-611](../../OpenGothic/common/graphics/shaders.cpp#L603); он выводит `textureEmmisive(t.rgb)` = `acesTonemapInv(pow(rgb,2.2))*3` — [main.frag:459-463](../../OpenGothic/shader/materials/main.frag#L459), [tonemapping.glsl:28-41](../../OpenGothic/shader/lighting/tonemapping.glsl#L28).
- Цвет вершины на CPU: для `AdditiveLight` RGB предумножается на альфу жизни, `a = 255`; для остальных (включая `MUL`) `rgb = visTexColor`, а альфа жизни уходит только в `.a` — [pfxbucket.cpp:648-658](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L648).
- Порядок отрисовки прозрачных групп внутри ведра: `Multiply, Multiply2, Transparent, AdditiveLight, Ghost` (вода закомментирована); ведро рисуется, только если его альфа совпадает — [pfxbucket.cpp:789-802](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L789).
- `Solid`/`AlphaTest` частицы рисуются в G-буфер и в тени — [pfxbucket.cpp:774-787](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L774); в G-буфере альфа участвует только в отбрасывании `t.a < 0.5` — [main.frag:580-583](../../OpenGothic/shader/materials/main.frag#L580).

### Inferences
- **MUL = чёрные квадраты (арифметика).** Итог пикселя `= 2·src·dst`, где `src = 3·acesInv(rgb^2.2)` текстуры × цвет вершины. `acesInv(0) = 0` → где RGB текстуры чёрный (обычный фон альфа-текстуры дыма), пиксель становится чёрным независимо от альфы. Альфа и плавное появление/исчезание на `MUL` не действуют вовсе: факторы `DstColor`/`SrcColor` альфу не используют, а цвет вершины для `MUL` альфой не умножается. «Не меняет фон» значение `src = 0.5`, то есть `acesInv(lin) = 1/6`, `lin ≈ 0.244`, `rgb ≈ 0.527` (~134/255). Белый 255 даёт `src ≈ 21.7` — пересвет фона ~43×. Ванильные MUL-текстуры (белый = нейтрально) здесь непригодны. Данными можно лишь нарисовать свою текстуру с фоном ~134 вместо чёрного, но и тогда частица будет появляться и исчезать скачком.
- **ADD для дыма плох.** Яркость складывается без насыщения и не зависит от экспозиции и времени суток: ночью серый аддитивный дым светится, днём его почти не видно на фоне пре-экспонированного неба.
- **BLEND — единственный практичный режим для дыма.** Он единственный полупрозрачный и освещаемый, с ограничениями из раздела 1.
- Через `NONE` можно получить «твёрдые» хлопья с отложенным освещением и тенью, но без полупрозрачности.

### Gaps
- Как именно выглядит MUL в кадре, по коду не проверить; чёрный фон текстуры — предположение о типичных текстурах дыма. Конкретные TGA базы не открывались.

## 3. Мягкие частицы: есть ли затухание у пересечения с геометрией

### Takeaway
Нет. Частица — плоский, параллельный экрану квад с проверкой глубины `LEqual` и без чтения буфера глубины в шейдере частиц, поэтому крыша или стена срезает спрайт жёсткой линией. `visZBias`, который сдвигает спрайт к камере, из описаний PFX не задаётся.

### Cited Findings
- Состояние конвейера частиц: `ZTestMode::LEqual`, глубина не пишется для `BLEND/ADD/MUL` — [shaders.cpp:500-535](../../OpenGothic/common/graphics/shaders.cpp#L500).
- Глубина и цвет сцены привязываются к шейдеру только если `isSceneInfoRequired` — это лишь `Water` и `Ghost` — [material.cpp:136-138](../../OpenGothic/common/graphics/material.cpp#L136), [pfxbucket.cpp:843-846](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L843); объявления `sceneColor`/`gbufferDepth` есть только под `WATER || GHOST` — [materials_common.glsl:148-151](../../OpenGothic/shader/materials/materials_common.glsl#L148). В forward-ветке частиц (`mainForward`) обращения к глубине нет — [main.frag:442-456](../../OpenGothic/shader/materials/main.frag#L442).
- Ориентация квада — общие для кадра векторы `pfxLeft/pfxTop/pfxDepth` из матрицы вида (параллельно экрану, не «на камеру» для каждой частицы) — [OpenGothic/common/graphics/sceneglobals.cpp:103-105](../../OpenGothic/common/graphics/sceneglobals.cpp#L103), [pfx.vert:90-99](../../OpenGothic/shader/materials/pfx.vert#L90).
- `visZBias` сдвигает вершины на `size.z` к камере (`size.z = 0.1·средний размер`) — [pfx.vert:154-155](../../OpenGothic/shader/materials/pfx.vert#L154), [pfxbucket.cpp:639](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L639); выставляется только в конструкторе спрайтов-вобов (`vob.bias != 0`) — [particlefx.cpp:34](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L34); конструктор из описания PFX его не трогает (по умолчанию `false`) — [particlefx.h:111](../../OpenGothic/common/graphics/pfx/particlefx.h#L111), [particlefx.cpp:39-110](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L39).
- Встроенное появление и исчезание — только по времени жизни (раздел 5), не по глубине — [pfxbucket.cpp:633-634](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L633).
- Копия глубины сцены для прозрачных уже существует (`stashSceneAux` перед водой и прозрачными; `scene.sceneDepth` привязывается для воды/призраков) — [renderer.cpp:729-739](../../OpenGothic/common/graphics/renderer.cpp#L729), [pfxbucket.cpp:843-846](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L843).

### Inferences
- Мягкие частицы — работа в движке, но небольшая: привязать `L_GDepth` к forward-варианту частиц и умножить альфу на `saturate((z_scene − z_particle)/fadeDist)`. Механизм привязки копии глубины уже есть у воды и призраков.
- Без этого данными остаётся: поднимать эмиттер выше кромки крыши (`shpOffsetVec_S` или позиция слоя), уменьшать начальный размер (`visSizeStart_S`) и рост (`visSizeEndScale`), чтобы молодые частицы не пересекали крышу. Встроенное нарастание альфы в первые 10 % жизни (раздел 5) немного прячет срез у трубы.

### Gaps
- Не проверено, совместим ли `scene.sceneDepth` по разрешению и времени с проходом прозрачных при включённом апскейле; для оценки трудоёмкости это стоит прочитать в `stashSceneAux`.

## 4. Сортировка и перерисовка: как копится плотность

### Takeaway
Сортировки нет ни между вёдрами, ни внутри ведра. Частицы рисуются одним instanced-вызовом в порядке слотов, а новые частицы занимают первые свободные слоты, так что порядок не зависит ни от глубины, ни даже от возраста. Плотность копится обычным «over»: `покрытие = 1 − (1−α)^N`. Самозатенения и поглощения внутри шлейфа нет: все частицы освещены одинаково, поэтому толща дыма не темнеет, а только становится непрозрачнее.

### Cited Findings
- Вёдра (по одному на определение PFX) хранятся в `std::list` и рисуются в порядке создания — [pfxobjects.cpp:81-92](../../OpenGothic/common/graphics/pfx/pfxobjects.cpp#L81); прозрачные меши рисуются целиком до всех частиц — [OpenGothic/common/graphics/worldview.cpp:499-502](../../OpenGothic/common/graphics/worldview.cpp#L499).
- Всё ведро — один вызов `draw(…, 6, 0, count)` по всем слотам, мёртвые слоты имеют размер 0 — [pfxbucket.cpp:851](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L851), [pfxbucket.cpp:625-628](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L625).
- Выпуск новых частиц каждый тик начинает поиск свободного слота с индекса 1 (`id%blockSize`) — [pfxbucket.cpp:586-604](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L586).
- Слов `sort`, `cull`, `frustum` в каталоге `graphics/pfx` нет (проверено `grep` по `OpenGothic/common/graphics/pfx/*`).
- Свет каждой частицы зависит только от её фрагмента: фаза, тень солнца в точке и окружающая константа; ни плотности соседей, ни глубины внутри облака — [main.frag:117-132](../../OpenGothic/shader/materials/main.frag#L117).

### Inferences
- При одинаковом цвете всех частиц ошибки порядка в «over» почти незаметны. Но если цвет или альфа меняются за жизнь (например, тёмный у трубы → светлый вверху), порядок слотов даёт мерцание и «прыгающие» слои, особенно при взгляде снизу вверх или сверху вниз вдоль столба.
- Чтобы шлейф выглядел объёмным, данными доступны только градиенты цвета и альфы по жизни. Настоящее самозатенение (верх светлее, низ и сердцевина темнее) требует работы в движке.
- Каждый перекрывающийся пиксель — это ещё один полный проход фрагментного шейдера с тенью; плотный дым из многих крупных спрайтов дорог по заполнению. Глобального ограничения нет (раздел 9).

### Gaps
- Стоимость перерисовки на M1 Pro не мерялась; цифр перформанса для дыма нет.

## 5. Размер, альфа и цвет за жизнь; поворот и разброс

### Takeaway
Цвет и размер — только линейная интерполяция «начало → конец». Альфа тоже линейна, но с зашитыми затуханиями: она нарастает с нуля за первые 10 % жизни и спадает к нулю за последние 10 %, это не настраивается. Случайного поворота спрайта, вращения во времени и случайного размера нет; случайны только время жизни, скорость и направление (равномерно в `avg ± var`).

### Cited Findings
- Возраст `a = 1 − life/maxLife` (0 при рождении, 1 при смерти) — [pfxbucket.cpp:31-33](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L31).
- Цвет: `cl = colorS·(1−a) + colorE·a` — [pfxbucket.cpp:631](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L631).
- Альфа: `clA = (min(a,0.1)/0.1)·αS·(1−a) + (min(1−a,0.1)/0.1)·αE·a`, с комментарием, что в ванили частицы тоже как будто плавно появляются и исчезают — [pfxbucket.cpp:632-634](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L632); `visAlphaStart/End` делятся на 255 — [particlefx.cpp:90-91](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L90).
- Размер: `scale = (1−a) + a·visSizeEndScale`; `szX/szY = visSizeStart·scale` — [pfxbucket.cpp:636-639](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L636). Размер в шейдере — полная ширина квада (`±0.5`) — [pfx.vert:78-79](../../OpenGothic/shader/materials/pfx.vert#L78), [pfx.vert:149-150](../../OpenGothic/shader/materials/pfx.vert#L149).
- Цвет и альфа в вершине — `uint8_t` без ограничения сверху — [pfxbucket.cpp:641-658](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L641).
- Случайность: `randf(base,var) = base + var·(2u−1)`, `u` — с шагом 1/10000 — [pfxbucket.cpp:259-265](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L259); применяется к жизни — [pfxbucket.cpp:275](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L275), к скорости — [pfxbucket.cpp:432-436](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L432), к углам — [pfxbucket.cpp:384-385](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L384). Размер и цвет случайности не имеют — [pfxbucket.cpp:609-662](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L609).
- Ориентация: `NONE` (по экрану), `VELO` (поворот по экранной проекции скорости и растяжение `1.5·(1−|dir.z|)`), `VELO3D` (вдоль скорости); поворот по времени или случайный угол отсутствуют — [pfx.vert:123-147](../../OpenGothic/shader/materials/pfx.vert#L123), [particlefx.cpp:282-290](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L282).
- `visYawAlign` (только поворот вокруг вертикали) выставляется лишь для спрайтов-вобов — [particlefx.cpp:33](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L33).
- `visTexIsQuadPoly = 0` даёт треугольник вместо квада — [pfx.vert:81-82](../../OpenGothic/shader/materials/pfx.vert#L81), [pfx.vert:149-152](../../OpenGothic/shader/materials/pfx.vert#L149).

### Inferences
- Кривых (несколько ключей для размера, цвета или альфы) нет — только две точки. Нелинейный вид данными можно имитировать **цепочкой эмиттеров** (`ppsCreateEm_S`, раздел 7) или **наложением** нескольких своих определений с разными временами жизни и размерами. Кривые от возраста частицы так не получить.
- Встроенное затухание по 10 % хорошо для дыма: у трубы нет «выскакивания». Но если `visAlphaStart` мал, а `visAlphaEnd` велик, пик будет поздно, а нарастание — первые 10 % — от нуля к уже интерполированному значению.
- Однообразие одинаково повёрнутых спрайтов данными ломается только несколькими своими текстурами (заранее повёрнутые варианты) в разных определениях, наложенных друг на друга. Случайный поворот и разброс размера на частицу — работа в движке: место в `PfxState` есть, но шейдер это не читает.
- Отрицательная или нулевая жизнь даёт `uint16_t` от отрицательного float (неопределённое поведение) — `lspPartVar` не должен превышать `lspPartAvg`.

### Gaps
- Кривые по ключам в оригинальном ZenGin (если они там были) в коде не отражены; сравнения с ванилью нет.

## 6. Анимация текстуры и свои текстуры в visName_S

### Takeaway
Последовательность `_A0.._An` циклически проигрывается со скоростью `visTexAniFPS`, но **один кадр на всё ведро** (все частицы всех эмиттеров этого PFX одновременно) по глобальным часам сцены. Нет случайного кадра на частицу, нет флипбука по возрасту, `visTexAniIsLooping` игнорируется. Свою текстуру дыма можно положить только через `OPENGOTHIC_TEXTURES` (DDS/PNG по имени) или в базу. Карты `content/materials` для частиц **не** применяются: конструктор материала частицы вызывает `loadTexture`, а не `loadMaterialAlbedo`.

### Cited Findings
- Кадры: `loadFrames(vis_name_s, vis_tex_ani_fps)`, `//TODO: visTexAniIsLooping` — [material.cpp:88-94](../../OpenGothic/common/graphics/material.cpp#L88); `texAniFPSInv = 1000/fps`, при `fps ≤ 0` — вся последовательность за секунду — [material.cpp:216-223](../../OpenGothic/common/graphics/material.cpp#L216).
- Поиск кадров: имя должно содержать `_A0`, кадры `_A0,_A1,…` берутся, пока `hasTexture` их находит — [resources.cpp:2204-2235](../../OpenGothic/common/resources.cpp#L2204); `hasTexture` требует расширение `.TGA` и ищет только в `hdTextures` и текстурах базы — [resources.cpp:2196-2202](../../OpenGothic/common/resources.cpp#L2196).
- Выбор кадра при отрисовке: `frame = (itm.timeShift + scene.tickCount)/mat.texAniFPSInv`, одна привязка `L_Diffuse` на вызов — [pfxbucket.cpp:827-830](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L827); `timeShift` нигде не присваивается (по умолчанию 0) — [pfxbucket.h:123](../../OpenGothic/common/graphics/pfx/pfxbucket.h#L123).
- UV-анимация (`texAniMapDirPeriod`) и `alphaWeight` для частиц выключены (`MESH_TYPE==T_PFX`) — [main.frag:253-259](../../OpenGothic/shader/materials/main.frag#L253).
- Текстура частицы: `Resources::loadTexture(src.vis_name_s,false,false)` — [material.cpp:89](../../OpenGothic/common/graphics/material.cpp#L89) → `implLoadTexture` → `implDecodeTexture`: сначала `hdTextures` (`OPENGOTHIC_TEXTURES`, ключ — имя без `.TGA`), затем текстуры базы, иначе ошибка и `nullptr` — [resources.cpp:2153-2156](../../OpenGothic/common/resources.cpp#L2153), [resources.cpp:1275-1318](../../OpenGothic/common/resources.cpp#L1275); каталог `OPENGOTHIC_TEXTURES` индексирует `.DDS`/`.PNG` по стему, DDS предпочтительнее — [resources.cpp:793-815](../../OpenGothic/common/resources.cpp#L793).
- Замены `content/materials/materials.json` доступны через `loadMaterialMaps`/`loadMaterialAlbedo` — [resources.cpp:1148-1185](../../OpenGothic/common/resources.cpp#L1148), [resources.cpp:2277-2286](../../OpenGothic/common/resources.cpp#L2277); путь частиц их не вызывает (см. выше). Документ говорит о текстурах вообще: «карты `content/materials` и `OPENGOTHIC_TEXTURES` → база» — [docs/asset-migration.md:33](../../docs/asset-migration.md).
- Если текстура частицы не нашлась (`visMaterial.tex == nullptr`), эмиттер не создаётся — [pfxemitter.cpp:32-34](../../OpenGothic/common/world/objects/pfxemitter.cpp#L32), [pfxemitter.cpp:51-54](../../OpenGothic/common/world/objects/pfxemitter.cpp#L51).
- Кадры анимации входят в «спрос» резидентности текстур вместе с основной — [resources.cpp:1432-1439](../../OpenGothic/common/resources.cpp#L1432).

### Inferences
- Анимированная текстура дыма будет «пульсировать» синхронно у всех частиц и всех очагов с этим PFX — это скорее вредит, чем помогает. Данными синхронность ломается только несколькими своими определениями (каждое — своё ведро) с разными последовательностями или разным `visTexAniFPS`; внутри одного определения кадр всё равно общий.
- Флипбук по возрасту частицы или случайный стартовый кадр — работа в движке: например, писать индекс кадра в `PfxState` и выбирать слой текстурного массива в шейдере, ведь сейчас кадр — отдельная привязка на весь вызов.
- Своя текстура дыма «чисто контентом» (в `content/`) сейчас невозможна: нужен `OPENGOTHIC_TEXTURES` или небольшая правка движка — `loadMaterialAlbedo` в `Material(const Def::ParticleEffect&)`, чтобы `materials.json` работал и для частиц.
- Имя в `visName_S` должно оканчиваться на `.TGA`, даже если файл — DDS/PNG из `OPENGOTHIC_TEXTURES`: поиск по `hdTextures` срабатывает только для имени с расширением TGA.

### Gaps
- Как упаковка контента (`building_set.py compose`, `OPENGOTHIC_CONTENT`) поставляет текстуры частиц, не проверено. Возможно, в планах есть иной путь, кроме `OPENGOTHIC_TEXTURES`.

## 7. Ветер, направление, dirMode/dirFOR и поворот эмиттера слоя

### Takeaway
Ветер на частицы не действует вообще: `WorldWind` используют только ткань и растительность. Движение — явный Эйлер: `pos += vel·dt; vel += flyGravity·dt` (см/мс), без сопротивления воздуха, турбулентности и коллизий. Направление: `RAND` (сфера), `DIR` (угол курса и угол возвышения ± разброс, курс сдвинут на +90°), `TARGET`. При `dirFOR = OBJECT` направление поворачивается матрицей эмиттера, а поворот слоя (`rotation`/`yaw_deg`) до неё доходит. Форма эмиттера (box/circle/sphere/line) **не** поворачивается — только `shpOffsetVec`.

### Cited Findings
- Интегрирование: `ps.pos += ps.dir*dt; ps.dir += decl.flyGravity*dt` — [pfxbucket.cpp:456-461](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L456); других сил нет.
- `WorldWind::sample`/`shaderMatrix` вызываются только тканью и растительностью — [worldwind.h:9-43](../../OpenGothic/common/physics/worldwind.h#L9), [OpenGothic/common/physics/dynamicworld.cpp:2815-2821](../../OpenGothic/common/physics/dynamicworld.cpp#L2815), [OpenGothic/common/graphics/visualobjects.cpp:824-831](../../OpenGothic/common/graphics/visualobjects.cpp#L824); в `graphics/pfx`, `pfxemitter.*` и `pfx.vert` слова «wind» нет, кроме комментария `MFX_WINDFIST_CAST` — [pfxbucket.cpp:415](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L415).
- `flyCollDet_B` читается и больше нигде не используется — [particlefx.cpp:79](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L79) (`grep flyCollDet` вне `particlefx.*` пуст).
- `DIR`: `head = 90 + randf(dirAngleHead, var)`, `elev = randf(dirAngleElev, var)`, вектор `(cos e·cos h, sin e, cos e·sin h)`; разброс ≥180° обнуляется (хак `STARGATE_PARTICLES`) — [pfxbucket.cpp:374-390](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L374). При `dirFOR` `OBJECT`/`object` вектор умножается на строки матрицы эмиттера, при `WORLD` — нет — [pfxbucket.cpp:391-403](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L391). Строка `"object"` строчными означает `Node` — [particlefx.cpp:250-258](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L250).
- `RAND`: равномерно по сфере — [pfxbucket.cpp:364-373](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L364). После выбора направление нормируется и умножается на `randf(velAvg, velVar)` — [pfxbucket.cpp:432-436](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L432).
- Форма: точки `BOX/SPHERE/CIRCLE/LINE` строятся в локальных координатах и масштабируются `shpDim·shpScale(t)` по мировым осям; матрица эмиттера к ним не применяется (кроме `MESH`); при `shpFOR = OBJECT` поворачивается только `shpOffsetVec` — [pfxbucket.cpp:280-361](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L280). `CIRCLE` лежит в плоскости XZ — [pfxbucket.cpp:316-325](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L316). `shpDistribType`/`shpDistribWalkSpeed` — TODO, `BOX` поверхность = объём — [pfxbucket.cpp:279](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L279), [pfxbucket.cpp:290-302](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L290).
- `useEmittersFOR = 1`: позиции частиц локальны и сдвигаются вместе с эмиттером; `0` — частица получает мировую позицию при рождении — [pfxbucket.cpp:429-430](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L429), [pfxbucket.cpp:688-692](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L688).
- Поворот эмиттера слоя: `emitter` принимает `rotation` или `yaw_deg` — [voblayer.cpp:363-371](../../OpenGothic/common/world/voblayer.cpp#L363), [voblayer.cpp:387](../../OpenGothic/common/world/voblayer.cpp#L387); вобы с PFX создают зацикленный активный эмиттер — [objvisual.cpp:329-335](../../OpenGothic/common/graphics/objvisual.cpp#L329); `setObjMatrix` → `setPosition` + `setDirection` (строки 3×3 матрицы) — [objvisual.cpp:362-363](../../OpenGothic/common/graphics/objvisual.cpp#L362), [pfxemitter.cpp:138-149](../../OpenGothic/common/world/objects/pfxemitter.cpp#L138), [pfxemitter.cpp:258-262](../../OpenGothic/common/world/objects/pfxemitter.cpp#L258).
- Цепочка `ppsCreateEm_S`: дочерний эмиттер создаётся в тике с позицией, активностью и зацикленностью родителя, но **без** `setDirection` — [pfxbucket.cpp:520-526](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L520); `setDirection` передаётся дочернему, только если он уже существует — [pfxemitter.cpp:147-148](../../OpenGothic/common/world/objects/pfxemitter.cpp#L147). Направление по умолчанию — единичная матрица — [pfxbucket.h:36](../../OpenGothic/common/graphics/pfx/pfxbucket.h#L36).

### Inferences
- **Ветер данными:** постоянный снос — горизонтальная составляющая `flyGravity_S` (например, `"0.00002 0.00004 0"` см/мс²) или наклон `dirAngleElev`/`dirAngleHead`; поворот всего шлейфа под направление улицы — `dirFOR = "OBJECT"` + `yaw_deg` в слое. Порывы и смена направления во времени невозможны: `ppsScaleKeys` меняет только темп выпуска, а `shpScaleKeys` — только размер формы.
- **Подъём без замедления.** Плавучесть = положительная `y` у `flyGravity`, но скорость растёт без предела; при отрицательной — замедляется и разворачивается вниз. Реальный шлейф (быстрый у трубы, вверху почти стоит и расползается) данными передаётся только тем, что `visSizeEndScale` растёт, а `velAvg` мал. Сопротивление воздуха и шум (curl noise) — работа в движке; готовый `WorldWind::sample` легко подать как силу в `PfxBucket::tick`.
- Дочерний эмиттер повёрнутого эмиттера слоя, вероятно, выпускает частицы в мировом базисе: `setObjMatrix` статичного воба вызывается при создании, раньше, чем появляется дочерний. То, что для статичного воба это единственный вызов, — вывод, не проверено.
- Дым проходит сквозь крыши и навесы: коллизий частиц нет.

### Gaps
- Не прочитано, вызывается ли `ObjVisual::setObjMatrix` для статичных вобов слоя повторно (это решает вопрос с поворотом дочернего эмиттера).

## 8. Время суток: можно ли сделать PFX разным днём и ночью

### Takeaway
Хуков нет. `m_bIsAmbientPFX` и `timeStartEnd_S` читаются в `ParticleFx` и больше нигде не используются; эмиттеры вобов и слоя всегда активны и зациклены. Днём и ночью меняется только освещение `BLEND`-частиц (раздел 1). `ADD`/`MUL` выглядят одинаково в любое время.

### Cited Findings
- Поля читаются — [particlefx.cpp:106-107](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L106); Daedalus-привязка `TIMESTARTEND_S`/`M_BISAMBIENTPFX` есть, только если программа их объявляет — [OpenGothic/common/game/definitions/definitiontypes.cpp:129-134](../../OpenGothic/common/game/definitions/definitiontypes.cpp#L129) (блок `if(s.find_symbol_by_name("C_PARTICLEFX.FLOCKMODE"))`); других обращений к `m_bIsAmbientPFX`/`timeStartEnd_S` в `OpenGothic/common` нет (проверено `grep`).
- Эмиттеры PFX-вобов создаются с `setActive(true)`, `setLooped(true)` — [objvisual.cpp:329-335](../../OpenGothic/common/graphics/objvisual.cpp#L329); у эмиттера слоя нет полей времени — допустимы только `key, kind, position, rotation, yaw_deg, pfx` — [voblayer.cpp:364](../../OpenGothic/common/world/voblayer.cpp#L364).
- Эмиссивный путь не умножается на экспозицию и не освещается — [main.frag:459-463](../../OpenGothic/shader/materials/main.frag#L459).
- Описания `particles.d` читаются один раз при создании базы определений частиц; после правки нужен перезапуск — [particlesdefinitions.cpp:11-15](../../OpenGothic/common/game/definitions/particlesdefinitions.cpp#L11), [docs/asset-migration.md:195](../../docs/asset-migration.md).

### Inferences
- Сделать дым ночью темнее и без фиолетового оттенка, а днём светлее, можно только в движке. Варианты: (а) исправить освещение частиц (раздел 1) — тогда один PFX будет правильно выглядеть в любое время; (б) поддержать `timeStartEnd_S` (включать и выключать эмиттер по игровому часу) и поставить два эмиттера; (в) добавить в слой поле времени работы эмиттера. Вариант (а) лучше: он лечит причину и не удваивает число эмиттеров.
- `m_bIsAmbientPFX` по смыслу ванильного ZenGin — «окружающий эффект», но как ванильный движок им пользовался, локальный код не показывает.

### Gaps
- Семантика `timeStartEnd_S`/`m_bIsAmbientPFX` в оригинальном ZenGin (формат строки, кто читает) в репозитории не задокументирована.

## 9. Прочие ограничения реалистичности: число частиц, дальность, LOD

### Takeaway
Главное практическое ограничение — радиус **40 м от камеры**: дальше эмиттер не выпускает частицы, и шлейф исчезает через одно время жизни. При приближении он заново растёт с нуля, предварительного прогрева нет. Число частиц на эмиттер = `ceil((lspPartAvg+lspPartVar)·maxPps/1000)`, а при нехватке слотов выпуск молча пропускается. Время жизни хранится в `uint16` — максимум ~65,5 с. LOD, затухания по расстоянию и отсечения по пирамиде видимости на частицу нет.

### Cited Findings
- `viewRage = 4000.f` (см) — [pfxobjects.h:22](../../OpenGothic/common/graphics/pfx/pfxobjects.h#L22); выпуск только при `nearby` — [pfxbucket.cpp:517-518](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L517), [pfxbucket.cpp:549-553](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L549); пустой блок освобождается, если эмиттер далеко — [pfxbucket.cpp:536-545](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L536); точка отсчёта — позиция камеры `camera.originLwc()` (`origin+shake`) — [OpenGothic/common/graphics/worldview.cpp:391](../../OpenGothic/common/graphics/worldview.cpp#L391), [OpenGothic/common/camera.h:135](../../OpenGothic/common/camera.h#L135).
- Новый блок начинает с `timeTotal = 0` — [pfxbucket.cpp:148-173](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L148); выпуск за тик = `ppsScale(t1)·ppsValue·dt` — [pfxbucket.cpp:16-29](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L16).
- Размер блока: `lt = lspPartAvg+lspPartVar`, `pps = ceil(maxPps())` (максимум ключей `ppsScaleKeys`·`ppsValue`), `blockSize = ceil(lt·pps/1000)` — [pfxbucket.cpp:43-48](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L43), [particlefx.cpp:155-179](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L155); при отсутствии свободного слота выпуск прекращается — [pfxbucket.cpp:598-603](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L598).
- Жизнь — `uint16_t life, maxLife` — [pfxbucket.h:109-116](../../OpenGothic/common/graphics/pfx/pfxbucket.h#L109), [pfxbucket.cpp:275-276](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L275).
- Симуляция и сборка буфера идут на CPU для всех вёдер каждый тик — [pfxobjects.cpp:30-51](../../OpenGothic/common/graphics/pfx/pfxobjects.cpp#L30); буфер — `Upload`-память, обновляется каждый кадр — [pfxbucket.cpp:747-762](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L747).
- Темп можно модулировать ключами: `ppsScaleKeys_S`, `ppsIsLooping`, `ppsIsSmooth`, `ppsFPS` (линейно или ступенями) — [particlefx.cpp:41-45](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L41), [particlefx.cpp:292-315](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L292); размер формы — `shpScaleKeys_S` по времени эмиттера — [particlefx.cpp:181-184](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L181), [pfxbucket.cpp:341-347](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L341).
- Свои описания: `particles.d/*.json`, `{"format":1,"effects":[{"name","base","set"}]}`; в `set` — любые члены `C_PARTICLEFX` с проверкой типа (int/float/string); имя `[A-Z0-9_]+`, без конфликта с базой; ошибка отклоняет файл целиком — [particlecontent.cpp:47-79](../../OpenGothic/common/game/definitions/particlecontent.cpp#L47), [particlecontent.cpp:121-173](../../OpenGothic/common/game/definitions/particlecontent.cpp#L121).

### Inferences
- Дым над кварталом издали (с холма, со стены) не виден, если камера дальше 40 м от трубы. Для «дымящего города» это главный разрыв, и он требует работы в движке: увеличить `viewRage` для отдельных эффектов, сделать прогрев при входе в радиус (выпустить частицы за `maxLifetime` вперёд) или упрощённый дальний LOD.
- Данными доступна «дышащая» труба: `ppsScaleKeys_S` с несколькими значениями, `ppsIsLooping = 1`, `ppsIsSmooth = 1` и малым `ppsFPS` дают клубы. Медленное расширение основания — `shpScaleKeys_S`. Это работает, потому что эмиттеры вобов зациклены.
- При долгоживущих частицах (12–20 с) и умеренном `ppsValue` (5–15) блок — 60–300 частиц на эмиттер. Предел — заполнение экрана, а не CPU, но это оценка, не замер.

### Gaps
- Производительность дыма (overdraw на M1 Pro, стоимость CPU-сборки на десятки эмиттеров) не мерялась.

## 10. Сводка: что можно данными (`particles.d` + слой), а что требует движка или шейдера

### Takeaway
Данными доступны форма, темп (в том числе пульсация), направление и поворот через слой, постоянный снос и плавучесть через `flyGravity`, двухточечные цвет, размер и альфа, `BLEND` со встроенными затуханиями и наложение нескольких своих определений. Работы в движке требуют освещение частиц (удвоенный фиолетовый ambient, нет точечных источников и неба, сомнительный знак фазы), мягкие частицы, сортировка, случайный поворот, разброс размера и кадра, флипбук по возрасту, кривые больше двух точек, ветер и сопротивление, коллизии, режим дня и ночи, дальность больше 40 м с прогревом, исправный `MUL`, а также свои текстуры частиц из `content/materials`.

### Cited Findings
- Данными (поля `C_PARTICLEFX` через `set`): `ppsValue`, `ppsScaleKeys_S`, `ppsIsLooping`, `ppsIsSmooth`, `ppsFPS`, `ppsCreateEm_S`, `shpType_S`, `shpFOR_S`, `shpOffsetVec_S`, `shpDim_S`, `shpIsVolume`, `shpScaleKeys_S`, `dirMode_S`, `dirFOR_S`, `dirAngle*`, `velAvg/Var`, `lspPartAvg/Var`, `flyGravity_S`, `visName_S`, `visOrientation_S`, `visTexAniFPS`, `visTexColorStart_S/End_S`, `visSizeStart_S`, `visSizeEndScale`, `visAlphaFunc_S`, `visAlphaStart/End`, `useEmittersFOR` — все читаются в [particlefx.cpp:39-110](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L39) и применяются в `pfxbucket.cpp` (разделы 5, 7, 9).
- Читаются, но **не действуют**: `shpDistribType_S`, `shpDistribWalkSpeed` ([pfxbucket.cpp:279](../../OpenGothic/common/graphics/pfx/pfxbucket.cpp#L279)), `flyCollDet_B`, `visTexAniIsLooping` ([material.cpp:92](../../OpenGothic/common/graphics/material.cpp#L92)), `mrk*` (маркеры), `flockMode/Strength`, `timeStartEnd_S`, `m_bIsAmbientPFX` (поиск обращений вне `particlefx.*` пуст); `visZBias`/`visYawAlign` из PFX не задаются ([particlefx.cpp:33-34](../../OpenGothic/common/graphics/pfx/particlefx.cpp#L33)).
- Альфа-функции: рабочие для дыма — только `BLEND`; `ADD` — эмиссив; `MUL` — эмиссив `2·src·dst` без альфы; `NONE` — твёрдый альфа-тест с отложенным светом; прочее — `Solid` ([parser.cpp:44-54](../../OpenGothic/common/utils/parser.cpp#L44), [shaders.cpp:513-535](../../OpenGothic/common/graphics/shaders.cpp#L513)).

### Inferences
- Порядок работ в движке по отдаче для дыма очага (оценка): 1) освещение `BLEND`-частиц — один `ambient`, `skyIrradiance`, точечные источники, проверка знака фазы; лечит «фиолетовый и ярче неба»; 2) мягкие частицы по копии глубины; 3) радиус 40 м и прогрев; 4) случайный поворот, размер и кадр на частицу (поля в `PfxState`); 5) ветер (`WorldWind`) и сопротивление в `PfxBucket::tick`; 6) `loadMaterialAlbedo` для текстур частиц (свой дым из `content/materials`); 7) сортировка по глубине внутри ведра, если градиенты по жизни дадут видимые артефакты порядка.
- До правок движка разумная настройка данными: `visAlphaFunc_S = "BLEND"`; тёмный `visTexColor*` (40–90 — учтите степень 2.2 и удвоенный ambient); медленный подъём (`velAvg` мал, `flyGravity_S` с небольшой положительной `y` и сносом по x/z); большой `visSizeEndScale`; `ppsScaleKeys` для клубов; эмиттер поднят над крышей; два-три наложенных своих определения с разными размерами и временами жизни против однообразия. Фиолетовый оттенок ночью этим не убрать.

### Gaps
- Ни один вывод не проверен кадром из игры в этой работе: кадры `vob-layer-day.png`/`vob-layer-night.png` из бенча Д2б ([docs/asset-migration.md:553](../../docs/asset-migration.md)) не открывались.
