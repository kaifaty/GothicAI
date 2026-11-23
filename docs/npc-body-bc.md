# GRU, MLP baseline и behavioral cloning

Этап 7 [плана](npc-body-control-plan.md) использует отдельный пакет [body-service](../body-service/pyproject.toml). Все model forward, численные тесты, обучение и игровые эпизоды выполняются на `dev-ubuntu`. Mac выполняет разработку и SSH orchestration. Состояние допуска checkpoint фиксируется по фактическим результатам независимого reporter.

## Представление данных

Первый корпус [этапа 6](npc-body-trajectories.md) остаётся неизменным. Дополнительные демонстрации записывает [collector](../scripts/body_demonstration_suite.py) с независимым [scripted teacher](../scripts/body_demonstration_teacher.py). Его code/config SHA, exact periodic observation, прошлые завершённые команды и сертификат выбора сохраняются до body action. После закрытия движка validator проверяет принятие, настоящее начало/завершение и физический результат. Только затем создаётся отдельный `records-approved.jsonl`; прежний canonical и raw журнал сохраняются. Старые scripted API microtests остаются non-expert.

Добор содержит поворот, подход, три варианта удара, блок, отскок и ожидание 300 мс, включая эпизоды у настоящей стены. Боковые шаги и ожидание 200 мс поступают из пригодных native labels. Каждый новый удар допускает настоящие корректирующие подходы/повороты после отбрасывания; их исполнения проверяются отдельно. Общий coverage gate применяется ко всем 11 классам объединённого periodic train view.

В native trace `hit_attempt="applied"` означает передачу попытки в обработчик урона. Реальный удар подтверждается отдельным `damage` с уменьшением HP. Для отскока каждая такая попытка связывается один к одному с более поздним `defence_result.jumped_back=true` того же игрового tick и пары участников; фактического урона быть не должно. Дополнительно проверяются настоящее перемещение, увеличение дистанции и исходная наблюдаемая угроза.

GRU и MLP получают **только настоящие periodic observations**: `periodic=true`, `post_simulation_post_animation`. Native exact captures остаются доказательствами разметки. Их labels присоединяются к последнему причинно предшествующему periodic snapshot той же identity, только при возрасте до 67 мс, физической eligibility и допустимой action mask. Несколько решений на одном periodic row исключаются вместе; причины и потери покрытия видны в model alignment report.

Общие `pack_features` и `policy_action_mask` находятся в [dataset module](../scripts/body_trajectory_dataset.py). История teacher queue, IDs, certificates и будущие исходы не входят в признаки. Native/combat goal aliases имеют одинаковое числовое представление. `sequence_dt_game_ms` идёт между periodic rows актёра: первый интервал — от начального native game time эпизода; смена control generation сбрасывает hidden state, сохраняя clock между соседними periodic rows. Новый мир/эпизод сбрасывает оба.

Raw engine mask сохраняется отдельно. Categorical head исключает `continue` при физической возможности нового примитива. Для locked rows остаётся continuation; если raw mask пуста, derived head допускает только token `continue` для памяти. Такой row не может дать expert loss или body command. Публичный API никогда не получает новую команду `continue`.

Splits наследуют целые `(setup_family, seed)` группы этапа 6: оба участника, зеркальные стороны и связанные demonstrations остаются вместе. Eval используется для выбора весов; test/OOD оцениваются после выбора. Normalizer и class weights рассчитаны только по train нового periodic view. Coverage минимум 20 usable labels каждого включённого класса проверяется после alignment, на фактических inputs обучения.

## Модели и обучение

Обе архитектуры используют один whitelist признаков, action vocabulary, normalizer и dataset. GRU: encoder MLP-256 → GRU-256 → MLP-128 → 12 masked categorical logits. MLP baseline: стек из восьми наблюдений → MLP-256/256/128 → тот же head. Float32, dropout 0.

BC использует train-only class-weighted cross entropy на настоящих expert decision opportunities. Chunks — 32 наблюдения, prefix/burn-in — 8; каждый chunk начинает память с нуля и прогревает prefix. Locked rows обновляют память, padding и prefix не входят в loss. Reset boundaries не пересекаются. Optimizer AdamW, gradient clipping; параметры и seeds фиксированы в [body-bc-v1.json](../evals/npc-body/body-bc-v1.json).

Для каждой архитектуры запускаются три training seeds. Epoch и seed выбираются только по eval macro F1 всех 11 классов, затем unweighted NLL. Истории всех кандидатов и выбранные SHA сохраняются в `selection.json`. Checkpoint содержит train normalizer, feature/action/sampling schemas, dataset/validator SHA, архитектуру и выбранный epoch. Tiny train-only overfit — отдельная проверка pipeline; его checkpoint запрещён для управления NPC.

CLI пакета: `build-dataset`, `verify`, `overfit`, `train`, `evaluate`, `serve`. `verify` проверяет step/sequence, reset, независимость NPC, невозможность invalid action и измеряет warm step latency. `evaluate` сохраняет per-action precision/recall, confusion matrix и audit predictions с raw/derived masks и log probabilities.

## Runtime и замкнутый цикл

JSONL `serve` фиксирует один checkpoint. Память разделена по session/world/entity/control generation; duplicate возвращает cached response без повторного forward. Actor-local stream sequence и реальные periodic buckets обнаруживают пропуски; глобальный observer sequence не используется как счётчик пропусков одного NPC. Неизвестная epoch, stale/conflicting row или gap вызывают явный reset/fallback.

На одного NPC допускается один запрос в работе и одна последняя pending observation. Перезапись mailbox записывается как gap. `reset` задаёт epoch и начальное игровое время; `end_episode` принимает только текущую epoch, очищает hidden/cache/clock и инвалидирует episode context. Старый terminal запрос не закрывает новый мир. Следующие observations требуют нового reset.

[Policy arena suite](../scripts/body_policy_suite.py) использует те же periodic inputs и shared packing. Команда привязана к последнему exact periodic row; event-only snapshots не обновляют память и не создают команды. Locked observations, включая временную потерю видимости, сохраняются для памяти без команд; native arbiter завершает handoff на безопасной границе. Журнал содержит каждый фактически обработанный input, logits, masks, команду и native execution.

Native/GRU/MLP сравниваются на одинаковых eval setup/seed/side. Для microtasks native primary и модели получают одного scripted противника; `native_with_scripted_opponent` сохраняет настоящий native AI primary. В дуэлях противник — native. Task success, primary victory, timeout, control loss и infrastructure failure учитываются отдельно; результат после native fallback не засчитывается модели. Terminal snapshot берётся до reset, затем runtime очищается через явный barrier.

## Запуск и артефакты

```sh
scripts/body-bc-remote.sh preflight
scripts/body-bc-remote.sh sync
scripts/body-bc-remote.sh env
scripts/body-bc-remote.sh build
scripts/body-bc-remote.sh prepare stage7-validation
scripts/body-bc-remote.sh setup stage7-validation
scripts/body-bc-remote.sh tests stage7-validation
scripts/body-bc-remote.sh smoke stage7-validation
scripts/body-bc-remote.sh demos stage7-validation
scripts/body-bc-remote.sh dataset stage7-validation
scripts/body-bc-remote.sh verify stage7-validation
scripts/body-bc-remote.sh overfit stage7-validation
scripts/body-bc-remote.sh train stage7-validation
scripts/body-bc-remote.sh evaluate stage7-validation
scripts/body-bc-remote.sh arena-smoke stage7-validation
scripts/body-bc-remote.sh arena stage7-validation
scripts/body-bc-remote.sh report stage7-validation
scripts/body-bc-remote.sh collect stage7-validation
```

`prepare` создаёт новый immutable source/config/binary snapshot в `~/gothic-ai/body/data/<run>/`. Отдельный `uv` environment находится в `~/gothic-ai/body/.venv`; frozen package/helper code задаётся через `PYTHONPATH` run. Pipeline по умолчанию использует CPU сервера, два потока и `CUDA_VISIBLE_DEVICES=''`; CPU выбирается после реального latency/RSS замера. Наличие CUDA wheels в cache не означает использование GPU. ASR/TTS/Decider/Gemma identities снимаются до и после каждой фазы; Docker и service environments не изменяются.

- [`body_native_model_profile.py`](../scripts/body_native_model_profile.py) записывает внешние CPU/thread/cgroup-сэмплы вокруг `realtime-native_duel-223-1-gru` полного ordinary realtime CPU-прогона: `python3 scripts/body_native_model_profile.py --diagnostics-script "$CODE/scripts/body_policy_diagnostics.py" --sampler-source "$CODE/scripts/body_native_causal_probe.py" --profile-output /absolute/profile -- "${DRIVER_ARGS[@]}"`. `CODE` — неизменяемый снимок драйвера, `DRIVER_ARGS` — его полный исходный argv с `--phase realtime --device cpu --policy-code-root "$CODE"`, без `--case`; перед запуском подготовь базу именно для его `--assets` через `scripts/base-ensure.sh`. Профиль не даёт допуска качества или выбора модели.

При установленном `gq` launcher подаёт отдельную заявку на фазу: pytest — `quick`, lockstep-сбор и обучение — `batch`, realtime и замеры задержек/скорости — `quiet` с прежней `CPUQuota=200%`. Ресурсы и `--max` объявлены в скрипте; окна у headless-фаз нет, видеопамять резервируют только CUDA-фазы. `train` и длительные `*-train` не умеют продолжать незавершённый seed: вместо прежних 11000/14400 с они получают `--max 2h` и предупреждение. Без `gq` сохраняются прежние `body-simulation.lock`, `body-training.lock` и transient user units. `smoke` — проверка инфраструктуры, без допуска корпуса/checkpoint. На Mac собираются только компактные JSON/Markdown/resource reports; raw trajectories и веса остаются на сервере.

Цикл PPO подаёт отдельный `batch` на каждый collect, update и collect оценки; диагностические дуэли этапа 9 используют `quiet`, включая lockstep `screening`/`probe`, поскольку сохраняют замеры latency. При копировании `body-rl-ppo-{loop,eval}.sh` или `body-stage9-duels.sh` на Ryzen копируй рядом [gq-job.sh](../scripts/gq-job.sh) (для PPO он также ищется в `$PPO_CODE/scripts`, для дуэлей — в `$DUELS_CODE/scripts`). Помощник использует `submit` → `wait`, отменяет свою заявку по INT/TERM/HUP и выбирает владельца из `GQ_OWNER` либо имени папки задачи под `~/gothic-ai/`. Путь к очереди переопределяется через `GQ_BIN`.

Если сбор полностью завершился и все эпизоды получили физическое утверждение, но последующая сборка датасета прервана, `recover <new-run> <capture-run>` проверяет исходные журналы и создаёт новый manifest со ссылками на неизменные эпизоды. Проверяются source/config/binary SHA, объявленные случаи и splits, закрытие runner, физические сертификаты и покрытие periodic view. Прерванный run остаётся с исходным статусом; его exit/RSS не подменяются результатом recovery. Датасет валидируется и проектируется по одному эпизоду, чтобы не удерживать весь raw корпус в памяти.

[Независимый reporter](../scripts/body_bc_report.py) отдельно оценивает `implementation_ready` и `checkpoint_ready`. Первый требует корректных данных/численных инвариантов/runtime/evidence/resources. Второй дополнительно требует effective coverage и GRU task success не ниже paired native больше чем на 15 процентных пунктов по каждой основной задаче. MLP остаётся самостоятельным сравнением. Это диагностический допуск; release, realtime задержки и PPO улучшение проверяются следующими этапами.

Reporter связывает model input строго с фактически доставленным `body_observations` IPC row. Исходный native JSON journal проверяется отдельно: структура, типы, IDs, flags и masks совпадают точно; для конечных float допускаются максимум два binary64 ULP, измеренных при parse/re-encode RapidJSON. Этот предел не является допуском на изменённые физические данные или разные входы policy.

Для исправления read-only reporter у уже завершённого frozen run предусмотрены `prepare-audit`, `audit-tests`, `audit-report`. Они сохраняют новый код в `validation/audit-code` со своим SHA manifest, проверяют неизменность исходных source/config/helper/failed-report и создают результат в `validation/audit-v1`. Предыдущие frozen исходники, отчёты, траектории, выбор весов и checkpoints сохраняются.

Фактические результаты первого запуска: [validation notes](../reports/npc-body/2026-10-14-bc/validation-notes.md).
