# Native melee reference, stage 1

`melee-v1.json` versions the setup of `scripts/bench/body-melee.bench`: three seeds (17, 29, 43), two
native human NPCs, each with a sword, leather armour, 300 HP, 60 strength/dexterity, 60% one-hand
chance, master one-hand animations and 30 edge/blunt protection. Each duel stops when either NPC is
down or after 60 seconds of game time. A timeout is truncation, not a victory. The player stands about
12 m away as an immortal observer. Both combatants remain mortal and use native `ZS_ATTACK`/fight AI.

The `bodyduel` bench command owns direct NPC pointers and creates fresh participants for every seed.
It removes the previous pair, unequips inherited gear before clearing the source inventory, equips the configured sword/armour,
sets the stats after equipment callbacks, checks ground/equipment/mortality, and seeds construction
and native combat separately. It fails the scenario on missing script symbols or failed setup. The
bandit and paladin retain hostile guilds. Trace IDs refer to NPC object lifetimes; repeated script
symbols or reused memory addresses do not identify a participant. Melee trace events involving the
player or another third participant make the report incomplete.

## Run on dev-ubuntu

From the repository root, use `scripts/body-baseline-remote.sh` for the user-local server build,
reference run and collection. Keep one game process at a time. The launcher forces the Linux
lavapipe CPU Vulkan renderer under Xvfb. This is **stage 1 reference**, and does not satisfy the stage 2 requirement for a
simulation executable without a graphics device. Mac is used for editing and short parser checks;
the baseline game and all subsequent simulations/training run on the server.

Full setup/run/collection from the Mac (only SSH/rsync and lightweight tooling run locally):

```sh
GOTHIC_DIR='/Volumes/DevSSD/Dev/games/Gothic II' \
BASELINE_SAVE="$PWD/save_slot_1.sav" \
  scripts/body-baseline-remote.sh all melee-reference
```

Use paths to your own assets/reference save if they differ. The launcher stores its own source,
toolchain, assets, binary and runs under `~/gothic-ai/body-baseline/` on `dev-ubuntu`. It uses an
ephemeral user systemd unit with CPU/RAM/time limits; it does not restart Docker or production units.
The scenario records `novideo`; it still uses the current graphical engine. No model service, API,
ASR/TTS process, artificially applied hit, held player attack or learning loop is involved.

After setup, a new run and its local collection can be requested separately:

```sh
scripts/body-baseline-remote.sh run melee-reference-repeat
scripts/body-baseline-remote.sh collect melee-reference-repeat
```

Run names identify immutable output directories; choose a new name for each attempt. `sync`,
`toolchain` and `build` are available as separate actions. The game starts from a copied save and
silent `Gothic.ini`; the engine uses Gothic II Night of the Raven data. The launcher refuses to start while another game/runner is active.

## Generate a report

`collect` copies raw output to `data/npc-body/reference/<run-name>/` in this repository. The manifest
is `manifest.json`; `resources.txt`, `launcher.log`, `game.log` and `events.jsonl` provide the measured
run evidence. Generate the compact committed report separately:

The pre-headless engine binary stays on the server in `<run>/reference/Gothic2Notr`; source is
archived as `source.tar.gz` with hashes in the manifest. `collect` omits the binary and private save.
The launcher executes the copied binary, so later builds cannot replace the running reference.

```sh
python3 scripts/body_baseline_report.py data/npc-body/reference/melee-reference \
  --manifest data/npc-body/reference/melee-reference/manifest.json \
  --output-dir reports/npc-body/melee-reference
```

Outputs are `body-baseline.md` and `body-baseline.json`. Exit code 0 means the expected seeded setup,
terminals, native choice/consume/attempt evidence, complete guard snapshots with the configured
phase/cadence, and measured damage are present and a manifest was supplied. Exit code 2 means
missing/invalid evidence. A crash or missing terminal never becomes a duel timeout. The report flags
duplicate JSON keys, sequence/time regressions, schema mismatch, setup mismatch, a third combatant,
and non-melee or unknown damage kinds.
It does not assume a requested or consumed animation landed a blow.

The manifest should preserve measured provenance: engine revision/dirty state and binary hash,
scenario/config hash, asset/script manifest and save hashes, host/toolchain/settings, renderer/device,
wall duration, CPU/RSS, GPU renderer allocation and relevant service RSS/availability before/after.
Unavailable measurements must be marked unavailable. The reporter copies this JSON without adding
invented build hashes, resource values or throughput. Raw trace files stay in the ignored run
directory; commit the compact metadata/report and hashes, not private game assets or saves.

## Interpretation and readiness

- `native_choice` identifies a generated fight instruction; `native_consume` records queue
  consumption. `native_attempt` records animation hooks from `set_anim` and `do_attack`, including
  weapon draw, idle and hit reactions while a target is present. Its `accepted` and `pose_changed`
  fields describe animation/pose behavior. These are separate events, and none alone proves damage.
- Animation hooks carry the current or last materialized `choice_id` and queued `primitive`, which
  may be 0. The reported hook count and interval include these animation calls rather than only
  newly selected fight actions. `choice_to_first_attempt_ms` measures elapsed game time to the first
  hook sharing a queue choice ID; it is an association, not policy response latency or proof that
  the choice caused that animation. Read the raw `hook`, `primitive`, requested animation and pose
  fields when interpreting a particular call. Reporter field names remain stable for schema v1.
- Snapshots come from `npc_pre_tick_animation_tags`. Guard booleans and animation layouts are
  privileged telemetry for analysis, not a validated future policy observation stream.
- Guard intervals are consecutive observed engine flags: `prehit`, `defence`, `jumpback`,
  `attack_anim`, `def_window`, `def_par_window`. They are not guessed windup/hit/recovery labels.
  The fixed 30 Hz substep is 33/34 ms; the 50 ms periodic snapshot target is quantized to up to
  67 ms, with additional snapshots on flag/layer edges. Subframe timing remains unknown.
- Duration quantiles exclude intervals already true at the first sample or still true at the
  terminal. Their censored counts remain visible. Animation frame metadata is preserved separately.
- Compare outcomes, HP loss, block/jump-back results, native action distribution, attempt/consume
  behavior and guard timing across these seeds. Three seeds establish an instrumented reference,
  not statistical superiority, training throughput or determinism. Source save/world time remains
  part of provenance; repeated seeds do not promise bitwise-identical traces.

Stage 1 is ready when the contract is checked against these measured native events, the versioned
scenario produces valid mortal melee episodes on `dev-ubuntu`, and the committed report records
timing and provenance without hiding missing evidence. The next gate remains a true headless
runner with parity against this same reference, including animation tags, damage and native queue
consumption.

Parser checks (run once by the coordinating agent, without a game):

```sh
cd server
uv run pytest ../scripts/tests/test_body_baseline_report.py
```


## Stage 2 headless runner

The real CPU runner is documented in [npc-body-headless-runner.md](../../docs/npc-body-headless-runner.md).
It runs on dev-ubuntu without a window, graphics backend or audio device:

```sh
scripts/body-headless-remote.sh preflight
scripts/body-headless-remote.sh sync
scripts/body-headless-remote.sh build
scripts/body-headless-remote.sh run stage2-validation
scripts/body-headless-remote.sh collect stage2-validation
```

The suite performs an actual same-source CPU-rendered control run, then headless lockstep,
delayed lockstep, realtime, archived animation-branch replay, senses, 100 resets and the IPC probe.
Only one game/runner is active at a time. The archived reference remains unchanged. See
[the comparison contract](headless-parity.md) for isolated RNG, exact branch consumption and the
scope of historical replay. `replay <fresh-name>` runs the bounded archival check alone; it does
not establish full stage 2 readiness. A normal historical RNG divergence is retained as diagnostic
output; all other required gates must pass for the full suite to exit 0.

The complete `stage2-v3` run passed on 2026-10-13:
[results and limits](../../reports/npc-body/2026-10-13-headless/headless-report.md).
After collection, generate a compact report without running a game:

```sh
python3 scripts/body_headless_report.py data/npc-body/headless/stage2-v3 \
  --output-dir reports/npc-body/2026-10-13-headless
```

## Stage 3 numeric observations

[body-observe-v1.json](body-observe-v1.json) fixes cadence and overhead budgets for a passive
20 Hz stream from four native NPCs. [Schema/runtime](../../docs/npc-body-observations.md) documents
post-simulation phase, visibility, runtime IDs, advisory availability and bounded queue semantics.

```sh
scripts/body-observe-remote.sh preflight
scripts/body-observe-remote.sh sync
scripts/body-observe-remote.sh build
scripts/body-observe-remote.sh run stage3-validation
scripts/body-observe-remote.sh collect stage3-validation
```

The full suite runs three on/off performance pairs, bounded overflow, a same-phase privacy oracle,
strict native melee parity and ten minutes of realtime. Its reporter validates raw data on dev-ubuntu,
including native combat in every minute. The compact report/provenance and small privacy evidence
are collected locally; long numeric journals remain on the server. `smoke` checks infrastructure only.
No body control or model inference is enabled by subscribing.

## Stage 4 combat API and arbitration

[body-api-v1.json](body-api-v1.json) declares all 12 primitives, lifecycle/fault cases and strict
native-off parity for seeds 17/29/43. The [API contract](../../docs/npc-body-api.md) documents
leases, action masks, command freshness and safe native handoff. A scripted provider uses the
same resolver and physical executor as native AI; no neural model is involved.

```sh
scripts/body-api-remote.sh preflight
scripts/body-api-remote.sh sync
scripts/body-api-remote.sh build
scripts/body-api-remote.sh run stage4-validation
scripts/body-api-remote.sh collect stage4-validation
```

Source/build/runs are isolated under `~/gothic-ai/body-api/`. Full IPC, mechanics and lifecycle
journals remain on dev-ubuntu. The independent reporter reads those journals and the archived
pre-refactor native traces; smoke and driver pass flags cannot establish readiness. Collection
returns compact reports and provenance. Test fixtures set initial states or inject failures;
animation, movement, collision and damage execute in the real engine.

## Stage 5 arena and episode runner

[body-arena-v1.json](body-arena-v1.json) fixes the reset, task, paired comparison and throughput
criteria. [Arena runtime](../../docs/npc-body-arena.md) documents the native episode API and
reusable Python environment.

```sh
scripts/body-arena-remote.sh preflight
scripts/body-arena-remote.sh sync
scripts/body-arena-remote.sh build
scripts/body-arena-remote.sh run stage5-validation
scripts/body-arena-remote.sh collect stage5-validation
```

The full series uses one dev-ubuntu runner: 100 dirty resets with a final canonical restoration,
distance/attack/block/terminal/timeout tasks, native/scripted pairs on both sides and a separate
throughput collection. Damage, motion and an active wait dirty each reset through real body
commands. A fixed save and two mortal profiles form each initial state. Raw journals remain on
the server; the independent reporter reconciles both fighters' counters with native events and
checks exact terminal samples, generations and physical side swaps.

Measured policy transitions require an accepted non-continue action and an actual execution
start. Locked observation ticks and transport queries do not count. Collection feasibility for
32768 decisions in six hours is separate from correctness/readiness of the environment; no
neural inference or optimizer performance is claimed. The source/build cache is private to this
worktree under `body-api`, while new runs/provenance live under `~/gothic-ai/body-arena/`.

The full `stage5-v2` series passed on 2026-10-13:
[results, collection forecasts and limits](../../reports/npc-body/2026-10-13-arena/validation-notes.md).

## Stage 6 combat trajectories and dataset

[body-trajectories-v1.json](body-trajectories-v1.json) fixes the initial native corpus, scripted
microtasks, recorder parity, grouped splits and coverage target. [Recorder and dataset contract](../../docs/npc-body-trajectories.md)
documents causal native snapshots, execution links and the ordered sequence loader.

```sh
scripts/body-trajectories-remote.sh preflight
scripts/body-trajectories-remote.sh sync
scripts/body-trajectories-remote.sh build
scripts/body-trajectories-remote.sh run stage6-validation
scripts/body-trajectories-remote.sh collect stage6-validation
```

Raw trajectories stay in `~/gothic-ai/body/data/` on dev-ubuntu. Collection returns manifests and
reports. Both fighters, mirrored placements and related repeats share a split group; normalization
and class weights use train only. OOD is held separately. Periodic locked rows update recurrent
memory without BC loss. Actual native decision snapshots carry a valid shared resolver mask;
the teacher queue and IDs remain diagnostics and never become model features.

Dataset validity and the minimum 20 expert labels per included class are separate gates.
Missing actions or conditions appear in the coverage report; no model is trained at this stage.

The full `stage6-v1` series passed on 2026-10-14:
[results and limitations](../../reports/npc-body/2026-10-14-trajectories/validation-notes.md),
[dataset index](../../reports/npc-body/2026-10-14-trajectories/dataset-index.json),
[coverage](../../reports/npc-body/2026-10-14-trajectories/coverage.md).
It contains 23 episodes and 681 usable BC labels (455 train). Diagnostic BC coverage remains
false: turn, jump-back, wait-300 and the wall condition need additional demonstrations.

## Stage 7 GRU, MLP and behavioral cloning

[body-bc-v1.json](body-bc-v1.json) declares GRU-256, an MLP with eight observations of history,
the shared numerical feature/action schema, CPU budgets, checkpoint selection and arena gates.
[body-demonstrations-v1.json](body-demonstrations-v1.json) declares 37 additional episodes:
24 train, four eval, four test, three near-wall cases requiring physical verification across
those splits, and two OOD cases.
The scripted teacher targets approach, turn, the three strikes, block, jump-back and wait-300.
Existing immutable native strafe-left/right and wait-200 examples supply the other classes.
The combined **periodic model view** must still contain at least 20 usable train labels for
each of the eleven non-continue actions; declaring or collecting an episode does not establish coverage.

Scripted expert approval requires registered teacher code/config hashes, a fresh periodic input,
an accepted command, a real execution start/completion and independent physical outcome proof.
Approval creates separate derived records and never promotes an old stage 6 scripted diagnostic
to expert by changing its label. Wall evidence comes from existing collision geometry, native
capsule probes, grounded placements and a verified outward route.

The model consumes only actual 20 Hz periodic `post_simulation_post_animation` rows.
Exact native decision captures remain audit evidence; eligible native labels align to the latest
causal periodic input within 67 game ms, with ambiguity and unavailable actions excluded.
Locked rows update memory without expert loss or body commands. Empty raw masks are retained
and use a shared continue-only model head for memory. Unexpected sequence or periodic-bucket
gaps reset memory and fall back explicitly.

Both architectures share the dataset, feature packer, train-only normalizer and derived policy mask.
Related sides, geometry variants and repeats retain the stage 6 seed-group assignments across
train/eval/test/OOD. Chunks contain 32 observations and up to eight prefix observations; prefix,
locked and non-expert rows contribute no BC loss. A train-only tiny overfit checks the pipeline
and produces a diagnostic checkpoint that cannot control a live NPC. Training selects epoch and
seed using eval macro F1 across all eleven actions, then eval NLL. Test/OOD metrics are generated
only for the selected frozen weights and never participate in selection.

Use a fresh run name. These commands perform all simulation, model forwards and training on
dev-ubuntu; the Mac launches orchestration and receives compact artifacts:

```sh
scripts/body-bc-remote.sh preflight
scripts/body-bc-remote.sh sync
scripts/body-bc-remote.sh env
scripts/body-bc-remote.sh build
scripts/body-bc-remote.sh prepare stage7-bc-validation
scripts/body-bc-remote.sh tests stage7-bc-validation
scripts/body-bc-remote.sh setup stage7-bc-validation
scripts/body-bc-remote.sh smoke stage7-bc-validation
scripts/body-bc-remote.sh demos stage7-bc-validation
scripts/body-bc-remote.sh dataset stage7-bc-validation
scripts/body-bc-remote.sh verify stage7-bc-validation
scripts/body-bc-remote.sh overfit stage7-bc-validation
scripts/body-bc-remote.sh train stage7-bc-validation
scripts/body-bc-remote.sh evaluate stage7-bc-validation
scripts/body-bc-remote.sh arena-smoke stage7-bc-validation
scripts/body-bc-remote.sh arena stage7-bc-validation
scripts/body-bc-remote.sh report stage7-bc-validation
scripts/body-bc-remote.sh collect stage7-bc-validation
```

`setup` verifies all declared physical placements. `smoke` checks demonstration infrastructure;
`arena-smoke` exercises one declared case with native/GRU/MLP and the real JSONL service probes.
Neither smoke establishes the full coverage or checkpoint gate. The full arena comparison has
54 episodes: distance, attack, block and duel on eval seeds 29/113 and both sides, plus two
near-wall duel cases, each run with native, GRU and MLP. Microtask baselines keep the same
scripted opponent as the learners while the primary uses native AI. Duel opponents use native AI.

Every learned command uses the exact latest periodic native row and actual shared arbiter mask;
event-only rows cannot advance hidden memory or issue commands. Per-episode journals bind
processed inputs, logits, selected actions, receipts/execution IDs, checkpoint and normalizer hashes.
Terminal snapshots are saved before the matching memory-clear handshake. The separate real
`serve` probes exercise cached retries, gaps, stale sequence/epoch, checkpoint changes, terminal
closure, stale terminal requests and fresh-reset numerical parity with the same trained weights.

The independent report separates implementation completeness from checkpoint readiness.
Per-task GRU success may trail paired native by at most 15 percentage points in this diagnostic
set; MLP behavior is reported separately. Actual native continuation after restoration earns no
model credit, and ordinary terminal cleanup alone does not imply native execution. Time limits,
control loss, infrastructure failures and physical task success remain distinct outcomes.
Raw journals and weights stay under `~/gothic-ai/body/data/<run>/`; code/config and the runner
are frozen per run. CPU quota, RSS, timings and the identities of all four protected services are
recorded. This section describes the workflow and required evidence; it does not assert that
training or admission gates have passed.


## Stage 8 combat gate

The [workflow](../../docs/npc-body-combat-gate.md) and
[declaration](body-stage8-v1.json) keep historical weights/data immutable,
select only fresh eval groups with continuous episode inference, and reserve
fresh screening for fixed weights. All simulations, numerical tests and model
forwards run on dev-ubuntu.

Use a fresh run name for every changed snapshot. The extended
`scripts/body-bc-remote.sh` provides `realtime-probe`, `combat-geometry-preflight`,
`combat-demos`, `stage8-dataset`, `stage8-verify`, `stage8-overfit`, `stage8-train`,
`stage8-evaluate`, `probe`, `shadow`, `realtime`, `stage8-faults`, `load`,
`screening` and `stage8-report`. Preflight and calibration never establish
checkpoint readiness. The final report requires actual four-NPC ownership and
fixed service workloads, as well as behavioral and latency evidence.

The additive `visibility-boundary-tests`/`visibility-boundary-probe` and
`compact-reply-tests`/`compact-reply-probe` phases check the actual engine without
loading a model. [Motor-v13](../../reports/npc-body/2026-10-14-combat/motor-v13/README.md)
has passed full native 17/29/43 parity, API37, realtime4, visibility8, compact10
and focused tests 10+49 on binary
`cd8abf1364b736a2d961f3969316e73b99a04b39881f644bbc711550506a2a8c`.
Use the same proof only with exactly that binary/source snapshot.

**Stage 8 is still in progress.** Child weights are fixed and numerically audited.
Gate-v3 has collected its preregistered known matrix: 3 shadow, 24 ordinary and
6 reaction cases. Independent known audit R2 has passed: GRU reaction shows
two actual blocks, HP300, neural control 1 and fallback 0. Its first
reader wrongly required `shadow=true` for the native comparator despite empty
controls/no ownership; that failed report is retained and only the separately
frozen reader predicate is corrected. The
[known-only summary](../../reports/npc-body/2026-10-14-combat/gate-v3/README.md)
binds the exact external snapshots. The load hooks are now wired. The subsequent
[gate-v4](../../reports/npc-body/2026-10-14-combat/gate-v4/README.md) collected
known 3/24/6 but failed one GRU distance control gate: 2/3 accepted decisions,
one strict stale rejection, physical task success and no fallback. Its old
logs have no tick BEGIN/native READ, so a future observation timestamp cannot
establish input arrival before tick start. Current native proof is
[Motor14](../../reports/npc-body/2026-10-14-combat/motor-v14/README.md) plus
[queue15](../../reports/npc-body/2026-10-14-combat/motor-v15-queue/README.md)
on the same `ac2dfe…5263` binary; 13 queue priority admissions were heartbeats,
not 13 body actions. Gate-v5 collected known 3/24/6 but its audit failed one
GRU reaction model-step budget: the second, locked memory-only step took
18.962995 ms (backend 18.459441 ms), above the unchanged 5 ms limit.
Both actual blocks retained HP300, control 1 and fallback 0; the first block
decision took 1.243341 ms. The spike remains counted and no per-step CPU/thread
samples establish an OS-stall cause. [Gate-v5 evidence](../../reports/npc-body/2026-10-14-combat/gate-v5/README.md)
preserves all four steps and both warmups. Frozen gate-v6 passed CPU profile
tests48 and final evaluation tests63. Its [closed known audit](../../reports/npc-body/2026-10-14-combat/gate-v6/README.md)
passed 3/24/6 with empty issues. All 2205 real forward calls used requested/
actual PyTorch intra-op1; interop12, default CPU2, weights and thresholds are
unchanged. GRU maximum per-case model p95 is 1.440461 ms, healthy control minimum
is1, and both real reaction blocks retain HP300/damage0/fallback0. The subsequent
[load failed](../../reports/npc-body/2026-10-14-combat/gate-v6/load-failure.md):
native4/GRU4/MLP2 completed, third MLP cohort released after observation_too_old,
and protected Gemma p95 degraded36.87%/38.34% under native/GRU load above10%.
All partial-cohort HP stayed3000 with no damage or LOS loss. Separate
[GC diagnostics](../../reports/npc-body/2026-10-14-combat/collector-gc/README.md)
do not prove the old169 ms gap cause. [Gate-v7 startup failed](../../reports/npc-body/2026-10-14-combat/gate-v7/README.md)
before quality cases (shadow0): private UV Python3.12.13 already owns375 frozen
objects before imports. The exact version/path/executable SHA opt-in bootstrap
passed actual375→0/freeze8891/close0 with dynamic GC enabled and private
thresholds700/10/10 preserved; pure tests28 passed. Unknown nonzero ownership is
not normalized. Frozen Gate-v8 preregistration passed50.177 s; after the shared
simulation lock cleared, runtime76/final70 passed. The [known8 audit](../../reports/npc-body/2026-10-14-combat/gate-v8/README.md)
passed closed3/24/6 with empty issues; both GRU reaction blocks retain
HP300/damage0/block1/fallback0. Registered repaired-population GRU control is1;
retained neutral-block diagnostic8/9 remains visible separately. All2518 real
forward profiles are CPU intra-op1, with closed GC bootstrap/static/restore
proofs. The [closed Gate-v8 load](../../reports/npc-body/2026-10-14-combat/gate-v8/load-failure.md)
failed after5/12 completed cohorts (native0–3 and GRU0): partial GRU1 action8
was source-mask valid but correctly refused by the current physical mask while
its target strafed. Native20/20 protected p95 changed −0.97253% ASR (PASS),
+10.02641% TTS, +24.24811% decider and +51.08487% Gemma (FAIL against10%).
Incomplete GRU5/20 is unevaluated. This load used native ac2.
[Motor16](../../reports/npc-body/2026-10-14-combat/motor-v16/README.md), binary
a1695d…bc0c2, passed full native parity17/29/43 with zero differences,
realtime4, visibility8, compact10 and API37; full-stage4 issues are empty.
The separate [Queue17](../../reports/npc-body/2026-10-14-combat/motor-v17-queue/README.md)
failed actual native post-tick ordering: `body_control_status` requests8/81/153
replied before an already-due next tick in started/unpaused realtime. For
request8, Tick17 END was1112734195979706 ns, next deadline1112734176246554 ns,
READ1112734196027605 ns and reply1112734196764587 ns at step17/game16327;
only afterward did Tick18 BEGIN1112734197511767 ns and END1112734214846055 ns
advance to step18/game16361. These are actual Linux CLOCK_MONOTONIC values.
Gate9 targeted runtime101/final78/combat-reader82 tests passed; quality cases
and model forwards remain zero. The narrow native fix passed
[Motor18](../../reports/npc-body/2026-10-14-combat/motor-v18/README.md) and
[Queue19](../../reports/npc-body/2026-10-14-combat/motor-v19-queue/README.md), runner
`681a1704364e07836f839c3e4382b16ae43b28ae3bf75fb46f7884e2b10a1359`.
Motor18 passed full native parity 17/29/43 with zero differences, API37,
realtime4, visibility8 and compact10. Queue19 passed the same unchanged four
strict probe checks: 18 actual WRITE admissions were all heartbeats, 10 fresh
query chains and three wait11 accepted/actually started with zero rejections;
six reader tests passed. These are not 18 combat commands. Failed Queue17
remains unchanged. The [Gate10 known collection](../../reports/npc-body/2026-10-14-combat/gate-v10/README.md)
failed before load/reaction/fresh: shadow3 and ordinary22 completed, followed by
partial `realtime-native_duel-223-1-gru`. Its 146 model inputs contain 145 actual
forwards and one age fallback; process wall p95/max were 2.591128/9.772362 ms.
Twelve of 13 eligible opportunities were accepted; one stale refusal gives
control0.9230769 below threshold. Source observation335/game23527 had
receive→process10.674 ms, receive→send12.8856 ms and model step0.87635 ms.
Later observations337/339/341/343 reached game23727; oldest337/game23561 aged
166 ms triggered `observation_too_old`, hidden reset and release6046, then
`native_restored` at game23761 and `unexpected_control_loss:released`.
Masked retry budget was not exhausted.

Native RPC6041 observations took187.74668 ms, RPC6042 action78.658525 ms,
RPC6044 events81.912808 ms and RPC6045 observations19.113965 ms; native
steps231/232/233/234/236 took70.005/103.477/81.268/89.944/71.352 ms against
usual9–10 ms. Snapshot packet p95/max0.420/1.96 ms and nearby GC0.349 ms do
not explain these delays; their cause is unproven. The separate no-model
native CPUv2 diagnostic retained 606 response attestations and CPU −23.31%, but ticks were
slower; these diagnostic measurements do not prove the Gate10 cause.

[Motor20](../../reports/npc-body/2026-10-14-combat/motor-v20/README.md) and
[Queue21](../../reports/npc-body/2026-10-14-combat/motor-v21-queue/README.md)
closed PASS on serial-native runner `a83efa28236ff9917b7e7d60a4a508de6feedd31d8d9ff4bc69adeb20ab777b7`. Motor20 passed full native parity
17/29/43 with zero differences, API37, realtime4, visibility8 and compact10.
Queue21 passed the same four strict checks and six reader tests. All20 ready
admissions were `body_heartbeat`; fresh query chains7, four wait11 accepted
and actually started, rejected0. Queue19 counts10/3 do not apply. Typed `worker_execution` header fields are `serial=true`,
`mode=caller_serial`, `worker_pool_created=false`, `native_task_parallelism_cap=1`;
the manifest has `native_serial_workers=true`. All2107 actual Queue21 response
packets matched this profile; attestations are not proof-case counts or a
whole-process Thread1 claim.
Gate11 is frozen: preregistration PASS26.768 s and targeted runtime101/final94
PASS. Independent known audit closed3/24/6: PASS39.393 s with empty issues;
reader508cf… verified the actual serial worker profile. Load actually started
at17:47 UTC after the foreign physics-disabled regression linux-v8 closed and
released the simulation lock, unit `gothic-body-bc-stage8-final-gate-v11-load`.
The actual log records baseline before any engine lease/tick:80 s baseline,
then12 cohorts (native4/GRU4/MLP4) with identical20 measured protected-service
requests before/during and the unchanged10% p95 budget.
The [closed Gate11 load](../../reports/npc-body/2026-10-14-combat/gate-v11/README.md)
failed with exit2: unit wall3:52.841, CPU1:38.928, memory5.6G. Native0–3 ended
`time_limit`; GRU0/2/3 ended `control_lost`, GRU1 `time_limit`; partial MLP0
explicitly released control and ended infrastructure
`unexpected_control_loss:released`. [Causal compact](../../reports/npc-body/2026-10-14-combat/gate-v11/load-failure.md)
binds actual GRU target_visibility_lost after completed RightStrafe4/Idle/no
active lock; fallback0 does not grant handoff credit. GRU controls are19/24,
153/158,32/35,75/78. Partial MLP source274 was eligible but live target_invalid
refused Approach1 while owned; receive→send4.848299 ms, native snapshot→send
6.709377 ms, RTT19.703637 ms; explicit release caused infrastructure.
Native20/20 protected ASR/TTS −1.24908%/+8.36735% PASS,
Decider/Gemma +10.59663%/+31.21631% FAIL. All GRU populations16/15/20/20 are
invalid windows; actual before/after4PIDs match, no restart is claimed.
Stage8 is now **STOPPED_AT_PREFLIGHT**. The single SafePeer calibration closed
six HP-preserving episodes without infrastructure failure, but approved actions
2/5/11 were0/2/0 against2/2/2. The72-episode collection, child279, new training
and a new combat gate never started; the private placement diagnostic is
cancelled. There is no retry or follow-on run scheduled. Selected207 weights,
normalizer, strict masks/freshness/visibility100 and combat thresholds remain
unchanged. Protected services are reported separately with the unchanged10%
budget. [Final stop report](../../docs/npc-body-stage8-stop-report.md).
Partial results do not establish the gate. The earlier exact lock refusal had no
launcher-load/service baseline/output/requests and remains historical startup
waiting, not a quality failure. Fresh screening has never started.
Guarded masked refresh and opt-in actual native-deadline polling retain weights,
normalizer, physical masks and quality thresholds.
Earlier failures and the closed load8 result remain preserved. Fresh screening
has never started; there is no overall combat gate PASS.

The optional active/unpaused realtime transport header
`next_tick_wall_monotonic_ns` records the actual Linux CLOCK_MONOTONIC tick
deadline, including a deadline already in the past. `native_deadline_polling`
requests slices≤5 ms and records actual overshoot; all periodic/event rows and
latency counts remain retained. The184 model feature schema is unchanged.
Known PASS does not establish the overall
combat gate.
Native parity, valid demonstrations or numerical evidence alone do not
establish checkpoint readiness. Existing v6
weights, data, neutral block failures and failed smoke directories stay immutable.

### Reactive defence child

[body-defence-demonstrations-v1.json](body-defence-demonstrations-v1.json) declares
72 real block demonstrations: 36 train and 12 each eval/test/OOD. Both bodies are
held only while the opponent begins an actual attack. The exact prepared periodic
primary observation starts the learner's scene with zero hidden state and `dt=0`.
Preparation remains in raw/canonical journals and contributes no model history or
expert loss. A block label requires accepted/started/completed action 9, an actual
mapped incoming hit paired with the native blocked-melee result, and unchanged HP.

[body-defence-bc-v1.json](body-defence-bc-v1.json) combines the immutable selected
135-episode v6 index with those 72 episodes. Its 207-episode child preserves group
assignments and fits normalization on combined train streams only. Six GRU/MLP
jobs select epoch/seed using full contiguous eval episodes from groups 223/227;
test/OOD and fresh screening do not select weights. The v3 preparation helper and
the current same-binary native motor proof are separate declared dependencies.

Run each phase serially with the coordinating agent. These are launcher commands
from the repository; simulations and numerical work execute on dev-ubuntu:

```sh
BODY_CHILD=stage8-defence-bc-v2
scripts/body-bc-remote.sh sync-code
scripts/body-bc-remote.sh prepare "$BODY_CHILD"
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh defence-tests "$BODY_CHILD"
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh defence-smoke "$BODY_CHILD"
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh defence-demos "$BODY_CHILD"
scripts/body-bc-remote.sh defence-dataset "$BODY_CHILD"
scripts/body-bc-remote.sh defence-verify "$BODY_CHILD"
scripts/body-bc-remote.sh defence-overfit "$BODY_CHILD"
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh defence-train-cuda "$BODY_CHILD"
scripts/body-bc-remote.sh defence-evaluate "$BODY_CHILD"
```

Use a new child name for a new attempt; the example name identifies the current
experiment and is not permission to overwrite it. `defence-smoke` collects six
cases and cannot satisfy the full corpus gate. `defence-train` is the CPU training
alternative; choose one training device before the run. Only the explicit
`defence-train-cuda`/`train-cuda` phase enables GPU 0. It caps allocator memory at
the smaller of 4% of VRAM and 1024 MiB, requires 3072 MiB free beyond that cap,
and uses an external watcher for total process GPU memory and free-memory limits.
Its declared ceilings are 1536 MiB for this unit's total GPU memory and a
2048 MiB minimum free-memory floor.
The watcher closes inherited lock descriptors, stops only its exact training
unit on a budget violation, and finishes before locks are released. Verify,
overfit, selected-weight evaluation and runtime gates remain CPU-only.

`GOTHIC_BODY_LOCK_WAIT_SECONDS` accepts integers 0–60. The default 0 fails
immediately when the required lock is occupied; 60 waits for up to one minute.
CUDA training acquires both the training and simulation locks. Tests and games
share the simulation lock. A timeout does not cancel another owner's job.

### Freeze the final gate

After selection and CPU evaluation finish, bind
[body-stage8-evaluation-v2.json](body-stage8-evaluation-v2.json) to the selected
child run/checkpoint/normalizer SHA values **before** freezing a new gate. Policy
code remains explicitly pinned to its declared reference run. The repaired block
protocol uses the immutable measured preparation helper; the other three task
matrices and the 20 fresh screening seeds remain as declared. Fresh screening
opens only after the selected weights and independent numerical audit are fixed.

```sh
BODY_GATE=stage8-final-gate-example
scripts/body-bc-remote.sh sync-code
scripts/body-bc-remote.sh prepare "$BODY_GATE"
scripts/body-bc-remote.sh prepare-model-reference "$BODY_GATE" "$BODY_CHILD"
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh final-evaluation-tests "$BODY_GATE"
```

`prepare-model-reference GATE SOURCE` copies only selected checkpoint bytes and
sidecars into `GATE/models/`, plus the selected normalizer into both
`GATE/dataset/normalization.json` and `GATE/model-reference/dataset/normalization.json`.
It preserves source selection/environment provenance in
`GATE/model-reference/declaration.json`. It copies no dataset index and makes no
new training/selection claim. Source/copy SHA values and the canonical runtime
normalizer are checked before every model-runtime phase. All paths below are on
dev-ubuntu under `~/gothic-ai/body/data/`; the Mac does not load checkpoints.

Use the already closed independent numerical audit, or run its separately
frozen source into a new audit output directory. Preserve the existing passed
report and both earlier failed readers. Preregister the new gate before any
quality episode:

```sh
# On dev-ubuntu; set these to the actual immutable run names.
BODY_ROOT="$HOME/gothic-ai/body"
BODY_CHILD_DIR="$BODY_ROOT/data/stage8-defence-bc-v2"
BODY_GATE_DIR="$BODY_ROOT/data/stage8-final-gate-example"
BODY_REFERENCE_DIR="$BODY_ROOT/data/stage8-combat-v6"
BODY_PROOF_DIR="$BODY_ROOT/data/stage8-motor-v13"
BODY_PYTHON="$BODY_ROOT/.venv/bin/python"
BODY_CHILD_REPORT="$BODY_CHILD_DIR/validation/defence-audit-v3/report.json"

"$BODY_PYTHON" "$BODY_GATE_DIR/code/scripts/body_final_evaluation.py" declare \
  --run "$BODY_GATE_DIR" --reference-run "$BODY_REFERENCE_DIR" \
  --models-run "$BODY_CHILD_DIR" --proof-run "$BODY_PROOF_DIR" \
  --child-report "$BODY_CHILD_REPORT" \
  --prior-failed-run "$BODY_ROOT/data/stage8-final-gate-v1" \
  --additional-failed-run "$BODY_ROOT/data/stage8-final-gate-v2" \
  --prior-known-run "$BODY_ROOT/data/stage8-final-gate-v3"
"$BODY_PYTHON" "$BODY_GATE_DIR/code/scripts/body_final_evaluation.py" bindings \
  --run "$BODY_GATE_DIR"
```

The audit reads saved data and emits no model forward. The declaration binds the
child's independent audit, selected weights, original v6 failures, current native
proof, executable/config hashes and runtime phase aliases. It prohibits retraining
or checkpoint selection after fresh screening. For a different native build,
preregister the actual same-binary proof run instead of reusing `stage8-motor-v13`.
`--prior-known-run` binds the completed external known PASS and its entire
closed source chain; it is not a fresh-screen result. The declared load-only
repair must prove that eight actual runtimes were warmed before any reset and
that compact/dispatch hooks are connected. A new gate still requires its own
external reader snapshot and new load/fresh evidence.

### Runtime gates and final report

For a new known capture, launch the selected fixed models with the coordinating
agent after preregistration. A `--prior-known-run` dependency preserves the earlier
closed result and the reason for the delivery-only retry; it never silently
copies episode outputs. Execute the new run's declared known phases before load:

```sh
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh shadow "$BODY_GATE"
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh realtime "$BODY_GATE"
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh realtime-reaction "$BODY_GATE"
```

Before opening load, freeze both external reader snapshots. The full-reader
snapshot binds the gate environment/declaration and its reader files. If the
native shadow comparator correction is needed, `combat_helper_correction` must
be `shadow_native_comparator_flag_only`, bind the original helper SHA and the
preserved first failed known-report path/SHA, and pass the exact declared
predicate-replacement check. The native comparator must still have no acquired ownership
or body commands. Gate code and raw evidence remain unchanged.

The standalone known-reader snapshot binds the same environment/declaration,
the full audit manifest SHA and its copied reader source. It reads only the
closed known phases; it does not open load or fresh screening. With both
snapshots already frozen, run the actual external entry point on dev-ubuntu:

```sh
BODY_AUDIT_DIR="$BODY_GATE_DIR/validation/final-audit-code-v2"
BODY_KNOWN_DIR="$BODY_GATE_DIR/validation/known-audit-code-v2"
"$BODY_PYTHON" "$BODY_KNOWN_DIR/body_stage8_known_report.py" \
  --run "$BODY_GATE_DIR" --audit-manifest "$BODY_AUDIT_DIR/audit.json" \
  --known-audit-manifest "$BODY_KNOWN_DIR/known-audit.json" \
  --output-dir "$BODY_GATE_DIR/validation/known-audit-v2"
```

Each audit attempt uses a new output directory; preserve a failed reader and
report instead of overwriting it. Only after known audit or its declared closed
dependency passes, continue
serially from the repository launcher:

```sh
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh load "$BODY_GATE"
GOTHIC_BODY_LOCK_WAIT_SECONDS=60 scripts/body-bc-remote.sh screening-reaction "$BODY_GATE"
```

`realtime-reaction` executes the repaired known-case protocol. `screening-reaction`
executes `--phase screening` and writes `GATE/screening/`; resource/service files
retain the execution name `screening-reaction`. The original `realtime` and v6
neutral block failures remain separate evidence. An actual block requires action
9 and a prevented melee hit with HP 300; killing or jumping away earns no block
credit. `load` uses four actual NPCs in one world and the fixed protected-service
workloads. Runtime uses the child's shared 184-feature schema, masks and command
arbiter; motion and damage execute in the declared engine build with its native
parity proof. Every runtime phase retains separate logs, peak-RSS/timing and
before/after service/GPU snapshots.

The coordinating reviewer separately freezes `body_stage8_final_report.py` and
`body_block_reaction_report.py` into an audit directory. Its snapshot JSON has
schema 1, kind `body_stage8_final_runtime_audit_snapshot`, the gate environment and
`final-evaluation.json` SHA values, and hashes for both reader files. Execute that
copied reader with the corresponding manifest and actual same-binary API evidence:

```sh
# On dev-ubuntu, after the independent audit snapshot has been frozen.
BODY_AUDIT_DIR="$BODY_GATE_DIR/validation/final-audit-code-v2"
"$BODY_PYTHON" "$BODY_AUDIT_DIR/body_stage8_final_report.py" \
  --run "$BODY_GATE_DIR" --audit-manifest "$BODY_AUDIT_DIR/audit.json" \
  --native-faults "$BODY_PROOF_DIR/faults" \
  --native-reference "$HOME/gothic-ai/body-headless/runs/stage3-v1/native-off" \
  --output-dir "$BODY_GATE_DIR/validation/final"
```

The output is `body-stage8-final.json`. Exit 0 requires the saved implementation,
behavioral, latency, ownership, fault, softlock and resource gates; exit 2 retains
the failed evidence. The final report distinguishes implementation readiness,
checkpoint readiness and the later release gate. Until this complete flow passes,
stage 8 remains in progress; no final checkpoint readiness is claimed.
