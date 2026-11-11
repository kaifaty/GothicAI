# Нестандартные оптимизации CPU и всего кадра (кроме SSAO и тумана)

Исследование 2026-11-07/08, только чтение кода и уже снятых данных; игра и сборки не запускались.

Опорные данные (вне git, читал только CSV и отчёты):
- **Ryzen**, Vulkan, 1728×1117, коммит `be62e3af`, сборка с профайлером: `/Volumes/DevSSD/Dev/projects/gothic/.claude/worktrees/engine-shader-usage-68d185/.bench/perf-20261107T203338Z-34985/` (рынок, лес, интерьер × 2 повтора).
- **Mac M1 Pro**, Metal, 100 %, тот же коммит, `OPENGOTHIC_GPU_PASSES=1`: `.../engine-shader-usage-68d185/.bench/perf-20261107T201323Z-25682/` и `perf-20261107T092943Z-22807/` (с ночной гаванью).
- Отчёты в git: [docs/performance.md](../../../../docs/performance.md), [anim-lod.md](../../../../reports/engine/2026-11-03-streaming-optimisation/anim-lod.md), [baseline.md](../../../../reports/engine/2026-11-03-streaming-optimisation/baseline.md), [прайс-лист шейдеров](../../../../reports/engine/2026-11-07-shader-optimisation/README.md).
- Мои скрипты разбора CSV лежат во временной папке сессии: `/private/tmp/claude-501/-Volumes-DevSSD-Dev-projects-gothic--claude-worktrees-affectionate-bhaskara-120f11/4957e2e2-1d15-442e-802b-35c51681f86b/scratchpad/` (`seg.py` раскладывает кадр по меткам `*_first_ns/*_last_ns`, `anim2.py` строит регрессию `parallel_animation` на сумму работы потоков, `gpu.py` сводит GPU-интервалы Metal).

Пути к коду ниже даны от корня репозитория (`OpenGothic/...`), строки — по `main` на `76606ebf`.

## Сводный рейтинг: что даст больше всего

### Takeaway
Два крупных выигрыша видно прямо по данным, и оба — **не вычисления, а организация кадра**. Первый: на Ryzen ~3,4 мс из 9,9 мс кадра (~35 %) — последовательный «хвост» внутри `parallel_animation`. Он не зависит от числа анимируемых NPC. Второй: на Mac в интерьере запись кадра ~12,5 мс синхронно ждёт GPU, и CPU с GPU идут строго по очереди. Сборочные флаги (PGO, ThinLTO, `-fno-semantic-interposition`) нигде в репозитории не включены — это третий по размеру и самый дешёвый пункт.

### Cited Findings
Рейтинг: выигрыш × уверенность / цена. Оценки мои, по данным ниже.

| # | Где | Что сейчас | Приём | Оценка выигрыша | На чём основано |
|---|---|---|---|---|---|
| 1 | `WorldObjects::updateAnimation`, [worldobjects.cpp:1043–1083](../../../../OpenGothic/common/world/worldobjects.cpp) | После параллельного прохода идут последовательные циклы по всем 1053 NPC: `updateMeleeShadow` (стр. 1049), `resolveMeleeContacts` (1061), `syncLogicalBodyState` (1081). Внутри `updateMeleeShadow` — две кучи-векторы и линейный `find_if` по общему вектору теневых тел под мьютексом, итого O(N²) | Перенести работу на NPC в параллельный проход, тот же поток, данные ещё в его кэше. Слот тела по индексу NPC вместо `find_if`. Тела считать только у NPC рядом с активным клинком. Векторы — переиспользуемые или кадровая арена | Ryzen −1,5…−3 мс (15–30 % CPU-кадра); Mac −1…−2 мс CPU | Регрессия по 3237 кадрам: `wall = 3,37 + 0,124·worker_sum`, r = 0,89 (рынок); код |
| 2 | Metal, окно между началом `Renderer::draw` и `InstanceStorage::commit` | В интерьере `storage_commit` начинается в среднем через 12,5 мс после `encode_main` (на улице через 0,55–0,63 мс). `fence_poll` = 0: ожидание переехало из забора внутрь записи кадра. Единственный путь Tempest Metal с синхронным ожиданием в этом окне — `MtBuffer::update` → `uploadBuffer(…, wait=true)` → `waitUntilCompleted` | Убрать синхронную выгрузку из кадра: буфер декалей PFX в `Upload`-куче на каждый `fId` или `updateAsync`. В сборке с профайлером — assert на `update()` приватного буфера в кадре | Mac, интерьер: кадр ≈ max(GPU 17,9; CPU ≈ 7,5) → −2 мс (−10 %) при 100 %; при 75 % того же порядка | Разбивка CSV по меткам; код Tempest |
| 3 | Сборка: [CMakeLists.txt](../../../../OpenGothic/CMakeLists.txt), [scripts/perf.sh:27](../../../../scripts/perf.sh), [scripts/ryzen-build.sh:4–7](../../../../scripts/ryzen-build.sh) | `RelWithDebInfo`, на Ryzen GCC `-O2 -g1`; ни LTO, ни PGO, ни `-march`; `CMAKE_POSITION_INDEPENDENT_CODE ON` (стр. 15) даёт `-fPIC` статическим библиотекам | ThinLTO + PGO (обучение на сценах `perf.sh`); на GCC — `-fno-semantic-interposition` или PIC только для разделяемых целей | 5–15 % CPU-кадра по чужим данным → Ryzen −0,5…−1,4 мс | Chromium, Python/Fedora, BOLT (ниже); механизм в нашем коде есть |
| 4 | Карты теней, [renderer.cpp:2306](../../../../OpenGothic/common/graphics/renderer.cpp), [camera.cpp:504–545](../../../../OpenGothic/common/camera.cpp) | Оба каскада перерисовываются каждый кадр. Проекция повёрнута по рысканью камеры (`view.rotate(rotation…)`, стр. 545) и не привязана к текселям | Дальний каскад #1 — через кадр, со своей матрицей. Затем статический кэш с мировой ориентацией и прокруткой, как у Insomniac | GPU Mac −0,5…−1 мс на рынке и в лесу (через кадр); кэш — больше | Вершинный интервал #1: 1,77 мс на рынке |
| 5 | `WorldObjects::tick`, [worldobjects.cpp:567–700](../../../../OpenGothic/common/world/worldobjects.cpp) | Каждый кадр тикают все 728 Simplified NPC (1,28 мс) и 325 Full (0,94 мс), последовательно; по `npcArr` ~13 полных проходов за кадр | Тик Simplified по кругу с бюджетом и накопленным dt; горячие поля NPC (позиция, политика, уровень, флаги) в SoA-массив | Ryzen −0,6…−1 мс | Зоны `npc_simplified_tick`/`npc_full_tick`; сутки `npc-day-parity` как приёмка |
| 6 | `WorldView::preFrameUpdate` и соседи, [worldview.cpp:590–611](../../../../OpenGothic/common/graphics/worldview.cpp) | До `storage_commit` 0,84 мс главного потока на Ryzen: CPU-частицы (`pfxGroup.tick` + `buildSsbo`), ветер (`updateInstance` на каждый объект из `std::unordered_set`), морфы, свет | Ветер — функция (позиция, время, параметры) — в вершинный шейдер; частицы — задачей параллельно с логикой; `unordered_set` → плотный вектор | Ryzen −0,3…−0,6 мс | Разбивка CSV; код |
| 7 | Ядро позы: [pose.cpp:539–590, 861–880](../../../../OpenGothic/common/graphics/mesh/pose.cpp), [animmath.cpp:9–40](../../../../OpenGothic/common/graphics/mesh/animmath.cpp), Tempest [matrix.cpp:229–270, 540–555](../../../../OpenGothic/lib/Tempest/Engine/math/matrix.cpp) | slerp с `acos` + 3 `sin` + 4 деления на кость; 4×4 умножение вне строки, в чужой статической библиотеке; «быстрая» ветка для аффинных матриц мертва из-за опечатки | nlerp, аффинное 4×3 в SIMD, починка ветки, частота обновления для видимых дальних (как в UE Animation Budget Allocator), позже ACL | По Амдалу мало: параллельная часть всего ~0,76 мс на Ryzen → −0,2…−0,4 мс | Регрессия; `anim-lod.md` |
| 8 | Распределитель памяти | Системный malloc; в горячих циклах есть аллокации на NPC за кадр | Опыт без кода: `LD_PRELOAD`/`DYLD_INSERT_LIBRARIES` с mimalloc | 0–3 %, после пункта 1 меньше | README mimalloc; код |
| 9 | `Workers`, [workers.h/.cpp](../../../../OpenGothic/common/utils/workers.cpp) | Не больше `ceil(N/128)−1` = 8 работников + главный поток; ожидание через mutex и condvar; QoS на Mac не задаётся | QoS user-interactive для работников на Mac; держать NPC на одном CCD/CCX на Ryzen; короткий спин перед сном | ≤0,1–0,3 мс | Вторичные источники по QoS и задержкам Zen 2 |
| 10 | Мелочи GPU | Sky LUT каждый кадр (фрагменты ~0,2 мс на рынке); ночью две пустые очистки карт теней; Bloom — 12 проходов (~0,65 мс фрагментов) | LUT неба раз в N кадров; не чистить пустые карты; однопроходный даунсэмплер | ≤0,1–0,3 мс | GPU-интервалы Mac |

Отброшено, потому что код или данные проблемы не показывают:
- **Параллельная запись команд, кэш команд между кадрами, обновления дескрипторов.** Рендер GPU-driven: indirect, bindless, отсечение на GPU. На Vulkan 69,3 графических команды на кадр (`draw_calls` в отчёте Ryzen). Время `encode_main` уходит в CPU-работу до записи, а не в вызовы API.
- **Вершины GBuffer и окклюдеры HiZ.** GBuffer: вершины 0,20–0,24 мс против фрагментов 1,47–2,38 мс. HiZ: вершины 0,08–0,09 мс, фрагменты 0,22–0,49 мс.
- **O(N²) во внимании NPC.** `Attention::tick` ограничен периодом и радиусом героя ([attention.cpp:108–118](../../../../OpenGothic/common/world/objects/attention.cpp)).
- **Пакетирование с радикс-сортировкой.** `DrawCommands::commit` сортирует только при изменении (`cmdDurtyBit`, [drawcommands.cpp](../../../../OpenGothic/common/graphics/drawcommands.cpp)).
- **Огромные страницы.** Признаков упора в TLB в данных нет.

### Inferences
- Порядок по Амдалу важнее, чем «ускорить ядро анимации». Параллельная часть анимации уже дешёвая (0,76 мс на Ryzen при 8 потоках). Ни SIMD, ни ACL кадр заметно не сдвинут, пока последовательный хвост 3,4 мс остаётся на главном потоке.
- Пункты 1 и 3 складываются только частично: LTO и PGO ускоряют и тот же хвост.
- На Mac выигрыши CPU видны в FPS только в интерьере (пункт 2) и там, где GPU станет короче CPU. Пункты 1, 5, 6 на Mac в основном укорачивают «попытку» кадра.
- Пункт 2 — единственный, который на Mac сразу даёт FPS: он снимает сериализацию CPU и GPU.

### Gaps
- Распределение 3,4 мс между `updateMeleeShadow`, `syncLogicalBodyState`, `pinAnimationLodTargets` и пробуждением потоков не измерено: у этих циклов нет своих зон профайлера. Первый шаг — четыре метки `FrameProfile::Scope`.
- Виновник ожидания в интерьере установлен по исключению. Его подтверждает одна метка вокруг `pfxGroup.preFrameUpdate` или Time Profiler с заблокированными стеками.

## Анимация: как считаются скелеты, что можно SoA/SIMD/ACL/LOD, кто анимируется вне кадра

### Takeaway
Поза каждого NPC считается полностью на CPU: выборка двух ключей, slerp на кость, иерархия 4×4, фильтры (наклон, foot IK, прицел, взгляд), палитра в `InstanceStorage`. LOD частоты для NPC вне frustum уже есть (раз в 4 кадра), и он отложил ~533–545 NPC за кадр. Главный резерв не в ядре позы, а в **последовательном пост-проходе по всем 1053 NPC**: там O(N²) поиск теневого тела, аллокации и чтение поз, только что записанных другими ядрами.

### Cited Findings
- **Конвейер.** `WorldObjects::updateAnimation` ([worldobjects.cpp:988](../../../../OpenGothic/common/world/worldobjects.cpp)):
  1. `pinAnimationLodTargets()` (стр. 1011) — два прохода по `npcArr` плюс «бойцы × N».
  2. `Workers::parallelTasks(npcArr, …)` (стр. 1014) — `Npc::updateAnimation` → `MdlVisual::updateAnimation` → `Pose::update` → `view.setPose`.
  3. Последовательный цикл `npc->updateMeleeShadow()` по всем NPC (стр. 1043–1056).
  4. `flushShadowBlades()` (1058).
  5. Последовательный `resolveMeleeContacts()` (1059–1066).
  6. `interactiveObj.parallelFor` (1069).
  7. Последовательный `syncLogicalBodyState()` по всем NPC (1080–1081).
- **Ядро позы.** `Pose::updateFrame` ([pose.cpp:539–590](../../../../OpenGothic/common/graphics/mesh/pose.cpp)) на каждую кость каждого слоя вызывает `mix` → `slerp`. В `slerp` — `std::acos`, три `std::sin` и четыре деления ([animmath.cpp:9–40](../../../../OpenGothic/common/graphics/mesh/animmath.cpp)). Затем `blend.drive`. `Pose::implMkSkeleton` (pose.cpp:861–880) делает `tr[parent]*mat` на кость.
- **Матрицы Tempest.** `Matrix4x4::operator*` определён вне строки в другой статической библиотеке ([matrix.cpp:540](../../../../OpenGothic/lib/Tempest/Engine/math/matrix.cpp)). Без LTO он не встраивается.
- **Мёртвая быстрая ветка.** Проверка на аффинность в `NvMultMatf` требует одновременно `a[2][3]==0.0f` и `a[2][3]==1.0f` ([matrix.cpp:241–248](../../../../OpenGothic/lib/Tempest/Engine/math/matrix.cpp); та же опечатка в `operator*`, стр. 549–553). Ветки 3×3 и 4×3 никогда не выполняются, всегда идёт полный 4×4 плюс `memcpy`.
- **Размер данных.** `Pose` держит `tr[96]` (Matrix4x4), `base[96]`, `raw[96]` (`AnimationSample`) и флаги ([pose.h, конец класса](../../../../OpenGothic/common/graphics/mesh/pose.h); `MAX_NUM_SKELETAL_NODES = 96`, [resources.h:74](../../../../OpenGothic/common/resources.h)). Сам `Npc` — 7424 байта: 7 824 896 байт на 1054 создания по [baseline.md](../../../../reports/engine/2026-11-03-streaming-optimisation/baseline.md). Анимационные данные при загрузке — 42,9 МБ (там же, строка `animation`).
- **Цена NPC.** 11,3–13,8 мкс на вызов `updateAnimation`. NPC за 60 м (`far2`) — 1006 из 1053, ~11,8 мкс ([baseline.md, «Анимация NPC: run / fly»](../../../../reports/engine/2026-11-03-streaming-optimisation/baseline.md)).
- **LOD уже есть.** NPC вне frustum и вне своей тени обновляются раз в `N=4` кадров ([animationlod.h](../../../../OpenGothic/common/world/animationlod.h)). На Ryzen это дало `parallel_animation` 5,135 → 4,631 мс при 343 отложенных NPC ([anim-lod.md](../../../../reports/engine/2026-11-03-streaming-optimisation/anim-lod.md)). Отсюда ~1,47 мкс стены на отложенного NPC при 12 мкс работы, то есть ~8 параллельных дорожек.
- **Свежие данные Ryzen (рынок, 2026-11-07).**
  - `parallel_animation` 4,17 мс, сумма работы потоков по NPC 6,15 мс; обновлено 508, отложено 545 NPC за кадр.
  - Регрессия по кадрам: `wall = 3,373 + 0,1237·worker_sum` (r = 0,89, лес 3,389 + 0,126, r = 0,90; интерьер 3,378 + 0,122, r = 0,96). Мой разбор: `scratchpad/anim2.py` над `perf-20261107T203338Z-34985/*/repeat-1/frameprofile.csv`.
  - Итог: ~3,4 мс не зависят от числа анимируемых NPC, параллельная часть ~0,76 мс.
- **То же на Mac** (`perf-20261107T201323Z-25682`): `wall = 2,251 + 0,110·worker_sum` (рынок, r = 0,56), `2,300 + 0,125·worker_sum` (лес, r = 0,69).
- **Что внутри `updateMeleeShadow`** ([npc.cpp:2378–2400](../../../../OpenGothic/common/world/objects/npc.cpp)):
  - создаёт `std::vector<HitZones::Posed> zones` и заполняет его через `push_back` без `reserve` ([hitzones.cpp:382–389](../../../../OpenGothic/common/graphics/mesh/hitzones.cpp));
  - создаёт второй вектор `body`;
  - вызывает `CollisionWorld::updateShadowBody`. Тот берёт `std::lock_guard` и ищет тело линейным `std::find_if` по `impl->shadowBodies` ([collisionworld.cpp:2574–2582](../../../../OpenGothic/common/physics/collisionworld.cpp)), а затем копирует вектор капсул;
  - работает при включённых по умолчанию `meleeBladeShadow = true` и `meleePhysicsHits = true` ([gothic.h:65–66](../../../../OpenGothic/common/gothic.h)), и `Npc::tick` каждый кадр сбрасывает `meleeShadowSuspended = false` ([npc.cpp:6943](../../../../OpenGothic/common/world/objects/npc.cpp)). Значит, цикл идёт по всем живым NPC, включая 545 отложенных: их поза устарела, а тело всё равно пересчитывается.
- **Кто читает теневые тела.** Только шаг клинков `stepShadowBlades`, и только при активном клинке ([collisionworld.cpp:2708–2709, 2851](../../../../OpenGothic/common/physics/collisionworld.cpp): ранний выход по `hasShadowBlades`).
- **Логическое тело.** `NpcLogicalState::sync` на каждого NPC ([npclogicalstate.cpp:359–](../../../../OpenGothic/common/world/npclogicalstate.cpp)) проходит слои, `std::map<std::string,float> speeds` и `externalBody`.
- **Невидимые NPC.** Всё равно идут через `view.visible()` в параллельном проходе, через последовательные `updateMeleeShadow`/`syncLogicalBodyState`, и каждые 4 кадра — полное обновление. Пропуск дальних есть только в безэкранном режиме `OPENGOTHIC_ANIM_SKIP_FAR` (worldobjects.cpp:998–1002).
- **Приёмы из других движков.** UE Animation Budget Allocator держит бюджет в миллисекундах (по умолчанию 1,0 мс). Он снижает частоту тика скелетов и по выбору интерполирует между обновлениями; частота 5 значит обновление раз в 5 кадров ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-budget-allocator-in-unreal-engine)).
- **ACL.** Цель проекта — быстрая распаковка на всём поддерживаемом железе, и в обмен на память её не жертвуют. В v1.0 скорость распаковки была то выше, то ниже, то наравне с движком сравнения ([ACL 1.0](https://nfrechette.github.io/2018/07/21/acl_v1.0.0)). Автор отдельно разбирает, что удаление ключей ради линейной интерполяции делает распаковку зависимой от скорости проигрывания ([Pitfalls of linear sample reduction, part 2](https://nfrechette.github.io/2019/07/25/pitfalls_linear_reduction_part2)).

### Inferences
- **O(N²).** Тела в `shadowBodies` идут в порядке первой вставки, совпадающем с порядком `npcArr`. Тогда поиск для i-го NPC проходит ~i элементов: ~554 тыс. сравнений за кадр по ~67 КБ (элемент `ShadowBody` ≈ 64 байта: указатель, вектор, `JPH::AABox` с выравниванием 16). На Zen 2 L1D 32 КБ такой массив не помещается — оценочно 0,2–0,5 мс на одном ядре. Плюс 5–6 аллокаций и освобождений на NPC (рост 1→2→4→8→16 при ~15 капсулах), мьютекс и копирование.
- **Когерентность.** Главный поток в последовательных циклах читает `skInst->transform()` (до 96 × 64 байта) и поля NPC, которые только что записали 8 работников, часть из них на других CCX. Каждая строка кэша — межъядерная передача: ~25 нс внутри CCX и 70–90 нс между CCX на Zen 2 (раздел про память ниже). Это ещё довод считать капсулы тела в том же работнике, что считал позу.
- **Ожидаемый выигрыш пункта 1** — сумма трёх частей:
  - капсулы тела и логическое тело в параллельный проход: они зависят только от своего NPC; общий счётчик аудита сделать потоковым;
  - `O(1)` слот вместо `find_if`: `Npc` хранит индекс своего `ShadowBody`, удаление через swap-and-pop;
  - тела только в радиусе клинка: `pinAnimationLodTargets` уже собирает `fighters`, worldobjects.cpp:953–979.

  Реалистично −1,5…−3 мс на Ryzen. Риск — порядок побочных эффектов: удаление окна клинка может вызвать скрипты урона (комментарий worldobjects.cpp:1040–1042). Часть с клинками остаётся последовательной, параллельной становится только часть с телами.
- **Ядро позы.** Ниже приведены мои оценки по числу операций, а не замеры.
  - nlerp вместо slerp между соседними ключами: соседние ключи близки, ошибка мала, а `acos` и `sin` уходят. На кость ~80–150 тактов → ~20.
  - Аффинная иерархия 4×3 в NEON/AVX2 вместо внешнего 4×4 плюс `memcpy`: ~3–4× на шаг иерархии.
  - Вместе это снимет, по моей оценке, 25–40 % из 12 мкс. Но по регрессии параллельная часть всего 0,76 мс стены, отсюда −0,2…−0,3 мс. ACL снизит трафик памяти (42,9 МБ сырых `AnimationSample` по 28 байт), но его цена — новый формат базы. Это шаг после пункта 1.
- **LOD видимых дальних.** Видимых `far2` ~244 за кадр (69 + 175 по счётчикам рынка Ryzen). Обновление раз в 2–4 кадра даёт −2…−2,7 мс суммы потоков, то есть −0,25…−0,35 мс стены при 8 дорожках. Принимать по клипу (правило проекта для движения).
- **SoA для позы целиком** (кость-ориентированные массивы для всех NPC) — большая перестройка ради параллельной части в 0,76 мс. Сейчас не окупается.

### Gaps
- Доля каждого последовательного цикла не измерена (нет зон).
- Число капсул `HitZones` у человека и монстров не посчитано: оно выясняется во время выполнения в `setSkeleton`.
- Опубликованных абсолютных чисел ACL для скелетов на ~60 костей (мкс на позу) я не нашёл в первоисточнике. Сводка поиска упоминала «распаковка в 1,6× быстрее на десктопе в Fortnite», но на странице блога я этого не нашёл — не использовать без проверки.

## Логика мира: тик NPC, восприятие, «кто рядом», O(N²), бюджет по кадрам

### Takeaway
Логика уже разделена на уровни симуляции (Full/Simplified) и политики (`AiNormal`/`AiFar`/`AiFar2`). Восприятие обрабатывается только у NPC в 30 м от героя. Пространственного индекса для NPC нет: «кто рядом» — линейный обход `npcArr`, но частые потребители либо ограничены по времени, либо работают с `npcNear`. Самая заметная цена — сам последовательный тик 1053 NPC (~2,2 мс на Ryzen). Её снижают частота тика дальних по бюджету и горячие поля NPC в плотном массиве.

### Cited Findings
- **Где время.** Ryzen, рынок: `world_logic` 2,89 мс, из них `npc_simplified_tick` 1,28 мс (728 NPC, ~1,76 мкс на NPC) и `npc_full_tick` 0,94 мс (325 NPC, ~2,9 мкс); остаток ~0,67 мс — предметы, mobsi, триггеры, эффекты, восприятие. `physics` 0,49 мс, `prepare` 0,29 мс (отчёт `perf-20261107T203338Z-34985/report.md`; число NPC по уровням — счётчики `npc_full`/`npc_simplified`).
- **Цикл тика.** `WorldObjects::tick` вызывает `npc.tick(d)` у всех NPC подряд, на главном потоке ([worldobjects.cpp:567–651](../../../../OpenGothic/common/world/worldobjects.cpp)). Затем отдельный проход назначает политику по расстоянию: 30 м → `AiNormal`, 60 м → `AiFar`, дальше `AiFar2` (стр. 702–726). Восприятие — только у `npcNear` и по `percNextTime` (стр. 730–744).
- **`Npc::tick`** ([npc.cpp:6924–](../../../../OpenGothic/common/world/objects/npc.cpp)) на каждого NPC вызывает десяток подтиков: `tickBodyImpact`, `tickFoodPool`, `tickMeleeParry`, `tickMeleeStamina`, `tickMeleeBarge`, `tickMeleeCharge`, `tickMeleeRiposte`, `tickMeleeAiSpecial`, `tickAnimationTags`, лицо и рот. Это функции-члены вне строки в файле на 12 405 строк.
- **Поиск соседей.** `WorldObjects::detectNpc` — линейный проход по `npcArr` с вызовом `std::function` на каждого найденного ([worldobjects.cpp:1180–1188](../../../../OpenGothic/common/world/worldobjects.cpp)). Пространственный индекс `SpaceIndex` есть только для `Interactive` и `Item` ([worldobjects.h:243–244](../../../../OpenGothic/common/world/worldobjects.h)). Вызывают `detectNpc` скрипты (`gamescript.cpp:1877, 1902, 2578`), `AI_SetNpcsToState` (npc.cpp:7612), поля заклинаний (world.cpp:1505, 1606), `npc.cpp:9593` и `Attention`.
- **`Attention::tick`** выходит раньше по `nextCheck` и по дальности героя ([attention.cpp:108–118](../../../../OpenGothic/common/world/objects/attention.cpp)). O(N²) каждый кадр там нет.
- **Полные проходы по `npcArr` за кадр:** `updateSimTiers`, `refreshMeleeFighter`, тик, политика, `prepareRagdolls`, `syncRagdolls`, два прохода в `pinAnimationLodTargets`, параллельная анимация, `updateMeleeShadow`, `resolveMeleeContacts`, `syncLogicalBodyState`, догон анимации — всего ~13 ([worldobjects.cpp](../../../../OpenGothic/common/world/worldobjects.cpp), [world.cpp:1189–1203](../../../../OpenGothic/common/world/world.cpp)). `npcArr` — `std::vector<std::unique_ptr<Npc>>`, объект 7424 байта.
- **Цена «пустого» прохода.** `prepare` (`prepareRagdoll` + `prepareLivingBody`; у 728 Simplified оба сразу выходят по `usesSimplifiedMovement()`, [npc.cpp:1778–1780, 3013–3017](../../../../OpenGothic/common/world/objects/npc.cpp)) стоит 0,29 мс на 1053 NPC — ~0,27 мкс на NPC.
- **Чужой опыт.** В исследовании стриминга перечислены дальние NPC у других: Oblivion — четыре уровня обработки, Kingdom Come 2 — ИИ-LOD (оба по аннотациям) ([streaming README](../../../../reports/engine/2026-11-03-streaming-optimisation/README.md)). UE Animation Budget Allocator — та же идея бюджета для анимации ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/animation-budget-allocator-in-unreal-engine)).

### Inferences
- **Тик по бюджету.** Simplified NPC тикают по кругу: ~1/2 или 1/4 за кадр, остальным копится `dt`. Бюджет в мкс (например, 400 мкс на Simplified) с порядком «давно не тикал — первым». Экономия на Ryzen −0,6…−1,0 мс. Риск: зависимость логики от `dt` (ожидания, маршруты, скрипты). Приёмка — сутки `npc-day-parity` с допусками; для анимации LOD это уже проходили.
- **Горячие и холодные поля.** Плотный массив `struct NpcHot { Vec3 pos; uint8_t policy, tier, weaponState, flags; }` в порядке `npcArr`, обновляемый в тике. Даёт:
  - проходы политики, уровней и выбора бойцов как поток по нескольким КБ вместо 1053 промахов в объекты по 7,4 КБ;
  - `detectNpc` по этому массиву без разыменования. А с равномерной сеткой по 64-метровым кускам, которая в стриминге уже есть (`ChunkId::fromPosition`), — ещё и с перебором только соседних клеток.

  Оценка −0,2…−0,5 мс по цене прохода ~0,1–0,3 мс. Нижняя граница — если «пустой» проход дешевле, чем `prepare`.
- **Распараллелить сам `Npc::tick` нельзя:** скриптовая VM Daedalus и мутации мира однопоточны. Этот путь я не предлагаю.
- **LTO и PGO.** Внешние короткие подтики в одном TU (`tickMeleeParry` и подобные) и вызовы `owner.tickCount()` через вне-строчный `World::tickCount()` ([world.cpp:1205](../../../../OpenGothic/common/world/world.cpp); 211 вызовов в npc.cpp) — та ветвистая логика, где PGO (раскладка горячих и холодных блоков) и LTO (встраивание через TU) дают больше всего.

### Gaps
- Сколько раз за кадр вызывается `detectNpc` и с каким радиусом, не измерено. Поэтому выигрыш от сетки неизвестен. Нужен счётчик `FrameProfile::Amount`.
- Насколько логика Simplified чувствительна к `dt` по кругу, решит только сверка суток.

## Запись команд рендера: вызовы, дескрипторы, аллокации, Metal на вызов, ожидание в `encode_main`

### Takeaway
Запись команд дешёвая: рендер GPU-driven, на Vulkan ~69 графических команд за кадр. Время `encode_main` на Ryzen (1,37 мс) — CPU-работа до записи: частицы, ветер, морфы, свет, упаковка `InstanceStorage`. «Необъяснённое ожидание» в интерьере на Mac — синхронное ожидание GPU внутри окна `preFrameUpdate…prepareGlobals`. По коду Tempest Metal единственный путь с таким ожиданием — `MtBuffer::update()` для приватного буфера. В этом окне каждый кадр так обновляется только буфер декалей PFX.

### Cited Findings
- **Порядок кадра.** `Renderer::draw` ([renderer.cpp:843–1039](../../../../OpenGothic/common/graphics/renderer.cpp)): `waitCompiler`, `updateLights` (869), камера и TAA, `prepareUniforms`, `wview.preFrameUpdate` (901), `wview.prepareGlobals` (902). Далее видимость → HiZ → GBuffer → SSAO → тени → освещение → вода, прозрачное, пост.
- **`WorldView::preFrameUpdate`** ([worldview.cpp:590–611](../../../../OpenGothic/common/graphics/worldview.cpp)): `pfxGroup.tick`, `gLights.tick`, `sGlobal.commitUbo` (607), `pfxGroup.preFrameUpdate` (609), `visuals.preFrameUpdate` (610: ветер и морфы). `prepareGlobals` → `instanceMem.commit` (зона `storage_commit`).
- **Разбивка CSV Ryzen, рынок** (3237 кадров, мс от начала `encode`): `encode_main` с 0,044, `storage_commit` 0,886–1,100, `encode_shadows` 1,377, `submit` 1,645. От начала записи до `storage_commit` — 0,84 мс (`scratchpad/seg.py`).
- **Разбивка CSV Mac, интерьер** (1605 кадров): `storage_commit` начинается через 12,52 мс (p5 10,75; p50 12,51; p95 14,11), `fence_poll` = 0,000 мс. Для сравнения: рынок 0,59 мс (`fence_poll` 8,1 мс), лес 0,55, ночная гавань 0,63 (`perf-20261107T092943Z-22807`). GPU интерьера 17,92 мс, «попытка» CPU 19,93 мс, интервал кадра 31,96 с / 1601 = 19,96 мс.
- **Ryzen, интерьер:** `encode_main` 1,41 мс — как на улице. Ожидание есть только на Metal.
- **Путь с ожиданием в Tempest Metal.** `MtBuffer::update` для `StorageModePrivate` вызывает `dev->uploadBuffer(…, true)` ([mtbuffer.cpp:58–64](../../../../OpenGothic/lib/Tempest/Engine/gapi/metal/mtbuffer.cpp)). `uploadBuffer` берёт новый command buffer **той же очереди** (`queue->commandBuffer()`), коммитит blit и при `wait` вызывает `cmd->waitUntilCompleted()` (стр. 74–99). `updateAsync` ждать не будет (стр. 66–72). На Vulkan `VBuffer::update` для неотображаемой памяти отправляет копию асинхронно, без ожидания ([vbuffer.cpp:34–](../../../../OpenGothic/lib/Tempest/Engine/gapi/vulkan/vbuffer.cpp)).
- **Кандидат.** `PfxBucket::preFrameUpdate` создаёт буфер декалей в куче `Device` и при `forceUpdate[fId]` вызывает `ssbo.update(pfxCpu)` ([pfxbucket.cpp:864–889](../../../../OpenGothic/common/graphics/pfx/pfxbucket.cpp)). Для частиц, не декалей, буфер в `Upload`, и `update` — просто `memcpy`. `forceUpdate` ставят `allocEmitter`/`freeEmitter`/`allocBlock` и `invalidateDecal` (pfxbucket.cpp:147–156, 205–245), а `invalidateDecal` вызывает `PfxEmitter::setPosition` при любом сдвиге эмиттера декали ([pfxemitter.cpp:101–125](../../../../OpenGothic/common/world/objects/pfxemitter.cpp), вызов на стр. 124). `setObjMatrix` → `setPosition` идёт из `ObjVisual::setObjMatrix` (`M_Pfx`, [objvisual.cpp:395–426](../../../../OpenGothic/common/graphics/objvisual.cpp)), `Interactive::moveEvent`, `FirePlace::moveEvent` и других.
- **Остальное в окне ждать не может:** UBO и патчи света — `Upload`-куча, `memcpy` ([lightgroup.cpp:446–480](../../../../OpenGothic/common/graphics/lightgroup.cpp); `Device::ubo` → `BufferHeap::Upload`, [device.h:102–104](../../../../OpenGothic/lib/Tempest/Engine/graphics/device.h)).
- **Цифры по API.** Vulkan, рынок: `draw_calls` 69,3 на кадр. Палитры: 866 вызовов и 1,74 МБ на кадр, из них 366 вызовов (0,74 МБ) — без изменений (`memcmp` отсекает). Патч `InstanceStorage` — 1,42 МБ в 6319 блоках за кадр, `storage_pack` 0,21 мс ([instancestorage.cpp:149–237](../../../../OpenGothic/common/graphics/instancestorage.cpp)).
- **Ветер.** `VisualObjects::preFrameUpdateWind` каждый кадр обходит `std::unordered_set<size_t> objectsWind` и для каждого объекта вызывает `updateInstance` ([visualobjects.cpp:917–925](../../../../OpenGothic/common/graphics/visualobjects.cpp); [visualobjects.h:210](../../../../OpenGothic/common/graphics/visualobjects.h)). Матрица — функция (позиция, `tickCount`, период, режим, сила, масштаб): `WorldWind::shaderMatrix`. Та же функция используется в ткани ([clothvisual.cpp](../../../../OpenGothic/common/graphics/clothvisual.cpp)). По прошлому разбору VSM, `updateInstance` вызывается ≈4800 раз за кадр, `commitVsmChanges` стоил 0,48–0,64 мс ([why-slower.md](../../../../reports/render-features/2026-11-04-vsm-cache/why-slower.md)).
- **Metal на вызов.** `MtCommandBuffer::implSetUniforms` проходит раскладку конвейера и для bindless-массивов вызывает `useResources` ([mtcommandbuffer.cpp:470–540](../../../../OpenGothic/lib/Tempest/Engine/gapi/metal/mtcommandbuffer.cpp)). `MtCommandBuffer::barrier` пустой. Command buffer — с `setRetainedReferences(false)` (стр. 230–235).
- **Present на Metal.** Каждый кадр — отдельный command buffer с blit всего кадра из своей текстуры в drawable ([mtswapchain.mm:163–213](../../../../OpenGothic/lib/Tempest/Engine/gapi/metal/mtswapchain.mm)). Это то же, что в журнале названо «выход MetalFX сразу в drawable без копии».

### Inferences
- **Механизм ожидания в интерьере.** Окно записи ≈ GPU-кадр (17,9 мс) − тик и анимация (~6,1 мс) = 11,8 мс, а измерено 12,5 мс. Значит, ожидание заканчивается ровно тогда, когда GPU доработал предыдущий кадр: так ведёт себя `waitUntilCompleted` на command buffer, поставленном в конец той же очереди. После него GPU простаивает, пока CPU пишет остаток кадра (~1,2 мс) и отправляет его. Отсюда интервал 20 мс при GPU 17,9 мс.
- **После починки** интервал ≈ max(GPU, CPU без ожидания ≈ 7,5–8 мс) ≈ GPU → −2 мс (−10 %) при 100 %. При 75 % по журналу интерьер 56 FPS; там выигрыш оценочно того же порядка.
- **Починка:**
  - буфер декалей в куче `Upload` на каждый `fId` — `item[fId]` уже раздельны по кадрам в полёте, и после ожидания `fence[cmdId]` GPU его не читает; или `updateAsync`;
  - общее правило: в кадре не вызывать `update()` приватных буферов. В сборке с профайлером — счётчик или assert в `MtDevice::uploadBuffer(wait=true)` вне загрузки.
- **Записывать команды параллельно или кэшировать их не нужно:** 69 команд. Резерв — перенести CPU-работу перед записью с главного потока:
  - **Ветер на GPU:** вершинный шейдер считает ту же функцию из позиции, времени и параметров; со стороны CPU каждый кадр уходят тысячи `updateInstance` и их доля в патче на 1,42 МБ.
  - **Частицы задачей:** `pfxGroup.tick` + `buildSsbo` параллельно с `world_logic`; позиция наблюдателя прошлого кадра для LOD частиц допустима.
  - **Обход ветра:** `unordered_set` → плотный `std::vector` с удалением swap-and-pop. Узловой обход ~5 тыс. элементов — тысячи промахов кэша.

  Итого −0,3…−0,6 мс главного потока на Ryzen.
- **Более дальняя мысль — конвейер «игровой поток / поток рендера»:** запись кадра N идёт параллельно с тиком N+1. Это сняло бы ~1,6 мс `encode` с критического пути Ryzen, но требует двойной буферизации состояния `VisualObjects`. Большая работа, только после пунктов 1–3.

### Gaps
- Какой эмиттер декали в сцене `perf-interior` сдвигается каждый кадр (свеча, mobsi, вложение NPC), из кода не видно. Нужна метка вокруг `pfxGroup.preFrameUpdate` или Time Profiler. Если ожидание окажется не там, остальные кандидаты в окне — `pfxGroup.tick`, `visuals.preFrameUpdate`, `gLights` — синхронных ожиданий в коде не имеют. Тогда искать неявное ожидание драйвера.
- Сколько стоят ветер и частицы по отдельности, неизвестно: внутри `encode_main` у них нет зон.

## Сборка: LTO, PGO, BOLT, `-march`, unity, mimalloc, `-fno-semantic-interposition`

### Takeaway
В репозитории нет ни LTO, ни PGO, ни BOLT, ни `-march`, ни стороннего распределителя. Mac и Ryzen собираются как `RelWithDebInfo` (`-O2`), Windows-релиз — MSVC `Release`. В коде есть конкретные причины, почему LTO и PGO помогут: горячая математика лежит в отдельной статической библиотеке Tempest, вызовы геттеров идут через TU, логика NPC ветвистая. Опубликованный выигрыш PGO у крупных C++-проектов — до 15 %. Для игровых движков первичных чисел я не нашёл.

### Cited Findings
- **Флаги.**
  - `CMAKE_POSITION_INDEPENDENT_CODE ON` ([CMakeLists.txt:15](../../../../OpenGothic/CMakeLists.txt)); `-Wall -Wconversion -Wno-strict-aliasing -Werror` (стр. 43).
  - Jolt: `OPENGOTHIC_JOLT_X86 "portable"` по умолчанию (стр. 216), `CROSS_PLATFORM_DETERMINISTIC ON` (227), свой `INTERPROCEDURAL_OPTIMIZATION OFF` (229), `-ffp-contract=off` (264).
  - Стенд: `-DCMAKE_BUILD_TYPE=RelWithDebInfo` ([perf.sh:27](../../../../scripts/perf.sh)).
  - Ryzen: `-O2 -g1 -DNDEBUG`; `-O3` там даёт ложные `-Werror=stringop-overflow` ([ryzen-build.sh:2–7](../../../../scripts/ryzen-build.sh); [docs/compute.md:11](../../../../docs/compute.md)).
  - Mac и Linux: `scripts/build-engine.sh` по умолчанию `RelWithDebInfo` (стр. 14). Windows-релиз: `BUILD_TYPE: Release` ([windows-release.yml:28](../../../../.github/workflows/windows-release.yml)).
  - Поиск по `-flto`, `INTERPROCEDURAL`, `-fprofile`, `march`, `mimalloc` в CMake и скриптах ничего не дал, кроме выключенного `INTERPROCEDURAL_OPTIMIZATION` у Jolt.
- **Где LTO даст встраивание.**
  - `Matrix4x4::operator*`, `project` и `mul` — вне строки в `lib/Tempest/Engine/math/matrix.cpp` (стр. 467, 497–505, 540); вызываются на каждую кость в `Pose::implMkSkeleton` и на каждую капсулу в `HitZones::posed`.
  - `slerp`, `mix`, `mkMatrix` — в `animmath.cpp`, а вызываются из `pose.cpp` и `animblend`.
  - `World::tickCount()` вне строки ([world.cpp:1205](../../../../OpenGothic/common/world/world.cpp)); в `npc.cpp` 211 вызовов `owner.tickCount()`/`world().tickCount()`.
- **PGO, чужие данные.** Chrome на Windows с PGO MSVC — «до 15 %» быстрее, в версиях 53 (64 бит) и 54 (32 бит) ([Chromium Blog, 2016-10](https://blog.chromium.org/2016/10)). По пересказу того же поста: новая вкладка −14,8 %, запуск −16,8 %, загрузка страниц −5,9 % ([Gigazine](https://gigazine.net/gsc_news/en/20161101-chrome-windows-faster-pgo)).
- **PGO в документации LLVM:** применение PGO к Clang и LLVM, по словам пользователей, сокращает общее время компиляции на 20 % ([LLVM: How to build with PGO](https://llvm.org/docs/HowToBuildWithPGO.html)).
- **BOLT.**
  - По пересказу Phoronix, статья CGO 2019 сообщает ~7 % поверх FDO и LTO для дата-центровых приложений Facebook и до ~20 % в отдельных случаях ([Phoronix](https://www.phoronix.com/news/BOLT-Nearing-LLVM-Monorepo); [статья BOLT](https://research.facebook.com/file/534990324471927/BOLT-A-Practical-Binary-Optimizer-for-Data-Centers-and-Beyond.pdf)).
  - Документация LLVM: BOLT поверх Clang, собранного с PGO и LTO, сократил сборку на ~12 % ([OptimizingClang.md](https://llvm.googlesource.com/llvm-project.git/+show/refs/heads/main/bolt/docs/OptimizingClang.md)).
- **`-fno-semantic-interposition`.** Python в Fedora, собранный с этим флагом, быстрее на 5–27 % в зависимости от нагрузки. Цена — нельзя подменять символы libpython через `LD_PRELOAD` ([Fedora Change](https://fedoraproject.org/wiki/Changes/PythonNoSemanticInterpositionSpeedup)). Независимый замер Arch на pyperformance — ~20 % ([Arch FS#65151](https://bugs.archlinux.org/task/65151)).
- **mimalloc.**
  - Подключается без перекомпиляции: `LD_PRELOAD=/usr/lib/libmimalloc.so` на Linux, `DYLD_INSERT_LIBRARIES=…/libmimalloc.dylib` на macOS; на macOS действуют ограничения безопасности.
  - Проект заявляет: на нагрузке Lean +13 % к tcmalloc, на sh6bench более чем в 2,5× быстрее jemalloc. Замер 2021 года на 16-ядерном AMD 5950X, с оговоркой, что на других нагрузках может отличаться ([mimalloc](https://github.com/microsoft/mimalloc)).
- **Аллокации на кадр в горячих циклах:** `updateMeleeShadow` (два вектора на NPC, npc.cpp:2384–2389), `Pose::combatLayers` возвращает `std::vector` (из `syncMeleeShadowSwing`, npc.cpp:2235), `LightGroup::prepareGlobals` — два вектора за кадр (lightgroup.cpp:418–419).

### Inferences
- **`-fPIC` на GCC.** Статическая библиотека `OpenGothic` с `-fPIC` (так CMake понимает `POSITION_INDEPENDENT_CODE` для библиотек) заставляет GCC считать внешние функции одного TU подменяемыми. Он не встраивает и не оптимизирует межпроцедурно такие вызовы, как `Npc::tick` → `Npc::tickMeleeParry`. Механизм тот же, что в Fedora. Исполняемый файл статический, подмена нам не нужна. Варианты: `-fno-semantic-interposition` для GCC или PIC только у `SHARED`-целей. Выигрыш неизвестен, 0–5 %, проверка — один флаг. На Mac (Mach-O, Apple clang) это не действует.
- **PGO + ThinLTO** — самый дешёвый «бесплатный» пункт для CPU-кадра. Обучающий прогон есть готовый: `perf.sh` (рынок, лес, интерьер, бой) и безэкранные сутки. Мак: Apple clang `-fprofile-instr-generate` → `llvm-profdata merge` → `-fprofile-instr-use`, вместе с `-flto=thin`. Ryzen: GCC `-flto=auto -fprofile-generate/-use` или clang. Windows: MSVC `/GL /LTCG` плюс PGO. Оценка 5–15 % CPU-кадра, на Ryzen −0,5…−1,4 мс.
- **Риски сборки:**
  - `-Werror` с GCC LTO может дать новые предупреждения при компоновке;
  - PGO-профиль устаревает с кодом: в CI нужен шаг «прогнать стенд, собрать с профилем» или профиль, который обновляют раз в неделю;
  - детерминизм физики не затронут: у Jolt свои флаги и `CROSS_PLATFORM_DETERMINISTIC`. Но `-ffast-math` включать нельзя.
- **BOLT** имеет смысл только после PGO+LTO на Linux; ~5–7 % по чужим данным; для Mach-O не применим.
- **`-march`/`x86-64-v3`.** Только для собственных сборок стенда и Ryzen. Раздаваемая сборка должна оставаться переносимой: для неё — мультиверсии функций, и только для ядра позы.
- **Unity-сборка** — дешёвая замена части LTO по встраиванию внутри модуля, но меньшая. Брать только если LTO упрётся в `-Werror`.
- **mimalloc** — опыт без кода, полчаса. Ожидание скромное (0–3 %): по коду аллокаций в кадре порядка нескольких тысяч, и большую их часть убирает пункт 1.

### Gaps
- Первичных опубликованных замеров PGO и LTO для игровых движков (UE, Godot, OpenGothic) я не нашёл. Чужие цифры — браузер, интерпретатор, компилятор.
- Какой `-mcpu` Apple clang выбирает по умолчанию для arm64 macOS, я не проверял.
- Встроит ли GCC `-O2` с `-fPIC` горячие подтики `Npc::tick`, без чтения ассемблера не установлено. Проверить через `-fopt-info-inline-missed` на `npc.cpp`.

## Кэш и память: раскладка структур, prefetch, огромные страницы, потоки на P/E-ядрах M1 и CCX Ryzen

### Takeaway
Главная проблема памяти — проходы главного потока по 1053 объектам `Npc` по 7,4 КБ сразу после того, как работники на других ядрах записали их позы. Лечится локальностью: работу на NPC делать там, где его поза ещё в кэше, а горячие поля держать в плотном массиве. Закрепление потоков на macOS невозможно, на Mac рычаг — QoS. На Ryzen задержка между CCX втрое выше, чем внутри.

### Cited Findings
- **Размеры:** `Npc` — 7424 байта, 1053 NPC ≈ 7,8 МБ ([baseline.md](../../../../reports/engine/2026-11-03-streaming-optimisation/baseline.md)); у `Pose` — массивы на 96 костей (`tr` по 64 байта, `base`, `raw`) ([pose.h](../../../../OpenGothic/common/graphics/mesh/pose.h); [resources.h:74](../../../../OpenGothic/common/resources.h)).
- **Пул потоков.** `Workers`: `MAX_THREADS = 16`, `taskPerThread = 128`, `taskPerStep = 16` ([workers.h](../../../../OpenGothic/common/utils/workers.h), [workers.cpp:78–79](../../../../OpenGothic/common/utils/workers.cpp)).
  - Для `npcArr` из 1053 элементов число задач `ceil(1053/128)−1 = 8`; работает и главный поток.
  - Пробуждение — `notify_all` на condvar, ожидание — `workDone.wait` (workers.cpp:196–235).
  - Потоки — `std::thread`, без QoS и без привязки к ядрам.
  - Регрессия даёт ~8 эффективных дорожек на Ryzen (наклон 0,124) и 8–9 на Mac.
- **Ложное разделение** почти не стоит: `progressIt`, `taskDone`, `workTbd` лежат рядом в одном объекте (workers.h), но на одну раздачу ~66 порций.
- **Ryzen 3950X:** четыре CCX по 4 ядра на двух чиплетах. Задержка ядро–ядро ~25 нс внутри CCX и ~70–90 нс между CCX ([Mynavi](https://news.mynavi.jp/article/20190808-874103/6); [Igor's Lab, 3900X: ~70 нс между CCX](https://igorslab.de/en/amd-ryzen-3900x-and-3700x-in-test-igorslab/5); [UNIKO's Hardware, 3950X](https://unikoshardware.com/2020/02/cpu-ryzen-9-3950x-best-review-english.html)). Источники расходятся в деталях; первичного замера AnandTech для 3950X я не открывал.
- **macOS.** Прямого закрепления потоков на Apple Silicon ядро не поддерживает; на тип ядра влияют через QoS (`pthread_set_qos_class_self_np`) ([gdt-cpus, исходник](https://docs.rs/crate/gdt-cpus/latest/source/src/platform/macos/affinity.rs)). Потоки с QoS background (9) работают только на E-ядрах. С QoS 17–33 — предпочтительно на P-ядрах, но при занятых P-ядрах могут уйти на E ([Eclectic Light: QoS](https://eclecticlight.co/2025/05/09/what-is-quality-of-service-and-how-does-it-matter/); [Eclectic Light: multithreading](https://eclecticlight.co/2024/06/20/making-apple-silicon-faster-2-multithreading/)). Вторичные источники: страницу Apple «Tuning your code's performance for Apple silicon» загрузить не удалось.
- **В интерьере на Mac** свободного CPU всё равно много: «попытка» без ожидания ~7,5 мс при GPU 17,9 мс (CSV выше).

### Inferences
- **Локальность важнее закрепления.** Пост-проход с капсулами и логическим телом в том же работнике, что считал позу, убирает межъядерные передачи строк поз (до 96 × 64 байта на NPC). На Ryzen каждая такая передача между CCX стоит ~70–90 нс против ~25 нс внутри.
- **Закрепить 8 работников анимации на одном CCD** (2 CCX) — дешёвый опыт на Ryzen через `pthread_setaffinity_np`. Выгода не доказана, а `gq` уже ограничивает набор CPU заявки. Низкий приоритет.
- **QoS на Mac.** Явно задать работникам `QOS_CLASS_USER_INTERACTIVE`, как главному потоку, — защита от «хвоста» на E-ядре, когда порция из 16 NPC (~200 мкс) попадает на медленное ядро. По регрессии Mac (r = 0,56–0,69) эффект малый или скрыт шумом. Сначала залогировать `qos_class_self()` работников.
- **Prefetch.** Явный `__builtin_prefetch` следующего `Npc` в последовательных проходах — дешёвый приём «из олимпиадного программирования», если пункт 1 почему-то нельзя. В проходе-указателе (`unique_ptr`) объекты независимы, и уровень параллелизма памяти уже есть. Ожидаю мало.
- **Огромные страницы.** Признаков упора в TLB нет. На macOS управления ими у приложения нет; на Linux THP для пула анимационных данных (42,9 МБ) возможен, но без замера не предлагаю.

### Gaps
- Каков QoS работников на Mac сейчас и как ОС распределяет их по 8P + 2E, не измерено.
- Счётчиков промахов кэша по последовательным проходам (perf c2c на Ryzen) нет. Он бы подтвердил долю когерентного трафика.

## Остальные проходы GPU вне прайс-листа: GBuffer, тени, HiZ, Sky LUT, Bloom, TAA; кэш статических теней

### Takeaway
После SSAO и тумана самая крупная невычтенная статья GPU на Mac — карты теней: вершинная стадия дальнего каскада #1 1,77 мс на рынке, у обоих каскадов в лесу 1,2–1,4 мс фрагментов на каскад. Кэша теней нет, и в нынешнем виде он невозможен: проекция крутится вместе с камерой. Дешёвый шаг — обновлять дальний каскад через кадр. Настоящий кэш требует мировой ориентации и привязки к текселям. GBuffer, HiZ, Sky LUT и Bloom малы.

### Cited Findings
- **GPU-интервалы Metal**, `perf-20261107T201323Z-25682`, среднее на кадр, мс; интервалы стадий вершин и фрагментов, перекрываются и не складываются — так требует [docs/performance.md](../../../../docs/performance.md):

  | Проход | Рынок: вершины / фрагменты | Лес: вершины / фрагменты | Интерьер |
  |---|---|---|---|
  | ShadowMap #1 | 1,77 / 0,55 | 0,87 / 1,23 | ночью без отрисовок (две пустые очистки) |
  | ShadowMap #0 | 0,32 / 0,33 | 0,21 / 1,44 | — |
  | GBuffer | 0,24 / 1,47 | 0,20 / 2,38 | 0,08 / 1,10 |
  | HiZ-occluders | 0,08 / 0,22 | 0,09 / 0,49 | 0,05 / 0,10 |
  | Sky LUT (2 прохода) | — / 0,21 | — / 0,21 | — / 0,14 |
  | Bloom (12 проходов) | — / 0,65 | — / 0,63 | — / 0,65 |
  | TAA / TAA-sharpen | 1,61 / 0,63 | 1,60 / 0,62 | 1,57 / 0,64 |
  | Освещение (солнце, окружение, лампы, небо) | 0,07 / 2,70 | 0,04 / 1,82 | 0,09 / 3,40 |

  Разбор: `scratchpad/gpu.py`. В этой сборке `TAA-sharpen` ещё отдельный: слияние в #984 вошло после `be62e3af`.
- **Карты теней.** `Renderer::drawShadowMap` каждый кадр очищает и рисует оба каскада ([renderer.cpp:2306–2316](../../../../OpenGothic/common/graphics/renderer.cpp)): 2048² (`shadowResolution`, [renderer.h:138](../../../../OpenGothic/common/graphics/renderer.h)), `ShadowLayers = 2` ([resources.h:71](../../../../OpenGothic/common/resources.h)). Ниже горизонта (`dir().y <= minShadowY`) кадр всё равно открывает и очищает обе карты: в интерьере это энкодеры 6 и 7 без отрисовок.
- **Проекция теней.** `Camera::mkViewShadow`: центр — цель камеры, базис повёрнут по рысканью камеры (`view.rotate(rotation, 0, 1, 0)`, [camera.cpp:545](../../../../OpenGothic/common/camera.cpp)), без привязки к сетке текселей. Каскад #0 шириной ≥1024 см, каскад #1 — 5120 см и глубиной 25 600 см (camera.cpp:525–536). Тексель #1 ≈ 2,5 см.
- **Как кэшируют другие.**
  - Insomniac: кэш CSM прокручивается вслед за плавно движущейся камерой, перерисовываются только открывшиеся края, затем поверх копии рисуется нестатическая геометрия. «Статичным» считается объект, не двигавшийся заданное время, например 5 с ([Acton, SIGGRAPH 2012](https://advances.realtimerendering.com/s2012/insomniac/Acton-CSM_Scrolling(Siggraph2012).pdf)).
  - Flax: статическая геометрия заранее рисуется в промежуточную карту, кэш копируется, поверх рисуются только динамические объекты ([Flax docs](https://docs.flaxengine.com/manual/graphics/lighting/shadows.html)).
  - CryEngine: кэшированные каскады перецентрируются, когда камера подходит к границе. Динамические объекты в них не обновляются без отдельной опции ([CryEngine: Cached Shadows](https://www.cryengine.com/docs/static/engines/cryengine-3/categories/1114113/pages/21267738)).
  - В UE кэш каскада прокручивается до порога перекрытия, потом перерисовывается целиком ([форум UE](https://forums.unrealengine.com/t/csm-caching-and-objects-culling-questions/2684008)).
  - Обновление каскада через кадр в найденных источниках прямо не описано.
- **В журнале** «кэш статических теней» стоит в «Мелком» ([docs/performance.md, «Следующие шаги», п. 5](../../../../docs/performance.md)). Кэш VSM для ламп уже пробовали: на Ryzen он стал дороже из-за мьютекса в `updateInstance` ([why-slower.md](../../../../reports/render-features/2026-11-04-vsm-cache/why-slower.md)).

### Inferences
- **Каскад #1 через кадр.** Хранить матрицу, с которой он нарисован, и отдавать её шейдеру в `uboGlobal.viewShadow[1]` до следующего обновления. Тогда старую карту читают с её собственной матрицей и разрыва нет; тени движущихся объектов в дальнем каскаде отстают на кадр. Ожидаемо −0,5…−1 мс GPU на Mac на рынке и в лесу (половина стадий #1). Приёмка по клипу поворота камеры и бегущих NPC. Это самый короткий путь к экономии на тенях.
- **Статический кэш** сначала требует перестроить проекцию: базис по миру (не по рысканью камеры) и привязка центра к текселям. Иначе любой поворот камеры инвалидирует кэш. Затем:
  - статика рисуется в кэш при сдвиге центра на N текселей (прокрутка копией) или при повороте солнца больше порога;
  - каждый кадр — копия кэша и отрисовка только динамики: NPC, подвижные vob'ы, ткань.

  Солнце в Gothic движется непрерывно, поэтому кэш освежается по углу солнца. Моя оценка по геометрии: при 0,06°/с смещение тени 10-метрового объекта ~1 см/с — для текселя 2,5 см это обновление раз в 1–2 с. Это не проверено на нашем ходе времени.
- **Ночные очистки.** Если солнце ниже горизонта, очищать карту один раз, а не каждый кадр: два энкодера в интерьере. Мелко: 2048² D32 — 16 МБ записи на карту.
- **Sky LUT** (~0,2 мс на улице) зависит от солнца и времени. Пересчёт раз в N кадров или при сдвиге солнца больше порога — мелкий, но верный выигрыш.
- **Bloom** уже 0,65 мс фрагментов на 12 проходов; однопроходный даунсэмплер (подход FidelityFX SPD) сэкономит постоянную часть проходов, порядка 0,05–0,1 мс. Низкий приоритет.
- **GBuffer по вершинам** (0,2 мс) и **HiZ** (≤0,6 мс) резерва не показывают.
- **TAA** (1,6 мс фрагментов) уже в прайс-листе шейдеров. Повтор не нужен.

### Gaps
- Точной цены проходов теней вычитанием (`OPENGOTHIC_GPU_SKIP="ShadowMap #1"`) нет. Таблица выше — интервалы стадий, которые могут перекрываться с compute (SSAO теперь идёт перед тенями).
- Как быстро движется солнце на нашем ходе игрового времени и какой порог поворота незаметен, надо мерить по клипу.
- Отсечение отбрасывающих тень по видимости их тени в кадре камеры (receiver-aware caster culling) — известная идея, но источника с числами я не искал.

## Олимпиадные приёмы: арены, кадровые аллокаторы, безветвенные циклы, радикс-сортировка, битовые флаги, хэш-таблицы, lock-free очереди

### Takeaway
Код местами уже следует этим приёмам: битовые маски грязных блоков в `InstanceStorage`, `memcmp` против лишних выгрузок, splitmix-фаза LOD. Есть три места, где олимпиадный приём прямо снимает измеренную цену:
- **O(N²) линейный поиск** теневого тела → индекс-слот;
- **узловые контейнеры** в горячих обходах кадра → плотные массивы;
- **временные векторы на NPC** → переиспользуемые буферы или кадровая арена.

Радикс-сортировка и lock-free очереди здесь не нужны: сортировка редкая, контенции нет.

### Cited Findings
- **Линейный поиск владельца:**
  - `CollisionWorld::updateShadowBody` — `std::find_if` по `shadowBodies` (collisionworld.cpp:2575);
  - `removeShadowBody` — `std::erase_if` (2587);
  - `takeShadowBladeContacts` и `updateShadowBlade` — линейно по `shadowBlades` (2606–2615, 2641–2648).
- **Ещё линейные поиски:** `WorldObjects::npcId` по указателю (worldobjects.cpp:751–758) — его зовут в циклах `updateAnimation`, когда массив изменился (1052–1055, 1062–1065); `InstanceStorage::alloc` и `free` — по вектору свободных областей (instancestorage.cpp:252–340).
- **Узловые контейнеры в кадре:**
  - `std::unordered_set<size_t> objectsWind`/`objectsMorph` (visualobjects.h:210–211) обходятся каждый кадр (visualobjects.cpp:918, 928);
  - `std::map<std::pair<int32_t,int32_t>, std::unordered_set<size_t>> textureChunks` (visualobjects.h:217);
  - у логического тела `std::map<std::string,float> speeds` и `find(next->name)` в `advance` (npclogicalstate.h:81; npclogicalstate.cpp:307–340).
- **Уже в духе DOD:** битовые маски с `fetch_or` и пропуском пустых слов в `InstanceStorage::commit` (instancestorage.cpp:31–47, 170–200); `memcmp` перед копированием палитры (366 из 866 вызовов отсекаются, отчёт Ryzen); упаковка SSAO в `packHalf2x16` ([docs/performance.md](../../../../docs/performance.md)).
- **Сортировка команд** `std::sort` идёт только при изменении набора (`cmdDurtyBit`, drawcommands.cpp:219–234).

### Inferences
- **Индекс-слот:** `Npc` хранит `uint32_t shadowBodySlot`, удаление — swap-and-pop с обновлением слота у переставленного. Поиск становится O(1). Мьютекс можно убрать в фазе, где пишут только сами владельцы слотов. Это часть пункта 1.
- **Кадровая арена** (bump-аллокатор, сбрасывается в начале кадра) или `thread_local` переиспользуемые векторы для `zones`, `body` и `combatLayers` убирают тысячи `malloc`/`free` за кадр. На macOS, где системный malloc дороже, эффект больше. Тот же результат даёт `out.clear()` у члена-вектора без освобождения.
- **Плотные массивы с битовыми флагами** вместо `unordered_set` для ветра и морфов: `std::vector<uint32_t>` плюс битсет «в списке». Обход становится потоковым.
- **Безветвенные циклы** уместны в выборке ключей анимации (`updateFrame`) только вместе с SoA. Ранжирую низко (параллельная часть 0,76 мс).
- **Хэш-таблицы (Swiss или robin hood)** нужны только там, где ключ не позиционный. Для указателей NPC лучше индекс, чем хэш.
- **Lock-free очередь** работе `Workers` не нужна: раздача — один `fetch_add` на порцию из 16.

### Gaps
- Сколько стоят `npcId` и поиски в `InstanceStorage` в реальных кадрах, не измерено. По коду их зовут редко: при изменении массивов или росте.
- Насколько дороже системный malloc на macOS, чем glibc, на нашем профиле аллокаций, без опыта с `DYLD_INSERT_LIBRARIES` не знаю.
