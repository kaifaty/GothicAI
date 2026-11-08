# GPU Apple M1 (G13) и инструменты Metal: как мерить регистры и занятость кернела и что оптимизировать в шейдерах

Исследование 2026-11-08, Mac M1 Pro (`metal-arch --default` → `applegpu_g13s (Apple M1 Pro)`), macOS 27.0.1, Metal toolchain 32023.921 (MetalToolchain v27.1), xctrace 27.0. Игра не запускалась, трассы не писались, код репозитория не менялся. Все локальные пробы — компиляция одного кернела без dispatch, в scratchpad.

Метки достоверности в каждой строке:
- **[док]** — документация, WWDC или Tech Talk Apple;
- **[файл Apple]** — файлы, которые Apple поставляет в macOS или Xcode (определения счётчиков, строки в бинарниках), прочитанные мной;
- **[RE]** — реверс-инжиниринг сторонних авторов (Asahi Linux, Dougall Johnson, Philip Turner);
- **[замер]** — мой локальный замер компилятором Apple на этом Mac, код пробы приведён в разделе 1;
- **[код]** — код этого репозитория;
- **[вывод]** — моё умозаключение, не проверенное замером кадра.

Главный результат. Реальное число регистров любого кернела можно получить за секунды, без Xcode и без игры: приватная опция пайплайна `1<<18` заполняет `MTLComputePipelineReflection.performanceStatistics`. У `ssao.comp` на G13S: **36 временных 32-битных регистров, 59 uniform-регистров, 0 байт спилла, 515 инструкций, 3344 байта threadgroup-памяти**. Гипотеза «SSAO упирается в регистры» в прямом виде неверна. До переноса в регистры потолок занятости ставила threadgroup-память (около 36 SIMD-групп на ядро), регистры дают потолок около 46 (вывод из RE-данных о размере регистрового файла). Вся история замеров проекта укладывается в картину «занятость важна до ~30–36 SIMD на ядро, выше — насыщение». Дальше выигрыш надо искать в цепочке задержек на выборку (чтение → ожидание → чтение-изменение-запись разделяемой памяти → барьеры) и в расходе по итерациям адаптивного цикла, а не в регистрах.

## 1. Как получить реальное число регистров, ограничитель занятости и цену по участкам кода этого кернела на этом Mac

### Takeaway
Работают три пути. (а) Приватная опция `MTLPipelineOption` бит 18 возвращает словарь статистики компилятора: регистры, спилл, состав инструкций, YAML-ремарки LLVM с числом инструкций по базовым блокам. Это те же данные, что показывает Xcode в Pipeline Statistics. (б) Xcode GPU Frame Capture, Pipeline Statistics: документированный путь, с Xcode 13 показывает регистры по PSO. (в) Счётчики M1 из определений Apple в `AGXMetalStatisticsExternalA14X-derived.js`: Compute Occupancy, лимитеры ALU/IC/текстур/кэша, Kernel Predicated Out ALU %. Дизассемблировать нативный код инструментами Apple нельзя: в `metal-objdump` нет принтера инструкций AGX. Построчная цена в Xcode 15 документирована только для M3/A17 Pro (семейство 9).

### Cited Findings

**Что есть в тулчейне и что из этого работает**
- В тулчейне есть `metal-objdump`, `metal-nm`, `metal-readobj`, `metal-lipo`, `metal-source`, `metal-tt`, `metal-nt` (`applegpu-nt` — ссылка на `air-nt`), `metal-binary-perf`, `metal-arch`; `metal-dis` и `gpuarchiver` нет — [замер]
- `metal-objdump --version` перечисляет цели `agx1`, `agx2`, `agx3`, но `metal-objdump -d` на G13S-бинарнике пишет `no instruction printer for target agx2---macho`. Дизассемблер вырезан из сборки — [замер]
- Двоичный архив пайплайнов Tempest (`~/.cache/gothic-ai/base/derived/pipelines/metal-01fce0269073bfea.bin`, 5,9 МБ) — толстый файл из двух срезов: `air64_v29` (MetalLib с AIR) и `applegpu_g13s` (`Mach-O 64-bit GPU executable`). В нативном срезе секции `__reflection`, `__vertex` (73 КБ), `__fragment` (247 КБ), `__compute` (480 КБ), `__descriptor`, `__metallib`. Скопировать срез: `metal-lipo -thin applegpu_g13s` — [замер]
- `metal-source --extract=metal-scripts --flatbuffers=json <архив>` достаёт из архива скрипт пайплайнов `.mtlp-json`: 100 compute и 51 render пайплайн, с полями `max_total_threads_per_threadgroup: 64`, `threadgroup_size_is_multiple_of_thread_execution_width: false` и другими. Описатели всех пайплайнов игры можно получить без её запуска — [замер]
- `metal-tt -arch applegpu_g13s -macos-version-min 27.0 -o out.metallib lib.metallib script.mtlp-json` собирает нативный G13S-бинарник офлайн. Внутри он вызывает `applegpu-nt ... -N script.mtlp-json` — [замер]; описание стадий — в `man metal-tt` и `man metal-nt` тулчейна — [файл Apple](file:///var/run/com.apple.security.cryptexd/mnt/com.apple.MobileAsset.MetalToolchain-v27.1.266.1.0asmHp/Metal.xctoolchain/usr/share/man/man1/metal-nt.1)
- `metal-nt -S` (выдача ассемблера) отказывает: `[AGX] Plugin interface not implemented: AIRNTEmitAssembly` — [замер]
- В плагине компилятора `libapplegpu-nt.dylib` есть строки комментария `// Function stack metrics: GPR spills=… bytes, other stack=… bytes, instructions=…, basic blocks=…, temp regs=…, flag regs=…, uniform regs=…, coeff regs=…`, статического симулятора AGX3 (`AvgGPRDynPressure … 32-bit GPRs per Thread`, `MeanOccupancyRequirement`) и опции `agx2-set-spill-to-lm-tgt-occupancy-limit` («Occupancy limit in percent over which the shaders are not even considered for spilling to LM») — [файл Apple] (строки бинарника). Компилятор умеет спиллить регистры в локальную (threadgroup) память, а не только в стек
- Переменные окружения компилятора: `AGC_ENABLE_STATUS_FILE`, `AGC_STATUS_LOG_DIRECTORY`, `AGC_DUMP_MACHO`, `AGC_TEMP_REGS_IN_BYTES`, `AGC_SHARED_REGS_BYTES_COMPUTE`, `AGX_PER_THREAD_LM_SPILL_BYTES` и всего 104 имени — [файл Apple]. `AGC_ENABLE_STATUS_FILE=1` пишет в `$HOME/Library/Logs/AGXCompilerHost/` IR, флаги fast-math и «ключ компиляции» (`maxTotalThreadsPerThreadgroup`, `genCompilerStatistics`, `maxTempRegisters` и др.). Но на этой версии компилятор падает с SIGSEGV (rc=139) сразу после стадии generic IR, до регистров — [замер]. Путь тупиковый
- В фреймворках Xcode (`GPUToolsCore`, `GPUDebugger.ideplugin`) есть имена `MTLPipelineOptionPrivateCompileTimeStatistics`, `MTLPipelineOptionPrivatePerformanceStatistics`, `processPipelinePerformanceStatistics:…`, тексты «Max theoretical occupancy», «Low Occupancy Due To Resources Pressure», «Reduce registers/threadgroup memory usage to increase your occupancy.», `Bottlenecks_RegisterSpill`, а в `GTMTL4PipelineOptions` — поле `maxNumRegisters` — [файл Apple] (строки бинарников)
- **Рабочий путь.** `newComputePipelineStateWithDescriptor:options:(1<<18) reflection:` заполняет `reflection.performanceStatistics` словарём с ключами: `Temporary register count`, `Uniform register count`, `Spilled bytes`, `Thread invariant spilled bytes`, `Constant calculation phase present`, `Constant calculation temporary register count`, `Instruction count`, `ALU instruction count`, `FP32/FP16/INT16/INT32 instruction count`, `Branch instruction count`, `Texture reads/writes instruction count`, `Wait instruction count`, `Threadgroup load/store/atomic instruction count`, `Device load/store/atomic instruction count`, `ComputeBufferPrefetch`, `Compilation time in milliseconds`, `Remarks` (YAML ремарок LLVM) — [замер]. Бит 24 в том же переборе обрушил `MTLCompilerService` (`XPC_ERROR_CONNECTION_INTERRUPTED`): перебирать биты не стоит, нужен только 18
- Xcode 13 в GPU frame debugger показывает число регистров для каждого render, tile и compute PSO — [док, WWDC21 10153](https://developer.apple.com/videos/play/wwdc2021/10153/)

**Числа SSAO и соседей на G13S** — [замер], MSL взят из кэша Tempest `~/.cache/gothic-ai/base/derived/msl/msl-c8f708e359624f87.bin` (собран 2026-11-07 23:18, версия с упаковкой `hitV` в `packHalf2x16`, 13×64 слов):

| Кернел | temp (32 бит) | uniform | спилл, Б | TG-память, Б | инструкций | FP32 / FP16 / INT16 | чтений текстур / ожиданий | TG load / store / atomic |
|---|---:|---:|---:|---:|---:|---|---|---|
| `ssao.comp` | **36** | 59 | 0 | **3344** | 515 | 209 / 80 / 57 | 7 / 4 | 6 / 18 / 3 |
| `ssao_blur.comp` | 36 | 25 | 0 | 1808 | 302 | 103 / 15 / — | 2 / 2 | — |

У SSAO есть «фаза вычисления констант» на 24 временных регистра и 80 инструкций (`agc.main.constant_program`): это пролог, который считает однородные для всех потоков выражения и кладёт их в uniform-регистры. `ComputeBufferPrefetch = promoted`: push-константы (`proj`, `projInv`) загружены в uniform-регистры и потоковых регистров не занимают. Ключ `Threadgroup memory` в словаре равен 0, а `staticThreadgroupMemoryLength` у PSO — 3344: статическую память словарь не считает. Ремарки регистрового аллокатора: `LoopSpillReloadCopies` (2 копии в цикле), `SpillReloadCopies` (9 копий в функции). Цикл инициализации на 13 итераций развёрнут целиком, цикл на 4 выборки развёрнут частично, в 2 раза — [замер]

**Число инструкций по базовым блокам SSAO** (ремарки `asm-printer/InstructionMix`), это замена построчной цены на M1 — [замер]:

| Участок кода | инструкций |
|---|---:|
| пролог констант (`constant_program`) | 80 |
| `main`: вход (размеры, `depthPix`, центр глубины, `unproject`) | 60 |
| `if(valid)`: `reconstructNormal` + базис `tangent` | 80 |
| выборка: Хаммерсли, `sqrt`×2, `sin`/`cos`, `tangent*h`, `project` (`for.body.i`) | 46 |
| выборка: `readZ` и проверка попадания (`if.end33.i`) | 12 |
| попадание: `unproject`, расстояние, `addHit` с чтением-записью `hitV` (`if.then37.i`) | 52 |
| промах: `addMis` (`if.else38.i`) | 22 |
| вторая выборка развёрнутой пары: подготовка (`cleanup.i`) | 46 |
| `isSolved` с атомиками и барьерами, на итерацию | ~30 |

Итого на поток: постоянная часть около 140–220 инструкций, на выборку 80–110, а в расходящейся SIMD-группе, где выполняются обе ветки, до ~132 — [замер + вывод]

**Калибровка единиц и порогов** — [замер]: кернел держит N живых значений через цикл.

| Живых значений | temp float | maxT float | temp half | maxT half |
|---:|---:|---:|---:|---:|
| 16 | 24 | 1024 | 12 | 1024 |
| 32 | 40 | 1024 | 20 | 1024 |
| 64 | 72 | 704 | 36 | 1024 |
| 96 | 104 | 512 | 52 | 1024 |
| 128 | 128 + спилл 192 Б | 384 | 68 | 768 |
| 160 | 128 + спилл 416 Б | 384 | 84 | 576 |
| 256 | 128 + спилл 1024 Б | 384 | 128 + спилл 64 Б | 384 |

`Temporary register count` считается в 32-битных регистрах, половинная точность занимает половину. Максимум — 128 регистров (256 половинок), дальше спилл. Ступени `maxTotalThreadsPerThreadgroup` совпадают с таблицей Alyssa Rosenzweig, если её «регистры» считать половинками: ≤104 → 1024, 144 → 704, 192–208 → 512, 240–256 → 384 — [RE, Asahi part III](https://alyssarosenzweig.ca/blog/asahi-gpu-part-3.html)

**Свод по всем 100 compute-кернелам кэша** — [замер]: 91 кернел держит `maxT=1024` при temp ≤ 48, что совпадает с грубой проверкой в [отчёте 2026-11-07](../../../../reports/engine/2026-11-07-shader-optimisation/README.md). Выше 52 regs — лучевые и RTSM-кернелы (`vbo-ibo-…-topLevelAS`: 112–128 temp, спилл 304–704 Б), кернел `textureSm1-skyLUT-probesLightingPrev-gbufferDiff-gbufferNorm` (68 temp, 9232 Б TG), `rayData-depth-result` (72 temp), `pos-vLights-dbgImage-lightTiles` (84), `pos-morphId-…-rtsm` (84). При малом числе регистров стек используют `push-outImage-depth-gbufNormal` и `nodeData-push-outImage-depth-gbufNormal` (по 304 Б при 48 и 40 temp), `bvhData-…-gbufHit` (176 Б): это стековые массивы, а не давление регистров. Кернел `mip0…mip7` (8×8) делает 32 чтения текстуры при 32 ожиданиях, то есть ни одно чтение не перекрывается с другим

**Счётчики M1 и их формулы.** Файл определений, который Apple кладёт для G13X: `/System/Library/Extensions/AGXMetalG13X.bundle/Contents/Resources/AGXMetalStatisticsExternalA14X-derived.js` — [файл Apple](file:///System/Library/Extensions/AGXMetalG13X.bundle/Contents/Resources/AGXMetalStatisticsExternalA14X-derived.js)
- Счётчики для compute: `Compute Occupancy`, `ALU Limiter` / `ALU Utilization`, `F32 Utilization`, `F16 Utilization`, `IC Utilization` / `IC Limiter`, `SCIB Utilization`, `Texture Sample Limiter` / `Utilization`, `Texture Cache Limiter`, `Texture Cache Miss Limiter`, `Texture Cache Read Miss Rate`, `Threadgroup/Imageblock Load Limiter`, `Threadgroup/Imageblock Store Limiter`, `Threadgroup Atomic Limiter`, `Kernel Predicated Out ALU Percentage`, `Buffer Read Limiter`, `GPU Last Level Cache Limiter`, `Lossless Compressed Texture Samples %`, `Compression Ratio of Texture Memory Read`, `USC L1 Cache Miss Rate`
- `ALULimiter = (work_norm + stall_norm)·2/num_cores`, `ALUUtilization = work_norm·2/num_cores`: лимитер — это работа плюс простои
- `F32Utilization = 32·X·4/(128·num_cores)`, `ICUtilization = 32·X·4/(32·num_cores)`: у конвейера целых и сложных операций (IC) знаменатель в 4 раза меньше, чем у F32
- `ComputeOccupancy = X_norm/(768·num_cores)`; описание: «how many compute shader simdgroups are concurrently running in the GPU relative to the GPU’s maximum number of concurrently running simdgroups»
- `Kernel Predicated Out ALU Percentage` — «Percentage issued ALU operations predicated out due to divergent control flow in the compute shader»
- Определения лимитера и утилизации у Apple: «Utilization is the work done… as the percentage of the hardware block's peak… Limiter… includes both work and stalls» — [док, Tech Talk 111374](https://developer.apple.com/videos/play/tech-talks/111374/)

**Построчная цена**
- Shader Cost Graph с аннотацией каждой строки, Performance Heat Maps (стоимость, расхождение потоков, число инструкций на поток), Shader Execution History и новые счётчики занятости (`Occupancy Manager Target`, `Shader Launch Limiter`, L1 eviction rate) — инструменты Xcode 15, «available for Apple family 9 GPU», то есть M3 и A17 Pro — [док, Tech Talk 111374](https://developer.apple.com/videos/play/tech-talks/111374/)

**Код пробы** — [замер]; собирается `clang -fobjc-arc -framework Foundation -framework Metal`, на вход — MSL-файл из кэша Tempest:

```objc
id<MTLLibrary> lib = [dev newLibraryWithSource:src options:[MTLCompileOptions new] error:&e];
MTLComputePipelineDescriptor* d = [MTLComputePipelineDescriptor new];
d.computeFunction = [lib newFunctionWithName:@"main0"];
d.maxTotalThreadsPerThreadgroup = 64;                 // как в Tempest: localSize
MTLAutoreleasedComputePipelineReflection r = nil;
id<MTLComputePipelineState> p = [dev newComputePipelineStateWithDescriptor:d
    options:(MTLPipelineOption)(1ull<<18) reflection:&r error:&e];   // приватный бит
NSDictionary* s = [(id)r valueForKey:@"performanceStatistics"];
// s[@"Temporary register count"], s[@"Spilled bytes"], s[@"Remarks"], p.staticThreadgroupMemoryLength
```

### Inferences
- **Модель занятости G13.** Ниже — вывод из двух RE-источников и калибровки, не документ Apple. Регистровый файл около 208 КБ на ядро ([Turner](https://github.com/philipturner/metal-benchmarks); Alyssa: «208 KiB»), до 3072 потоков (96 SIMD-групп) на ядро ([Turner](https://github.com/philipturner/metal-benchmarks): «384–3072»; [Alyssa](https://alyssarosenzweig.ca/blog/asahi-gpu-part-3.html): «24576 = 1024 * 24 simultaneous threads» на 8 ядер M1), threadgroup-памяти около 60 КБ на ядро ([Turner](https://github.com/philipturner/metal-benchmarks)). Регистры распределяются по пику всего шейдера ([док, Tech Talk 111375](https://developer.apple.com/videos/play/tech-talks/111375/)). Отсюда потолок по регистрам — ⌊212 992 / (temp·4·32)⌋ SIMD-групп, по threadgroup-памяти — ⌊61 440 / TG⌋ групп × SIMD в группе:

  | Вариант SSAO | потолок по регистрам, SIMD/ядро | потолок по TG-памяти, SIMD/ядро | итог из 96 |
  |---|---:|---:|---:|
  | до упаковки: 6400 Б + 16 | 46 | 9 групп × 2 = 18 | 18 (19 %) |
  | сейчас: 3328 + 16 = 3344 Б, 36 temp | 46 | 18 × 2 = 36 | **36 (37 %)** |
  | 8-битная сетка: ~1808 Б, 36 temp | 46 | 33 × 2 = 66 | 46 (48 %) |
  | «четыре чтения пачкой» (в моём варианте 56 temp) | 29 | 36 | 29 (30 %) |

  История проекта ложится на модель. 18 → 36 SIMD дало −0,33…−0,76 мс, 36 → 46 — ничего, 36 → 29 сделало кадр хуже на 0,4…1,8 мс. Turner отдельно пишет, что загрузка ALU «maxes out at 24 simds/core». Вывод: SSAO упирался в занятость только до ~30–36 SIMD на ядро. Дальнейшее снижение регистров или TG-памяти само по себе почти ничего не даст.
- **Что значит «28 % занятости».** Константа 768 в формуле похожа на 3072/4, то есть счёт идёт четвёрками потоков; тогда 100 % = 96 SIMD на ядро, а 28 % ≈ 27 SIMD в среднем по старому блоку compute. Это ниже потолка SSAO (36), что ожидаемо: в блоке были однопоточные по сути dispatch Irradiance и Exposure. Вывод: показатель блока нельзя приписывать SSAO.
- **Грубая цена ALU.** 1,66 млн потоков × ~3–4,5 тыс. инструкций ≈ 5–7·10⁹ поток-инструкций. Ядро выдаёт 4 SIMD-инструкции за такт (4 планировщика, [Turner](https://github.com/philipturner/metal-benchmarks)): 16 ядер × 128 × ~1,3 ГГц ≈ 2,7·10¹² в секунду, то есть ~2–2,6 мс только на выдачу при одной такт-инструкции. Сложные операции идут на конвейере IC с вчетверо меньшей пропускной способностью, а таблица выборок, снявшая часть из них, не дала ничего. Значит, ALU не главный ограничитель, но и не мелочь. Частота ~1,3 ГГц здесь не проверена.
- **Порядок измерения на этом Mac.**
  1. Пробой (бит 18) — регистры, спилл и инструкции по блокам каждого варианта до замера кадра. Это секунды и не требует свободного Mac.
  2. Счётчики, но отдельно для SSAO. После переноса SSAO и blur идут своим compute-энкодером перед картами теней (`renderer.cpp:926`), поэтому в Metal System Trace их можно читать отдельно от остального compute. Для разделения SSAO и blur на время замера можно временно поставить конец энкодера между ними.
  3. Читать пары лимитер/утилизация: ALU, IC, Texture Cache Miss Limiter, Threadgroup Load/Store/Atomic Limiter, Kernel Predicated Out ALU %, Compute Occupancy.
  4. Вариант, который поднимает регистры, проверять по таблице потолков выше: при TG = 3344 Б регистры можно поднимать до ~46 temp без потери занятости.
- **Полезный флаг в Tempest.** `OPENGOTHIC_PSO_STATS=1`: в `MtCompPipeline` и `MtPipeline` при промахе кэша PSO создавать пайплайн с битом 18 и писать в журнал регистры, спилл и TG-память. Это закроет фрагментные шейдеры, которых моя проба не касалась. Только для отладки: опция приватная.

### Gaps
- Нативного дизассемблера G13 от Apple нет. Секцию `__compute` можно было бы разобрать дизассемблером [applegpu](https://github.com/dougallj/applegpu) (Python), но я не запускал сторонний код. Неизвестно, хватит ли его разметки, чтобы найти границы функций в Mach-O Apple.
- Поддерживает ли бит 18 render-пайплайны (`MTLRenderPipelineReflection.performanceStatistics`), не проверено.
- Есть ли на G13 предел числа одновременных threadgroup на ядро, не нашёл. Если есть, группы 8×8 (2 SIMD) могут упираться в него раньше памяти.
- Как часто выполняется «фаза констант» (раз на dispatch, раз на группу или иначе), у Apple не описано. В Mesa для AGX есть пролог (preamble), который пишет uniform-регистры ([Mesa MR 20562](https://gitlab.freedesktop.org/mesa/mesa/-/merge_requests/20562), [MR 272](https://gitlab.freedesktop.org/asahi/mesa/-/merge_requests/272)), но частоту выполнения на AGX явно не нашёл.
- Работает ли построчный профайлер Xcode (Shader Cost Graph) на M1, Apple не пишет; документировано только семейство 9.
- Размер текстурного L1 на G13 не нашёл. У Turner есть только 8 КБ L1 данных на ядро.

## 2. `maxTotalThreadsPerThreadgroup`, `[[max_total_threads_per_threadgroup]]`, размер группы, SIMD 32 и квадры

### Takeaway
Объявленный предел влияет на распределение регистров только у тяжёлых шейдеров: компилятор не превышает бюджет, который даёт заказанное число потоков. У SSAO при 36 регистрах значения 32, 64, 128 или 1024 ничего не меняют. `threadGroupSizeIsMultipleOfThreadExecutionWidth` код SSAO не меняет. Размер группы важен не через регистры, а через барьеры (сколько SIMD ждут друг друга), TG-память на группу и форму SIMD (8×4 пикселя у группы 8×8).

### Cited Findings
- Tempest задаёт `setMaxTotalThreadsPerThreadgroup(localSize)` и `setThreadGroupSizeIsMultipleOfThreadExecutionWidth(false)` для каждого compute-пайплайна (`mtpipeline.cpp:214–215`) — [код](../../../../OpenGothic/lib/Tempest/Engine/gapi/metal/mtpipeline.cpp)
- Apple: при 0 Metal сам считает предел «based on the device's capabilities and the compute shader's memory usage»; значение должно совпадать с атрибутом `[[max_total_threads_per_threadgroup]]`; «Metal may lower the thread limit… which can reduce runtime performance». О распределении регистров страница молчит — [док](https://developer.apple.com/documentation/metal/mtlcomputepipelinedescriptor/maxtotalthreadsperthreadgroup)
- Кернел с 128 живыми float без предела получает 128 temp (со спиллом) и `maxT=384`; с пределом 512 — 104 temp и `maxT=512`; с пределом 256/64/32 — 104–124 temp. Кернел с 96 float: 104 temp без предела и при 512, но 80 при 64 и 32 — [замер]
- SSAO при пределе 32, 64 и 128 — те же 36 temp; при 32 на 5 инструкций меньше (510 против 515). `threadGroupSizeIsMultipleOfThreadExecutionWidth = YES` не меняет ни регистров, ни числа инструкций SSAO и blur — [замер]
- В SDK macOS 26 появилось `requiredThreadsPerThreadgroup` у `MTLComputePipelineDescriptor`, `MTLFunction`/`MTLLibrary` и тайловых пайплайнов (`MTLComputePipeline.h:151`, `API_AVAILABLE(macos(26.0))`) — [файл Apple](file:///Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/Metal.framework/Headers/MTLComputePipeline.h)
- SIMD-группа — 32 потока; на ядре 4 планировщика, каждый выдаёт за такт одну инструкцию одной SIMD-группы — [RE, Turner](https://github.com/philipturner/metal-benchmarks)

### Inferences
- **Предел.** Для 8×8 Tempest уже передаёт 64. Опускать предел SSAO смысла нет. Поднимать регистры (например, ради чтения на шаг вперёд) можно до ~46 temp, и занятость не упадёт, пока потолок держит TG-память 3344 Б (раздел 1).
- **8×4 (одна SIMD на группу).** Барьер `threadgroup_barrier` синхронизирует одну SIMD, компилятор срезал 5 инструкций. TG-память группы падает вдвое (1680 Б), а групп на ядре вдвое больше — тот же потолок 36 SIMD. Плюс: не ждём вторую SIMD на каждом из 26 барьеров. Минус: может проявиться неизвестный предел числа групп на ядро. Если барьеры оставляем, это дешёвый опыт.
- **16×8 или 32×1.** 16×8 — 4 SIMD ждут друг друга на каждом барьере, TG на группу удваивается, потолок прежний: хуже. 32×1 — SIMD становится строкой 32×1, и выборки глубины теряют двумерную локальность: хуже для кэша. Для SSAO оставить 8×8 или перейти на 8×4.
- **Квадры в compute.** В Metal квадра — 4 последовательных потока SIMD-группы. При группе 8×8 и линейном индексе x + 8y это полоса 4×1 пикселя, а SIMD — блок 8×4. Это вывод из линейной нумерации, в документации я его не проверял.
- **`requiredThreadsPerThreadgroup`** точнее предела: компилятор знает точный размер. Пользы от него я не замерял, а API доступно только с macOS 26.

### Gaps
- Почему у кернела с 96 float при пределе 64 регистров стало меньше (80 против 104), а не больше: это эвристика планировщика, документации нет.
- Есть ли выигрыш от `requiredThreadsPerThreadgroup`, не проверено.

## 3. 16 бит: `half` и `short` на G13, курсы и конверсии; переводить ли части SSAO в `half`

### Takeaway
На M1 fp16 и fp32 FMA идут с одной скоростью, выигрыш fp16 — только в регистрах и памяти. Значение `half` занимает ровно половину 32-битного регистра, конверсии между 16 и 32 битами, по RE и Apple, обычно бесплатны. Для SSAO сейчас fp16 почти ничего не даст: регистры не ограничивают (потолок держит TG-память), а компилятор уже держит индексы в 16-битных целых (57 инструкций INT16). fp16 стоит тратить на кернелы и фрагментные шейдеры с temp > 52, если они идут в кадре по умолчанию.

### Cited Findings
- M1 и A15: F16 FMA — 256, F32 FMA — 256 операций за такт ядра; F16 add — 128, F32 add — 128. На A14 F32 FMA вдвое медленнее (128) — [RE, Turner](https://github.com/philipturner/metal-benchmarks)
- Задержки и пропускная способность в тактах: FFMA16/FFMA32 — ~2,2; RECIP32 — 6; RSQRT32 и SQRT32 — 8; SIN32 — 14,28; SIN16 — 13,56; IMUL32 — 4; CONVERT(F→I32) — 4 — [RE, Turner](https://github.com/philipturner/metal-benchmarks)
- «Free conversions between 16-bit and 32-bit»; «The optimizer folds size conversion instructions into uses and definitions»; «every thread can access up to 256 half-word registers» — [RE, Alyssa](https://alyssarosenzweig.ca/blog/asahi-gpu-part-3.html)
- Регистры `r0…r127` с половинками `r0l…r127h`, uniform-регистры `u0…u255` с половинками; у 32-битных результатов денормали сбрасываются в ноль, у 16-битных нет — [RE, Dougall Johnson](https://dougallj.github.io/applegpu/docs.html)
- «Half and short types also require less energy and might achieve higher peak rates… type conversions are usually free»; `ushort` хватает для позиций в группе и для сетки до 8K–16K — [док, WWDC21 10153](https://developer.apple.com/videos/play/wwdc2021/10153/)
- FP16 «use fewer registers than their FP32 equivalents» и снижает трафик буферов — [док, Tech Talk 111375](https://developer.apple.com/videos/play/tech-talks/111375/)
- 32 живых `half` — 20 temp, 32 `float` — 40 temp; 96 `half` — 52 temp и `maxT=1024`, 96 `float` — 104 temp и `maxT=512` — [замер]
- В SSAO уже 80 инструкций FP16 и 57 INT16 из 515, в blur — 15 FP16 — [замер]. Компилятор сам сужает типы там, где может.
- Проект: TAA в `half` снял −0,33 мс, а моменты `E[c²] − mean²` в `half` ломаются; `mediump` в MSL не доходит, нужен `float16_t` — [отчёт 2026-11-05](../../../../reports/engine/2026-11-05-shader-compile/README.md)

### Inferences
- **В SSAO в `half` можно перевести** (по точности, вывод): направление `h` из полусферы, веса `distW`, расчёт ячейки `pyramidUV/pyramidId`, накопитель `occlusionAll` (сумма ≤ 25 ячеек по ≤ 2, точность 1/1024 достаточна), `f0`. **Нельзя:** глубину `z`, `at1`, `pos0`, `pos2`, `dp` (разность близких мировых координат), матрицы. Экономия — порядка 4–8 temp. При TG 3344 Б это занятость не поднимает; поднимет, только если TG-память уйдёт ниже ~2,6 КБ (тогда потолок задают регистры). Отдельно не делать.
- **Где fp16 окупится.** Кернелы с temp > 52 из свода, если они в кадре по умолчанию: пробы освещения (68 temp, 9232 Б TG), `rayData-depth-result` (72), освещение тайлов (84). И особенно фрагментные `light.frag` и `main.frag`, которые пока не измерены. Сначала флаг `OPENGOTHIC_PSO_STATS` или Xcode Pipeline Statistics.
- **Сами `sin`, `cos`, `sqrt`** стоят 8–14 тактов на конвейере IC, но таблица выборок их уже сняла без выигрыша. Значит, путь выборки упирается не в них.

### Gaps
- Цены конверсий half↔float в виде таблицы у Turner нет; утверждение «бесплатно» — от Alyssa и Apple, без чисел.
- О редукциях `simd_*` в fp16 на M1 (что они медленнее fp32) в этот раз первоисточника не нашёл; утверждение есть только в [отчёте проекта](../../../../reports/engine/2026-11-05-shader-compile/README.md).

## 4. Что SPIRV-Cross делает с кодом, что из этого вредит и стоит ли писать MSL руками

### Takeaway
Для SSAO код SPIRV-Cross не вредит: глобальные `Private`-переменные (`tangent`, `occlusionAll`, `hits`) становятся локальными переменными `main0` со ссылками `thread&` во встроенных функциях и полностью уходят в регистры (0 alloca, 0 спилла). Push-константы попадают в uniform-регистры. Реально теряется одно: подсказки циклов `[[loop]]`, `[[dont_unroll]]` и `[[unroll]]` в MSL не попадают, компилятор Apple решает сам (SSAO: внутренний цикл развёрнут ×2). Писать MSL руками для SSAO смысла мало: нужные приёмы (голосование SIMD, шаффлы) выражаются в GLSL через `GL_KHR_shader_subgroup_*`, и SPIRV-Cross переводит их в `simd_*`/`quad_*`.

### Cited Findings
- Tempest вызывает `CompilerMSL` с платформой macOS, `msl_version` по устройству (≤ 3.1, `mtdevice.mm:40`), argument buffers tier 2 при non-uniform indexing, `readwrite_texture_fences = false` при MSL ≥ 2.0. `newLibrary` вызывается с `MTL::CompileOptions` по умолчанию (`mtshader.cpp:27–50, 244–249`) — [код](../../../../OpenGothic/lib/Tempest/Engine/gapi/metal/mtshader.cpp)
- MSL каждого шейдера лежит на диске в кэше Tempest (`<OPENGOTHIC_BASE_CACHE>/derived/msl/msl-<хеш>.bin`, текст + привязки) — [отчёт о кэше MSL](../../../../reports/engine/2026-11-03-streaming-optimisation/msl-cache.md); в нём 100 compute-кернелов — [замер]
- MSL SSAO: массивы обёрнуты в `spvUnsafeArray<T,N>`; `hitV` — `threadgroup spvUnsafeArray<spvUnsafeArray<uint,64>,13>`; глобальные GLSL-переменные объявлены внутри `main0` и передаются как `thread float3x3& tangent` и т. п.; матрица копируется `float4x4 param = _492.proj;`; все функции `always_inline`; `fast::normalize`, `fast::max`; барьеры `threadgroup_barrier(mem_flags::mem_threadgroup)`; атомики `atomic_fetch_max_explicit` и `atomic_fetch_add_explicit` на threadgroup — [замер]
- Флаги, с которыми AGC компилирует SSAO: «Object fast-math flags: {nonan=1, noinf=1, nosignedzero=1, allowrcp=1, unsafealgebra=1}», «SimplifyGenericIR: 0 alloca's» — [замер, статус-файл AGC]
- `Uniform register count = 59`, `ComputeBufferPrefetch = promoted`, `Spilled bytes = 0` — [замер]
- Ни в одном из 100 кернелов кэша нет `#pragma` циклов, есть только `#pragma clang diagnostic`. Макросы `SPIRV_CROSS_UNROLL [[unroll]]` / `SPIRV_CROSS_LOOP [[dont_unroll]]` SPIRV-Cross выводит только в GLSL (`spirv_glsl.cpp:949–950`), в `spirv_msl.cpp` обработки `LoopControl` нет — [замер; код](../../../../OpenGothic/lib/Tempest/Engine/thirdparty/spirv_cross/spirv_glsl.cpp)
- Ремарки AGC: `loop-unroll PartialUnrolled` ×2 (внутренний цикл SSAO), `FullyUnrolled` 13 итераций (обнуление `hitV`) — [замер]
- SSAO с запретом развёртки (`#pragma clang loop unroll(disable)`) — 32 temp, 380 инструкций; с полной развёрткой — 32 temp, 778 инструкций, 9 чтений, 6 ожиданий — [замер]
- SPIRV-Cross переводит subgroup-операции в `simd_is_first`, `simd_all`, `simd_any`, `simd_prefix_inclusive_*`/`exclusive_*` и `quad_broadcast` (`spirv_msl.cpp:15575, 16525–16628`) — [код](../../../../OpenGothic/lib/Tempest/Engine/thirdparty/spirv_cross/spirv_msl.cpp)
- Опции `CompilerMSL::Options`, которые Tempest не трогает: `fixed_subgroup_size`, `emulate_subgroups`, `force_native_arrays`, `invariant_float_math` (по умолчанию false), `use_framebuffer_fetch_subpasses`, `agx_manual_cube_grad_fixup` (`spirv_msl.hpp:321–517`) — [код](../../../../OpenGothic/lib/Tempest/Engine/thirdparty/spirv_cross/spirv_msl.hpp)
- Расширения subgroup, `textureGather` или `float16_t` уже встречаются в `scalar_ops.glsl`, `rtsm/*`, `swrt/*`, `taa_resolve.frag`, `shadow_sampling.glsl`, `light.frag`, `probe_ambient.frag`, `probe_lighting.comp`, `surf_lighting.comp` — [код](../../../../OpenGothic/shader)

### Inferences
- **Потеря подсказок циклов** — единственный системный дефект пути GLSL → MSL. Лечится правкой `CompilerMSL`: в заголовке цикла с `LoopControlDontUnrollMask` выводить `#pragma clang loop unroll(disable)`, с `UnrollMask` — `unroll(full)`. Затрагивает `fog.frag` (`[[dont_unroll]]`) и `ssao.comp` (`[[loop]]`). Сам по себе выигрыш не гарантирован: у SSAO частичная развёртка — это вторая выборка в полёте при +4 регистрах, полезно.
- **Стековые массивы** — главный источник спилла в наших кернелах: 304 Б при 40–48 temp. В GLSL это массив с динамическим индексом; SPIRV-Cross его не создаёт, а переносит как есть. Проверить, какие из этих кернелов идут в кадре по умолчанию: судя по именам привязок (`push-outImage-depth-gbufNormal`, `nodeData-…`, `bvhData-…`), это трассировка, которая на Mac выключена.
- **MSL руками для SSAO** не окупится. Для blur — тоже. Выгоды ручного MSL: `texture2d<half>` (выборка сразу в `half`), `ushort2` для координат, точные `#pragma` развёртки, `simd_shuffle` с произвольным номером дорожки. Всё, кроме прагм, есть и в GLSL: `float16_t`, `GL_KHR_shader_subgroup_shuffle`, `GL_EXT_shader_16bit_storage`. Ручная копия шейдера рядом с GLSL — это два источника правды, а у нас Vulkan как контроль.
- **`invariant_float_math`** выключен — fast-math действует. Риск из [отчёта 2026-11-07](../../../../reports/engine/2026-11-07-shader-optimisation/README.md) остаётся: `nonan=1` может выкинуть проверки `isnan`.

### Gaps
- Добавлен ли учёт `LoopControl` в MSL в новых версиях SPIRV-Cross upstream, не проверял.
- Как SPIRV-Cross обходится с `GL_KHR_shader_subgroup_shuffle` и `shuffle_relative` в MSL для macOS (в `simd_shuffle` или эмуляция), по коду не проверял.

## 5. Квады и SIMD: общий доступ к соседним данным, голосование без барьеров

### Takeaway
Самый перспективный приём для SSAO — заменить `isSolved` (3 барьера и 2 атомика на итерацию, 26 барьеров на группу) и `shMaxSteps` (атомик + барьер) голосованием внутри SIMD-группы (`subgroupMax` / `subgroupAny` → `simd_max` / `simd_any`). Тогда каждая SIMD выходит из цикла сама, и барьеров в цикле нет. Регистры те же (36), инструкций меньше. Переиспользовать чтения глубины для нормали через шаффлы в нынешней форме нельзя: соседний поток половинного разрешения смотрит на 2 пикселя глубины в сторону, а `reconstructNormal` читает на 1.

### Cited Findings
- SIMD broadcast/shuffle_rotate 32 бит: пропускная способность 2,04, задержка ~3,7 такта; ballot — 2,02 / 4,27; quad broadcast/shuffle — 2,01 / ~2,8; пропускная способность шаффла внутри SIMD — 256 Б за такт на ядро — [RE, Turner](https://github.com/philipturner/metal-benchmarks)
- `icmp_ballot`/`fcmp_ballot` строят маску активных потоков с истинным сравнением и рассылают её активным потокам — [RE, Dougall Johnson](https://dougallj.github.io/applegpu/docs.html)
- Вариант SSAO с `isSolved` через `simd_max` + `simd_any` (без разделяемой памяти и барьеров в цикле): 36 temp, 495 инструкций вместо 515, TG load 3 вместо 6, TG store 16 вместо 18, ветвлений 11 вместо 12 — [замер]. Барьер `shMaxSteps` в этом варианте оставлен.
- `reconstructNormal`: `uvL/uvR = uv ∓ 2/depthSize` в координатах NDC — это ±1 пиксель глубины в текстурных координатах, а центр соседнего потока половинного разрешения — `2·depthPix` соседа, то есть +2 пикселя глубины (`ssao.comp:186–195, 226–231`) — [код](../../../../OpenGothic/shader/ssao/ssao.comp)
- `ssao_blur.comp`: группа 8×8, `shared vec2 samples[LANE_SIZE][LANE_SIZE]` и `shared uint sSsaoMax`, 1808 Б, 36 temp — [код](../../../../OpenGothic/shader/ssao/ssao_blur.comp); [замер]

### Inferences
- **Голосование SIMD вместо `isSolved`.**
  - Что меняется: критерий решения считается по 32 потокам (блок 8×4), а не по 64. При 32 потоках на SIMD это и есть естественная «группа». Правило «меньше 4 нерешённых потоков» превращается в `subgroupBallot` + `bitCount < 4` или `subgroupAny`. Вид надо проверить: шум адаптивности станет блоками 8×4 вместо 8×8.
  - Что уходит: 24 барьера, ожидание второй SIMD на каждом, 2 атомика на итерацию.
  - Цикл: условие `i < shMaxSteps` заменяется на `i < subgroupMax(maxSteps)`, барьер в начале не нужен. SIMD-группа целиком из неба (`maxSteps = 0`) выходит сразу.
  - Это один из немногих приёмов, который режет цепочку ожиданий, а не арифметику.
- **Нормаль.** Варианты:
  - (а) читать `gbufNormal` — одно чтение вместо четырёх и ~80 инструкций блока `if(valid)`. Но это шейдинговая нормаль с картами нормалей, а не геометрическая: вид меняется, принимать по паре кадров;
  - (б) база ±2 пикселя и глубина соседей через `subgroupShuffle` (дорожки ±1 по x, ±8 по y внутри 8×4), с запасным чтением на краю SIMD. Это расхождение ветвей, а экономия только в прологе, который составляет ~5 % инструкций. Не первоочередное.
- **Blur.** Горизонтальных соседей можно брать шаффлом (дорожки ±1), вертикальных — ±8, но ореол между SIMD-группами и группами всё равно требует разделяемой памяти. Выигрыш — меньше TG-трафика, не больше занятости (1808 Б — потолок 66 SIMD). Blur стоит 0,8–1,6 мс, отдельный опыт.
- **`textureGather`** в SSAO не подходит: соседи нормали не смежны (±1 пиксель через центр), а выборки полусферы случайны.

### Gaps
- Сколько стоит `threadgroup_barrier` на G13 в тактах, не нашёл ни у Turner, ни у Dougall: семантика барьера в документах applegpu помечена как `TODO()`.
- Сохраняет ли SPIRV-Cross семантику `subgroupBallot` и `subgroupMax` при неполной SIMD-группе на краю экрана, на нашем коде не проверял.

## 6. Память тайла: imageblocks, tile shaders, memoryless; что можно слить в нашем кадре

### Takeaway
Тайловая память живёт только внутри одного render-энкодера; compute-dispatch её не делит. SSAO по природе — случайный доступ с большим радиусом, ему тайл не подходит. Слить с пользой можно только проходы, которые читают тот же пиксель (поэлементные), и только в одном render-энкодере. В Tempest для Metal нет ни memoryless, ни тайловых шейдеров, ни framebuffer fetch.

### Cited Findings
- «Tile memory is actually persistent across draw passes within one render command encoder»; compute-dispatch тайловую память не делит; поэлементные фильтры — во фрагментные функции, фильтры с соседями внутри тайла — в tile shading, свёртки и scatter/gather — в compute; memoryless живёт «only within encoder lifetime»; пример Apple: слияние проходов снизило трафик памяти на 62 % (2,16 ГБ → 810 МБ на кадр 4K) — [док, WWDC21 10153](https://developer.apple.com/videos/play/wwdc2021/10153/)
- Сжатие без потерь не работает с `PixelFormatView`, линейными и буферными текстурами; для не-private текстур — `optimizeContentsForGPUAccess` — [док, WWDC21 10153](https://developer.apple.com/videos/play/wwdc2021/10153/)
- В бэкенде Metal Tempest нет ни одного упоминания `Memoryless`, tile- или imageblock-API (поиск по `gapi/metal/`) — [код](../../../../OpenGothic/lib/Tempest/Engine/gapi/metal/)
- Проект уже разбирал: G-buffer в памяти тайла невозможен без перестройки кадра (все буферы читаются позже, между GBuffer и светом стоит SSAO); прозрачное слить нельзя (`Material::Ghost` пишет глубину); слияние энкодеров Sun&Moon/Multiply/отражений/тумана выигрыша не дало — [отчёт 2026-11-07](../../../../reports/engine/2026-11-07-shader-optimisation/README.md)

### Inferences
- **SSAO в тайле — нет**: радиус выборки в пикселях (до сотен у близкой геометрии) больше любого тайла.
- **Кандидаты в кадре** (вывод):
  - туман и отражения поверх `sceneLinear`, если они читают только свой пиксель сцены: framebuffer fetch (`[[color(n)]]`, в SPIRV-Cross — `use_framebuffer_fetch_subpasses` для subpass input) уберёт чтение `sceneLinear` как текстуры. Нужны subpass input в Tempest (Vulkan) и их перевод в Metal — большая работа на 0,5–0,8 мс потолка;
  - промежуточные буферы, которые живут внутри одного энкодера, как memoryless. Таких после правки «RCAS в тонмаппинге» не осталось.
- **Imageblocks в обычных compute-кернелах** вне render-прохода, судя по WWDC21, смысла не имеют: тайловые dispatch бывают только внутри render-энкодера.
- Вывод: тайловая память сейчас даёт меньше всего на единицу работы. Её место — в будущей перестройке кадра (GBuffer + свет в одном энкодере с SSAO на прошлом кадре или на глубине из предпрохода), а не в точечных правках.

### Gaps
- Может ли тайловый dispatch (`dispatchThreadsPerTile`) в нашем кадре заменить отдельный compute-проход, конкретно не разбирал: WWDC21 этого API не показывает, спецификацию MSL по imageblocks в этот раз не читал.

## 7. Энкодеры и перекрытие: последовательный и параллельный dispatch, отслеживание опасностей, одна или две очереди

### Takeaway
В Tempest для Metal `barrier()` пустой, а синхронизацию обеспечивает автоматическое отслеживание опасностей Metal на уровне ресурсов. Compute-энкодеры создаются последовательными (serial). Перекрытие работает между энкодерами одной очереди — это показал перенос SSAO перед картами теней (−0,4…−0,5 мс). Внутри одного последовательного compute-энкодера dispatch-и не перекрываются никогда, поэтому однопоточные по сути dispatch (Exposure, Irradiance) держат весь GPU. Вторая очередь для перекрытия не нужна.

### Cited Findings
- `MtCommandBuffer::barrier(...) { // nop }` (`mtcommandbuffer.cpp:299–301`); compute-энкодер создаётся `impl->computeCommandEncoder()`, без `MTLDispatchType` (`mtcommandbuffer.cpp:386–390`); новый энкодер — на каждую смену типа (render/compute/blit) — [код](../../../../OpenGothic/lib/Tempest/Engine/gapi/metal/mtcommandbuffer.cpp)
- Буферы создаются с `MTL::HazardTrackingModeDefault` (`mtbuffer.cpp:23`), текстуры — без явного режима, то есть с отслеживанием по умолчанию; очереди — `queue` и `uploadQueue` (`mtdevice.h:313–314`) — [код](../../../../OpenGothic/lib/Tempest/Engine/gapi/metal/)
- `MTLDispatchType.serial` — «dispatch encoded commands serially during your pass», `.concurrent` — «concurrently» — [док](https://developer.apple.com/documentation/metal/mtldispatchtype)
- Отслеживаемый ресурс: Metal «automatically delays write operations until previous read operations finish, and prevents subsequent commands from running until write operations complete»; «Hazard tracking provides automatic safety at the cost of some runtime overhead»; с `untracked` синхронизировать надо самим — барьерами, fences, events — [док](https://developer.apple.com/documentation/metal/mtlhazardtrackingmode)
- Перенос SSAO перед картами теней: −0,5 мс на рынке, −0,4 в лесу, в интерьере (без карт теней) — в шуме; «Irradiance и Exposure — по одной группе на весь GPU» — [отчёт 2026-11-07](../../../../reports/engine/2026-11-07-shader-optimisation/README.md)
- Мало потоков — отдельная причина низкой занятости: ядра голодают, если работы мало для заполнения GPU — [док, Tech Talk 111374](https://developer.apple.com/videos/play/tech-talks/111374/)

### Inferences
- **Где ещё может помочь перекрытие** (вывод, по одному замеру на каждое):
  - (а) Exposure и Irradiance — однопоточные по сути dispatch — в своём энкодере, поставленном рядом с вершинной работой (карты теней, VSM, GBuffer) или перед SSAO, чтобы трекер ресурсов разрешил перекрытие. Это то же, что уже сработало с SSAO, без изменений в Tempest;
  - (б) независимые от карт теней LUT тумана (`fogLut`, `cloudsLUT`, `tLUT`) — тоже перед картами теней, отдельно от затенения тумана, которое от них зависит;
  - (в) параллельный dispatch (`MTLDispatchTypeConcurrent`) для блока compute, где идут несвязанные кернелы. Нужно реализовать `barrier()` в Tempest как `memoryBarrierWithScope`/`memoryBarrierWithResources` для такого энкодера. Это риск гонок и правка всех мест, где сейчас «работает само». Делать после (а) и (б), если они не исчерпали выигрыш.
- **Untracked и fences** стоит рассматривать не ради GPU, а там, где трекер ресурсов создаёт ложные зависимости. Например, проходы, которые пишут разные мипы или разные области одной текстуры (HiZ, `mip0…mip7`), трекер на уровне ресурса сериализует. Вывод: документация о зернистости трекера («resource») говорит именно это, но наш кадр по этому признаку я не проверял.
- **Две очереди не нужны**: перекрытие энкодеров в одном командном буфере уже работает. Вторая очередь добавит только явную синхронизацию через `MTLEvent`.

### Gaps
- Документа Apple о правилах перекрытия render- и compute-энкодеров одной очереди на Apple GPU в этот раз не нашёл; опираюсь на замер проекта.
- Перекрывает ли Metal последовательные dispatch внутри serial-энкодера при отсутствии зависимостей — по документации нет, но прямого замера в проекте нет.

## 8. Форматы текстур и сжатие: глубина для SSAO, `read` против `sample`, кэш

### Takeaway
SSAO читает полноразрешённый `Depth32F` (3456×1918 × 4 Б ≈ 26,5 МБ) в случайных точках полусферы с ближайшей фильтрацией. При этом проверка попадания уже квантует глубину до 1/32768 (15 бит). Значит, копия глубины в половинном разрешении (R32F) или 16-битная (R16Unorm для проверки, R16F линейной глубины для расстояния) сокращает рабочий набор текстурного кэша в 2–8 раз. Это самый вероятный путь к цене SSAO в лесу и интерьере, где она втрое выше. Подтверждать счётчиками `Texture Cache Miss Limiter` и `Texture Cache Read Miss Rate`.

### Cited Findings
- `zBufferFormat`: первым выбирается `Depth32F`, затем `Depth24x8`, `Depth16` (`renderer.cpp:67–76`); SSAO получает `zbuffer` с `Sampler::nearest`, результат `aoFormat = R8` в разрешении эффекта, blur — в полном (`renderer.cpp:2461–2501`, `renderer.h:239`) — [код](../../../../OpenGothic/common/graphics/renderer.cpp)
- `readZ` — `depthBuf.sample(sampler, uv, level(0.0))`; проверка попадания — `floor(z*32768.0+0.5) < floor(at1.z*32768.0)`; расстояние — через `unproject(vec3(at1.xy, z))` (`ssao.comp:135–169`) — [код](../../../../OpenGothic/shader/ssao/ssao.comp)
- «RGBA32F will be slower than FP16 variants when sampling filtered values»; меньшие форматы снижают трафик и занимают меньше кэша — [док, WWDC21 10153](https://developer.apple.com/videos/play/wwdc2021/10153/)
- L1 данных — 8 КБ на ядро, L2 у M1 Pro — 256 КБ, системный кэш M1 Pro — 24 МБ, строка кэша — 128 Б — [RE, Turner](https://github.com/philipturner/metal-benchmarks)
- Среди счётчиков M1 есть `Texture Cache Read Miss Rate`, `Texture Cache Miss Limiter`, `Lossless Compressed Texture Samples %`, `Compression Ratio of Texture Memory Read` — [файл Apple](file:///System/Library/Extensions/AGXMetalG13X.bundle/Contents/Resources/AGXMetalStatisticsExternalA14X-derived.js)
- SSAO в лесу и интерьере втрое дороже рынка; гипотеза о промахах кэша у близкой геометрии и рецепт мипов глубины (Scalable Ambient Obscurance) — в [отчёте 2026-11-07](../../../../reports/engine/2026-11-07-shader-optimisation/README.md)

### Inferences
- **Копия глубины для SSAO.**
  - Формат: R32F в половинном разрешении — без потери точности и в 4 раза меньше рабочий набор. R16Unorm — ещё вдвое меньше, проверка попадания сохраняется (16 бит ≥ 15 бит квантования), но `unproject` для веса расстояния потеряет точность вдали, потому что нелинейная глубина у дальней плоскости сжата. Компромисс — R16F линейной глубины вида: относительная точность 1/2048 для расстояния, проверку попадания вести по линейной глубине с допуском.
  - Цепочка мипов (SAO): дальние выборки читают мелкий мип, и цена перестаёт зависеть от радиуса в пикселях.
  - Создание копии — один дешёвый полноэкранный проход, а в половинном разрешении её может писать и сам blur прошлого кадра.
- **`read` против `sample`.** `texture.read(ushort2)` избавляет от нормализации координат и состояния сэмплера; это несколько инструкций на выборку, а не кэш. Вторично: делать вместе с копией глубины, в которой координаты всё равно пересчитываются.
- **D16 против D32F для половинной копии.** Если копия — цветовая цель (R16/R32F), формат глубины не важен. Делать копию буфером глубины (D16) имеет смысл, только если она нужна как вложение глубины; для SSAO не нужна.
- **Сжатие.** `gbufNormal` как R32U получает `ShaderAtomic` (`mttexture.cpp:44`) — открытый вопрос проекта, влияет ли это на сжатие. Счётчик `Lossless Compressed Texture Samples %` на проходах освещения ответит на него без документации.

### Gaps
- Размер текстурного кэша и раскладка тайлов текстур на G13 в открытых источниках не нашёл.
- Сжимается ли `Depth32F`-вложение без потерь и разжимается ли бесплатно при выборке из compute — не документировано; проверять счётчиком.

## 9. M3 и новее (семейство 9): dynamic caching — что меняется у игроков с новыми Mac

### Takeaway
На M3, M4 и новее регистры, threadgroup- и tile-память выделяются динамически по фактическому использованию, а не по пику шейдера. Статические потолки занятости из раздела 1 там не действуют: им на смену приходит Occupancy Manager, который ограничивает занятость, чтобы данные не вытеснялись из L1. Правки, которые режут цепочку задержек и число инструкций (барьеры, расход на итерацию, рабочий набор текстур), помогут везде. Правки, которые только сокращают регистры или TG-память, помогут в основном M1 и M2.

### Cited Findings
- До семейства 9 SIMD-группа резервировала «the maximum register usage at any point in the program» на всё время жизни; в семействе 9 «register memory is now dynamically allocated and deallocated over the lifetime of the shader», регистровый файл стал кэшем; то же для threadgroup и tile memory; «you, the developer, need to optimize occupancy a lot less often than in the past» — [док, Tech Talk 111375](https://developer.apple.com/videos/play/tech-talks/111375/)
- Ядро семейства 9 исполняет FP16, FP32 и целые параллельно, «up to 2x ALU performance», но для этого нужны инструкции из нескольких SIMD-групп; FP16 рекомендован «wherever possible» — [док, Tech Talk 111375](https://developer.apple.com/videos/play/tech-talks/111375/)
- Occupancy Manager: «Shader cores can set a target on the maximum occupancy to balance thread utilization and cache thrashing»; диагностика — `Occupancy Manager Target`, L1 eviction rate, L1 residency и bandwidth; профайлер со стоимостью по строкам и тепловыми картами — на семействе 9 — [док, Tech Talk 111374](https://developer.apple.com/videos/play/tech-talks/111374/)
- По данным проекта, удвоенный темп fp16 появился в M5 (WWDC25) — [отчёт 2026-11-05](../../../../reports/engine/2026-11-05-shader-compile/README.md), первоисточник в этот раз не перепроверял

### Inferences
- На M3+ SSAO с 3344 Б TG и 36 temp получит занятость по факту, и потолок 36 SIMD исчезнет. Если там SSAO и станет дешевле, то не от наших правок памяти.
- Для M3+ важнее L1: `hitV` (TG-память) и текстурный рабочий набор делят один кэш. Копия глубины меньшего размера (раздел 8) должна помочь и здесь, по той же причине.
- Профилировать SSAO на M3+ разумно встроенным в Xcode профайлером со стоимостью по строкам: это единственная платформа, где Apple его документирует. Пролог и путь выборки там видны построчно.

### Gaps
- Своих замеров на M3+ у проекта нет; доля игроков на M3+ неизвестна.

## 10. Сводный список правок для SSAO и пайплайна — по ожидаемой пользе на единицу работы

### Takeaway
Сначала инструмент (флаг статистики PSO), затем три правки SSAO, которые бьют по цепочке задержек, а не по регистрам: голосование SIMD вместо барьеров, меньше итераций при тех же 32 выборках, чтение на шаг вперёд в пределах ~44 temp. Затем рабочий набор текстур (копия глубины, мипы) и перекрытие однопоточных dispatch с вершинной работой. Перевод в fp16 и ручной MSL — в конце и только по данным статистики.

### Cited Findings
- Регистры, TG-память и инструкции SSAO и вариантов — раздел 1 и [замер] выше: база 36 temp / 515 инструкций, чтения пачкой 56 temp, без развёртки 32 temp / 380, полная развёртка 32 temp / 778, голосование SIMD 36 temp / 495
- 16 выборок вместо 32 сняли только −0,25 / −1,4 / −1,5 мс из 3,1 / 5,1 / 5,5 мс трассировки — [отчёт 2026-11-07](../../../../reports/engine/2026-11-07-shader-optimisation/README.md). При `numIter = 8` число итераций и барьеров (3 на итерацию) при этом не меняется: меняется только число выборок на шаг, 4 → 2 (`ssao.comp:25–27`) — [код](../../../../OpenGothic/shader/ssao/ssao.comp)
- Таблица направлений (минус `sin`, `cos`, `sqrt`) не дала ничего: 4,85 → 4,82 мс — [отчёт 2026-11-07](../../../../reports/engine/2026-11-07-shader-optimisation/README.md)

### Inferences
Порядок и ожидания — мои выводы. Каждый пункт — отдельный вариант с A B B A, как в проекте.

1. **Флаг `OPENGOTHIC_PSO_STATS=1` в Tempest** (бит 18 при создании PSO, журнал: temp, uniform, спилл, TG, инструкции). Только отладка, приватная опция. Закрывает фрагментные шейдеры и даёт проверку каждого варианта до замера. Работы — около часа.
2. **SSAO: голосование SIMD вместо `isSolved` и `shMaxSteps`** (`subgroupMax`, `subgroupBallot`/`subgroupAny`). Уходят 26 барьеров на группу, SIMD-группы выходят независимо, небо выходит сразу. Вид меняется только в адаптивности (блоки 8×4 вместо 8×8). Самый вероятный выигрыш среди коротких правок.
3. **SSAO: 4 итерации по 8 выборок вместо 8 по 4** при тех же 32 выборках. Вдвое меньше проверок решения и барьеров, если пункт 2 не сделан; при сделанном пункте 2 — вдвое меньше `resolveOcclusion` и голосований. Адаптивность грубее: проверить, сколько выборок тратится в среднем.
4. **SSAO: чтение глубины на одну выборку вперёд** (программная конвейеризация на глубину 1). По пробе держать temp ≤ ~44: при TG 3344 Б занятость тогда не упадёт. «Четыре пачкой» (56 temp) не повторять — это уже показало регрессию.
5. **Копия глубины для SSAO**, затем мипы (SAO): половинное разрешение R32F или 16 бит (раздел 8). Целится в лес и интерьер. Сначала счётчики `Texture Cache Miss Limiter` и `Texture Cache Read Miss Rate` на SSAO отдельно.
6. **Exposure, Irradiance и независимые LUT тумана — в отдельные энкодеры рядом с вершинной работой** (карты теней, VSM), как уже сработало с SSAO. Без изменений в Tempest. Параллельный dispatch с реальными барьерами — только если этого не хватит.
7. **SSAO: ячейки `hitV` из разделяемой памяти — в регистры** (25 ячеек × 8 бит = 7 слов, выбор слова цепочкой `select`/сдвигов вместо динамического индекса). Уходят TG-чтение-запись на каждом попадании и почти вся TG-память. Цена — +7 temp и ALU, которое здесь не узкое место. Опыт с проверкой пробой: temp ≤ 44 и 0 спилла.
8. **SSAO: нормаль из `gbufNormal`** вместо 4 чтений и ~80 инструкций — только если пролог окажется заметным в построчной цене (на M3) или в варианте. Вид меняется, нужна пара кадров.
9. **Подсказки циклов в SPIRV-Cross для MSL** (`LoopControl` → `#pragma clang loop unroll(...)`): чинит потерю `[[dont_unroll]]` в `fog.frag`; польза — по замеру тумана.
10. **fp16 и снижение регистров** — только для шейдеров, у которых флаг из пункта 1 покажет temp > 52 и которые идут в кадре по умолчанию (кандидаты — `light.frag`, `main.frag`, пробы освещения).
11. **Кернелы со стековым спиллом** (304 и 176 Б): проверить, работают ли они на Mac по умолчанию. Если да — убрать динамически индексируемые локальные массивы.

### Gaps
- Ни одна правка из списка не замерена в кадре; выигрыш в мс не оценён и может быть нулевым. Доказано только, что регистры SSAO и blur сейчас не ограничивают занятость, а «чтения пачкой» поднимают регистры до порога, на котором занятость падает.
