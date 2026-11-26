# Конвейер tick/encode: запись кадра N параллельно с тиком N+1

Дата: 2026-11-26. Статус: исследование + дизайн v1. Код `main` `9f8e49364`. Продолжает [архитектуру CPU](../2026-11-26-cpu-architecture/README.md) и [C1/C2](../2026-11-26-encode-par/README.md) (инкрементальный параллелизм подготовки исчерпан). Рецепт конвейера уже есть в репозитории: [renderer-vb-plan.md, раздел «CPU, ECS-тик и запись команд»](../../docs/renderer-vb-plan.md#cpu-ecs-тик-и-запись-команд) (расчёт E): публикация поколений на commit, запись по опубликованным данным, владелец энкодера применяет в прежнем порядке и владеет барьерами/submit/present; накладные H = 0,15–0,25 мс на снимок/синхронизацию/слияние.

## Цель и оценка

Снять с главного потока запись команд кадра (~1,7 мс: encode_main 1,345 + shadows 0,12 + water 0,10 + post 0,08 + ui 0,01, рынок C1 ON) перекрытием с тиком следующего кадра (~4 мс: world_logic 2,3 + parallel_animation 0,96 + prepare 0,37 + physics 0,47). Ожидаемый выигрыш полного кадра ~1,2–1,5 мс (encode минус H и минус доля подготовки, остающаяся на главном потоке). Поведение симуляции и порядок команд не меняются: тот же тик, та же последовательность записи, те же submit/present.

Текущая структура кадра (`common/mainwindow.cpp:1563-1708`): fence[slot].wait → tick → finishStep (параллельные позы) → стриминг/updateTextures → capture → resetRecycled(slot) → UI paint + uiMesh[slot].update → encode (drawGameFrame) → submit → present → slot flip → limiter. Подготовка рендера (preFrameUpdate, prepareUniforms, prepareGlobals-commits) сидит ВНУТРИ encode (`renderer.cpp:965-967`).

## Дизайн v1 (один поток записи, без параллелизма внутри записи)

**Аудит (2026-11-26, explore-агент, полный список ниже в «Нарушения инварианта»):** безопасной точки fork в текущем коде НЕТ. Запись читает не только GPU-буферы: `DrawCommands::cmd/ord/maxPayload`, `DrawBuckets::bucketsCpu/currentFrame/desc`, `DrawClusters::clusters` (size), `VisualObjects::objects/impostorDraws`, `LightGroup::lightSourceData.size()`, список бакетов PFX, `GrassField::draws`, `BloodDecals::data`, скаляры `scene.tickCount/originLwc/view/proj`, `Sky`, и **текстурные хендлы** (`publishTexture` move-assignит `*record.texture` на месте во время стриминга). GPU-буферы (`dataGpu/clustersGpu/uboGlobal/lightSourceSsbo`) разделены после commit при условии порядка submit (in-place патчи записаны в командный буфер своего кадра). Поэтому проект = публикация draw-состояния на кадр + поток записи, а не просто перенос encode.

Ключевое упрощение аудита: **состояние, которое пишет только подготовка рендера (prep), безопасно без снимка** — prep N+1 идёт после join N (taa/ssao/eye/gi/fogSky/vsm.frameNo, `Sky`, `uboGlobalCpu`, `scene.*`, grass `draws`, blood `data`, если их prepare вынести в prep). Снимки нужны только для того, что пишут **тик/стриминг**: cmd/ord, bucketsCpu (+текстурные хендлы), clusters size, impostor-список, число светов, список pfx-бакетов.

1. **Слой публикации (1a, без потоков):** на commit/prep каждого кадра публикуем per-fId снимки tick-изменяемого draw-состояния; запись читает опубликованное. COW: cmd/ord/buckets меняются только при стриминге/смене видимости — если с прошлого commit не менялись, публикация = переиспользование прежнего снимка (ref-counted), копии нет. clusters: снимок size (содержимое запись не читает вне bench). impostor-список и pfx-список строятся в prep. buckets-снимок копирует текстурные хендлы по значению (Tempest Texture2d ref-counted — старый impl жив до освобождения снимка). Serial-путь читает то же опубликованное → идентичность.
2. **Recycle-тег (1b):** `Resources::recycle` из потока записи тегируется кадром записи, а не глобальным `recycledId`; bin освобождается после fence своего кадра. Инвариант порядка submit сохраняется.
3. **Поток записи (1c):** выделенный std::thread (НЕ Workers — пул не реентерабелен, позы тика dispatch'ат в него). Fork после prep+publish+commits; поток записи пишет команды, делает submit N и present N (все present в одном потоке записи, в порядке кадров; главный поток present не делает). Главный: join перед prep N+1; fence/resetRecycled как сейчас. Латентность не растёт: present N сразу после submit N, как сегодня. Исключение swapchain из потока записи пробрасывается главному на join. Offscreen — present пропускается обоими путями. UI/инвентарь: снимок items на prep, либо отдельный буфер на главном — решить при реализации.
4. **Выключатель:** `OPENGOTHIC_RENDER_PIPELINE=1`, по умолчанию ВЫКЛ. Serial-путь = текущий код.
5. **Порядок команд внутри записи не меняется** (один поток записи пишет ту же последовательность) — трасса записи совпадает по построению; проверяется пиксельным контролем и паритетом, полную трассу не строим.

### Нарушения инварианта (аудит, ранжировано)

- **Снимки (тик/стриминг пишут):** cmd/ord/maxPayload/firstPayload (drawcommands.cpp:703-841,439,428; писатели commandId/addClusters 271-320, visualobjects.cpp:443,647,708,893-905, landscape.cpp:46); bucketsCpu+currentFrame+desc (drawcommands.cpp:181-234; писатели alloc/invalidate drawbuckets.cpp:60-105, commit:320); clusters size (drawcommands.cpp:439,635; писатели updateInstance/alloc/free); objects/impostorDraws (visualobjects.cpp:1413-1427); lightSourceData.size() (renderer.cpp:1513, renderer_vsm.cpp:125,155,189, renderer_gi.cpp:92); pfx bucket list (pfxobjects.cpp:129-171, getBucket:173-201); текстурные хендлы (resources_textures.cpp:1086 vs drawcommands.cpp:214).
- **Prep-перенос (читают игру/пишут общее, оставить на главном до fork):** blood.prepare (blooddecals.cpp:229), grass prepare (grassdraw.cpp:46-201), updateRtScene/updateLights/prepareLightsBvh, prepareTaa/Motion/Camera/Eye/FogSky/Uniforms/preFrameUpdate/prepareGlobals, cloth setVisibilityTest (renderer.cpp:960), prepareSky/prepareHistory/prepareSSAO/prepareGi/prepareSurfels/prepareFog/prepareExposure/prepareFogSkyVis/prepareEpipolar, drawVsm CPU-часть.
- **Порядок/освобождение:** submit N < submit N+1 (очередь одна, in-place патчи); present N после submit N и до fork N+1 (vswapchain.cpp:516-548,607 acquire в present); recycle-бин и retireTexture зависят от порядка submit; `Sprite::pageRawData` waitIdle при смене атласа (вне задачи, сериализует UI).
- **Вне задачи (аудит):** PfxObjects::draw*/tick не берут sync, который берут писатели PfxSlot (латентная гонка сегодня); Shaders::materialPipeline/impostorPipeline создаются лениво без замка (гонка+спайк при записи из потока) — для конвейера нужен замок или предсоздание.

## Результат v1 (пара OFF perf-20261126T134741Z-57059 / ON perf-20261126T135718Z-69409, одна сборка LTO)

Оценка до замера была завышена: из ~1,65 мс encode на главном остаётся подготовка (world_pre_frame 0,572 + storage_commit 0,184 + clusters_commit 0,085 и прочее), перекрытию подлежит только запись ~0,8 мс минус синхронизация. Замер подтвердил: рынок −0,713, лес −0,361 мс p50. Порог приёмки скорректирован до ≥0,5 мс рынка (запись полностью скрыта тиком).

| p50 кадра, мс | OFF | ON | Δ |
|---|---:|---:|---:|
| рынок | 5,836 | 5,123 | −0,713 |
| лес | 5,599 | 5,238 | −0,361 |
| рынок mean | 6,036 | 5,374 | −0,662 |
| лес mean | 5,663 | 5,278 | −0,385 |

Зоны (рынок): `encode_main` (prep на главном) 1,076→1,068; `encode_record` 0,652→0,006 на главном (запись ушла на поток; её время в зоны главного не атрибутируется — перекрытие видно по кадру); `submit` 0,050→0,001, `present` 0,127→0,005 на главном; `fence_poll` 0,018→0,089 (кадр быстрее, местами ждём GPU). Запись ~1,3 мс полностью скрыта тиком N+1 (~2,8 мс). Выигрыш меньше оценки 1,2–1,5 из-за квоты заявки 4 ядра (главный + поток записи + пул поз + uploadTh).

Идентичность: serial-путь публикации против main+C1 — интерьер mean|d| 0,073, px>8 = 0%. OFF vs ON: интерьер 0,06 (идентичен), рынок 0,52 (px>8 0,20%), лес 1,84 (px>8 6,21%) — фаза ветра/светляки между прогонами; кадры структурно идентичны, порчи нет. **Проверка именно конвейерного кадра:** при `shot` и включённом конвейере capture читает пиксели кадра, который записал и подал поток записи (join + wait fence + readPixels представленного кадра), а не перерисовывает serial-путём; filming (`rec`) остаётся serial, чтобы кадры видео не алиасились с offscreen-целью.

Дефект, найденный и исправленный при отладке: `publish()` разыменовывал `cur` до первой публикации (nullptr на первом кадре) → SIGSEGV; символизация через LTO/ASan давала ложный след (деструктор вектора в commit). Исправлено на `clusters.size()`.

Суточный паритет OFF/ON и боевой e2e m5b с ON — в работе, результаты дописываются до мержа.

## Приёмочные проверки (после правок ревью)

- Суточный паритет OFF (run 20261126T140753Z) / ON (run 20261126T142454Z), headless lockstep, seed 73106: 304 028 NPC-выборок, 1052 NPC, same ZS/TA/HP/inventory 100%, ошибка позиций 0,00 м, `--require-journal-identical true --require-done true --require-full-length true` + допуски -6c → exit 0. Симуляция конвейером не изменена.
- Боевой e2e `m5b` с `OPENGOTHIC_RENDER_PIPELINE=1` на Mac (Ryzen-база без program records для storygraph offline): код выхода 0, случай отыгран целиком (волк, Ксардас, бегство, триггеры DIR_M5B_PROTECT_HERO).
- Пара perf OFF (perf-20261126T141648Z) / ON (perf-20261126T143306Z): рынок p50 5,728→? (ON market/forest валидированы; shot windowed упал на касте swapchain — исправлено: конвейерный readback только offscreen, windowed capture serial). Пиксели конвейерного кадра проверяются offscreen-парой shot.

## Правки по ревью Codex (PR #1845)

- P1: публикация бакетов инвалидируется сменой generation текстур (`publishTexture` меняет holder без alloc/invalidate) — иначе устаревшие mip/placeholder даже в serial.
- P1: join записи перенесён ДО capture-блока (screenshoot перерисовывает и переопубликовывает состояние) и добавлен в `resizeEvent` до сброса swapchain/offscreen-цели.
- P2: underwater-флаг публикуется на prep (`prepInWater`), запись не читает живую камеру.
- P2: profiling-контекст передаётся потоку (`captureContext/adoptContext/dropContext`, thread-local override): скоупы и GPU-сабмиты записи атрибутируются кадру N; records живут вечно, UAF нет.
- P2: capture при включённом конвейере читает представленный конвейерный кадр (см. выше); filming — serial.
- P2: оценка выигрыша и порог приёмки пересчитаны по записи-без-подготовки (см. выше).
- Mac-сборка: `optional<Encoder>` делал деструктор MainWindow throwing (Encoder dtor noexcept(false)) — заменён на сырой указатель с unique_ptr в потоке записи.

## Поэтапный план реализации и оценка

Проект больше партии: ~20 нарушений инварианта в 10+ файлах. Этапы, каждый за выключателем и со своим замером:

- **1a. Слой публикации (serial-идентичный).** COW-снимки на commit/prep: `DrawCommands::cmd/ord/maxPayload` (ord — указатели в cmd, пересобрать в снимке), `DrawBuckets::bucketsCpu` + `currentFrame`→fId + `desc` per-fId (Material-хендлы копировать `Texture2d` по значению — ref-counted, старый impl жив до освобождения снимка; меши/буферы — сырые указатели, lifetime через recycle-бины), `DrawClusters` size, `LightGroup` count, список pfx-бакетов + их текстурные хендлы, impostor-список (CPU-чтение `objects` вынести из `drawImpostors` в prep). Запись читает опубликованное; serial-путь тоже → идентичность проверяется shot-парой с архивом main. Цена публикации в стабильных кадрах ~0 (COW), в кадры стриминга ~0,1 мс копии.
- **1b. Recycle-тег кадра записи** + замок ленивого создания PSO (`Shaders::materialPipeline/impostorPipeline`) — иначе гонка и спайк при записи из потока.
- **1c. Поток записи и гейты порядка.** Выделенный std::thread; fork после prep+publish; submit N на потоке записи; главный: present N после submit N и до fork N+1, join перед prep N+1; fence/resetRecycled как сейчас. Capture/bench-пути — serial.
- **Замер этапа 1:** пара OFF/ON perf `market forest shot` + суточный паритет ×2 + `m5b` ON (бюджет выше). Ожидание: encode главного потока ~0, p50 −1,0…−1,5 мс; H ≤ 0,25 мс.
- **Этап 2 (только если этап 1 принят):** вынос части prep с главного потока / параллелизм внутри записи — отдельное решение и бюджет.

Риск и трудоёмкость: этап 1 — основной объём (публикация + порядок), оценивается в несколько сессий; латентные гонки аудита (PfxObjects sync, ленивые PSO) чинятся в 1b как предусловие. Если публикация окажется дороже выигрыша (копии в кадры стриминга), этап отклоняется по тому же правилу, что C2.

## Приёмка

- Цена: пара OFF/ON perf `market forest shot` (2 заявки, одна сборка): p50 полного кадра ON ниже OFF не менее чем на 0,8 мс; зона encode главного потока ON ~0; сумма зон не выросла (H ≤ 0,25 мс).
- Идентичность: shot-кадры структурно идентичны + интерьер-контроль mean|d| ≈ 0 (как C1); суточный паритет NPC `--require-journal-identical` (симуляция не тронута) — 2 безэкранных прогона batch на dev-ubuntu.
- Бой: один боевой e2e с флагом ON, проходящий на main: `m5b` (группа OFF, бой волка и захваченного Ксардаса, без квоты модели). `society-melee-handoff` на main падает — не брать.
- Трасса записи (по рецепту E): последовательность типов команд/барьеров OFF и ON совпадает — v1 проверяется косвенно (одна и та же функция записи, один поток) + пиксельный контроль; полную трассу не строим (механизм не предполагается задачей).

## Бюджет (точное число прогонов и минут очереди)

- 2 perf-заявки `market forest shot` (сборка ~10 мин + прогоны ~15 мин + очередь ≤30 мин каждая) = ~50 мин очереди.
- 2 суточных паритета batch dev-ubuntu (~30 мин каждый, параллельно в пределах профиля) = ~30 мин.
- 1 боевой e2e ON (~5 мин) = ~5 мин.
- Итого 5 прогонов, ~85 мин очереди. Сверх — спросить пользователя.

## Литература и ловушки (внешний поиск недоступен; помечено как известное)

- UE4/UE5 render thread: игровой и рендер-поток разнесены на кадр; типичная цена — +1 кадр латентности рендера; у нас латентность не растёт (tick N+1 стартует раньше, submit/present в прежнем порядке относительно кадров).
- Frostbite/Our Machinery parallel rendering: запись на нескольких потоках требует per-thread пулов и снимка состояния; вторичные command buffers внутри render pass требуют inheritance (dynamic rendering: VkCommandBufferInheritanceRenderingInfo). v1 не использует secondary buffers — один PRIMARY на кадр, как сейчас.
- Ловушки из аудита Tempest (C1): кэши PSO/descriptor/bindless под глобальными замками (контеншн, не гонка); ResourceState per-buffer не видит чужие чтения — барьеры остаются у одного писателя кадра; vkResetCommandPool требует внешней синхронизации пула (пул на буфер, буфер на слот — писатель один).
- Ловушка квоты gq (C2): заявка perf --threads 4, CPUQuota 400%; поток записи + главный + пул поз + uploadTh конкурируют за 4 ядра — перекрытие может не дать полного выигрыша на стенде; замер честен для стенда, на машине без квоты больше.

## Ограничения

- v1 (реализовано): `OPENGOTHIC_RENDER_PIPELINE=1`; поток записи пишет команды кадра, submit и present; главный поток делает join перед prep следующего кадра (после tick/finishStep/стриминга/capture). Serial-фallback на кадр при capture, активном видео, открытом инвентаре и не-Idle загрузке. Инвентарь/видео не снимаются на поток (items пишутся UI-потоком) — отдельный снимок или буфер отложен.
- **Выгрузка мира во время записи не защищена:** tick N+1 может удалить world, пока запись N читает wview. Условие pipeFrame проверяет Idle на момент fork, но не переход в загрузку после fork. Для приёмочных прогонов (perf, m5b) переходов нет; перед включением по умолчанию нужен join на выгрузке (отдельная правка).
- GPU-cull травы (`OPENGOTHIC_GRASS_GPU_CULL=1`, выключен по умолчанию): после переноса prepareGrass в prep его cull-dispatch записывается до buildHiZ и читал бы пирамиду прошлого кадра. Если когда-то включат по умолчанию — разделить prepare/cull.
- На history-кадрах grass prepare выполняется дважды (prep + prepareHistory): список идентичен, пиксели те же, сдвигается только каденс stats wasteFrame.
- Metal: запись на другом потоке допустима, но синхронные ожидания upload в encode_main (MtBuffer::update) останутся на потоке записи — там другой фикс; v1 проверяется на Vulkan, Metal не трогаем (serial-путь по умолчанию).
- Capture/bench-пути (shot/rec/treeProbe/captureLandscapeDepth) читают кадр синхронно — v1 оставляет их на serial-пути (флаг не действует при capture), иначе readPixels ждёт GPU внутри записи.
- Стриминг и recycle главного потока во время записи N: recycle-тег кадра записи (1b) освобождает ресурсы после fence своего кадра; alloc/realloc instanceMem меняет dataGpu только на commit N+1 (после join).