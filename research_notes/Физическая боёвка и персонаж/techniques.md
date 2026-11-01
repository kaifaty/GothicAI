# Физическая анимация и управление персонажем: техники на 2026 год

Записи от 23 октября 2026 года. Они дополняют [`npc_body_control_architectures.md`](../npc_body_control_architectures.md). Тот документ выбирает **тактическую** policy: GRU выбирает примитивы `FightAlgo`. Здесь разобран нижний уровень: как тело персонажа двигается под физикой, когда его ведут к позе анимации, держат в равновесии или им управляет обученная сеть. Что уже работает в проекте: Jolt, ragdoll из капсул `HitZones` на `SwingTwistConstraint`, кинематические «живые тела» в радиусе 20 м, активная цепочка конечности после попадания стрелы, герой и NPC на `CharacterVirtual`, шаг 1/60 с и не больше двух шагов за кадр ([docs/physics.md](../../docs/physics.md)). Этап 7 «Бой» помечен «под вопросом».

## Какие подходы существуют и чем они отличаются (как работает, качество, устойчивость, стоимость, данные, применение)

### Takeaway
Есть три семейства подходов. (1) Анимация ведёт физику: моторы или PD тянут ragdoll к позе. Это дёшево, данных не требует, хорошо изучено и применяется в выпущенных играх. Но ведомое тело не умеет держать равновесие и нуждается в стабилизации корня. (2) Кинематика плюс IK и motion matching: самые отзывчивые, физика у них только вторичная. (3) Обученные физические контроллеры (DeepMimic, AMP, ASE, PHC, PULSE, MaskedMimic, SuperTrack) держат равновесие и восстанавливаются после толчков. Цена — дни или недели GPU на обучение, MLP на миллионы параметров на каждого персонажа и привязка к симулятору. Ни одна из найденных работ не заявляет готовности к реальному времени в игре.

### Cited Findings

**Анимация ведёт физику (Jolt, Unreal)**
- В Jolt у `Ragdoll` три способа задать позу. `SetPose` телепортирует тела. `DriveToPoseUsingKinematics(pose, dt)` задаёт телам скорости, чтобы они пришли в позу за `dt`. `DriveToPoseUsingMotors(pose)` включает моторы суставов. Вариант `DriveToPoseUsingMotors(prevPose, pose, dt)` ведёт и по позиции, и по скорости. Есть также `AddImpulse`, `AddLinearVelocity`, `SetLinearAndAngularVelocity` на весь ragdoll и `ResetWarmStart` после резкой смены позы — [Jolt Ragdoll.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Ragdoll/Ragdoll.h)
- В режиме `PositionAndVelocity` мотор Jolt создаёт силу `stiffness·(target_pos − pos) + damping·(target_vel − vel)`. Для задания пружины в режиме ускорения есть `ESpringMode::MassNormalizedStiffnessAndDamping`. Добавлен `RagdollSettings::CalculateConstraintPriorities`: он повышает приоритет суставов ближе к корню — [Jolt Release Notes](https://github.com/jrouwe/JoltPhysics/blob/master/Docs/ReleaseNotes.md)
- Частота пружины мотора должна лежать в (0, 0,5 × частота симуляции]. Для симуляции 60 Гц документация называет 20 Гц жёсткой, а 2 Гц мягкой пружиной; damping 1 соответствует критическому. Предел момента задаётся `mMaxTorqueLimit`. Позиционные моторы игнорируют предел момента, угловые — предел силы — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- По умолчанию `mNumVelocitySteps = 10`, `mNumPositionSteps = 2`, `mBaumgarte = 0.2` — [Jolt PhysicsSettings.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/PhysicsSettings.h)
- Пример Jolt `PoweredRigTest` каждый кадр сэмплирует анимацию и вызывает `DriveToPoseUsingMotors` в режиме Position или PositionAndVelocity — [PoweredRigTest.cpp](https://github.com/jrouwe/JoltPhysics/blob/master/Samples/Tests/Rig/PoweredRigTest.cpp)
- В Jolt есть `SkeletonMapper`: он переносит позу между подробным скелетом анимации и упрощённым скелетом ragdoll в обе стороны. Скорости телам ragdoll можно задавать в `PhysicsStepListener::OnStep` в начале каждого шага — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- Главный тест производительности Jolt: «16 кучек по 10 ragdoll (3680 тел) с включёнными моторами». От 1 до 8 потоков ускорение 4,9×, с SMT на 16 потоках 5,7×. Абсолютные значения есть только на графике (шкала до ~400 шагов/с), в тексте их нет — [Jolt Multicore Scaling](https://jrouwe.nl/jolt/JoltPhysicsMulticoreScaling.pdf)
- В Unreal базовый механизм частичной физики — `Set All Bodies Below Simulate Physics` и `Set All Bodies Below Physics Blend Weight`. При весе 1,0 кость и всё ниже неё полностью ведёт физика, при 0,0 возвращается анимация. Реакцию на удар делают так: найти кость, включить симуляцию, поднять вес до 1 и опустить обратно — [UE docs: Physics-driven animation](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-driven-animation-in-unreal-engine)

**Кинематика: motion matching и learned motion matching**
- Motion matching периодически ищет в базе mocap кадр по признакам: позиции и скорости стоп, скорость таза, будущая траектория. Затем переходит к нему и сглаживает разрыв, например инерциализацией. Новых движений метод не синтезирует. Впервые применён в *For Honor* — [Ubisoft La Forge: Learned Motion Matching](https://www.ubisoft.com/en-us/studio/laforge/news/6xXL85Q3bF2vEj76xmnmIu/introducing-learned-motion-matching), [DReCon](https://www.ubisoft.com/en-us/studio/laforge/news/VjEIwquaIyEZZSw5RZI0V/drecon-datadriven-responsive-control-of-physicsbased-characters)
- Learned MM заменяет поиск и базу тремя сетями: decompressor, stepper и projector. Стоимость CPU постоянная. Память уменьшилась примерно в 10× на простом примере и примерно в 70× на большой базе: 590 МБ → 17 МБ весов, 8,5 МБ после 16-битного квантования. Данных о выпуске в игре в статье нет — [Ubisoft La Forge](https://www.ubisoft.com/en-us/studio/laforge/news/6xXL85Q3bF2vEj76xmnmIu/introducing-learned-motion-matching)
- DReCon (Ubisoft, SIGGRAPH Asia 2019) соединяет motion matching с физикой. MM выбирает движение по траектории, направлению, стилю и непрерывности стоп; обученная policy отслеживает его на физическом персонаже. Обучение идёт с «худшим пользователем», который случайно дёргает геймпад. Авторы называют стоимость «удивительно низкой», но числа приведены только на графике (Xeon E5-1650 v3). В выпущенной игре метод не заявлен — [Ubisoft La Forge: DReCon](https://www.ubisoft.com/en-us/studio/laforge/news/VjEIwquaIyEZZSw5RZI0V/drecon-datadriven-responsive-control-of-physicsbased-characters)

**Обученные физические контроллеры: числа**

| Метод | Симулятор, частоты | Обучение | Данные | Сеть, рантайм | Источник |
|---|---|---|---|---|---|
| AMP (2021) | Bullet, симуляция 1,2 кГц, policy 30 Гц, PD-цели | 100–300 млн сэмплов, **30–140 ч на 16 ядрах CPU** на задачу | mocap-клипы + keyframe | — | [AMP](https://arxiv.org/abs/2104.02180) |
| ASE (2022) | Isaac Gym, 4096 сред на **одной V100**, симуляция 120 Гц, низкий уровень 30 Гц, высокий 6 Гц | >10 млрд сэмплов, **~10 суток на одной GPU** | 187 клипов, ~30 мин, включая гладиатора с мечом и щитом | — | [ASE](https://arxiv.org/abs/2205.01906) |
| SuperTrack (Ubisoft, 2021) | 240 Гц, 20 итераций солвера; **симуляция 300–1000 мкс на кадр** | балансирование появляется через ~2 ч на GTX 1070; полное обучение **20–40 ч** (LaFAN 40 ч, рельеф 50 ч) | 20 тыс. – 1,5 млн кадров; для интерактивного контроллера хватило **10–20 мин** записанной игры с MM | **policy ~1800 мкс на кадр**, ~20 МБ весов, Xeon E5-1650 | [SuperTrack](https://theorangeduck.com/page/supertrack-motion-tracking-physically-simulated-characters-using-supervised-learning) ([PDF](https://theorangeduck.com/media/uploads/other_stuff/SuperTrack.pdf)) |
| PHC+ / PULSE (2023/24) | Isaac Gym, policy 30 Гц, симуляция 60 Гц, PD на 69 актуаторах, момент ≤500 Н·м | PHC+ ~10^10 сэмплов, PULSE ~10^9; время и GPU не указаны | AMASS | MLP [2048, 1536, 1024, 1024, 512, 512]; латент 32; успех PHC+ 100% / 99,2% (train/test), PULSE 99,8% / 97,1% | [PULSE](https://arxiv.org/html/2310.04582v2) |
| MaskedMimic (2024) | Isaac Gym, 16 384 среды, контроллер 30 Гц, симуляция 120 Гц | **~2 недели на 4×A100**, ~30 млрд шагов (учитель) / 10 млрд (ученик) | AMASS + HumanML3D + SAMP | prior на transformer; трекинг 99,2%, VR 98,1%, рельеф 95,4% | [MaskedMimic](https://arxiv.org/html/2409.14393) |
| SLMP (март 2026) | Isaac Gym, физика и PD 60 Гц | **на 2×RTX 4090:** трекер ~24 ч, prior ~12 ч, бой двух агентов ~8 ч | 2 ч кикбоксинга (Xsens 180 Гц) + ~8 мин бокса из AMASS | трекер [2048…512]; выживание случайных прогонов на 10 с 99,8% (VAE 10,2%) | [SLMP](https://arxiv.org/html/2603.01294) |
| SuperMimic (препринт, окт. 2026) | «стандартный» rigid-body симулятор + дифференцируемая world model, BPTT | **~3 ч на одной GPU** превзошли базовую линию, обученную 7 суток на 128 GPU (модель GPU не названа) | до 264 ч движения | 25 провалов из 129 514 против 179 у базовой линии | [SuperMimic](https://arxiv.org/abs/2610.28480) |

- В блоге NVIDIA о MaskedMimic: «Optimizing inference speed may enable deployment in real-time games». На VR-трекинге MaskedMimic 98,1% успеха, PULSE 93,4%, ASE 37,6%, CALM 10,1% — [NVIDIA blog](https://developer.nvidia.com/blog/unified-whole-body-control-for-physically-simulated-humanoids/)
- SuperTrack на графике сравнения получился устойчивее к толчкам, чем PPO, во всём диапазоне величины толчка. Он отслеживает мировую позицию корня и прерывает эпизод при расхождении больше 1 м или 90° — [SuperTrack PDF](https://theorangeduck.com/media/uploads/other_stuff/SuperTrack.pdf)
- В PULSE случайные прогоны иногда «застревают в упавшем или стоячем положении» — [PULSE](https://arxiv.org/html/2310.04582v2)

**Применение в выпущенных играх**
- Euphoria (NaturalMotion, Dynamic Motion Synthesis) использовалась в GTA IV, GTA V, Red Dead Redemption 1 и 2, Max Payne 3, Star Wars: The Force Unleashed 1 и 2, Backbreaker, Clumsy Ninja. В 2017 году NaturalMotion прекратила лицензирование — [Wikipedia: Euphoria](https://en.wikipedia.org/wiki/Euphoria_(software))
- Jolt используется в Horizon Forbidden West и Death Stranding 2 — [Jolt README](https://github.com/jrouwe/JoltPhysics)
- Half Sword (Half Sword Games) — средневековый бой целиком на физике. Игра выросла из студенческого проекта с active ragdoll, которые рубят друг друга мечами. Замахи, блоки и парирования решает физика; игрок управляет конечностями и оружием от первого лица. Описание сделано по поиску: сама статья Epic Store вернула 403, движок не подтверждён — [Epic Games Store news](https://store.epicgames.com/news/how-half-sword-makes-bloody-brutal-medieval-combat-feel-real)
- EA и Frostbite выступали на GDC с докладом «Physics Driven Ragdolls and Animation at EA: From Sports to Star Wars» о ragdoll, которые следуют анимации. Содержание недоступно: GDC Vault на обслуживании — [GDC Vault](https://www.gdcvault.com/play/1025210/Physics-Driven-Ragdolls-and-Animation)

### Inferences
- По соотношению цены и качества для нас естественный порядок такой: кинематика + IK для локомоции; активный ragdoll на моторах Jolt для реакций (он уже частично есть); learned tracking только как исследовательская ветка. Причина — числа выше: обучение стоит дни или недели GPU, а policy размера PHC (~8–9 млн параметров по слоям [2048…512]; моя оценка ~17 MFLOP на вызов) на 10 NPC × 30 Гц даёт ~5 GFLOP/с на CPU игры.
- Для малой команды полезнее всего SuperTrack: обучение с учителем через world model, 20–40 ч на GPU уровня GTX 1070, интерактивный контроллер из 10–20 минут записанной игры. Но 1,8 мс на policy одного персонажа на Xeon 2014 года не умещаются в бюджет 10 персонажей, если сеть не уменьшать.
- Все обученные контроллеры привязаны к своему симулятору: Isaac Gym/PhysX, Bullet. Перенос на Jolt без дообучения внутри Jolt не стоит считать работающим. Об этом нет ни одной работы, но обучение во всех случаях опирается на точную модель PD и контактов.

### Gaps
- Не найдены реальные цифры стоимости `DriveToPoseUsingMotors` на один ragdoll в Jolt: график есть, чисел в тексте нет.
- Не получена документация Unreal Physical Animation Component и Physics Control: страница вернула пустой контент. Описывать их параметры по памяти я не стал.
- Нет первичных источников по физической анимации в Uncharted, The Last of Us, Ghost Recon (GDC Vault недоступен), по поведениям Euphoria (balance, protective fall) и по SIMBICON, virtual model, ZMP. Классические контроллеры равновесия не проверены в этом проходе.
- CALM и ControlVAE: на страницах arXiv нет чисел обучения и рантайма.
- Нет подтверждённых данных об игре, выпущенной с обученным физическим контроллером гуманоида (DeepMimic, PHC и подобные) в бою.

## Исследования физического ближнего боя: оружие, контакты, стоимость обучения

### Takeaway
Основная работа по теме — Won, Gopinath и Hodgins (SIGGRAPH 2021). Меч крепится к кисти **weld joint**, симуляция идёт на 480 Гц, управление на 30 Гц, stable PD. Обучение: день имитации и 2–5 дней соревнования на кластере из 24 серверных CPU. Результат правдоподобен, но боксёры заметно медленнее профессионалов, а захваты в эту схему не укладываются. Ближайшая свежая работа (SLMP, 2026) учит бой двух безоружных агентов за ~8 ч на 2×RTX 4090 поверх трекера, на который ушло ~24 ч. Устойчивость к толчкам там не измеряли, оружия нет.

### Cited Findings
- **Won 2021, окружения.** Бокс: увеличенные кисти размером с перчатку, арена 5×5 м, удары только в верх тела, раунд 60 с, побеждает тот, кто нанёс больше урона (физической силы). Фехтование: «мы прикрепили клинок weld joint к концу правой кисти, имитируя крепко зажатый меч». Арена 12×2 м, касание только торса, по правилам рапиры; встречная атака в течение 1 с даёт ничью. Управление — целевая поза, моменты считает stable PD для всех суставов, кроме корня — [Won et al. 2021, PDF](https://ai.meta.com/research/publications/control-strategies-for-physically-simulated-characters-performing-two-player-competitive-sports/)
- **Won 2021, числа.** Симуляция 480 Гц, управление 30 Гц, DD-PPO в RLlib. Экспертов 8 для бокса и 4 для фехтования. Каждый эксперт — 2 слоя по 64 нейрона и выход на 51. Кодировщик задачи бокса — двухслойный LSTM на 128, у фехтования — MLP 128: долгая память там не нужна. Железо — 12 машин по 2 Xeon E5-2698 v4. Имитация ~1 день (2e8 переходов). Соревнование: бокс 4–5 дней (5e8–7e8), фехтование 2–3 дня (2e8–3e8). Соревновательное обучение в ~1,5 раза медленнее, потому что персонажей двое — [Won et al. 2021](https://dl.acm.org/doi/10.1145/3450626.3459761)
- **Won 2021, ограничения.** Многоагентным задачам нужен «почти миллиард» переходов против ~100 млн у одиночных. Борьбу и захваты одна policy-актор не охватывает. Естественность ограничена исходным mocap: боксёр с малым опытом дал медленных персонажей — [Won et al. 2021](https://dl.acm.org/doi/10.1145/3450626.3459761)
- **SLMP (HKUST, март 2026).** Датасет — 2 ч кикбоксинга: удары руками и ногами, колени, локти, уклоны и нырки. Бой через self-play с редкой наградой: попадание засчитывается при контакте в пределах 0,3 м и силе больше 30, нокдаун даёт +50, собственное падение −50. Выживание случайных прогонов на 10 с — 99,8%. Устойчивость к толчкам и оружие не исследованы — [SLMP](https://arxiv.org/html/2603.01294)
- **ASE.** Датасет включает гладиатора с мечом и щитом (Reallusion, 187 клипов, ~30 мин). Низкоуровневый навык учится ~10 суток на одной V100 — [ASE](https://arxiv.org/abs/2205.01906)
- Дальнейшие многоперсонажные работы, например SMPLOlympics и «Simulation and Retargeting of Complex Multi-Character Interactions», ссылаются на Won 2021 как на основу — [SMPLOlympics](https://arxiv.org/html/2407.00187v1), [Multi-character interactions](https://arxiv.org/pdf/2305.20041)

### Inferences
- Для меча в Jolt подход Won прямо переносится. Оружие — отдельное тело, прикреплённое `FixedConstraint` к кисти (аналог weld). Альтернатива — составная форма кисти с клинком. Fixed проще ломать (обезоруживание) и давать ему предел силы. Короткое оружие у кисти со 120–480 Гц рискует проходить сквозь цели, поэтому нужен CCD (`LinearCast`) или подшаги.
- Даже исследовательский уровень (480 Гц, дни обучения на сотнях ядер, медленные боксёры) не даёт «быстрого и читаемого» боя, привычного по играм. Для Gothic, где бой держится на чётких окнах атак и анимациях, полная физика бойца — шаг в другую игру (жанр Half Sword или Exanima), а не улучшение Gothic.
- Самый реалистичный выигрыш от физики в бою — это вторичные эффекты: отдача клинка при попадании в блок или стену, реакция корпуса и конечностей, падение, обезоруживание. Это соответствует этапу 7 из `docs/physics.md`: «отталкивание оружием, физический блок, удары по направлению клинка».

### Gaps
- Не найдена отдельная работа о физически симулируемом фехтовании или бое на мечах после 2021 года. Поиск по «physics fencing» и «sword fighting RL» вывел на теннис и робототехнику. Боевые агенты NVIDIA и Tencent не найдены.
- Нет данных о том, как в исследованиях моделируют блок клинок о клинок: трение, отскок, предел силы хвата.

## Детерминизм, шаг, итерации солвера, стоимость на CPU

### Takeaway
Jolt детерминирован при одинаковом бинарнике и порядке вызовов API. Кроссплатформенный детерминизм стоит ~8% производительности. Ragdoll в играх считают на 60 Гц: Horizon перешёл с 30 Гц на 2×60 Гц и стал стабильнее и дешевле. Обучающие системы используют 60–1200 Гц, причём контакт и PD у них заметно жёстче. В проекте 10 кинематических тел NPC стоят p95 0,285 мс на шаг физики на M1 Pro.

### Cited Findings
- Детерминизм Jolt требует того же бинарника и того же порядка вызовов, меняющих симуляцию. `CROSS_PLATFORM_DETERMINISTIC` даёт одинаковый результат на разных компиляторах, ОС и архитектурах ценой ~8%. Порядок результатов `BroadPhaseQuery`, коллбеков `ContactListener` и `GetActiveBodies` не детерминирован — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- По документации система стабильна на 60 Гц с одним collision step. В примере 60 Гц с двумя collision steps каждый проход длится 1/120 с — [Jolt docs](https://jrouwe.github.io/JoltPhysics/)
- Horizon Forbidden West перешёл с 30 Гц, которые использовались во всех прошлых играх студии, на два шага по 60 Гц и потратил на физику меньше времени. На боссе Slitherfang было в среднем 3,3 мс со всплеском 21 мс, стало 2,1 мс со всплеском 10 мс; ragdoll «намного стабильнее благодаря более высокой частоте» — [Rouwe, GDC 2022, notes](https://jrouwe.nl/architectingjolt/ArchitectingJoltPhysics_Rouwe_Jorrit_Notes.pdf)
- Частоты обучающих систем: AMP — 1,2 кГц (Bullet), Won — 480 Гц, SuperTrack — 240 Гц и 20 итераций, ASE и MaskedMimic — 120 Гц, PHC и PULSE — 60 Гц, SLMP — 60 Гц — [AMP](https://arxiv.org/abs/2104.02180), [Won 2021](https://dl.acm.org/doi/10.1145/3450626.3459761), [SuperTrack PDF](https://theorangeduck.com/media/uploads/other_stuff/SuperTrack.pdf), [ASE](https://arxiv.org/abs/2205.01906), [MaskedMimic](https://arxiv.org/html/2409.14393), [PULSE](https://arxiv.org/html/2310.04582v2), [SLMP](https://arxiv.org/html/2603.01294)
- SuperTrack: физика 300–1000 мкс на кадр при 240 Гц и 20 итерациях, policy ~1800 мкс на кадр (Xeon E5-1650) — [SuperTrack PDF](https://theorangeduck.com/media/uploads/other_stuff/SuperTrack.pdf)
- **Замеры проекта на M1 Pro.** 10 живых NPC с кинематическими телами: медиана FPS OFF 73,61 / ON 77,39, медиана p95 шага физики ON 0,285 мс, максимальный шаг 35,668 мс (справочно) — [reports/physics/2026-10-15-living-npcs](../../reports/physics/2026-10-15-living-npcs/validation-notes.md). Активная цепочка от стрелы: полный `CollisionWorld::tick` ON 0,320 / 0,684 / 1,986 мс (среднее / p95 / максимум) против OFF 0,210 / 0,258 / 0,403 мс — [reports/physics/2026-10-15-arrow-reaction](../../reports/physics/2026-10-15-arrow-reaction/validation-notes.md). 20 одновременных смертей с ragdoll: ON 71,67 FPS / p95 20,16 мс, OFF 66,59 FPS / p95 23,29 мс — [docs/physics.md](../../docs/physics.md)

### Inferences
- Жёсткие моторы под 60 Гц ограничены сверху 30 Гц частоты пружины. Чтобы клинок держался «в руке» в ударе, скорее всего понадобится 120 Гц: два подшага по 60 Гц на кадр, как в Horizon. Можно и включать 2 collision steps только тогда, когда рядом идёт бой. Текущий предел «не больше двух шагов за кадр» это позволяет.
- Если обучать внутри Jolt на Ryzen (x86) и запускать на Mac (ARM), побайтовой повторяемости нет без `CROSS_PLATFORM_DETERMINISTIC`. Для переноса policy она не нужна, а для воспроизведения боя и e2e нужна.
- Активный ragdoll по сравнению с кинематическим телом в проекте пока стоит десятые доли миллисекунды p95. До бюджета ~2 мс для 10 бойцов запас есть, но полное динамическое тело с 10 итерациями солвера, контактами с оружием и 120 Гц нужно мерить отдельно.

### Gaps
- Нет измерений Jolt для «10 полностью активных ragdoll на моторах, 120 Гц» на M1 Pro: ни внешних, ни в проекте.
- Не найдены данные о том, сколько итераций velocity и position нужно цепочке кисть — меч при жёстком моторе в Jolt.

## Что реально достижимо в небольшом движке на Jolt: игрок и ~10 бойцов на M1 Pro при 60 FPS

### Takeaway
Реалистичный вариант: кинематическая анимация Gothic остаётся владельцем движения и корня. Jolt ведёт **части тела** моторами к анимационной позе: руки и корпус с оружием, при сильном ударе всё тело. Таз удерживается «рукой бога» (скорость корня через `DriveToPoseUsingKinematics` или `OnStep`). При нокдауне — переход в обмякший ragdoll, подъём уже реализован. Learned physics controller на 10 бойцах сейчас упирается сразу в три вещи. Первая — правило проекта: моделей на Mac не запускаем. Вторая — сетевая задержка до `dev-ubuntu` при цикле равновесия 30 Гц. Третья — обучение в чужом симуляторе.

### Cited Findings
- Правило проекта: «Локальные модели на MacBook не запускаем… ASR, LLM, TTS, эмбеддинги — ничего». Всё, что требует GPU, живёт на `dev-ubuntu`. Ryzen с RTX 3080 10 ГБ — общая машина для обучения и симуляций под общим замком — [AGENTS.md](../../AGENTS.md)
- Задержка до `dev-ubuntu`: urgent p50/p95 29/31 мс при 10 запросах/с и 33/88 мс при 40 запросах/с — [npc_body_control_architectures.md](../npc_body_control_architectures.md)
- Обученным трекерам нужен policy-тик 30 Гц в одном цикле с физикой (PHC, ASE, MaskedMimic, Won) — см. таблицу выше.
- Стоимость обучения на одной потребительской GPU: ASE ~10 суток на V100; SLMP ~24 + 12 + 8 ч на **двух** RTX 4090; SuperTrack 20–40 ч на GPU класса GTX 1070; AMP 30–140 ч на 16 ядрах CPU — [ASE](https://arxiv.org/abs/2205.01906), [SLMP](https://arxiv.org/html/2603.01294), [SuperTrack PDF](https://theorangeduck.com/media/uploads/other_stuff/SuperTrack.pdf), [AMP](https://arxiv.org/abs/2104.02180)
- В проекте уже есть режимы тела «выключен / кинематический / активный / обмякший». Цепочка конечности после стрелы становится динамической, и моторы Jolt возвращают её к анимации, а таз и игровое движение сохраняются — [docs/physics.md](../../docs/physics.md)

### Inferences
- **Достижимо (по оценке, низкий риск).** Активный ragdoll с моторами на 10 NPC рядом, по аналогии с работающей цепочкой стрелы. Вес физики по цепочке задаётся силой удара. Ослабление моторов — «stagger», при пороге импульса — нокдаун в обмякший режим. Отдача меча при попадании в блок или стену: физическая рука с клинком плюс Fixed constraint, затем возврат к анимации. Look и aim IK и foot IK уже есть. Этого хватит на этап 7 из `docs/physics.md`.
- **Достижимо со средним риском.** Мягкое равновесие без обучения: таз держит «рука бога» с ограниченной силой. Если ошибка позы или скорость центра масс превышает порог, сила снимается и персонаж падает. Это уже не SIMBICON и не ZMP, а «костыль», которым обычно и обходятся.
- **Пока недостижимо в рамках правил.** Обученный трекер (SuperTrack или PHC) для каждого бойца. На Mac запуск противоречит правилу «без моделей», а удалённо его не обслужить: цикл 30 Гц плюс RTT ~30–90 мс разрушает обратную связь равновесия. Разрешить это может только пользователь — например, исключение для маленькой policy внутри игры на CPU. До этого learned physics остаётся экспериментом на Ryzen.
- Если исключение дадут, обучать стоит **внутри Jolt на Ryzen** (CPU: 16 ядер, 32 потока), подход SuperTrack или AMP-масштаба. Арифметика: Won-подобному фехтованию нужно ~2,5e8 переходов × (480/30) ≈ 4e9 шагов пары персонажей. Время зависит от стоимости шага пары в Jolt, которую нужно измерить. Я её не нашёл.
- Игрок: держать на `CharacterVirtual` с кинематическим корнем (раздел ниже). Физическими делать только руку с оружием и реакцию на попадания.

### Gaps
- Нет замера: сколько стоит один активный ragdoll Jolt с моторами на M1 Pro при 60 и 120 Гц.
- Нет данных о стоимости инференса маленькой policy (~100 тыс. параметров, как эксперты Won) на ARM CPU в игровом цикле. Её можно оценить, но не цитировать.

## Гибридные архитектуры выпущенных игр: кинематика как цель, физика следует, IK правит контакты

### Takeaway
В выпущенных играх, которые удалось подтвердить, кинематическая анимация остаётся целью или владельцем, а физика ей следует. Примеры: blend weight по костям в Unreal, моторы к позе в Jolt, Euphoria. Motion matching (For Honor) и learned MM дают кинематическую цель. DReCon и SuperTrack показывают исследовательский вариант «MM как цель → обученный трекер на физике», но выпуск в игре не заявлен.

### Cited Findings
- В Unreal штатно смешивают анимацию и физику по поддереву костей через `Physics Blend Weight`, от 0 до 1 — [UE docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/physics-driven-animation-in-unreal-engine)
- В Jolt есть motor-driven и kinematic-driven ragdoll, а также `SkeletonMapper` для переноса позы — [Jolt README](https://github.com/jrouwe/JoltPhysics), [Ragdoll.h](https://github.com/jrouwe/JoltPhysics/blob/master/Jolt/Physics/Ragdoll/Ragdoll.h)
- Euphoria стоит в Rockstar RAGE (GTA IV/V, RDR 1/2, Max Payne 3) и в серии Force Unleashed. Лицензирование прекращено в 2017 году — [Wikipedia](https://en.wikipedia.org/wiki/Euphoria_(software))
- Motion matching впервые применён в *For Honor*. DReCon получает от MM траекторию и стиль, а физическую позу ведёт обученная policy — [DReCon](https://www.ubisoft.com/en-us/studio/laforge/news/VjEIwquaIyEZZSw5RZI0V/drecon-datadriven-responsive-control-of-physicsbased-characters)
- SuperTrack учит трекер на 10–20 минутах записанной игры с кинематическим MM-контроллером и передаёт policy разницу корней симулированного и кинематического персонажей — [SuperTrack PDF](https://theorangeduck.com/media/uploads/other_stuff/SuperTrack.pdf)
- Overgrowth (Wolfire): процедурная анимация из немногих ключевых кадров. Доклад GDC 2014 «An Indie Approach to Procedural Animation». Детали доклада не получены, известно только описание — [Game Developer](https://gamedeveloper.com/design/video-an-indie-approach-to-procedural-animation)

### Inferences
- Обобщённая схема по всем источникам: анимация, MM или графы клипов задают целевую позу и корень → IK (стопы, взгляд, прицел) правит контакты → физика ведёт выбранные цепочки к позе моторами с весом → при толчке выше порога вес физики растёт, моторы слабеют → при падении ragdoll обмякает → подъём через позу тела и анимацию. Проект уже реализовал большую часть этого конвейера.

### Gaps
- Не удалось получить первичные описания гибридов Naughty Dog, Ubisoft (Ghost Recon), EA Frostbite и Guerrilla (персонажи Horizon). Так что «как именно в AAA» подтверждено только для Unreal, Jolt и Euphoria на уровне возможностей.

## Как сохраняют отзывчивость игрока: задержка ввода и управление корнем

### Takeaway
Прямых измерений задержки ввода для физических персонажей найти не удалось. Источники показывают общий приём: корень и траекторию задаёт кинематический контроллер (MM, character controller), а физическое тело его отслеживает и не может далеко уйти. В исследованиях это расхождение явно ограничивают; в играх физика остаётся вторичной.

### Cited Findings
- DReCon: отзывчивость обеспечивает MM, а policy учится под «худшего пользователя» со случайно меняющимся вводом. Авторы описывают отзывчивость качественно, без чисел — [DReCon](https://www.ubisoft.com/en-us/studio/laforge/news/VjEIwquaIyEZZSw5RZI0V/drecon-datadriven-responsive-control-of-physicsbased-characters)
- SuperTrack передаёт policy мировую разницу корня, штрафует дрейф и прерывает эпизод при расхождении больше 1 м или 90° — [SuperTrack PDF](https://theorangeduck.com/media/uploads/other_stuff/SuperTrack.pdf)
- Half Sword даёт игроку прямое управление конечностями и оружием от первого лица. Это отдельный жанр физического симулятора, а не классическая отзывчивость action-RPG — [Epic Games Store news](https://store.epicgames.com/news/how-half-sword-makes-bloody-brutal-medieval-combat-feel-real)
- В проекте герой уже на `CharacterVirtual`, `MoveAlgo` задаёт желаемое смещение, физический корень точен, визуальное отставание ограничено 10 см — [docs/physics.md](../../docs/physics.md)

### Inferences
- Для героя корень не отдаётся физике. `CharacterVirtual` остаётся единственным владельцем позиции, анимация задаёт позу, а физика только добавляет отклонение верха тела и руки с оружием. Отдача сохраняется, но не меняет, когда игрок может снова действовать.
- Физическая реакция героя на удар должна быть визуальной и короткой: моторы ослаблены на 100–300 мс и восстанавливаются, корень остаётся под `CharacterVirtual`. Окна блока и атаки остаются за `FightAlgo` и анимацией. Числа 100–300 мс — проектная гипотеза, источника на них нет.
- Для NPC допустимо больше физики, чем для героя: нокдаун, спотыкание, потеря оружия. Их ощущение управления никого не волнует, а читаемость для игрока сохраняется за счёт возврата к анимации.

### Gaps
- Нет источников с измеренной задержкой ввода для active ragdoll или physical animation в выпущенных играх.
- Нет данных GDC о реализации «hand of god» в конкретных играх. Термин и приём я описываю как общепринятые, но первоисточник в этом проходе не найден.
