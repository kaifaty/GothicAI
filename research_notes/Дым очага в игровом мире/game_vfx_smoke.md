# Дым из труб, очагов и костров в реальном времени: приёмы игровых VFX (2010–2026) и как не получить «невидимый» или «плотный блин» дым

Заметки исследователя. Цель — каталог приёмов, диапазонов параметров и практик приёмки, применимых к системе частиц движка уровня 2002 года (спрайты лицом к камере, размер/цвет/альфа от начала к концу жизни, анимированные текстуры, смешивание alpha или additive). Где приём требует поддержки шейдера или движка, это отмечено. Оценки без источника помечены «(оценка)».

Обозначения: «спрайт» = billboard-квад лицом к камере; «flipbook» = SubUV/атлас кадров; «эрозия» = alpha erosion.

---

## Две классические ошибки: дым не виден на фоне неба / дым читается как плотный блин или столб билбордов

### Takeaway
Обе ошибки — про **тон и плотность**, а не про количество частиц. Невидимость на небе — это слишком малая разница тона дыма и неба (светлый или аддитивный дым на светлом небе), плотный блин — это высокая альфа каждого спрайта, много перекрытий, одинаковые спрайты без эрозии и без роста. В одной и той же ветке о моде дыма для The Witcher 3 игроки называют обе ошибки: ванильный дым «был ли он всегда таким плохим», а усиленный мод — «слишком плотный, не растворяется в небе, как атомная станция».

### Cited Findings
- Цвет и тон дыма не принадлежат самому дыму, их задаёт свет. На тёмном фоне (свет падает сбоку/спереди) дым выглядит **голубым**: частицы рассеивают синий сильнее красного; на светлом небе тот же дым виден **на просвет** и выглядит **оранжевым/жёлтым** (синее рассеяно в стороны); дым, поднимающийся от тёмного леса в светлое небо, может «переходить» из одного цвета в другой. Крупные капли теряют это свойство, и дым выглядит белым — [Gurney, «What Color is Smoke?» (по Миннарту)](https://gurneyjourney.blogspot.com/2012/12/what-color-is-smoke.html)
- Источники по живописи/фото сходятся в правиле: дым темнее на ярком небе и светлее на тёмном или облачном фоне; дым против солнца светлее и прозрачнее (Леонардо: дым «между солнцем и глазом» светлее и прозрачнее любого другого в пейзаже) — [поисковая сводка; Da Vinci Notebooks](https://interconnected.org/more/davinci/570.html) (страница не открывалась, цитата из сниппета поиска)
- Unity в документации стандартных шейдеров частиц прямо относит режим **Subtractive** («затемняет фон») к пару и дыму, Additive — к свечению — [Unity Manual: Standard Particle Shaders](https://docs.unity.cn/Manual/shader-StandardParticleShaders.html)
- Ветка мода «Alternative Chimney Smoke» для The Witcher 3: автор сделал дым «punchy», поднял максимум частиц, заменил текстуры, убрал низкие LOD и увеличил дальность высшего LOD; 5 вариантов (sharper, smooth, mix, puffy, dark and oily). Комментарий к сравнению с ванилью: «was it always this bad?». Критика мода: дым выглядит неестественно, **не растворяется в небе**, слишком плотный — «Like a nuclear plant»; ещё один игрок: дым появился и над открытыми кострами для готовки, и дальние костры стали похожи на пожар; вопрос «можно ли случайную плотность» — [CDPR forums, Alternative Chimney Smoke](https://forums.cdprojektred.com/forum/en/the-witcher-series/the-witcher-3-wild-hunt/mod-discussions/8259220-mod-compilation-alternative-chimney-smoke)
- Типичные провалы читаемости эффектов: нет фокальной точки, передний и задний план сливаются в средних тонах, аддитивные слои складываются в «засветку» (luminance wash) — [Ground Zero, «Readable chaos»](https://studios-groundzero.pages.dev/journal/readable-chaos) (студийный блог, источник невысокого веса; по сниппету поиска)
- Слои частиц нужны для глубины («several layers of particles are often used to provide visual depth complexity»), но каждый слой — это overdraw; один спрайт может съесть «целый экран» перерисовки — [GPU Gems 3, гл. 23, High-Speed, Off-Screen Particles](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles)
- Unity о 6-way дыме: эффект остаётся плоским, его советуют для фоновых/декоративных дымов, пара, взрывов, а глубину добавлять объёмными эффектами — [Unity Blog, 6-way lighting](https://unity.com/blog/engine-platform/realistic-smoke-with-6-way-lighting-in-vfx-graph)

### Inferences
- Против «невидимости на небе»: дым должен отличаться от неба **по тону**, а не только по альфе. Днём на светлом небе дым, видимый на просвет, должен быть **темнее и теплее** неба (серо-бурый, а не белый); на фоне тёмного склона/леса — светлее и холоднее. Значит, один фиксированный цвет частиц подходит не ко всем фонам; запас дают тёмная составляющая у основания плюма и светлый край.
- Аддитивный дым на светлом небе исчезает по определению (он может только осветлять). Для дыма — обычное alpha-смешивание; аддитив — только для искр и свечения у самого огня. Затемняющий слой (Multiply/Subtractive) — способ сделать дым читаемым на ярком небе, если движок даёт такой режим (в ZenGin/OpenGothic есть режим MUL — проверить, как он реализован).
- Против «блина»: плотным должен быть только **узкий низ** плюма у устья трубы (первые 10–20% жизни); дальше альфа каждого спрайта мала, и плюм читается за счёт перекрытия 3–8 полупрозрачных спрайтов, а не за счёт одного непрозрачного (оценка). Плюм должен расширяться и бледнеть с высотой — тогда он «растворяется в небе», а не обрывается.
- Случайная плотность между трубами (разный ppsValue/альфа на разных домах) — дешёвый способ убрать «одинаковые столбы», о котором прямо просили игроки Witcher 3.
- Костёр/очаг под открытым небом и труба — разные эффекты: перенос «трубного» дыма на кухонные костры превращает их на расстоянии в пожар (жалоба из той же ветки).

### Gaps
- Не найдено ни одного источника с числовым критерием «минимальный контраст дыма и неба» (например, разница яркости в процентах); это остаётся настройкой на глаз.
- Страница Леонардо и сводка о «темнее на ярком небе» взяты из поисковой выдачи, полные тексты не открывались.

---

## Спрайт и текстура: flipbook, как его делают, эрозия, края, освещённые карты (normal / six-way)

### Takeaway
Стандарт индустрии — flipbook 8×8 (64 кадра) из симуляции Houdini/EmberGen/FumeFX; разрешение кадра при этом мало (256 px на атласе 2K), поэтому студии экономят кадры (motion vectors) или переиспользуют одну текстуру через перекраску и освещение. Жёсткие края дают альфа без плавного рампа 0→1 и режим Masked вместо Translucent. «Освещённые» дымы делают через six-way lightmaps, а не через normal maps: нормали для дыма выглядят плохо при смене времени суток.

### Cited Findings
- Skull & Bones (Ubisoft, Mederic, бывший VFX Tech Director, 2018): анимированные текстуры — **8×8 = 64 кадра на атласе 2K×2K (≈256×256 на кадр)**; маски освещения — 1K, 8×8 по **128×128** на кадр; кадры — рендеры pyro из **Houdini**; общий объём текстур всего набора — **8.06 МБ**. Мотивация: «анимированная текстура показывает 1/64 разрешения в каждый момент», поэтому в текстуру кладут не цвет и нормали, а **данные освещения**, а цвет задают маленькой рамп-текстурой — [Skull & Bones, Real Time VFX](https://realtimevfx.com/t/smoke-lighting-and-texture-re-usability-in-skull-bones/5339)
- Там же: альбедо-рамп — **16×16 DXT1**; художники меняют оттенки серого в Photoshop, и тон дыма меняется по всей игре разом, сохраняя единство цвета, когда в одном эффекте смешиваются несколько анимированных текстур; эмиссив-рамп 128×16 (BC7), канал R анимированной маски сдвигает баланс «дым/огонь» — [Skull & Bones](https://realtimevfx.com/t/smoke-lighting-and-texture-re-usability-in-skull-bones/5339)
- Атлас 2048² с 64 кадрами (8×8) даёт **256 px на кадр**; Houdini и EmberGen экспортируют flipbook с запечённым светом (emissive + прозрачность) для billboard'ов в UE5 — [Loronso, «Creating optimized, convincing explosions for UE5»](https://loronsodev.github.io/posts/explosion-fx)
- EmberGen экспортирует flipbook/спрайт-листы (EXR, PNG, TGA) и умеет зацикливать симуляции (параметры First/Second Interval, Static/Dynamic Blend) — [JangaFX EmberGen](https://jangafx.com/software/embergen); при этом был баг: в вьюпорте цикл бесшовный, а в экспортированном flipbook — нет (сдвиг на кадр) — [JangaFX forum](https://forums.jangafx.com/t/embergen-seamless-looping-not-working/544)
- Klemen Lozar, frame blending с motion vectors: пример — SubUV 8×8 (64 кадра ≈ **2 с при 30 fps**); с векторами движения эффект можно растянуть «**более чем в 10 раз** при той же воспринимаемой частоте кадров»; чем меньше кадров, тем сильнее нужна дисторсия. Векторы — из пасса скорости FumeFX (+ Twixtor для экранных векторов), текстура векторов уменьшена (512 при базовой 4096), **без сжатия и без sRGB**. Цена: две выборки SubUV, каждая с выборкой текстуры векторов, плюс lerp — «shader complexity and texture memory» — [Klemen Lozar, Frame Blending with Motion Vectors](https://klemenlozar.com/frame-blending-with-motion-vectors/)
- Ограничение motion-vector блендинга: большие изменения между соседними кадрами всё равно дают артефакты — [Real Time VFX, Motion Vectors thread](https://realtimevfx.com/t/motion-vectors-for-animated-textures-how-do-you-generate-them/575?page=2); коммерческий генератор векторов TFlow (Unity/Unreal) — [TFlow](https://realtimevfx.com/t/tflow-motion-vector-generator/18214)
- Unity: режим flipbook **Simple** показывает кадры по одному, **Blended** смешивает соседние кадры для плавности — [Unity Standard Particle Shaders](https://docs.unity.cn/Manual/shader-StandardParticleShaders.html)
- **Alpha erosion** (Black Point Fade, Alpha Dissolve, Alpha Min/Max Scale) — «масштабирование уровней альфа-текстуры, чтобы альфа выглядела выедаемой» — [Realtime VFX Dictionary](https://www.tech-artists.org/t/the-realtime-vfx-dictionary/3417)
- Почему эрозия даёт жёсткий край: эрозия «вышла за диапазон» на сплошном белом плато альфы; лечение — более ровный рамп 0→1 в альфе; более мягкая альфа уменьшила рваные края — [EStampes sketch, Real Time VFX](https://realtimevfx.com/t/estampes-sketch-15-wip/5388)
- Жёсткая чёрная кромка flipbook в UE: вероятная причина — материал в режиме **Masked**, а не Translucent (бинарный срез) — [Unreal forums](https://forums.unrealengine.com/t/hard-edge-in-smoke-explosion-flipbook/589509)
- Ступенчатая альфа из наложенных спрайтов с убывающей альфой + гауссово размытие и ручное смягчение краёв — [Overwatch Pharah rocket study, Real Time VFX](https://realtimevfx.com/t/overwatch-fx-pharah-rocket-study/758)
- Six-way lighting (Unity, Mathieu Muller, 20.01.2023): шесть карт освещения (сверху, снизу, слева, справа, спереди, сзади относительно камеры), упакованы в две RGBA-текстуры; альфа текстуры A — прозрачность, альфа B свободна (маска эмиссива). Дают внутренние тени и **rim light при контровом свете**. Нормали и полностью запечённый цвет, по Unity, проигрывают в реализме или гибкости. Стоимость «сравнима с обычным освещённым спрайтом». Источники карт: HDA для Houdini (VFXToolbox), аддон Blender, опция **«Six point»** в EmberGen (официально не поддержана — интенсивность света не контролируется, нужна ручная правка) — [Unity Blog](https://unity.com/blog/engine-platform/realistic-smoke-with-6-way-lighting-in-vfx-graph)
- Six-way в Skull & Bones: в маске хранятся только верх/лево/право/низ (касательные к камере), перед и зад **аппроксимируются в шейдере**; упаковать верх и низ в один канал нельзя — это тени, а не нормали, тень с одной стороны не предсказывает другую — [Skull & Bones](https://realtimevfx.com/t/smoke-lighting-and-texture-re-usability-in-skull-bones/5339)
- Unity six-way сейчас работает и в URP, и в HDRP; учитывает light probes, APV, ambient probe и динамические источники; дым считается чисто диффузным (reflection probes не влияют) — [Unity docs, Six-way lighting](https://docs.unity3d.com/Packages/com.unity.visualeffectgraph@17.6/manual/six-way-lighting.html)
- В Killzone Shadow Fall дым освещали «с нескольких направлений» и собирали результат в текстуры, чтобы получить объём — [пересказ слайдов во вторичном блоге](https://fxrnd.tistory.com/32) (вторичный источник); Houdini использовался для текстур дыма и пыли — [SideFX, Killzone Shadow Fall](https://www.sidefx.com/community/killzone-shadowfall/)

### Inferences
- Для трубного дыма в движке без блендинга кадров flipbook — не обязательный путь: кадр flipbook без блендинга «щёлкает» при малом FPS анимации. Дешевле 2–4 **статичных** спрайта-вариантов мягкого неправильного облачка + случайный поворот + рост размера (оценка). Flipbook имеет смысл, если в нём заложена **эрозия** (кадры постепенно «выедаются»), т. е. анимированная текстура проигрывается один раз за жизнь частицы.
- Если движок умеет только линейную альфу start→end, огибающую «нарастание → плато → затухание» и эрозию можно запечь в непрерывающийся flipbook (первые кадры бледные, последние — рваные и тонкие), синхронизировав его длительность с жизнью частицы (оценка).
- Края: альфа текстуры должна доходить до 0 **задолго** до края квада (запас 10–15% по краю) и не иметь сплошного белого плато; силуэт облачка неправильный, без круга (оценка, согласуется с находками про плато и Masked).
- Six-way/«освещённые» карты требуют шейдера (смешение карт по направлению света); для движка 2002 года это доработка шейдера частиц. Промежуточный вариант без шейдера — запечь в текстуру мягкий «верхний свет» (светлее сверху, темнее снизу) и тонировать цветом частицы.

### Gaps
- Число кадров для именно трубного (медленного) дыма в шипнутых играх не найдено; данные есть только для взрывов/столбов дыма (64 кадра).
- Не найдено описаний, как дым из труб текстурируют в Witcher 3, RDR2, KCD; в ветке Witcher 3 сказано лишь, что мод меняет текстуры и число частиц.

---

## Настройки частиц для тонкого читаемого плюма: эмиссия, жизнь, размер, альфа, скорость, разброс, вращение, слои

### Takeaway
Публичные «рецепты» дают порядок величин: 25 частиц/с при жизни 3–5 с (≈100 видимых спрайтов) — это уже «тёмный дым» среднего масштаба; для тонкого трубного плюма нужно меньше и бледнее. Рост размера 2–3× за жизнь, альфа «импульсом» (нарастание и спад), горизонтальная компонента начальной скорости больше вертикальной для сносимого дыма. Порядок отрисовки «старые поверх молодых» и разбиение на 2–3 кластера улучшают столб.

### Cited Findings
- Epic, «тёмный дым» в Niagara: Spawn Rate **25**; Lifetime **3.0–5.0 с** (random); Sprite Size **50–90** (ед. UE = см); Add Velocity X **12–32**, Y 0, Z **5–7** (см/с); Acceleration Force Z **20**; Sphere Location радиус **30**; альфа — кривая **Pulse** (рост и спад), цвет за жизнь не меняется; материал M_smoke_subUV из Starter Content — [Epic Docs, Dark Smoke in Niagara](https://dev.epicgames.com/documentation/unreal-engine/how-to-create-a-dark-smoke-effect-in-niagara-for-unreal-engine)
- Видимое число частиц ≈ spawn rate × lifetime; дым начинают скромного размера и растят примерно до **2–3×** за жизнь при медленной скорости вверх — [Ray's Unreal lessons](https://rays-unreal.netlify.app/m09_l02_creating_particles) (учебный конспект, вес невысокий)
- Пар над едой: неравномерная кривая масштаба за жизнь + drag для «подъём и расползание» — [Unreal forums](https://forums.unrealengine.com/t/how-to-make-a-niagara-sim-to-simulate-heat-coming-from-fresh-food/620400)
- Unity Learn, дым: Start Lifetime **4**, Start Speed **1**, Start Size **10**, светло-серый Start Color — [Unity Learn, Lesson 7.1](https://learn.unity.com/tutorial/lesson-7-1-creating-visual-effects); другой урок: Duration 10, Start Speed ≈ **2**, Noise Strength ≈ **0.1** — [Unity Learn, Creating Smoke](https://learn.unity.com/tutorial/challenge-creating-smoke-with-particle-systems)
- Сортировка столба дыма: автор Skull & Bones избегает Z-сортировки, предпочитает **старые поверх молодых** для столбов дыма и советует разбивать эмиттер на **2–3 меньших кластера** — [Skull & Bones](https://realtimevfx.com/t/smoke-lighting-and-texture-re-usability-in-skull-bones/5339)
- Source Engine (2004) имеет готовую сущность «смоукстек» с базовым разбросом, скоростью расширения, скоростью, начальным/конечным размером, частотой, длиной струи, **twist** и **углом/скоростью ветра** — [Valve Developer Wiki, WiseSmokeStack](https://developer.valvesoftware.com/wiki/WiseSmokeStack) (страница закрыта анти-бот защитой; состав параметров по сниппету поиска)
- В Witcher 3 моддер поднимал **максимум частиц** и дальность высшего LOD, чтобы дым читался; игроки сочли результат слишком плотным — [CDPR forums](https://forums.cdprojektred.com/forum/en/the-witcher-series/the-witcher-3-wild-hunt/mod-discussions/8259220-mod-compilation-alternative-chimney-smoke)
- OpenGothic (локальный код проекта): альфа частицы — **линейно** от `visAlphaStart` к `visAlphaEnd`, но с принудительными рампами нарастания и затухания на первых и последних **10%** жизни (комментарий: в ванильной игре частицы «даже с резким переходом» выглядят с фейдом); размер — линейно от 1 до `visSizeEndScale`; цвет — линейно `visTexColorStart`→`visTexColorEnd`; жизнь — `lspPartAvg ± lspPartVar` — `OpenGothic/common/graphics/pfx/pfxbucket.cpp`, строки 275, 609–636 (локальный файл репозитория)

### Inferences
Стартовые диапазоны для **трубы деревенского дома** (все — оценка, требуют подбора в игре; масштаб в метрах):
- Жизнь: **5–9 с** (длиннее, чем у «тёмного дыма» Epic, чтобы плюм вытягивался на 4–8 м и растворялся, а не обрывался).
- Эмиссия: **3–8 частиц/с** → **20–60 видимых спрайтов**; для слабого очага — нижняя граница.
- Размер: старт **0.4–0.8 м** (чуть шире устья трубы), конец **×3–5**; рост быстрее в начале (если движок умеет только линейно — компенсировать начальной скоростью разлёта).
- Альфа одного спрайта: пик **0.15–0.35** для тонкого плюма; нарастание за первые 10–20% жизни, затем плавный спад до 0. Плотность плюма — из перекрытия, а не из альфы.
- Скорость: вверх **0.5–1.5 м/с** с затуханием (drag) или со слабым ускорением вверх; разброс направления **5–15°** у устья.
- Вращение: случайный стартовый угол 0–360°, медленное вращение ±5–20°/с, чтобы не повторялся силуэт (если движок поддерживает поворот спрайта).
- Слои: (1) плотное «ядро» у устья — короткая жизнь, меньше размер, выше альфа, чуть темнее; (2) тонкие широкие «завитки» — длинная жизнь, большой рост, низкая альфа. Два эмиттера дешевле, чем один с высокой плотностью.
- Для OpenGothic/ZenGin: встроенные 10%-рампы уже дают fade-in/out; чтобы получить «Pulse» (рост и спад), достаточно `visAlphaStart` ≈ пику и `visAlphaEnd` = 0; рост размера — через `visSizeEndScale` 3–5; оттенок «теплее у основания, холоднее/светлее наверху» — через `visTexColorStart/End`.

### Gaps
- Не найдено опубликованных параметров трубного дыма из шипнутых игр (Witcher 3, RDR2, KCD, Skyrim): мод-страницы Nexus закрыты для автоматического чтения (403), а в ветке Witcher 3 числа не приводятся.
- Значения по умолчанию у Source `env_smokestack` проверить не удалось (анти-бот защита Valve Developer Wiki).

---

## Soft particles / depth fade: почему дым «срезан» крышей или стеной и как это лечат

### Takeaway
Жёсткая линия появляется там, где плоский спрайт пересекает непрозрачную геометрию: аппаратный z-тест бинарен. Лечение — в пиксельном шейдере сравнить глубину частицы с глубиной сцены и гасить альфу на переходе (`saturate(scale * (sceneDepth − particleDepth))`), лучше по нелинейной кривой. Это требует доступа к буферу глубины из шейдера частиц; без него — обход расстановкой и размерами.

### Cited Findings
- NVIDIA, «Soft Particles» (Tristan Lorach, 2007): нативный z-тест — «бинарный, всё или ничего», поэтому мягкого перехода не даёт; предложены два способа — читать буфер глубины как текстуру (DX10) или писать глубину во вторую цель рендера (DX9); вместо линейного фейда предлагается **нелинейная контрастная кривая**, у линейного на иллюстрациях всё ещё видны края — [NVIDIA whitepaper](https://developer.download.nvidia.com/whitepapers/2007/SDK10/SoftParticles_hi.pdf) (по сводке поиска; PDF не распознан полностью)
- Формула фейда: `saturate(scale * (myDepth − sceneDepth))` гасит альфу у пересечений и убирает резкие края; там же техника рендера частиц в буфер 2×/4× меньшего разрешения против overdraw (8800 GTX, 1600×1200: 25 fps в полном разрешении → 51 fps mixed 4×4 → 61 fps low 4×4) — [GPU Gems 3, гл. 23](https://developer.nvidia.com/gpugems/gpugems3/part-iv-image-effects/chapter-23-high-speed-screen-particles)
- Частицы не пишут глубину, поэтому могут читать первый непрозрачный пиксель за собой — [vfxdoc, fading](https://vfxdoc.readthedocs.io/en/latest/shaders/fading/) (по сниппету)
- Синонимы: Depth Fading Particles / Soft Particles / Z-Feathered Particles; **Distance Fade** — гашение альфы по расстоянию до камеры, чтобы частицы не «резали» экран — [Realtime VFX Dictionary](https://www.tech-artists.org/t/the-realtime-vfx-dictionary/3417)
- Unity: Soft Particles гасят частицы у поверхностей в буфере глубины, чтобы не было жёстких краёв; **Camera Fading** с Near/Far — гашение у камеры — [Unity Standard Particle Shaders](https://docs.unity.cn/Manual/shader-StandardParticleShaders.html); в URP для soft particles в Shader Graph нужно включить Depth Texture в ассете пайплайна (отчёт пользователя) — [Unity Discussions](https://discussions.unity.com/t/urp-shader-graph-soft-particles/1534206)

### Inferences
- Нужна поддержка движка: шейдер частиц должен читать глубину сцены (в отложенном/forward+ рендере она обычно уже есть). Параметр «толщины» фейда для дыма у крыши — порядка **0.3–1 м** (оценка); слишком большой делает дым прозрачным у любой стены.
- Обход без soft particles (движок 2002 года): ставить эмиттер **выше среза трубы** на 0.2–0.5 м; держать стартовый размер меньше ширины крыши вокруг устья; не давать частицам опускаться (без отрицательной гравитации у дыма); ветровой снос направлять так, чтобы плюм не ложился на конёк крыши; использовать z-bias/смещение к камере осторожно (в OpenGothic у описания частиц есть флаг `visZBias` — `OpenGothic/common/graphics/pfx/particlefx.h`; что именно он делает, не проверялось).
- Также нужен **camera fade**: игрок, проходящий через плюм костра, иначе видит «стену» спрайта перед лицом.

### Gaps
- Полный текст и код NVIDIA-статьи не извлечены (PDF не распознан инструментом); формула кривой контраста не процитирована.
- Поддерживает ли шейдер частиц OpenGothic depth fade, не проверялось (это область исследования движка).

---

## Освещение дыма: unlit, ambient, sun-lit; почему дым светится или окрашивается небом ночью; как держат день и ночь

### Takeaway
Unlit-дым, настроенный под солнечный день, ночью либо светится, либо (в физически корректном движке) становится «смоляным»; универсальной модели нет. Рабочие приёмы, от дешёвых к дорогим: (1) глобальный параметр «время суток» (яркость + цвет), которым тонируют все unlit-эффекты (RIME); (2) тонирование при рождении частицы светом в точке эмиттера (Source); (3) повершинное освещение «солнце + ambient» (Skull & Bones); (4) six-way/полное освещение частиц (Unity HDRP/URP, Killzone). Ошибка «дым светится ночью» — это emissive/unlit или слишком сильный ambient, а «дым красный/синий ночью» — тинт неба/туманa, умноженный на неподходящий цвет частицы.

### Cited Findings
- RIME (Tequila Works, со слов Simon Trümpler): общий набор материал-параметров с «time of day» (яркость) и «day color» (оранжевый утром и вечером, голубоватый днём); им тонировали **unlit/emissive** эффекты, смена значений обновляла всё сразу без кода — [Real Time VFX, simonschreibt](https://realtimevfx.com/t/klemen-lozar-smoke-help/7718/15); механизм в UE — Material Parameter Collections, общий параметр для всех материалов, пример с имитацией смены времени суток — [Unreal Engine blog, MPC](https://www.unrealengine.com/en-US/blog/material-parameter-collections)
- Ветка «Opinions about particle lighting in modern PBR engines»: дым, выглядящий хорошо днём, ночью «pitch black»; фиксы (unlit-свечение, обратная компенсация eye adaptation, параметры для левел-артистов, ручная подстройка по сцене) названы хаками; «no one lighting model that can cover all situations» (cEssex, Star Citizen); освещение частиц часто «не учитывает просвечивание вообще» (Leemon, хвалит DICE Battlefront); VFX-художники часто проверяют эффект в превью или при одном освещении — [Real Time VFX](https://realtimevfx.com/t/opinions-about-particle-lighting-in-modern-pbr-engines/29454)
- Там же (TheVman): Source Engine берёт **значение света в одной точке — начале системы частиц — при рождении** и тонирует частицы; минус — частица сохраняет тинт, если улетает в тёмную зону; плюс — почти поведение lit-частиц по цене unlit — [Real Time VFX](https://realtimevfx.com/t/opinions-about-particle-lighting-in-modern-pbr-engines/29454)
- Skull & Bones: освещение считается **в вершинном шейдере**: `Lighting = lightColor * lightmap + ambientColor`; поддержаны **солнце + 1 точечный источник**; цель — разное время суток, «где normal maps выглядят плохо» — [Skull & Bones](https://realtimevfx.com/t/smoke-lighting-and-texture-re-usability-in-skull-bones/5339)
- Unity 6-way: днём — свет солнца и неба сквозь дым; ночью — статические/движущиеся точечные и прожекторные источники; ambient/probe освещение поддержано, режим **ambient-only** — самый дешёвый (без light loop); probes на квадe считаются только в 4 углах (артефакты у стен, на улице обычно не проблема) — [Unity Blog](https://unity.com/blog/engine-platform/realistic-smoke-with-6-way-lighting-in-vfx-graph)
- Unity: «Standard Particles Surface» — PBR-освещение без лайтмапов; «Unlit» — быстрее, освещения нет — [Unity Standard Particle Shaders](https://docs.unity.cn/Manual/shader-StandardParticleShaders.html)
- Killzone Shadow Fall (GDC 2014, Valient): частицы освещаются в общем проходе освещения через несколько опорных точек на частицу, переиспользуя карты теней; прозрачность может управляться освещением (туман, пыль) — [GDC2014 Killzone Graphics, slides](https://www.guerrilla-games.com/media/News/Files/GDC2014_Valient_Killzone_Graphics.pdf) (по сводке поиска)
- inFAMOUS Second Son (Sucker Punch, GDC 2014, Matt Vainio): дым реагирует на тени и освещение, команда следила, чтобы частицы имели правильную экспозицию в разное время суток; curl noise в реальном времени — [fxguide](https://www.fxguide.com/fxfeatured/art-directing-effects-for-infamous-second-son/); [GDC Vault](https://gdcvault.com/play/1020158/The-Visual-Effects-of-inFAMOUS)
- Ошибки ночью у других: дым и взрывы в UE «странно краснеют» ночью при тусклом синеватом свете — [Unreal forums](https://forums.unrealengine.com/t/my-smoke-explosion-particles-turn-strangely-red-during-my-night-cycle/2435056); в Arma дым и пыль пересвечены цветом на закате/рассвете — [BI feedback tracker](https://feedback.bistudio.com/T77341) (по сниппету); Skyrim: частицы пыли/тумана «светятся» с модом тёмных ночей — [Nexus forums](https://forums.nexusmods.com/topic/8777738-help-making-particles-not-glow-when-using-true-nights); яркость дыма в Skyrim с ENB регулируют интенсивностью частиц ENB — [Nexus, wind smoke mod](https://nexusmods.com/skyrimspecialedition/mods/78495) (по описанию в выдаче)
- Glow/bloom: спрайтовое свечение до bloom, чтобы bloom не пересвечивал; TAA/TSR/DLSS дают ghosting на VFX — [Real Time VFX](https://realtimevfx.com/t/opinions-about-particle-lighting-in-modern-pbr-engines/29454)
- Премультиплицированная альфа: одним режимом `ONE, ONE_MINUS_SRC_ALPHA` рисуются и огонь (α=0 → аддитив), и дым (RGB×α, α = прозрачность) — [JVM-Gaming](https://jvm-gaming.org/t/pre-multiplied-alpha-for-drawing-particles-in-one-pass/39549); [Wikipedia, Alpha compositing](https://en.wikipedia.org/wiki/Alpha_compositing)

### Inferences
- Для движка 2002 года без освещения частиц минимальный правильный набор — **глобальный множитель цвета частиц по времени суток** (как RIME): днём ~1.0 и нейтрально-тёплый, в сумерках — оранжевый сдвиг, ночью — сильное затемнение (условно 0.15–0.3 от дневной яркости) и холодный сдвиг (оценка). Требует одной uniform-переменной в шейдере частиц; эмиттеры менять не нужно.
- Следующий шаг — «тинт при рождении» от освещения в точке эмиттера (ambient + солнце/луна + ближайший огонь) — дёшево и учитывает и время суток, и тень от скал/домов. Для трубы, которая не двигается, недостаток метода (частица уносит тинт) почти не виден.
- Ночью дым над домом видим только как **тёмный силуэт на чуть более светлом небе** или в свете огня/луны; поэтому ночью не делать его светлее неба. Нижнюю часть плюма над очагом/костром можно подсвечивать тёплым цветом снизу (огонь), а верх оставлять тёмным — это и есть «lit by fire» без системы освещения частиц (оценка).
- «Дым окрашивается небом» в движке с туманом/атмосферой обычно значит, что к частице применяется туман дистанции или sky-ambient, рассчитанный для непрозрачной геометрии; при прозрачной частице с низкой альфой он доминирует. Проверять отдельно для ночи (оценка, источники лишь описывают симптом).
- Вариант «прятать дым ночью» (снижать ppsValue/альфу) упоминается в практике как допустимый арт-выбор; Escape from Tarkov показывает обратный приём (вспышки видны в основном ночью) — физически корректная видимость зависит от фона.

### Gaps
- Не найдено описания, как именно решены день/ночь для дыма из труб в Witcher 3, KCD, RDR2 (публичных докладов на эту тему не нашлось).
- Как OpenGothic освещает частицы (unlit / ambient / ночной ambient), не проверялось; в `shader/materials/main.frag` есть `diffuseLight()` с `scene.ambient` и `NightAmbient`, но применяется ли он к частицам, не установлено.

---

## Ветер: снос дыма из трубы, когда в системе частиц нет ветра

### Takeaway
Без глобального ветра снос делают **на уровне эмиттера**: постоянное боковое ускорение (вектор «гравитации» с горизонтальной компонентой), наклон направления эмиссии или горизонтальная компонента начальной скорости; разнообразие даёт мягкий низкочастотный шум (curl noise), если он есть. В Source и Skyrim ветер для дымов был свойством самой системы частиц (угол/скорость ветра, модификатор силы ветра), а моды Skyrim позже подключили дым к погодному ветру.

### Cited Findings
- Epic «тёмный дым»: горизонтальная начальная скорость X **12–32** больше вертикальной Z **5–7**, плюс ускорение вверх Z 20 — плюм заваливается в сторону без отдельного модуля ветра — [Epic Docs](https://dev.epicgames.com/documentation/unreal-engine/how-to-create-a-dark-smoke-effect-in-niagara-for-unreal-engine)
- Source smokestack: параметры угла и скорости ветра и twist (закрутка) прямо в сущности — [Valve Developer Wiki, WiseSmokeStack](https://developer.valvesoftware.com/wiki/WiseSmokeStack) (по сниппету)
- Skyrim: мод делает дым из труб, костров, жаровен зависимым от **направления и силы ветра**, а значит и от погоды — [Nexus SSE 78495](https://nexusmods.com/skyrimspecialedition/mods/78495) (по описанию в выдаче); SKSE-мод **Particle Wind** применяет ветер ко всем частицам, глобальная сила `fDefaultStrength` в ini, переопределения по системам (шаблоны вроде `*Smoke*`) в JSON — [Nexus SSE 174812](https://www.nexusmods.com/skyrimspecialedition/mods/174812); тот же мод влияет на огонь, факелы, свечи, пар, брызги водопадов — [PC Gamer](https://www.pcgamer.com/games/the-elder-scrolls/make-skyrims-weather-feel-more-impactful-with-this-particle-mod)
- Curl noise (Bridson, SIGGRAPH 2007): шум в потенциальном поле, ротор которого — бездивергентная скорость, поэтому частицы не скапливаются; для дыма нужна отдельная компонента всплытия вверх — [The Little Grasshopper (Philip Rideout)](https://prideout.net/blog/old/blog/index.html@p=63.html)
- Unity Noise module (на curl noise): сильный высокочастотный шум — для искр, **мягкий низкочастотный — для дыма** — [Unity Manual, Noise module](https://docs.unity3d.com/es/2020.2/Manual/PartSysNoiseModule.html)
- Curl noise — очень сильная сила, при сочетании с ветром силу ветра приходится заметно поднимать — [Autodesk Softimage, CurlNoise](https://download.autodesk.com/global/docs/softimage2014/en_us/files/iceref_CurlNoise_Framework.htm); влияние шумом прямо на позицию может провести частицы сквозь поверхности — [Smode, Turbulence](https://www.smode.io/documentation/ref/compo/particles/field/turbulence.htm)
- inFAMOUS Second Son использовал генерацию curl noise в реальном времени для частиц — [fxguide](https://www.fxguide.com/fxfeatured/art-directing-effects-for-infamous-second-son/)
- OpenGothic: вектор `flyGravity` (Vec3) каждый тик прибавляется к скорости частицы — `ps.dir += decl.flyGravity*dtF` — `OpenGothic/common/graphics/pfx/pfxbucket.cpp`, строка 461 (локальный файл)

### Inferences
- В ZenGin/OpenGothic снос дыма можно сделать без новой системы: `flyGravity` с небольшой **горизонтальной** компонентой (постоянный ветер, плюм изгибается тем сильнее, чем выше) и небольшой положительной вертикальной (всплытие); плюс наклон `dirAngleElev/dirAngleHead` для начального направления (оценка; единицы — см/мс², проверить на месте).
- Чтобы трубы в одной деревне не выглядели «по линейке»: одно направление ветра на весь мир (иначе соседние дымы разлетаются в разные стороны — сразу заметно), но разная сила и разброс по эмиттерам ±20–30% (оценка).
- Если движок даст глобальный ветер (как Particle Wind в Skyrim) — проще одна глобальная переменная направления/силы, которую шейдер или симуляция частиц добавляют к скорости, с множителем на эмиттер.
- Без шума плюм — «ровная трубка»; дешёвая замена curl noise — случайный разброс начальной скорости + разная жизнь + вращение спрайтов (оценка).

### Gaps
- Значения силы ветра для дыма в Skyrim (`BSWindModifier`, «Strength» в NIF) упоминались в выдаче, но страница-источник не определена и не открыта.
- Числа сноса (м/с) для трубного дыма в шипнутых играх не найдены.

---

## Примеры шипнутых игр и их разборы

### Takeaway
Глубоких публичных разборов именно трубного дыма у Witcher 3, KCD, RDR2, Skyrim, Morrowind и Gothic Remake/Elex найти не удалось; полезные первичные источники — доклады и посты о дыме в целом (Skull & Bones, Unity 6-way, inFAMOUS, Killzone) и обсуждения модов, где игроки описывают, что не так с ванильным дымом.

### Cited Findings
- **The Witcher 3**: мод Alternative Chimney Smoke — новые текстуры, больше частиц, убраны низкие LOD, увеличена дальность высшего LOD; меняет только файлы частиц и текстур; отзывы — см. раздел о двух ошибках — [CDPR forums](https://forums.cdprojektred.com/forum/en/the-witcher-series/the-witcher-3-wild-hunt/mod-discussions/8259220-mod-compilation-alternative-chimney-smoke)
- **Kingdom Come: Deliverance II**: игроки сообщают о «застывшем» дыме из труб («it's just chimney smoke. It doesn't move»), настройка частиц не помогала — [Steam discussions](https://steamcommunity.com/app/1771300/discussions/0/669453270933137499) (баг-репорт, не официальный источник)
- **Skyrim**: моды подключают дым к ветру и погоде; яркость с ENB — через интенсивность частиц — [Nexus SSE 78495](https://nexusmods.com/skyrimspecialedition/mods/78495), [Particle Wind](https://www.nexusmods.com/skyrimspecialedition/mods/174812)
- **Morrowind / MGE XE**: в ветке о дыме Красной Горы отмечено, что анимированные текстуры (Ghost Fence) анимируются вблизи, но в дальнем рендере MGE XE становятся **статичными**, поэтому реалистичный компромисс — статичный шлейф пепла — [Nexus forums, A Smoking Red Mountain](https://forums.nexusmods.com/topic/2608604-a-smoking-red-mountain)
- **Skull & Bones** (дым и взрывы на море): 64-кадровые flipbook + маски освещения, повершинный свет солнце + 1 точечный, рамп-перекраска, «старые поверх молодых» — [Real Time VFX](https://realtimevfx.com/t/smoke-lighting-and-texture-re-usability-in-skull-bones/5339)
- **inFAMOUS Second Son**: дым реагирует на тени и свет, экспозиция по времени суток, curl noise; один рывок-дым порождает больше половины лимита частиц inFAMOUS 2 — [fxguide](https://www.fxguide.com/fxfeatured/art-directing-effects-for-infamous-second-son/) (последнее — по сниппету)
- **Killzone Shadow Fall**: освещение частиц в общем проходе, переиспользование теней — [GDC 2014 slides](https://www.guerrilla-games.com/media/News/Files/GDC2014_Valient_Killzone_Graphics.pdf)
- **RDR2**: найден только общий разбор VFX (облака в реальном времени по погоде и времени суток, туман над водой) без дыма из труб — [DTF](https://dtf.ru/id17412/62607-kak-v-red-dead-redemption-2-ustroeny-vizualnye-effekty)
- **RIME**: глобальный тинт времени суток для unlit-эффектов — [Real Time VFX](https://realtimevfx.com/t/klemen-lozar-smoke-help/7718/15)

### Inferences
- Повторяющийся сюжет в модах — **LOD и дальность**: дым из труб важен именно на дальних планах деревни, и его выключение/истончение на дистанции делает деревню «неживой»; но на дальних планах он же сильнее всего рискует стать «пожаром». Для дальних планов нужен отдельный режим (меньше частиц, крупнее спрайты, та же общая плотность) (оценка).
- Застывший дым (KCD II) игроки замечают сразу — движение важнее детализации.

### Gaps
- Не найдено материалов по дыму в Gothic Remake (THQ Nordic/Alkimia) и Elex (Piranha Bytes).
- Не найдено GDC/80.lv/ArtStation-разборов именно трубного дыма у RDR2, Witcher 3, KCD.
- Страницы модов Nexus (Witcher 3 #2751 и др.) недоступны автоматическому чтению (HTTP 403), описания взяты из сниппетов поиска.

---

## Как VFX-лиды принимают дым: в движении или по кадрам, критерии ревью

### Takeaway
Дым ревьюят **в движении и в контексте**: тайминг и ощущение силы, единая сила движения всех частей, отсутствие засветки и слияния с фоном, растворение в небе, проверка при разных условиях освещения. Частая ошибка — проверка только в превью-окне или при одном освещении. Формальных студийных чек-листов в открытом доступе нет; критерии собраны из публичных разборов и форумных ревью.

### Cited Findings
- VFX-художники часто тестируют в превью или при одном освещении и не проверяют другие сценарии — [Real Time VFX, particle lighting](https://realtimevfx.com/t/opinions-about-particle-lighting-in-modern-pbr-engines/29454)
- Главная правка в ревью «smoke ball»: работать над таймингом — в текущей версии ничто не передаёт силу — [Real Time VFX, Smoke ball](https://realtimevfx.com/t/smoke-ball-vfx/14850) (по сниппету)
- Simon Trümpler переделал дымовой эффект так, что «всё движется с одной силой», и приглушил внутреннее свечение, чтобы выделить яркое ядро; ранний прототип был отброшен как слишком шумный — [Real Time VFX, Simon Trümpler sketchbook](https://realtimevfx.com/t/simon-trumpler-vfx-sketchbook/4177?page=12) (по сниппету)
- Дым «оторван» от скорости пламени — повод для ещё одного прохода по движению — [Polycount](https://polycount.com/discussion/comment/1705575) (по сниппету)
- Критерии игроков к трубному дыму: растворяется ли в небе, не слишком ли плотный, нет ли одинаковой плотности у всех, не превращаются ли костры в пожар на расстоянии — [CDPR forums](https://forums.cdprojektred.com/forum/en/the-witcher-series/the-witcher-3-wild-hunt/mod-discussions/8259220-mod-compilation-alternative-chimney-smoke); двигается ли дым вообще — [Steam, KCD II](https://steamcommunity.com/app/1771300/discussions/0/669453270933137499)
- Провалы читаемости: нет фокальной точки, слияние переднего и заднего плана в средних тонах, засветка от сложения аддитивов — [Ground Zero, Readable chaos](https://studios-groundzero.pages.dev/journal/readable-chaos) (низкий вес)
- Постобработка и TAA могут менять вид VFX (ghosting, bloom) — значит, смотреть надо в финальной картинке игры, а не в редакторе — [Real Time VFX](https://realtimevfx.com/t/opinions-about-particle-lighting-in-modern-pbr-engines/29454)

### Inferences
Практический протокол приёмки трубного дыма для небольшой команды (оценка, собран из находок выше):
1. **Клипы, а не кадры**: 8–15 с видео с каждой точки — дым оценивается по движению (растворение, ритм, нет ли «пульса» от рождения частиц). Пара кадров «было/стало» — только для тона и плотности.
2. **Матрица условий**: полдень на светлом небе; пасмурно; закат против солнца (контровой свет); ночь при луне; ночь у костра. Дым должен читаться во всех, кроме, по выбору, ночи без источников.
3. **Матрица фонов**: на небе, на тёмном склоне/лесу, на соседнем доме (проверка «блина» и среза о крышу).
4. **Дистанции**: вплотную (camera fade, срез о крышу), 20–40 м (основной план), 100+ м (не пожар ли, не исчез ли при LOD).
5. **Критерии**: виден ли на небе без «белого пятна»; растворяется ли к концу, а не обрывается; виден ли отдельный спрайт/круглый край; одинаковы ли соседние трубы; движется ли весь плюм в одну сторону с одной силой; не «щёлкает» ли flipbook; нет ли засветки при наложении.
6. **Стоимость**: число живых частиц и overdraw на кадр в деревне с N трубами — до приёмки вида.

### Gaps
- Не найдено опубликованных студийных чек-листов ревью VFX-дыма; источники — форумные ревью и блоги.
- Не найдено данных, как именно в индустрии разделяют приёмку по видео и по кадрам; рекомендация «клипы для движения, кадры для тона» — собственный вывод.
