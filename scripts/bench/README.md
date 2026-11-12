# Test bench

## Как работает сейчас

Bench families are extracted into `common/testbench_<family>.cpp`, with shared state types and helpers in `testbench_internal.h`.
The dispatcher calls `bool <family>Command(...)` wrappers for the extracted command groups:
`npc_sim`, `water`, `character`, `monster`, `furniture_parts`, `placed_items`, `item_hulls`, `fracture_pieces`, `destruction`, `body_impact`, `body`, `movable`, `noise`, `spell`, `item_wake`, `living`, `melee`, `routine`, `aim_spots`, `npc_commands`, `world`, `camera` and `overlay`.
Each wrapper handles its command branches in their original order and returns whether it matched.
The dispatcher `exec` and `expandForeach` live in `common/testbench_exec.cpp`.
The frame loop (`TestBench::tick`, `run`, waits, route/volley stepping and loopback sound) lives in `common/testbench_loop.cpp`.
CSV/heartbeat writing, final reports and shared ragdoll formatting live in `common/testbench_report.cpp`.
Saved-item archive observations live in `common/testbench_saved_items.cpp`.
`Bench` is defined in `testbench_internal.h`; its single instance, dynamically initialized settings,
reset/configuration, context builders and the remaining engine hooks stay in `testbench.cpp`.
Healing observations, commands and checks live in `common/testbench_healing.cpp`; its `Healing` state
in `testbench_internal.h` keeps the current use separate from walk-speed references retained across `begin`.
Family functions take that state by reference; the dispatcher also passes the melee-shadow actors.

Water and corpse-water fixtures, observations and helpers live in `common/testbench_water.cpp`.
Their `Water` state and `WaterContext` references to shared bench fields are in `testbench_internal.h`;
Water-film fixtures live in `testbench_water_film.cpp` and use the same shared water helpers.

Hero and human NPC CharacterVirtual fixtures, observations and helpers live in `common/testbench_character.cpp`.
Their `Character` state and `CharacterContext` references are in `testbench_internal.h`;
shared movement samples (`CharacterMoveFrame`) and walkers (`LivingWalker`) are declared there too.
The independent `ownMeshChest*` family lives in `common/testbench_own_mesh.cpp`.

Ground-monster CharacterVirtual fixtures, observations and helpers live in `common/testbench_monster.cpp`.
Their `MonsterCharacter` state and `MonsterCharacterContext` references are in `testbench_internal.h`.
`TestBench::monsterCharacterCollision` stays in `testbench.cpp`; camera/follow commands and render proof live in `testbench_camera.cpp`.

Destruction fixtures, observations and commands live in `common/testbench_destruction.cpp`.
Their `Destruction` state and `DestructionContext` references are in `testbench_internal.h`.
`TestBench::destructionTracesDebug` delegates its body there through `DestructionContext`;
`destructionCamera`, `destructionSkinSample` and `furnitureBody` live in that family;
the other `TestBench::destruction*` hooks stay in `testbench.cpp`.
Shared helpers have one definition and declarations in `testbench_internal.h`.

Body-impact fixtures and commands live in `common/testbench_body_impact.cpp`;
their `BodyImpact` state and `BodyImpactContext` references are in `testbench_internal.h`.
Physics-trace debug commands (movable tables and wall arrows) live in `common/testbench_traces.cpp`;
`PhysicsTracesContext` references their original bench fields, and `TestBench::physicsTracesDebug` is a one-line delegate.
P30 commands (`bodyarena`, `bodylive`, `bodyreplay`, `bodyobserve`/`bodyapi`, `bodyduel`,
`bodyobserveprobe`, `bodyprobe`) and their animation/live helpers live in `common/testbench_body.cpp`.
`BodyAnimation*` state types and `BodyContext` references are in `testbench_internal.h`.
The `TestBench::body*` hooks stay in `testbench.cpp`; the `body_traces` command lives in `testbench_exec.cpp`.

Movable-object, candle, physics-noise and spell fixtures and commands live in `common/testbench_movables.cpp`.
Their `Movables` state, shared `LivingPhysics` samples and `MovablesContext` references are in `testbench_internal.h`.
The dispatcher calls `movableCommand`, `noiseCommand` and `spellCommand` at each group's first original branch;
shared helpers used by other families retain one definition.
The bodies of `TestBench::physicsNoiseDebug`, `physicsNoise` and `physicsNoiseHeard` live in that family,
with one-line delegates passing `MovablesContext` from `testbench.cpp`.
The other `TestBench::` hooks stay in `testbench.cpp`.

Item-wake fixtures, observations and commands live in `common/testbench_item_wake.cpp`.
Living-body counts, observations and `living` / `living-seat` commands live in `common/testbench_living.cpp`.
Their `ItemWake` / `Living` state and context references are in `testbench_internal.h`;
`itemWakeCommand` and `livingCommand` run at their groups' first original branches.
The shared `ragdollNumber`, `ragdollVector` and `ragdollMatrix` live in `testbench_report.cpp`.

Ragdoll archives, observations, arena checks and commands live in `common/testbench_ragdoll.cpp`.
Own-mesh physics and chest fixtures and commands live in `common/testbench_own_mesh.cpp`.
Their `RagdollBench` / `OwnMesh` state types and context references are in `testbench_internal.h`;
`ragdollCommand` and `ownMeshCommand` run at their groups' first original branches.
`ragdoll frame20` lives in `testbench_exec.cpp`; shared ragdoll formatting helpers live in `testbench_report.cpp`.

Archery fixtures, observations, aim helpers and commands live in `common/testbench_archery.cpp`.
Their `ArcheryBench` state, shared `Trail` type and `ArcheryContext` references are in `testbench_internal.h`.
`archeryCommand` is called at the first branch of each of nine command groups, guarded by that group's command names.
`quiver` and `bowgeom` live in `testbench_exec.cpp`; `TestBench::arrowAt` and reset stay in `testbench.cpp`.

Physics, world-data, cadence, waitragdoll / standup, crowd / down / push and world / performance /
streaming / deterministic commands live in `common/testbench_world.cpp`, along with `physicsDump`.
Their `WorldBench` state, shared waypoint `Spot` and `WorldContext` references are in `testbench_internal.h`.
`worldCommand` runs at the first original branch of each of six groups, guarded by that group's command names.
`traces` and `ragdoll frame20` live in `testbench_exec.cpp`; tick lives in `testbench_loop.cpp` and reset stays in `testbench.cpp`.

Manual-aim spot probes (`cansee`, `slopes`/`stairs`, `ledges`, `corner`, `walls`, `perches`, `edge`, `edgedist`)
and their private perch helpers live in `common/testbench_aim_spots.cpp`.
`aimSpotsCommand` runs at the first branch of each of two groups, guarded by that group's command names,
with a reference to the shared `hasPrev` field. `Spot` / `SpotKind` and shared helper declarations are in
`testbench_internal.h`; `findSpots` lives in `testbench_aim_spots.cpp` and `expandForeach` in `testbench_exec.cpp`.

Melee-shadow and melee-physics fixtures, dodge / AI-response helpers and commands live in `common/testbench_melee.cpp`.
Routine-return and guard fixtures, observations, shared hazard-site helpers and commands live in `common/testbench_routine.cpp`.
Their `Melee` / `Routine` state types and context references are in `testbench_internal.h`;
`meleeCommand` and `routineCommand` run at their groups' first original branches.
Tick lives in `testbench_loop.cpp`; reset and the other `TestBench::` hooks stay in `testbench.cpp`;
`meleePhysicsCamera` retains the fixture's original camera setup there.

Cloth fixtures and commands live in `common/testbench_cloth.cpp`; water film fixtures and commands
live in `common/testbench_water_film.cpp`. Their `ClothBench` / `WaterFilm` state and
`ClothContext` / `WaterFilmContext` references are in `testbench_internal.h`.
`clothCommand` and `waterFilmCommand` run at their first original branches; tick and cold-load observation
hooks in `testbench_loop.cpp` pass the family context; the capture hook in
`testbench_camera.cpp` reaches the same water-film family through a context delegate.

Hero and NPC inventory, placement, stats, routines, fights, aiming, hits and dodge commands live in `common/testbench_npc_commands.cpp`.
Their `NpcCommands` state and `NpcCommandsContext` references are in `testbench_internal.h`.
`npcCommandsCommand` runs at the first original branch of each of four groups, guarded by that group's command names;
intervening `npcSimCommand` and `archeryCommand` calls retain their order in `exec`.
Animation commands live in `testbench_exec.cpp`.

Scenario timing, logging, routing, movement and input commands live in `common/testbench_control.cpp`.
Conversation and AI probes live in `common/testbench_dialog.cpp`.
Their `ControlContext` / `DialogContext` references are in `testbench_internal.h`; shared helpers have one definition.
Each family runs at its first original branch, guarded only by its command names.
Intervening animation commands retain their positions in `exec`; tick lives in `testbench_loop.cpp`, reset in `testbench.cpp`.

Camera placement, `views`, framing/track/sight/aim telemetry and the body of `TestBench::capture` live in
[`common/testbench_camera.cpp`](../../OpenGothic/common/testbench_camera.cpp).
Raster overlays, exposure, hit zones, arrow trails, video piping and recording finalization live in
[`common/testbench_overlay.cpp`](../../OpenGothic/common/testbench_overlay.cpp).
`CameraBench` / `OverlayBench` own their original fields in `testbench_internal.h`; `Bench` inherits them
so tick/reset and the other families keep their shared field access. `CameraContext` / `OverlayContext`
carry references to that state and the other fields used by these families.
`cameraCommand` and `overlayCommand` run at the first original branch of their groups; all eleven camera
and both overlay command conditions depend only on `c`. Monster camera/follow calls retain their full
argument guards in `exec`. `TestBench::capture` and the shared contextless camera helpers are one-line delegates.
`Canvas` and `VideoPipe` have shared declarations and one definition per method in `testbench_overlay.cpp`.

Runs a scenario through the windowed game or the headless runner and records metrics and a report.
Windowed scenarios can also capture video with a debug overlay and point at suspicious frames.

```bash
scripts/bench.sh smoke                      # a scenario from scripts/bench/<name>.bench, or a path
BENCH_HOST=local NO_BUILD=1 scripts/bench.sh ik-sweep  # local, skip the incremental build
```

`BENCH_HOST=auto` (default) tries dev-ubuntu, then Ryzen, then this Mac for headless scenarios;
windowed scenarios try Ryzen, then Mac. Both remote hosts use `scripts/ryzen.sh` and copy all output back.
Before remote `bench` and `e2e`, `ryzen.sh` selects Mac base/video parts matching this checkout's
import formats, copies missing parts to the host's cache, and synchronizes both game INI files.
Ready parts in the host cache or shared store need no copy; dev-ubuntu only uses its cache.
When Mac has no compatible part, the host cache/store is searched for this checkout's format
and the same installation as the host's `game-base.json`. A match reports `ready (host)` and
passes its exact fingerprint without copying. If videos have no compatible replacement on
either machine, the original host video fingerprint is retained.
The run receives the exact fingerprints without changing a prepared host's `game-base.json`;
a fresh host gets settings and that pointer automatically. `scripts/ryzen.sh setup` is optional.
Explicit nonempty `OPENGOTHIC_BASE` or `OPENGOTHIC_BASE_FINGERPRINT` skips base selection/copy;
an explicit `OPENGOTHIC_VIDEOS_FINGERPRINT`, including empty, does the same for videos.
If neither Mac nor host has a compatible base, `bench`/`e2e` stop before locks or queue requests:
start the game on Mac on this version to import it. Remote `pytest` instead prints one warning
and continues with the host's `game-base.json`, without forcing a base fingerprint.
`setup` still requires compatible Mac parts and does not use this host fallback.
Each fallback prints one line explaining the SSH, base synchronization, lock, queue-wait or game-startup failure.
Once returned output contains `game.log`, that host's result is final, including a failing exit code;
the game is not run again elsewhere. The remote lock/queue wait defaults to 120 seconds in `auto`
(`RYZEN_LOCK_WAIT`). `BENCH_HOST=dev-ubuntu` and `BENCH_HOST=ryzen` require that host, default to a
900-second wait and never fall back; `BENCH_HOST=local` runs on this machine. `DEV_UBUNTU_HOST`
overrides the dev-ubuntu SSH alias (default `dev-ubuntu`); dispatch uses
`RYZEN_HOST="$DEV_UBUNTU_HOST" scripts/ryzen.sh bench`. A windowed scenario with
`BENCH_HOST=dev-ubuntu`, including `BENCH_HEADLESS=0`, exits with code 2: `окна на dev-ubuntu нет`.
`KEEP=1`, a VideoToolbox encoder, a custom `GOTHIC_DIR` or other Mac-only game paths require local
execution (`auto` falls back, an explicit remote host exits with code 2).
`NO_BUILD=1` only skips a local build; remote hosts always check the engine source hash.

A line `# @timeout <seconds>` in the scenario sets its `TIMEOUT` default (an explicit `TIMEOUT` wins).
A line `# @headless` in the scenario selects `gothic-body-runner --game-parity --mode lockstep`,
without a window, GPU or sound. `BENCH_HEADLESS=1` selects this path for any scenario;
`BENCH_HEADLESS=0` selects the window even for a marked scenario. `KEEP=1` with the headless path
exits with code 2. The runner defaults to `gothic-body-runner` beside `GAME_BIN`; `RUNNER_BIN`
overrides it for local runs. It uses the same `Gothic.ini`, `[GAME]` flags and `OPENGOTHIC_*` environment as the
window; `WORLD` becomes `--world`, otherwise the copied save becomes `--save`. Window variables
`TEMPEST_*` are not needed. A local build selects the `gothic-body-runner` target. On remote hosts,
preparation builds both `Gothic2Notr` and the runner together; a cached engine without an executable
runner is rebuilt and republished for a headless bench. A rebuild without the runner removes its
old executable before publishing the new engine digest.

On dev-ubuntu, preparation defaults to `gq --class quick --threads 6 --mem 10G`
(`RYZEN_JOBS=6`); Ryzen retains its existing build request. dev-ubuntu has no shared storage:
`RYZEN_BENCH_ARCHIVE` is rejected with code 2 before upload.

The headless game request defaults to `gq --class batch --threads 4 --mem 6G`, without display or
GPU-memory resources. `RYZEN_BENCH_CLASS` still overrides the class; a `batch` request's `--max`
is capped at two hours. Unmarked windowed scenarios keep their existing queue settings.
Both paths use the same completion marker, heartbeat, progress, watchdog and post-run checks below.
The runner's final JSON status remains in `game.log`. `report.md` and `report.json` identify the run
as `headless lockstep`, `window` or `offscreen` and include the game's wall seconds; stdout still prints
`bench: game ran N s`.

### Offscreen (`TEMPEST_OFFSCREEN`)

`TEMPEST_OFFSCREEN=1` (Linux/Vulkan; macOS ignores it) runs the windowed game without a window. Tempest
opens no X display (it runs with `DISPLAY`, `WAYLAND_DISPLAY` and `XAUTHORITY` unset), creates Vulkan
without surface extensions, `VK_KHR_swapchain` or a present queue, and `MainWindow` draws each frame into
its own image of `TEMPEST_WINDOW_SIZE` (default 800x600) instead of the swapchain, presenting nothing.
Captures (`shot`, `rec`, `ui on`) are drawn as with a window, so stills, videos and `events.jsonl` match a
windowed run. Nothing waits for the screen, so the limiter at the end of `render` holds a frame to at least
`1000 / TEMPEST_OFFSCREEN_FPS` ms: default 200, the pace of a FIFO window on Ryzen's 199.92 Hz screen, one
frame per game step; `0` removes the limit. `bench.sh` records the run mode `offscreen`; `rec` still needs
`ffmpeg`. Without presenting these are meaningless: the frame profile's `Present` zone,
`LoadProbe::presented`, `[app-cadence]` (`OPENGOTHIC_FPS_LOG=1`), FPS and low-1 %; frame timing stays
with windowed benches. `scripts/ryzen.sh` runs benches offscreen by default; `RYZEN_BENCH_WINDOW=1` and
the other [window conditions](../../docs/ryzen.md#как-работает-сейчас) bring the window back. Explicit `TEMPEST_OFFSCREEN=1` keeps `quiet` offscreen unless a visible window or presentation timing is requested. `metrics.csv.wall_ms` records elapsed wall time per frame; heartbeat events retain wall time and total steps every 10 s, so offscreen frame throughput can be checked without presentation FPS.
[Note](../../reports/compute/2026-11-09-offscreen-bench/README.md).

For performance comparisons (before/after or FPS), run both takes on the same machine:
pin `BENCH_HOST=dev-ubuntu`, `BENCH_HOST=ryzen` or `BENCH_HOST=local`.
`host.txt` and the report header identify the host, including `dev-ubuntu` when its SSH alias is overridden.

`knockdown-launch` проверяет защиту рождения рэгдолла: SLD_826 с `ITMW_2H_SLD_AXE` (W2H) сбивает оглушённого SLD_815, по 20 проб в чередующихся диапазонах 30–40, 50–90 и 100–170 см, затем 5 проб оглушения без удара на контрольной точке и 5 на склоне дуэли. `RAGDOLL_BIRTH_GUARD=1|0` задаёт `[GAME] ragdoll_birth_guard` до загрузки мира. Сценарий генерирует `reports/physics/2026-11-02-knockdown-launch/generate.py`; фиксированные зёрна и подготовка одинаковы в ON/OFF. `ragdoll_birth_peak` пишет пики скоростей COM таза/спины/головы за первые 300 мс симуляции, дистанцию корней, виновника и снятие защиты; замер — после каждого ненулевого `Update`. `stun-drift <цель> <атакующий|player>` перед `stun <цель> posture` наблюдает корень каждые 100 мс от 0 до 1600 мс: проекцию к атакующему, горизонтальное смещение, высоту, анимацию, оглушение, скольжение, полёт и флаг второго такта. Отдельный `knockdown-drift` из того же генератора запускает только 5 ровных и 5 склоновых проб; источник оглушения — герой, цель перед оглушением освобождена от внешнего управления. `put-at <npc|player> <x> <y> <z>` ставит на землю по точным координатам (NPC замораживается и смотрит на героя). Анализ: `python3 reports/physics/2026-11-02-knockdown-launch/analyze.py --drift <папка>`; `summary.csv` нокдаунов не меняется. [Отчёт, команды и анализатор](../../reports/physics/2026-11-02-knockdown-launch/README.md).

Результаты нокдаунов ON/OFF и опыта дрейфа — в отчёте; сценарий дрейфа не запускает 60 нокдаунов.

`game.log` must contain `[bench] done` for a complete scenario (including an explicit `quit`).
Closing the window, a crash, `TIMEOUT` (default 300 seconds), or the stall watchdog before that marker
returns code 1 and prints one reason with elapsed/limit seconds to stderr. Partial output still goes
through `bench_report.py` and the existing scenario checks. Code 0 means completion and successful
report/checks; code 2 covers invalid setup/options. Ryzen admission/transport codes are preserved;
scenario interruption never returns the special codes 75 or 76. After the marker, closing or timing
out a game left open by `KEEP=1` keeps the usual report/check exit status.

With `OPENGOTHIC_BENCH_OUT`, the engine publishes `heartbeat.json` at scenario start and about every
10 wall seconds, including long `wait` and `npcday` commands. It writes a temporary file and renames
it over the previous pulse. Fields are `elapsed_seconds` since scenario start, `frame`, one-based
`command_index`, `command_count`, full `command` text, and `game_time` (`day N HH:MM`). While `npcday`
runs, `npcday_elapsed_minutes` and `npcday_target_minutes` give elapsed and requested game minutes.
The pulse stops at completion; without the output environment variable it is not written.

`BENCH_PROGRESS=300` prints one flushed stdout line every 300 wall seconds while the scenario runs,
for example `bench: 28 min, command 5/5 npcday 1440, npcday 1060/1440 game min`. Before the first
pulse it prints `bench: N min, starting (game.log X KB)`. The time prefix includes world loading.
Game-minute counters in progress lines are integers; fractional minutes are omitted.
`BENCH_PROGRESS=0` disables these lines. On Ryzen they appear during the game in `scripts/gq logs <id>`.

`BENCH_STALL=600` terminates a live, unfinished game if neither the heartbeat nor any other output
file changes for 600 wall seconds. Growing `game.log` counts as loading activity. The watchdog sends
SIGTERM, waits up to 10 seconds, then sends SIGKILL and reaps the process; `TIMEOUT` uses the same
bounded shutdown. `BENCH_STALL=0` disables the watchdog. The watchdog stops after `[bench] done`,
including in `KEEP=1` mode. `BENCH_PROGRESS` and `BENCH_STALL` are forwarded to Ryzen like `TIMEOUT`.
When it kills a stalled game, stderr says `bench: game stalled: no output or heartbeat for 600 s, killed`.
Fake-game checks live in `scripts/tests/test_ryzen.py`; a short live bench checks the engine pulse.

`streaming-flythrough` measures runtime GPU chunk streaming: a god-mode hero
follows the existing WayMatrix path from `NW_CITY_MERCHANT_TRADE_04` through
`NW_CITY_HABOUR_BENCH_01` to `NW_FOREST_PATH_16`. `route <speed_cm_s> <wp1> <wp2> ...`
blocks command execution until the connected path is complete. Each frame places the hero at
the interpolated path position, including waypoint height, using the normal simulation step.
The two passes use 500 cm/s (run) and 2000 cm/s (fly), with ten seconds to settle before each.
It records no video. Each pass waits for all desired Near/Far chunks before measuring;
Outside chunks can be released along the route.

`scripts/bench.sh streaming-flythrough` starts a fresh `NEWWORLD`, selects `quiet` on Ryzen,
and enables frame profiling only for this bench's build. Output `frameprofile.csv` contains
submitted frame intervals; `streaming.csv` contains residency update/work times, per-chunk
publication times (semicolon-separated in a frame), counts and
Near/Far transitions. `report.md` / `report.json` report each pass's frame p50/p99/max,
Near/Far transitions (entering or leaving that state), peak Near count, update/work p99,
publication p99/max (one sample per chunk), published/released counts and GPU allocation
at the end of each pass. An empty route publication sample is `n/a`, not zero. A separate
warmup table measures Far publications before each pass: count, per-chunk p99/max,
maximum publications in one frame and maximum queue time. These samples begin before
the ten-second wait and end at `streaming_ready`; they do not enter route frame or
publication/release counts. Local execution requires `OPENGOTHIC_ENABLE_FRAME_PROFILE=ON` as
well as `OPENGOTHIC_ENABLE_APP_CADENCE=ON`; an unsupported build exits before opening a game.
The run fails if either route does not finish or either pass lacks metrics. Use the same
host, resolution and content overrides for later comparisons; `OPENGOTHIC_STREAMING=0`
disables the manager and deliberately cannot produce this residency measurement.

Run comparison series in alternating order (OFF, ON, OFF, ON), with at least two runs
per side on the same host. Judge `run` and `fly` with `perf_report.py series`: it compares
the median per-run p99 and accepts ON − OFF <= max(1 ms, OFF p99 max − min), so the
measurement's own OFF spread sets the allowance when it exceeds the configured tolerance.
It prints the per-run values and verdicts and writes `series.md` / `series.json` to `--out`
(the last `--on` directory by default); exit code 0 means both passes pass, 1 means a failure,
and 2 means invalid arguments.

```bash
python3 scripts/perf_report.py series --off /path/off-1 /path/off-2 \
  --on /path/on-1 /path/on-2 --tolerance-ms 1.0 --out /path/series
```

`BENCH_HOST=local OPENGOTHIC_STREAMING=0 scripts/bench.sh streaming-shots` and the same
command with `OPENGOTHIC_STREAMING=1` capture `perf-market.png`, `perf-harbour.png` and
`perf-forest.png` from the first viewpoints of those performance scenarios. `streaming wait`
yields a frame for camera/residency updates, then waits until the publication queue is empty
and all desired Near/Far chunks are published (default timeout 60000 real milliseconds).
The wait also includes static vob views. The disabled-streaming path is already ready.
`streaming-shots` ends with `perf-market-teleport.png`: after one update beyond the city's Far
range, it captures the first frame back in the market without waiting for Far publication.
Use `OPENGOTHIC_VOB_STREAMING=0` and `=1` to compare static vob residency. Claude compares
the resulting frames on Mac.

`texture-owners-shots` is the same kind of still comparison for owners that get their texture level by
distance (NPCs, items in the world, interactive vobs): `owner-npc-face`, `owner-npc-body`,
`owner-npc-10m`, `owner-npc-15m`, `owner-chest`, `owner-sword`, `owner-bench`, and
`owner-npc-teleport` — the first frame after a jump back to the NPC, shot without a streaming wait.
The hero waits 20 m under the ground, so the camera alone sets the distance. Compare levels against
the ring rule (`OPENGOTHIC_TEXTURE_SCREEN_MIPS=0`) with `shots_diff.py --shots <names>`; with
`OPENGOTHIC_TEXTURE_TARGET_MB=0` every texture stands on its wanted level. Claude compares the frames.

`streaming-shots` runs with `deterministic on`: the world stands still while `streaming wait`
waits and four frames more once everything is loaded (also when it was loaded on arrival), then the
TAA history and the SSAO history start over at phase 0 and 16 fixed steps run with visibility in
a canonical cluster order (sorted by sphere and meshlets, not by the ids streaming handed out)
before `shot`; texture levels settle and stand still until the shot.
Two runs of one binary then give byte-identical `perf-market` and `perf-harbour` PNGs, and
`perf-forest` and `perf-island` PNGs in most pairs: at 1920x1080 on Ryzen, virtual shadow maps on,
five runs gave 0 changed pixels at the market and the harbour in all ten pairs, 0 or 1 pixel in the
forest and 0, 3 or 35–38 pixels (max 2 levels) on the island, the same pixels every time (issue
#946). `perf-market-teleport` is shot without a streaming wait by design and still depends on
loading. Accept a render change off ↔ on as two consecutive runs on one host and binary, toggled by
the feature's own variable:
`uv run --no-project --with pillow --with numpy python
reports/engine/2026-11-03-streaming-optimisation/shots_diff.py off=<dir> on=<dir>` must report 0
changed pixels at the four waited points (`--all-pairs` compares every pair of several runs). If a
point differs, run off and on once more: the change is accepted when every one of the four points
gives 0 in at least one off ↔ on pair. [Report](../../reports/engine/2026-11-03-streaming-optimisation/deterministic-shots.md).

The virtual shadow maps stay on for the comparison on Ryzen: their GPU passes no longer depend on
the order of shader invocations.
[Report](../../reports/engine/2026-11-10-still-shots-repeat/README.md): the SSAO cycle's phase that
was not reset, the two shader bugs behind the VSM noise (an uninitialised depth in
`fog_epipolar.comp`, a page lost in `vsm_mark_pages.comp`), the second state of a still (whether the
wait had to wait) and what is left.

`motion-vectors` and `motion-vectors-clips` check the motion vectors of objects (graphics plan 1b,
[report](../../reports/render-features/2026-11-10-motion-vectors/step-1b.md)). `motion-vectors` takes stills: a still
world with a moving camera (creatures hidden, indoors), an NPC walking past a still camera, the hero running with
`cam follow`, a tree, the forest and the town banner in the wind. With `OPENGOTHIC_MOTION_VIEW=1` they show the vectors
(hue the direction, brightness the length, black exactly 0; `OPENGOTHIC_MOTION_VIEW_PX` the scale); with
`OPENGOTHIC_MOTION_VECTORS=0` every such still is black. `motion-vectors-clips` records clips a–e (walk, run, fight,
banner and tree, market turn); run it with `OPENGOTHIC_MOTION_VECTORS=0`, `=1` and `OPENGOTHIC_TAA=0` at 1920x1080 on
Ryzen and measure with `reports/render-features/2026-11-10-motion-vectors/metrics.py`.

`taa-reactive-stills` and `taa-reactive-clips` check the TAA reactive mask of particles and animated translucent
(graphics plan 1v, [report](../../reports/render-features/2026-11-10-taa-reactive/step-1v.md)); the mask is on by
default (the user accepted the clips on 2026-11-10). `taa-reactive-stills` takes stills of the night torch, the harbour braziers,
a campfire by day, the waterfall below Xardas' tower, the thunderstorm rune and the shore; with
`OPENGOTHIC_TAA_REACTIVE_VIEW=1` they show the mask (black exactly 0, red to white 0..1). `taa-reactive-clips` records
clips 1-5 (the torch with its sparks, standing and passed by; braziers and smoke by day with the water; a fireball; the
waterfall); run it with `OPENGOTHIC_CONTENT=<abs>/reports/content/remaster/wall-torch/particles/content` (the sparks
layer), `OPENGOTHIC_TAA_REACTIVE=0`, `=1`, `=1` with the view and `OPENGOTHIC_TAA=0` at 1920x1080 on Ryzen with
`WORLD=NEWWORLD.ZEN`, and measure with `reports/render-features/2026-11-10-taa-reactive/metrics.py`.

## План и журнал

- 2026-11-11: `deterministic on` walks visibility clusters in a canonical order (sorted by sphere and meshlets,
  not by the ids streaming handed out). On the Mac it removed the dependence on the id order; on Ryzen four
  runs still give the discrete states of issue #946 (0–32 px), so the acceptance rule stays as it is.
- 2026-11-11: a 1920x1080 clip records in real time on Ryzen (Vulkan): the readback of a captured frame
  reads cached host memory (24–38 ms → 0.6 ms) through one kept staging buffer, and filming draws every
  frame into one image instead of allocating one each step; 31–34 % → 99 % of real time.
- 2026-11-10: a deterministic `streaming wait` holds the loaded world four frames before the reset (a still
  had a second state by whether the wait had to wait: clouds and exposure lag a frame or two); two
  shader fixes took the thread-order noise out of VSM, so stills are compared with VSM on again.
- 2026-11-10: `taa-reactive-stills`, `taa-reactive-clips` (graphics plan 1v); `stats mana <n> circle <n>` lets the
  hero cast a rune.
- 2026-11-10: `deterministic on` also starts the SSAO history over at phase 0 of its sample cycle
  (stills of one binary differed by 350–775 thousand pixels at 1920x1080); the exact comparison on
  Ryzen runs with `OPENGOTHIC_VSM=0`, VSM stills take a tolerance; `shots_diff.py --all-pairs`.
- 2026-11-09: offscreen run mode (`TEMPEST_OFFSCREEN=1`, `TEMPEST_OFFSCREEN_FPS`): the game draws into its
  own image without a window or display; Ryzen benches use it by default.
- 2026-11-05: `deterministic on` for `streaming-shots`: frozen world during `streaming wait`, TAA reset,
  16 fixed steps, ordered visibility and settled texture levels before `shot`; byte-identical pairs
  at four points on Mac in most pairs, rare single-point residue (fixed by the entry above).
- 2026-11-02: route headless `auto` benches through dev-ubuntu → Ryzen → Mac; keep windowed
  routing unchanged. Reuse `ryzen.sh` with dev-ubuntu build limits and setup/cleanup without NAS.
- 2026-11-02: remove stale runners from window-only rebuilds before caching; keep `RUNNER_BIN` local.
- 2026-11-02: add marked headless lockstep benches with game-parity simulation and a Ryzen `batch`
  request without display/GPU resources; record the run method and game wall time in reports.
- 2026-11-02: require the completion marker, publish the engine heartbeat, stream progress and stop
  stalled games before the outer Ryzen job limit; preserve reports from interrupted runs.

## Frame profile

### Как работает сейчас

Build with `cmake -S OpenGothic -B OpenGothic/build -G Ninja
-DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENGOTHIC_ENABLE_FRAME_PROFILE=ON`, then
`cmake --build OpenGothic/build --target Gothic2Notr`. Set
`OPENGOTHIC_FRAME_PROFILE=/absolute/path/frames.csv` when starting the game.
For a short existing scenario use `BENCH_HOST=ryzen
OPENGOTHIC_FRAME_PROFILE=<absolute output>/frames.csv scripts/bench.sh cloth-on <absolute output>`;
the Ryzen engine must also have been configured with the option ON (the runner normally builds it OFF).
The recorder buffers up to 20,000 render attempts and writes once at shutdown. Clear the environment
variable to disable recording; build with the option OFF (default) to remove all clocks and Metal handlers.

Every attempt has a row, including fence-busy returns and captures. Existing columns retain their names.
The complete column set is:

| Columns | Units / meaning |
|---|---|
| `attempt`, `cmd_id` | Zero-based attempt number; reusable command slot, not a unique frame id |
| `dt` | Simulation step, milliseconds |
| `segment`, `frame` | Bench recording name and frame number within that recording; empty/zero outside recordings |
| `begin_ns`, `observer_ns`, `end_ns` | Absolute `steady_clock` timestamps, nanoseconds; observer is zero when not sampled |
| `outcome` | 0 unfinished, 1 submitted, 2 fence busy, 3 capture, 4 error |
| `flags` | Bitmask: 1 submit completed (CPU), 2 present completed (CPU), 4 capture requested, 8 capture read completed, 16 fence busy, 32 error |
| `overflow` | Number of attempts beyond the buffer limit, repeated in every row |
| `<zone>_ns`, `<zone>_calls`, `<zone>_first_ns`, `<zone>_last_ns` | Elapsed sum in nanoseconds, call count, first start and last end as absolute `steady_clock` nanoseconds; all zero if unused |
| `palette_calls`, `identical_calls`, `patch_calls`, `patch_blocks` | Counts |
| `palette_bytes`, `identical_bytes`, `patch_bytes`, `grow_bytes`, `upload_execution_bytes` | Bytes |
| `draw_calls`, `dispatch_calls` | Encoded draw and compute dispatch counts; dispatch counting currently covers Metal |
| `rt_tlas_builds`, `rt_dirty_slots` | With `-rt 1`: TLAS builds recorded into the frame (0 or 1) and slots of the ray scene written into it |
| `gi_probes_live`, `gi_probes_traced`, `gi_probes_from_hits`, `gi_probes_freed` | With `-gi 1`: probes alive after the allocation, probes traced anew (allocated by pixels and at hits), of them allocated at the hits of other probes' rays (requests of the last frame), and probes freed (unused for longer than the expiry, or the oldest when places run out); of a frame one or two frames back (`probe_stats` writes them into a host-visible buffer of the frame slot, the CPU reads it when the slot's fence is done; no wait); `per_frame` in the report is the mean |
| `gpu_ns`, `gpu_done`, `gpu_buffers` | Sum of the attempt's command buffer GPU time in nanoseconds (Metal `GPUEndTime-GPUStartTime`, Vulkan a timestamp pair per buffer); 1 only when all submitted buffers have valid timing; submitted buffer count |

The complete `<zone>` list (each produces all four columns above):
`tick`, `finish_step`, `camera`, `observations`, `bench`, `fence_poll`, `recycle`, `ui`,
`encode`, `submit`, `present`, `limiter`, `world_logic`, `prepare`, `physics`, `sync`, `world_misc`,
`parallel_animation`, `native_export`, `forced_sync_animation`, `actor_animation_elapsed_sum`,
`pose_elapsed_sum`, `palette_elapsed_sum`, `storage_join`, `storage_commit`, `storage_pack`,
`storage_dispatch`, `storage_grow`, `storage_upload_execution_elapsed`, `bodynet`, `cloth`, `water`,
`destruction`, `ai_bridge`, `sound`, `npc_full_tick`, `npc_simplified_tick`, `npc_logic_shadow`, `encode_shadows`, `encode_main`,
`encode_water`, `encode_post`, `encode_ui`, `animation_npc_pass`, `melee_shadow`, `melee_contacts`,
`interactive_animation`, `logical_body_sync`, `pfx_prepare`, `upload_wait`, `rt_scene`.

`animation_npc_pass`, `melee_shadow`, `melee_contacts`, `interactive_animation` and `logical_body_sync`
split `parallel_animation`: the worker pass over NPCs (with the posing of their melee shadow bodies), then
the serial loops after it (publishing those bodies and the blades, the blade flush with contact resolution,
interactive objects, the logical-body sync). `melee_shadow` poses a body itself only for an NPC the worker
pass skipped or whose bones moved since (a script, a sync on read).
`pfx_prepare` is `PfxObjects::preFrameUpdate` inside the frame encode. `upload_wait` is the CPU wait of a
synchronous buffer upload on Metal (`MtBuffer::update` of a device-memory buffer): inside a frame it lasts
until the GPU finishes what was committed before, so a nonzero value there is a stall.
`rt_scene` is `WorldView::updateRtScene` (only with `-rt 1`): the objects marked since the last frame (moved, shown,
hidden, created, freed) into their slots of the ray scene, and in a frame with changes the patch of the changed slots
and the TLAS build recorded into it (pass `RT-TLAS`); a frame without changes records nothing. The two merged landscape
BLAS are built once, and the descriptor arrays of the buckets anew only when a bucket is new. Only these heavy events
log a line in a frame-profile build,
`render: rt scene reason=<landscape|bucket> instances=<N> blas=<N> triangles=<N> tlas_bytes=<N> blas_bytes=<N> build_ms=<ms> slots=<N> buckets=<N>`:
instances that rays meet (the merged landscape and the visible slots), unique BLAS among them, triangles summed over
them (index ranges / 3), the TLAS storage, the sum over the unique BLAS storage, the CPU time of that frame's
`RtScene::commit`, the slots given out and the buckets. The bytes are the Vulkan acceleration-structure sizes
(`accelerationStructureSize`); Metal keeps none and writes `-1`.

`npc_full_tick` and `npc_simplified_tick` split the `Npc::tick` loop inside `world_logic` by NPC simulation
tier (`common/world/npcsimtier.h`); their calls count runs of consecutive same-tier NPCs, not NPCs. The
amounts `npc_full`, `npc_simplified` and `npc_tier_transitions` count NPCs per tier and tier changes.

`OPENGOTHIC_NPC_LOGIC_AUDIT=1` enables the logical-body shadow audit (default 0). `npc_logic_shadow`
measures clock advancement and mutation-input processing. Audit compares the previous forecast before
applying the tick's inputs. `npc_logic_samples`, `npc_logic_body_samples`, `npc_logic_weapon_samples`,
`npc_logic_body_mismatch` and `npc_logic_weapon_mismatch` count scalar comparisons after excluding fields
changed by explicit inputs. Unknown natural-next observations are compared before their re-anchor and are
not reported as lifecycle matches.
`npc_logic_early`/`npc_logic_late` observe real layer removals; `npc_logic_wait_early`/`npc_logic_wait_late`
observe native wait releases independently. `npc_logic_reanchored` counts executor inputs by kind in JSON.
`npc_logic_events` and `npc_logic_event_mismatch` count events, with a mismatch only beyond one world step.
Full event schedules are audit-only; the default state retains the minimal weapon-event clock. During `npcday`,
timing also runs with the audit disabled so the default path can be measured separately. The `npcday` summary
adds `logic_audit`, including action/animation breakdowns and prediction-error percentiles; the parity comparer reads its named gameplay fields and ignores these diagnostics.

`bodynet` sums the full local P30 policy step over active NPCs; service policies do not enter it.
`cloth` covers main-thread preparation and the 30 Hz solver/finish; `water` sums main-thread buoyancy
preparation/finish and the synchronous Jolt step listener, which may run on a worker.
`destruction` covers deferred fracture, movable/fragment synchronization and cleanup; fragment dynamics
remain inside `physics`. `ai_bridge` measures the inbound queue and message parsing/dispatch on main;
`sound` measures the main-thread listener/effects update, including its mutex wait, not the audio mixer.
The five `encode_*` zones measure sequential CPU command recording, not GPU execution.
Zones can nest; worker elapsed sums (including storage uploads) must not be added to enclosing wall time.
Actor/pose/palette zones only cover the actors selected by the bench.

Metal captures the exact attempt record at command submission and updates atomic counters from that
buffer's completed handler, including presentation, uploads/readback and acceleration-structure builds
submitted during the attempt. Slot reuse cannot redirect a late completion to a later attempt.
There are no added waits. Missing/failed/incomplete timing gives `gpu_done=0,gpu_ns=0`;
`gpu_buffers` still counts captured submissions.
Vulkan (frame-profile build) writes a timestamp query at the start and at the end of each command buffer
(`BOTTOM_OF_PIPE`, so the start waits for the queue's earlier work) and reads them, without waiting, when the
buffer's fence is seen signalled, like the pipeline statistics of `triangles`. Ticks become nanoseconds by
`timestampPeriod`; a graphics queue with `timestampValidBits=0` gives `gpu_done=0`. Buffers submitted
outside `VDevice::submit` (uploads, acceleration-structure builds of the data manager) are not counted.
Pending completions at CSV flush are left incomplete. This is diagnostic timing, not a performance stand.

On Metal, `OPENGOTHIC_GPU_PASSES=1` additionally samples native encoder stage boundaries and writes
`<frames.csv>.gpu.csv`: attempt, slot, segment, frame, encoder index/type, joined pass labels,
GPU/vertex/fragment nanoseconds, draw/dispatch counts and validity. Passes sharing an encoder stay
joined; intervals may overlap and must not be summed into total GPU time. Unsupported or invalid
samples have `valid=0`. Detailed counters have measurable overhead: leave them off for the GPU
acceptance runs, then enable them separately when a pass breakdown is needed.

On Vulkan, `OPENGOTHIC_GPU_PASSES=1` writes the same `.gpu.csv` columns with one row per
`setDebugMarker` region (from the marker to the next marker or the buffer's end): `labels` is the marker,
`type` is `render` when the region issued draws and `compute` otherwise, `vertex_ns`/`fragment_ns` are 0.
Each region is a pair of `BOTTOM_OF_PIPE` timestamps, so a row runs from the end of the earlier work to the
end of its own: work running without a barrier overlaps, rows must not be added into `gpu_ns`.
Up to 255 regions per command buffer are timed, later ones get `valid=0`. Without the variable no region
queries are written. `scripts/perf_report.py run` sums a label's valid rows per frame into `gpu_passes`
(`{"frames", "mean_ms", "p95_ms"}` per label) and a table in `report.md`; no `.gpu.csv`, no key.

### План и журнал

- 2026-10-28: Metal completion timing and buffer counts; P30, cloth, water, fracture, inbound bridge,
  sound and major render encoding zones. Validation: Mac/Linux builds and the existing Ryzen cloth bench.

`own-mesh-barrel` frames the complete authored `NW_HARBOUR_BARREL_01` cargo barrel
in Khorinis harbour at noon, writes `own-mesh-barrel.png`, then measures thirty
seconds of normal app cadence without video. Use a fresh `WORLD=NEWWORLD.ZEN`, `MOVABLE_VOBS=0`,
`OPENGOTHIC_CENSUS=<absolute output>/census.json`, and `OPENGOTHIC_CONTENT=<test content>`
for the GLB replacement; `OPENGOTHIC_CONTENT=` gives the original-mesh comparison.
For FPS comparisons use `RYZEN_BENCH_CLASS=quiet`; [the barrel report](../../reports/content/2026-10-own-meshes/README.md)
records the replacement, original-mesh screenshots and alternating branch/main runs.
`own-mesh-crate` is the same take two metres away, framed on the `NW_HARBOUR_CRATE_01`
row next to that barrel (`own-mesh-crate.png`); the barrels stay in its frame.
`own-mesh-table` frames the `NW_CITY_TABLE_PEASANT_01` in a harbour house (candle-lit room,
candle and books stay on it) and writes `own-mesh-table.png`.
`own-mesh-market` frames one copy of each of the 21 market-set visuals (`NW_CITY_MERCHANT`, see
[the market report](../../reports/content/2026-10-market-set/README.md)), writes `<visual>.png` per frame,
then the census, an overview `market-overview.png` and thirty seconds of app cadence. Run it twice on the
same host with `WORLD=NEWWORLD.ZEN`, `OPENGOTHIC_CENSUS=<absolute output>/census.json` and either
`OPENGOTHIC_CONTENT=` (originals) or the set root `~/.local/share/gothic-ai/content-staging/market-set/content`.
A `shot` is written at the end of its frame, so the scenario waits after each one before moving the camera.
`own-mesh-relief` takes daylight and indoor close-ups of the props' textures (barrel, crate, table, bench,
chest, cart, signpost, stall roof, inner window, cupboard; `<prop>_view.png` and `<prop>_near.png`), then the census.
It checks the tile relief of the atlases ([report](../../reports/content/2026-10-prop-relief/README.md)): run it with
the props' root and with a root of the earlier bake, `OPENGOTHIC_CENSUS=<absolute output>/census.json`, `TIMEOUT=900`.
`own-mesh-culling` frames four of those spots (the room with the bookshelf, the bench, the cart with the lamp post and
rope coils, the cupboard) 1 s, 2.5 s and 4 s after the teleport; two runs give the same PNGs to the pixel. Until
2026-10-21 props went missing from such frames on the Mac at random, their shadows kept: the Hi-Z mips 5-6 were built
from stale texels ([report](../../reports/engine/2026-10-21-hiz-culling/README.md)). Mac frames shot before that
may lack props in either take of a comparison.
`own-mesh-backfaces` takes close-ups of the seven market props whose parts were inside out (barrel group,
carpet, shield, oil lamp, wolf head, candle, rope; `backfaces_<prop>[_<side>].png`): the barrel group and the rope from
four sides, the carpet at its four edges, the wall props from their `own-mesh-market` spots at about half the distance,
then the census. Run it with the set root and with a root of the earlier bake.
`own-mesh-mob` frames the harbour `BENCH_NW_CITY_01.ASC` beside the wooden wall
(prop-survey 20649, ZEN vob.id 23479), then uses the native `AI_UseMob` seat/stand
fixture at `NW_CITY_HABOUR_BENCH_01`. It writes empty, seated and standing screenshots,
NPC motion traces and a final census. Run with `WORLD=NEWWORLD.ZEN`, `BENCH_HOST=ryzen`,
`GOTHIC_AI=0`, `OPENGOTHIC_TEXTURES=`, an explicit `OPENGOTHIC_CONTENT=<test content>`
and `OPENGOTHIC_CENSUS=<absolute output>/census.json`; pass an absolute output directory
to `scripts/bench.sh own-mesh-mob`. Repeat with `OPENGOTHIC_CONTENT=` for the original.
[The paired Ryzen check](../../reports/content/2026-10-own-meshes-mob/README.md)
records matching seated positions/yaw, native standing and mesh census entries.

`own-mesh-fracture` breaks `NW_HARBOUR_BARREL_01` with the real one-handed
attack animation from `destruction-step2-melee`. It takes before/after screenshots,
records the attack, writes a checkpoint with the fragments, and flushes the resource
census. Use `WORLD=NEWWORLD.ZEN`, `GOTHIC_AI=0`, `OPENGOTHIC_TEXTURES=`,
`OPENGOTHIC_CONTENT=<generated test content with its .fracture>` and an absolute
`OPENGOTHIC_CENSUS=<absolute output>/census.json`. Repeat with `OPENGOTHIC_CONTENT=`
and the unchanged original `OPENGOTHIC_FRACTURE` cache for comparison.
NEWWORLD has other visuals sharing `NW_CITY_BARREL_01.TGA`; for a zero-original-texture
census, map this name to the procedural barrel DDS in the temporary test material index
as shown in the report. The replacement mesh and pieces still use `OWN_TEST_BARREL_WOOD.TGA`.
Run through `BENCH_HOST=ryzen scripts/bench.sh own-mesh-fracture <absolute output>`;
[the report](../../reports/content/2026-10-own-meshes-fracture/README.md) records
source identity, piece volume, native hits and original resource opens.

`own-content-fracture` breaks the barrel and then the crate of a content set the same way,
with before/after screenshots and a checkpoint of the pieces each. Run it with
`WORLD=NEWWORLD.ZEN MOVABLE_VOBS=1 DESTRUCTION=1 GOTHIC_AI=0 OPENGOTHIC_TEXTURES=`,
`OPENGOTHIC_CONTENT=<a copy of the working content>` and, for the originals,
`OPENGOTHIC_CONTENT= OPENGOTHIC_FRACTURE=~/.local/share/gothic-ai/fracture`. Each
`destruction2_melee_complete` event gives hits and pieces, `destruction_checkpoint` the
`max_displacement_cm` of every piece: how far the prop fell apart.
`own-wood-fracture-clip` is the acceptance take of the same pair for the
[wood fracture](../../reports/content/2026-11-wood-fracture/README.md#клип-было--стало-и-ограждения):
per prop a close `<prop>-close-before|after.png` pair, a take `<prop>-side.mp4` from the blow
until the pieces sleep (the hero strikes towards +z; both cameras stand at an angle on the struck
+x/-z side, the close one 2.9 m off, the take 3.8 m) and
`destruction2 view <camera xyz> <target xyz>` places the held camera in centimetres from the centre
of the prop. The lunge leaves the hero inside the prop, so after the take `destruction2 stand` puts
him back where he stood before the blow (`destruction2_stand` gives the distance) and the close
"pieces" frame shows the breach. The scenario declares `# @timeout 600`. Each break writes `destruction2_pieces` 0.5, 1, 2, 5, 13 and 15 s after it: per piece
the centre's travel, the angle of a flat piece's face and long axis from vertical, sleep, speed and
`fell` (centre below the floor), its `bond_group`, and per break the bond groups (`groups`,
`largest_group_volume_fraction`, `single_pieces`) and their bodies now present, asleep and through the
ground (`bodies`, `bodies_sleeping`, `bodies_fell`: pieces of one group share one body; without
bonds a group is a piece); at 15 s the native hull penetration into a floor plane, into the
real ground (`native_floor_penetration_cm`, Jolt `CollideShape` against landscape and static objects)
and into the other pieces (pairs the break's hoop filter never collides — a group of METAL only against a
group with WOOD — and pieces of one bond group are skipped),
the visible (vertex) penetration below that plane and below the ground under each vertex
(`visible_terrain_penetration_cm`), hoops on the ground (`metal_on_ground`) and the 13→15 s jitter. Every break also
writes `destruction2_start` (each piece's centre over the ground and native hull depth at birth) and,
frame by frame for 5 s, `destruction2_fell` for the first frame a piece's centre is under the ground or
over no ground, with its previous frame; a fallen piece's row adds the static surface above and under
it and water at it. `destruction_sleep15` and `destruction_checkpoint` give each piece its
`center_cm`, `in_water` (water at most 50 cm under the centre of mass) and `under_ground` (a static
surface within 20 m above). `destruction_fragment_retired` logs each sleeping piece that
`cleanupFragments` removes (`expired` or `distant`, over 30 m from the hero).
A bond group retires whole, one event per piece. `destruction_compare.py` does not count such a piece as
lost; a piece missing without one fails, and so does a bond group that is only partly present. Its
15 s sleep gate counts bodies (`bond_group`; without it, pieces) and requires every body on land asleep; a piece in water passes when it is not under
the ground, and the result line reports whether it sleeps at the final checkpoint. Run with the
`own-content-fracture` settings plus `TEMPEST_WINDOW_SIZE=1280x720`; `OPENGOTHIC_FRACTURE_MOTION`
varies the version 3 piece motion, `OPENGOTHIC_FRACTURE_BONDS` the bond rule. Inside a `movable measure` window the pieces are not sampled: the
timed frames stay free of bench work.

`own-wood-remnant-clip` takes the [repeat blows on remnants](../../reports/physics/2026-11-destruction-bonds/README.md#клип-повторных-ударов)
with the same place, seed, noon and cameras: per prop one take `<prop>-remnant.mp4` from the accepted
one-handed blow through three more breaks to a walk over the pieces, and a close frame
`<prop>-close-break1..4.png` after each break (`destruction2 stand` first). `destruction2 remnant <n>`
strikes the largest bond group of the broken fixture prop with the native forward attack, at most `n`
swings, until the prop has more bodies. Before each swing it places the hero by where that attack's
blade comes down: 126 cm forward and 28 cm right of the feet (`melee-blade-shadow.jsonl`, top contacts).
From the first blow's side by 30 degree steps, rays straight down along the line to the group's centre of
mass find the first point of the group's own body no lower than 30 cm under its top (the floor of an open
crate is out of the blade's reach); a side with a loose piece found first, or higher within 20 cm of that
point (rays every 4 cm), is skipped. Events: `destruction2_remnant_attempt` per swing (side, landing),
`destruction2_remnant_complete` with `broken`, `swings`, bodies and the largest group before and after,
and every group's pieces. `destruction2 walk <cm>` then walks the hero, holding forward (4 s at most a
pass), along a straight line through the centre of the loose pieces, clear of the largest group by its
radius plus 40 cm, there and back. The line must pass within 10 cm of the hull of a loose piece from 0.5 m
(one the hero pushes; smaller ones it walks through); failing that, lines through each such piece; with
none the scenario aborts. `destruction2_walk` gives `pieces_on_path`, per pass the progress, the feet over
and under the ground and the pieces moved, and over the walk `moved_pieces` and `counted` (true only if a
piece moved). Every break, the first and each remnant's, writes `destruction_break` (with
`remnant`) and `destruction2_start` / `destruction2_pieces` under its own case: `<case>-remnant-<n>` for
the n-th remnant break, sampled from its own moment; `hero_debris_penetration_cm`,
`hero_piece_penetration_cm` and `hero_light_piece_penetration_cm` give the depth of the hero's capsule (its
controller's radius, `hero_radius_cm`) in remnants, in single pieces from 0.5 m and 10 kg, and in lighter ones,
which are no floor: the feet pass into their tops by design.
`reports/physics/2026-11-destruction-bonds/clip_groups.py` names the groups. The scenario declares `# @timeout 900`.

Corpse water: `water-bodies` records a real harbour WindFist throw, a dead wolf and a
sinking skeleton, then saves `session-water-bodies.sav` and tries normal corpse loot
from dry ground and while swimming. `water-bodies-shallow` observes a corpse on the
submerged shore. Start `water-bodies-reload` in a new process with `SAVE` pointing to
that complete archive; its initial snapshot precedes the first native step.
`water-bodies-perf-on` and `water-bodies-perf-off` measure the same ten human corpses
over a 30-second normal-cadence window without recording video.
`python3 scripts/water_bodies_compare.py deep DIR`, `shallow DIR`,
`reload SAVED_DIR LOADED_DIR` and `perf ON_DIR OFF_DIR` check the native parts,
immersion, sleep, 30-second stability and the 2 ms physics p95 budget.

`p18b-base` checks a selected base through the native world transition path:
`NEWWORLD` → `ADDONWORLD` → `OLDWORLD` → `DRAGONISLAND` → `NEWWORLD`, then native
`playvideo LOGO2`. Run with `WORLD=NEWWORLD.ZEN`, `MUSIC=1`, the task base/cache/store,
and an absolute census/output path. `world <ZEN> [waypoint]` requests the existing
`World::triggerChangeWorld` transition and waits for native loading;
`world-check <ZEN>` fails unless that world is current. Both commands use only indexed worlds.
Successful benchmark music selection logs `Dx8 music: <SGT>` after the mixer accepts it.
A normal startup logs `Main menu ready: <id>` after menu initialization; no keyboard driver is needed.

A run takes the game start (~10 s from a save) plus the scenario time. On Mac the game window is minimized,
does not take the focus and plays no sound, so it can run while someone works on the same machine.
The time step is fixed (30 fps), so a run does not depend on the frame rate and repeats (up to the
random idle animations the game picks).

Recast navigation experiments run through `gothic-body-runner` on Linux. The normal game
does not build or load a navmesh. Set `[GAME] movable_vobs=0` before loading the world so
the authored prop collision meshes are static in both takes; the repeat script sets it
in its own working directory. Bench commands (coordinates/distances are in engine cm,
Recast internals are metres):

- `navmesh build <absolute cache directory>` exports the current static Jolt triangles,
  builds or loads a tiled mesh keyed by world geometry and agent parameters, and writes
  `navmesh-stats.json` and `navmesh-polygons.jsonl`. It also times a second cache read.
- `navmesh coverage` writes all undirected waypoint edges and complete-path results to
  `coverage.jsonl`, plus `points.jsonl`. Partial Detour paths count as unreachable.
- `navmesh pairs <seed> <count per region>` writes deterministic 10–40 m waypoint → `FP_*`
  pairs for market, harbour, Onar, Lobart, port alleys and upper-town streets.
- `navmesh start <direct|mesh> <NPC> <waypoint> <FP> <limit ms>` places the actor and begins
  a measured walk. `direct` uses the same `AiQueue::aiGoToPoint` as `npcgo`; `mesh` sends
  direct point targets along Detour's string-pulled corners. Both use native walking.
- `navmesh follow <direct|mesh> <NPC> <FP> <limit ms>` starts the same measurement from
  the current position, preserving the original intentional-stuck fixture setup.
- `navmesh finish` writes `trial.json`. Scenario execution waits while the measured walk
  is active, ending at <100 cm from the target or at the time limit. No teleport occurs
  during the walk. The immutable mesh can be reused after a fresh runner reset.

Use `reports/navigation/2026-10-16-navmesh/run.sh` on Ryzen under the shared simulation
lock. It runs `scripts/navmesh_measure.py`, which resets the saved world for every take,
uses `OPENGOTHIC_STUCK_LOG`, and can resume a measured series. `summarize` writes the
coverage/walk tables and two top-down PNGs using only the Python standard library.

A line `off: <command>` or `on: <command>` runs in that take only: `OPENGOTHIC_BENCH_TAKE=off|on`, set by
`compare/film.sh` in gothic-shorts (without it the take is `on`). `OPENGOTHIC_NPCAIM=0|1`
sets the NPC archers' manual aim over Gothic.ini.

Scenarios for foot IK: `ik-sweep` (12 slopes all over the world: run up, run down, walk across; ~2 min),
`ik-stairs` (8 stairs: walk up and down, run up), `ik-moves` (stand, turn on the spot, strafe, walk
backwards, sneak), `ik-ab` (IK off and on on the same run), `smoke` (the bench itself).
Four legs: `quad-wolf` (the player as a wolf on 6 slopes), `quad-zoo` (molerat, shadowbeast, waran,
lurker, giant bug, sheep on 2 slopes each). The player becomes the creature (`transform`), so the
usual player commands drive it. Gaze: `gaze` (Xardas and the hero talk; the hero stands in front,
goes around him, behind his back and back; then Xardas at his bookstand turns the head only),
`gaze-ambient` (no conversation: Rupert at the market notices the hero walking up, standing, walking,
running and sneaking past; a script look; the hero looks at the npc in focus; a drawn weapon).
Lean: `lean` (running in circles left and right, a slalom, walking in a circle, running and walking up
and down a 27 degree slope). With `lean off` on top of a copy it gives the same run without the lean,
for `bench_report.py diff`.
Transitions between animations: `anim` (the hero on a flat floor: start and stop, walk to run, turning
on the spot, strafing, walking backwards, jumps, sneaking, fists: attacks, a combo, a block). With
`blend off` on top of a copy it gives the old cross-fade, for `diff`; `gaze-ambient` and `quad-zoo` show
an npc's and the creatures' transitions too.
Aim: `aim` (the hero draws a bow at a sheep hung level, above, steeply above, to the side; the same with
aim off; a crossbow; a slope; down from three ledges near Xardas' tower, 17, 36 and 46 degrees, and
from a ship's deck at 70 and 80 degrees; a fire bolt; three shots in a row; last a sheep right at the
foot of a gallery's wall in Xardas' tower, 78 degrees down: hidden from the head, so without the aim no
focus; with it the hero leans over the edge, the focus takes it, the arrow kills it).
Manual bow pole: `bow-pole` (30 cm from Lobart's roof edge, a wolf ~85 degrees below; two shots
and reloads from the bow side, one from the drawing side, then a slowed bow-side take).
Flinch: `flinch` (a frozen bandit: blows only to the flinch from the front, behind, both sides, to the
head, off the middle, a weak one, three in a row, the same with the flinch off; an arrow in his back;
the hero's fist from the front, behind and the side as the game deals it, with its stumble; his sword
swung from the left, the right and straight (a swing pushes along the blade); the hero
struck standing and while aiming a bow, where the game has no stumble; a wolf: the head only).
Lip sync: `lips` (Xardas says a voiced line of the game, the camera at his face; then the same line
with the old talk loop), `lips-dialog` (the hero talks to Xardas through the dialog window and picks
the first choice: the lines of both are played the way the game plays them).
The hero's head: `hero-head` (his face close up - still, the moods of the AI lines, a voiced dialog line, the eyes:
he listens to Xardas, thinks, Xardas stands around him, he looks down and up - and running from behind in the market; the clips of the original and of the new head, side by side,
[step 0](../../reports/content/2026-11-hero-head/test-head.md)).
Faces: `faces` (the face animations of a head one after another, then two of them while Xardas speaks),
`face-fade` (faces fade in and out, one gives way to the next, three a moment apart, the lips over a
fading face), `mood-ai` (given lines of an AI server with moods: start `gothic-ai serve --scenario m1 --llm off
--no-tts` on a port of your own and run the bench with `GOTHIC_AI_URL=ws://127.0.0.1:<port>`; with a TTS
service instead of `--no-tts` the lines are voiced in their mood). Gestures and a lie of an own head in the game, close up with
sound: `face-cues-clip` ([below](#face-cues-clip)). Columns `face_neutral`, `face_friendly`,
`face_angry`, `face_hostile`, `face_afraid`, `face_pain` of `metrics.csv`: how much of each face of the
watched npc shows, 0..1.
Wolves: `wolf` (docs/beasts.md, no server: the bridge's `ai beast listen|sit|calm|flee|attack|resume` on the
wolf nearest the hero): a wolf growls at him, listens, is soothed and sits; he comes up close: it gets up and
growls, sits again once he steps back; he goes off and comes back: it leaves him be; it bites him, talked
down it backs off and watches him. `cam two`, `cam shoulder`, `cam face` take the nearest living npc of a
symbol (several wolves share one).
The necromancer's answer: `necro` (docs/xardas-wrath.md: Xardas kills the hero with a spell and raises him as
a skeleton, the screen goes black and the end card comes up; `ai necro`, no server needed).
Blinks: `blink` (Xardas' face close up: quiet, saying a line, his eyes jumping after the hero put here
and there, a frightened face, the eyes closed; with `blink off` on top of a copy nobody blinks, as
OpenGothic was). Columns `bk_on`, `bk_w` (how shut a blink keeps the eyes, 0..1), `bk_n`, `bk_pause`,
`bk_shift`, `bk_double` (counters: blinks, of them in a pause of the voice, at a shift of the eyes, the
second of a double one).
Gestures: `gestures` (the hero picks the first dialog choice of Xardas three times, the camera at his
chest: his hands while he speaks; with `gestures off` on top of a copy the old way, a random gesture
when a line starts and none plays). For the moods, the same camera with an AI server (`mood-ai`
above): one line `friendly`, two `angry`. Columns `ge_on` (gestures go with its lines), `ge_talk` (its
line is being said), `ge_id` (the gesture playing, 1 is `T_DIALOGGESTURE_01`), `ge_mood` (0 neutral,
1 friendly, 2 angry, 3 hostile, 4 afraid, 5 pain), `ge_starts`, `ge_cuts`, `ge_stops`, `ge_nods` (counters),
`ge_pitch`, `ge_yaw`, `ge_roll` (degrees: how the voice turns the head, a nod is pitch down), `ge_listen`,
`ge_think` (it listens to the hero, it thinks of an answer), `ge_lnods` (nods as the hero's words come),
`ge_aside` (how far the eyes are aside while it thinks, 0..1).
Listening: `listen` (Xardas' face close up: quiet, the hero speaks to him 6 s, he thinks 2.5 s, he answers;
with `gestures off` on top of a copy he stands still, as before).
Deals: `deal` (the deal window of an AI server, the interface in the frames: start `gothic-ai serve
--scenario deal_demo --llm off --no-tts` on a port of your own and run the bench with `GOTHIC_AI_URL`;
Xardas offers a thing he has for 50 gold the hero lacks, then for 30, and the hero accepts).
Two people talking: `gossip` (Lobart and Hilda talk about the hero's lie at Lobart's farm, each line said to
the other: start `gothic-ai serve --scenario gossip_demo --llm off --no-tts` on a port of your own and run
the bench with `GOTHIC_AI_URL`; the server puts Hilda 5 m from Lobart and the hero 7 m from them; `nearby`,
`pair` and `where` every second: during each line both look at each other, `look=talk at=<the other>`).

Output goes to `.bench/<scenario>-<time>/` (not in git):

| File | What |
|---|---|
| `report.md` | one line per recorded segment, then the worst events with their contact sheets; a "Manual aim" table when the bow was aimed by hand: shots, hits by zone, the sway at full draw, the draw time; the tables of the arrows, the dice and the damage when the log has them (below) |
| `report.json` | the numbers of `report.md`: per segment, the events, and the tables of the arrows, the dice and the damage (`archery`, `archery_predicted`, `dice`, `damage`, `time_to_kill`, `fights`) |
| `sheets/<segment>.png` | 16 frames of a segment in a 4x4 grid |
| `sheets/evNN-*.png` | frames around an event |
| `<segment>.mp4` | the video, 800x600 by default (`TEMPEST_WINDOW_SIZE=<w>x<h>` overrides it), 30 fps |
| `metrics.csv` | a row per frame: player state, IK state, joints before and after IK, ground |
| `bench.log`, `game.log` | what the scenario did, the engine log |
| `host.txt` | `ryzen` or `mac`, also shown in the report header |

More frames or a comparison:

```bash
scripts/bench_report.py frames .bench/<run> <segment> 20 40      # frames 20..40 in one sheet
scripts/bench_report.py compare <run>/off.mp4 <run>/on.mp4        # side by side, a on the left
scripts/bench_report.py .bench/<run>                              # report again (e.g. after changing a threshold)
scripts/bench_report.py aggregate .bench/<run a> .bench/<run b>  # the arrows, dice, damage of several runs, by segment
```

`compare` also works across runs: the same scenario before and after a change.

The arrows, the dice and the damage come from the game's notes in `bench.log` (`arrow shot`, `arrow aim`,
`arrow hit`, `arrow missed`, `dice shot`, `arrow dice`, `damage`, `fight over`; `by` names the shooter, `PC_HERO`
when a line of an older run does not say), their times from the `note` rows of `events.jsonl` (none in older
runs: no rates then). A table is there only when its lines are:

| Section | What |
|---|---|
| Archery accuracy | per segment and shooter: shots, shots a minute; each hand's arrow flown from `arrow shot` (`p = from + v·t - ½·g·t²`) to where it passes closest to the point meant (the hero: the last `aim zone` before it; an npc: `true` of its `arrow aim`), the miss as the shooter sees it in degrees, + right / + up: bias, σ (RMS about the bias, 95% CI by chi-square; MAD / 0.6745); an npc's row `- err` with its own `err` taken off (the body and the bow alone); hits by zone with Wilson 95% intervals; the hero's `stats` (skill, sway, cone) or the npc's aim (`sigma`, settled / waited / flinch) |
| Predicted hits from σ | when `zones` dumped the target in the same log: its capsules seen from the shooter in 1 cm cells (the first capsule a ray enters takes the cell, as `HitZones::rayTest`) under a Gaussian of the bias and σ above: P(head), P(body), P(limb), P(any) against the counted. The dump is moved to where the shots aimed when the target stood elsewhere (`dump moved cm`; the hero aims at the middle of its `aimat` zone, an npc's point meant counts for the trunk's); its turn is not |
| Dice | per segment and shooter: `dice shot`s, rolls (`arrow dice`: arrows that reached someone), the mean logged chance, expected hits Σp ± sqrt(Σp(1-p)), hits, z; each roll again under OpenGothic's chance (by the path) and vanilla G2's (by the straight distance, truncated; hit iff roll < chance): mean chance, hits, outcomes that flip against the log; how much longer the path is than the distance |
| Damage and fights | per segment, shooter and victim: shots (melee and spells: the blows that landed), hits by zone, net damage per hit, damage a minute (per shot times the cadence), the cadence (gaps between the shooter's shots), `fight over` of the victim |
| Time to kill | each sequence of shots (time, raw, k, hit) replayed on hero ch1 160 HP / 20 protection, ch3 400/50, ch4 520/100, Bandit_M 160/35, a paladin 520/100: from every shot as the first one, round the sequence (after its last shot the mean gap), the time and the shots until the HP is gone: mean and 5-95%. Per hit `max(round(raw·draw) - prot, 0)·zone` (head 2, body 1, limb 0.6) as the game does since 2026-10-13, or as it did before, the zone before the armour `max(round(raw·k) - prot, 0)`; at least 5 a hit (G2) |

`aggregate` pools the shots of several runs (repeats of a scenario) by segment name and prints the same tables;
gaps and sequences stay within a run.

Before and after a change, with the same scenarios (the version before is built from `main` in a
temporary worktree, the scenario files are taken from the current one):

```bash
git worktree add --detach /tmp/before main && (cd /tmp/before && scripts/build-engine.sh)
W=$PWD; (cd /tmp/before && scripts/bench.sh $W/scripts/bench/ik-sweep.bench $W/.bench/before-sweep)
scripts/bench.sh ik-sweep
scripts/bench_report.py diff .bench/before-sweep .bench/ik-sweep-<time>   # events per segment, before -> after
git worktree remove --force /tmp/before
```

## Overlay

- thin white: the legs as the animation has them, before IK;
- thick blue (left) and orange (right): the legs after IK;
- crosses on the ground: where the ground rays hit under the ankle (cross) and the toes (dot), green
  under a planted foot, yellow under a swinging one; the short line is the ground normal;
- bottom right: the feet zoomed in;
- top left: frame number and time in the segment (they match `frame` in `metrics.csv`);
- lips (under the gaze text): `F` the two mouth shapes (VISEME frames) mixed with `A`, intensity `I`;
  the thin white bar is the voice, the thick blue one how wide the mouth is shown;
- gaze: a yellow line where the face of the watched one looks, a red dot at the partner's eyes; text:
  weight, error `E`, turn `Y` and `P` (degrees), parts of the turn: head `H`, chest `C`, waist `W`;
- orange `AIM`: weight, error `E` (where the arrow points after the turn against where the shot leaves),
  where the shot has to go `Y`, `P`, the turn `T` and the bend `B` (+ up) of the upper body, degrees;
- red `HIT`: blows so far `#`, strength `S` and height `H` (0 the hips, 1 the head) of the last one, the
  trunk's bend `B` away from it and where its top goes `Y` (+ to the right of the facing), the twist `T`
  about up and the head `N` against the trunk, degrees;
- green `LEAN`: roll `R` (+ to the left) and trunk pitch `P` (+ forward) in degrees, speed `V` (cm/s),
  turn `T` (degrees per second, + to the left), slope `S` along the facing (degrees);
- violet `BLEND`: bones blending from a switch of animations `N`, the largest rotation offset `A` (degrees);
- `BLINK`: blinks `N`, of them in a pause of the voice `P`, at a shift of the eyes `S`, second ones `D`;
- `GEST`: `TALK` while the watched one speaks, the gesture playing `G`, the mood of its line, gestures
  started `S`, started over one `C`, stopped early `X`, nods `N`, the nod now `P` (degrees); `THINK` or
  `LISTEN` instead of `TALK` while it thinks of an answer or listens to the hero;
- top right: IK on/off, weight `W`, pelvis offset `P` and for four legs shoulders offset `C` (cm), per
  leg planted/swinging, contact error `E` (cm), knee deviation from the animation `K` (degrees); `HIDDEN`
  if something is between the camera and an ankle. Four legs: front paws in violet (left) and pink.

## Events (scripts/bench_report.py)

| Flag | Fires when |
|---|---|
| `float` / `sink` | a planted ankle is more than 3 cm above / below the ground it should stand on |
| `pelvis` | the body height (hips or shoulders) jumps more than 2 cm in one frame |
| `toe_sink` | the toes go 3 cm deeper into the ground than in the animation (steps lower than 25 cm) |
| `pop` | the IK correction of a knee, ankle or toe changes abruptly: second difference over 3 frames above 12 cm |
| `knee` | the knee bends more than 25 degrees away from the animated direction |
| `toe_twist` | planted toes turn sideways more than 12 degrees beyond the intended outward turn |
| `stretch` | a planted leg is straight and still more than 2 cm short of the ground |
| `gaze_off` | looking more than 15 degrees past the partner although it is within reach of the turn |
| `aim_off` | while the bow holds still, the arrow points more than 3 degrees past where the shot leaves although the turn could reach it |
| `gaze_jerk` | the face turns more than 10 degrees in one frame, in the world (the body may turn under a steady gaze); a turn to the partner goes up to 9 |
| `anim_pop` | a joint of the animated pose (before lean, IK, gaze) jumps at a switch of animations (a frame before it to 6 after): second difference over 3 frames above 20 cm; the event names the bone and the animations before and after |
| `lip_open` | the mouth is open wider than 0.3 (of the widest shape) while the voice is silent, 4 frames or more |
| `lip_shut` | the voice is louder than 0.7 while the mouth is shut (under 0.1), 4 frames or more |
| `face_jump` | a face comes or goes at once: its weight changes more than 0.5 in one frame (faces fade in about a second, `T_HURT` in 0.1 s) |
| `flinch_none` | a blow, and the upper body gives way less than 1.5 degrees in the 8 frames after it |
| `flinch_slow` | the upper body is not back 1.5 s after the last blow |

Contact flags (`float`, `sink`, `toe_sink`) for a single frame count only when they are at least twice
the threshold: a foot leaving the ground a frame early is not visible.

Columns of the table: `contact` is the error of planted feet against a flat animation floor (only
meaningful with IK on); `spread` is how much the height of planted feet above the ground varies (works
for a body tilted as a whole too: compare IK off and on); `limp` is how much lower the body is on the
steps of one leg of a pair than on the other's. `gaze on` is the share of frames the watched one looks
at someone, then how many times it turned to someone and why (`talk`, `script`: AI_LookAtNpc, `focus`:
the hero's, `attention`: the hero near, a weapon drawn). `gaze err` is the angle between where the face
looks and the partner, while the partner is within reach of the turn. `lean roll / pitch` is the largest
roll into a turn and the range of the trunk pitch (+ forward) of the watched one. `aim err` is the
angle between where the arrow points and where the shot leaves (while the bow holds still), `bend` the
largest bend of the upper body (+ up). `lips corr` is the correlation of the voice level and the shown
mouth opening over the frames lip sync is on (~0.8-0.9 on Xardas' lines). `anim pop` is the second
difference of the joints of the animated pose (cm, the largest joint per frame): p99 and max over the
segment, then the number of switches of animations and the largest value next to one. Within an
animation it is how the animation moves (a running human 11-25 cm, a running waran up to 80): compare
the switches, with `blend off` and on. `hits, peak, back` are the blows the watched one took, the
largest flinch (trunk bend, twist or head, degrees) and how long it took to come back after the last
blow. `gaze_jerk` is not counted for a second after a blow: it throws the head on purpose.
`blinks /min` counts the blinks seen (the eyes going half shut), with those in a pause of the voice and
at a shift of the eyes; people blink some 15-25 times a minute, more while they speak. Flag `blink_shut`:
a blink keeps the eyes shut longer than 7 frames.
`gestures talk / listen %` is how much of the time the watched one speaks a gesture plays, and how much
of the time it is quiet again (a second after its voice), then the gestures started, started over one
that played and stopped before their end (the voice is over), and the nods on stressed syllables. The head
moving with the voice adds a few degrees to `gaze err` while one speaks.

A segment marked `(still)` hardly moved: the player got stuck, the scenario did not do what it meant to.
One reason: an npc with an important dialog came to the hero, and the dialog holds the player;
`bench.log` then says `dialog started: <npc>`. `freeze` that npc first (Lothar at the market).

## Scenario commands

One command per line, `#` starts a comment. The scenario starts once the save is loaded and ends the
game when it is done (`KEEP=1` keeps it open). Anything not listed is passed to the Marvin console
(`cheat god`, `set time 12 0`, `toggle footik`, ...).

| Command | |
|---|---|
| `playvideo <name>` | Marvin command calling the game video player; case-insensitive name, optional `.bik`. Explicitly requests the player even with `-nointro`; the first request starts lazy preparation and is skipped while the part is not ready. Negative `scaleVideos` still disables playback. The scenario pauses during playback and resumes afterwards; `pos` records position, world tick and world time in `events.jsonl`. |
| `wait <ms>` | game time |
| `wait real <ms>` | wall time; the game keeps ticking while scenario commands wait |
| `waitarrive [timeout ms]` | until the player has walked where it was sent |
| `goto <waypoint>`, `goto pos <x> <y> <z>` | teleport (`pos`: where no waypoint is, a roof, a rock) |
| `walkto <waypoint>` | walk there (AI path) |
| `walkmode walk\|run\|sneak` | |
| `hero-mob <SCHEME>`, `hero-mob leave` | the hero takes the nearest free mob of that scheme (`BENCH`, `CHAIR`, `CAULDRON`, `BSANVIL`, `LAB`, ...) as when the player uses it, event `hero_mob` with `used`; `leave` gets up |
| `face uphill\|downhill\|across\|<yaw deg>`, `face wp <waypoint>`, `face rel <deg>`, `turn <deg>` | turn the player at once; `rel`: the watched npc's facing turned by that much (the angles of `place`) |
| `press\|release\|tap <key>`, `hold <key> <ms>`, `release all` | player input; keys: `forward back left right turnl turnr jump walk sneak weapon melee bow spell action parade breath dodge` (`dodge`: the melee dodge's own key, the way from the movement keys held at the press; `left`/`right` strafe; aiming a bow: hold `action`, a shot: `tap forward` while holding it; in the manual aim holding `action` draws, releasing shoots, `parade` lowers the bow, `breath` (= walk) holds the breath) |
| `aimwalk on\|off` | the legs stepping under the aim (graphics/mesh/aimwalk); off: forward and back do nothing while aiming |
| `animinfo <name>...` | the hero's animations of those names: layer, frames, the nodes they move (when not all), the root's way and speed, the hips' turn at a few frames |
| `bladepath <name>\|<overlay.MDS>...` | the tip of the hero's weapon at each frame of those animations, with the grip, the root bone (the game keeps its x, z at the actor) and the speed, in the model's frame (+z ahead); the frames of the fight tags; an overlay (`HUMANS_1HST2.MDS`) is put on for the names after it |
| `wheel <delta>` | the mouse wheel as the window gets it (-1 down, 1 up): aiming a bow by hand, down cants it for a quick shot, up straightens it |
| `cam auto [dist] [height]` | follow the player from the side that sees the feet; switches sides if the feet stay hidden |
| `cam follow <dist> <height> <angle>` | follow, `angle` from the facing: 0 in front, 90 on the left |
| `cam lift on\|off` | `follow` and `world` keep the camera above the ground under it (on, the default); off: where it is put, so a floor above it (a gallery, a hall's ceiling) does not lift it there |
| `cam front <dist> <height>`, `cam world <dist> <height> <yaw>`, `cam fixed`, `cam hold`, `cam look <height>`, `cam game` | `fixed` stays where it is and turns after the player; `hold` keeps the frame as it is (a scene the player may fall out of); `game` is the game's camera as the player has it, without the clip of `view clean` |
| `cam at <x y z> <tx ty tz>` | from a point of the world at another, then held like `cam hold` (a cut: TAA starts over); the hero can stand elsewhere, `goto` him near it first so the world around is loaded |
| `cam arrow [dist 600] [height 40] [side 1\|-1] [ahead 80]` | from the side of the newest arrow's way (`overlay trail` records it), `ahead` cm in front of it, kept 1.2 m over the ground; from the hero until an arrow flies. The arrow's drop reads only from the side |
| `slowmo <k>` | the game runs k times slower than the video (1: normal, up to 20): a smaller game step per recorded frame. The game's sound is recorded as silence while slowed, so it stays in step with the picture |
| `track <npc[,npc...]\|off> [head\|body]` | each recorded frame, where each named npc is on the picture: `events.jsonl` kind `track` with `npc`, `x y` (the head, or the trunk with `body`), `hx hy`, `bx by`, `lx ly` (the head, the trunk, the legs: the limb capsules below the knees) in pixels of the frame, and `w h`. Comma-separated names share the recording; each keeps its own track. For labels that follow the npc in a cut |
| `bodylive on\|off` | opt-in instrumentation of ordinary realtime body control (off by default): `body_live_control` wraps controller events, `body_live_sample` records each tracked npc's HP and target HP per captured frame. Does not acquire a lease or start a policy; requires body arena/replay off |
| `bodylive wait /absolute/file [timeout ms]` | pause scenario commands until the file exists while the normal bridge keeps running; default timeout 120000 ms. Used to finish NPC preparation before recording |
| `view clean [margin]\|nopfx\|full` | `clean` clips away everything closer to the camera than the subject minus `margin` (default 100 cm): trees and bushes in between; particles are hidden. The ground right in front of the camera is clipped too (white) |
| `view hide static movable morph pfx animated` | hide draw types (debugging only: static and movable objects share draw commands, hiding them hides parts of creatures too); `pfx` here hides the particles and sprites of `PfxObjects` too (fires, lamp flames; `view clean` and `nopfx` keep them). A `shot` is written with the next frame drawn: change the view a frame after it (`wait 200`), or the shot shows the change |
| `vsm sides 1\|2\|3\|0` | the sides a lamp's cube face keeps from the next frame: `1` the side away from the light alone (before 2026-11-10), `2` both sides of the world mesh alone (2026-11-10), `3` both sides of objects too (the default since 2026-11-11, [report](../../reports/render-features/2026-11-11-lamp-objects-two-sided/README.md)), `0` as `[INTERNAL] vsmLampSides` says; A/B shots of one frame. Without mesh shaders (`-ms 0`, `OPENGOTHIC_MS=0`, Metal) always `1` ([report](../../reports/render-features/2026-11-10-lamp-one-sided/README.md)) |
| `vsm study <bits>` | the shadow test on a mesh's facets from the next frame, for shots of one view ([report](../../reports/render-features/2026-11-11-facet-shards/README.md)); bits add up: `1` the sun reads the facet's plane over one pixel and `2` the lamps keep the facet's plane on facets edge-on to them — both as before 2026-11-11; `4` instead of the light, the facets by the cosine of their plane to the light (red turned away, yellow steeper than 84°, blue-grey the rest; dark where the shadow test answers shadowed); `8` instead of the sun's light, where the plane over a shadow texel's footprint changes the sun's shadow against the plane over a pixel (magenta darker, green lighter; bright by more than a tenth of the sun, dim by more than a fiftieth); `0` the game's. The same bits from the environment — `OPENGOTHIC_VSM_STUDY=<bits>`, read once at startup — hold for a whole run, so `scripts/perf.sh` measures the cost of the test with one binary ([numbers](../../reports/render-features/2026-11-11-facet-shards/README.md#цена-в-кадре)) |
| `overlay on\|off`, `overlay zoom on\|off`, `overlay bright on\|off` | `bright` stretches dark images (forests, interiors) |
| `overlay trail [off] [keep ms] [line]` | the arrows' ways in gold (with the zones of `showzones`), no legs, no text; turns the overlay on. A way stays `keep` ms (1500) after its arrow stops; `line`: also the straight line the arrow left along (where the bow pointed), dashed white, as far as the arrow has flown: the drop is the gap between them |
| `rec <name> [novideo]`, `stop [shadow]` | record a segment (video and metrics, or metrics only); `stop shadow` also copies both rolling melee shadow journals into segment files (filter cumulative rows by fight game-time interval) |
| `shot <name>` | one PNG |
| `streaming wait [timeout_ms]` | Wait for all desired Near/Far chunks and the publication queue; timeout defaults to 60000 real ms. |
| `deterministic on\|off` | Bench-only deterministic frame (off by default): every frame is a step (no skip under 5 ms); while `streaming wait` waits the world takes no step; when it is ready the world stands four frames more, the renderer drops the TAA history and jitter phase, then 16 fixed steps run before the next command, and visibility appends clusters in a canonical order (sorted on the CPU by sphere and meshlets; one GPU invocation, `visibility_pass*_ordered`) until the next `shot`. Readiness also waits for empty texture demotion queues and no reload in flight; texture residency then stands still until the shot. `streaming-shots` uses it. |
| `npctier <npc> full\|simplified\|auto` | Override one NPC tier until reset or reload. Hero, training and arena actors stay Full. |
| `npctier-test <npc> <start waypoint> <end waypoint>` | Verify 20 switches in each direction during movement and turns, unchanged remaining path/destination and every arrival. `npc-tier-transitions` runs the fixture on a fresh NEWWORLD. |
| `npcspeed <start waypoint>` | Compare Sequence speed with Full and Simplified movement on one authored edge for human/orc/wolf in walk/run/sneak; appended to `npc-tier-transitions`. Removes ambient NPCs for calibration. |
| `npcday <game minutes> [period 5] [shared]` | Journal every NPC over continuous game time ([NPC day parity](#npc-day-parity)); the scenario waits until the time has passed. Every NPC gets a private script random stream from the last `seed`; `shared` keeps the game's one shared stream. |
| `ui on\|off` | the interface in the captured frames, as the player sees it: subtitles, the input line, the deal window (off by default: only the world) |
| `label <text>` | text on the overlay |
| `ik on\|off`, `ik param <name> <value>`, `ik params` | foot IK and its parameters, without rebuilding; `ik off` on a four-legged creature brings back the old whole-body tilt |
| `transform <instance>`, `transform back` | the player becomes a creature (`Wolf`, `Molerat`, `Shadowbeast`, `Waran`, ...), like the shape change spell |
| `bones` | the player's skeleton to `bench.log`: index, parent, name, bind pose position |
| `watch <npc\|nearest\|player>` | the camera and the gaze metrics follow this npc (a script symbol, `NONE_100_XARDAS`) |
| `talk <npc\|nearest> <ms>` | the npc and the hero talk to each other that long (no words): they look at each other |
| `freeze <npc\|nearest> [off]` | the npc leaves what it does and stands still, like a body the AI server attaches |
| `ani <npc\|nearest\|player> <name>` | it plays that animation, as `play_ani` does (`T_STAND_2_PEE` goes on to its loop `S_PEE`) |
| `heropose kneel\|none\|check` | the hero kneels as after a surrender (`player_pose`), or gets up. After `heropose kneel` and `wait 2500`, `check` exercises actual mouse rotation through a movement tick (keeps kneeling and turns 15 degrees), then `ptt_start` (keeps kneeling), then `Forward` (ends kneeling). Success writes `heropose check passed` and the engine writes `[ai] hero pose kneel ends: key Forward` to `game.log`; the bridge must be enabled, no server connection is needed |
| `place <dist> <angle>`, `orbit <deg> <ms>` | the hero stands around the watched npc (angle from its facing, 0 in front) or goes around it; not where no ground is found (inside a rock: it would fall through the world) |
| `place <dist> <angle> <npc\|nearest>` | that npc stands so around the hero (angle from the hero's facing), facing him; the hero stays |
| `gazeinfo` | gaze state of the watched npc to `bench.log`: why it looks, at whom |
| `lean on\|off` | the lean into turns and on slopes |
| `blend on\|off`, `blend param <name> <value>` | transitions between animations (`AnimBlend`; off: the old cross-fade); params `min`, `max`, `scale` (blend time, s), `turn` (rad/s), `move` (cm/s), `out`, `side`, `blow` (the longest switch into a blow of a weapon body, s) |
| `flinch on\|off` | the upper body giving way to blows |
| `hit <npc\|player> <angle> [strength] [height] [twist]` | a blow from that side of it (angle from its facing, 0 in front, as `place`) to the flinch only: no damage, no stumble; strength 0..1 (1), height 0 the hips .. 1 the head (0.6), twist -1..1 about up (0) |
| `strike <victim\|player> [attacker\|player]` | the attacker's blow lands as the game deals it (the stumble, the damage, the flinch); the victim gets its hit points back below a third |
| `clear [radius]` | the npcs around the hero (2500 cm) leave the world, not the watched one: rats, wolves that would join in |
| `aim on\|off` | the upper body turning to the target with a bow, a crossbow, a spell; leaning over an edge (and the focus seeing from there) |
| `bowdraw on\|off` | the hand draws the string as far as the bow is drawn by hand (`graphics/mesh/bowdraw`): the bow held as an archer holds it (up in the hand, the arrow on the hand at the bow's middle, the string drawn straight back: the `S_DRAWN` morph), the nock straight along the arrow as far as the arm reaches, the string with it; the hand follows through after the shot; in the reload the upper body holds the aim and the hand goes round the head to the quiver and back to the string. `bow-draw.bench` films it close by. CSV `bd_w bd_draw bd_span bd_travel bd_miss bd_follow bd_fit bd_offset bd_angle` (the string against the arrow), `hd_hand hd_fore hd_arrow` (cm from the head's capsule, < 0 in it), `bd_hold bd_nock` (the reload: held; 0..1 to the quiver, 1..2 to the string), `rh_x rh_y rh_z` (the drawing hand in the body's frame), `bd_held bd_overgrip bd_atmiddle bd_nockgap bd_square bd_cant bd_bowmove bd_bowmiss` (the archer's pose, `docs/manual-aim.md`: held so 0..1, cm the arrow is over the grip and over the bow's middle, cm from the nock to the string, degrees between the arrow and the limbs, degrees the bow leans, cm the bow hand raises the bow and does not reach; `bd_nockgap` -1: no arrow nocked) |
| `bowgeom` | the bow and the bow hand of the hero in the arrow's frame (x along it from its slot, y up, z to the side), cm: the bow's box, the grip, the string's middle at rest and drawn by `S_READY` and `S_DRAWN`, the bow hand's bones, the head |
| `animspeed <anim> <k>` | the hero's animation of that name plays k times as fast from its next start (`Pose::setAnimSpeed`: a layer keeps its speed, the next one starts at 1). The manual aim sets `T_BOWRELOAD` from dexterity (`stats` prints it: `reload x`); this overrides it until dexterity changes. The hero's animation events go to bench.log (`anim event <type> <item> <slot>`, `anim morph <anim> <slot>`): at any tempo each once, in the same order |
| `sfx <name>...` | the files a sound effect plays (its variants `_A1`..), `(missing)` if the game's data has no such file. The hero's sounds go to bench.log (`sound <name>`) |
| `stats [npc <npc>] [str <n>] [dex <n>] [1h <n>] [2h <n>] [bow <n>] [cbow <n>] [hp <n>] [hpmax <n>] [mana <n>] [circle <n>] [talent auto\|0\|1\|2]` | the hero's (an npc's) strength, dexterity, hit chances (the skill), hit points; `mana` sets mana and its maximum, `circle` the magic circle (a rune's spell then casts); prints what the manual aim makes of them (sway, draw, hold, arrow speed). Arguments apply left to right. `talent` sets the trained archers' animations (`_BOWT1` at 30%, `_BOWT2` at 60% in G2; `auto` as the hit chances say), and the 1H/2H animation level only when that weapon's hit chance is explicit in the same command; with it or `npc` also `stats of <npc>: melee hit chances and animation levels, archery talent, reload ms, hp, point protection` |
| `seed <n>` | the random numbers start over: the scripts' (the dice, the release cone), the bench's (`volley` jitter), the hero's sway |
| `npcequip <npc> <item> [count]` | into the npc's inventory, the first put on |
| `arc <npc> <count> <dist> <step deg>` | that many of the npc stand that far in front of the hero, a step apart round it, facing it: the crew of `fight all` (a new arc replaces it, `clear` takes the gone out) |
| `wander [radius cm 3000] [walk\|run]`, `wander stats` | the npcs of `arc` are held (as the AI server holds a body) and walk by the waynet to a random waypoint within the radius of the hero, then to another: a crowd for the speed probe of docs/navigation.md; `stats`: how many walk, how many ways they went |
| `stuck begin <label> [radius cm 3000]`, `stuck end` | measure native routine walks around the hero's position at `begin`, without controlling NPCs; requires `OPENGOTHIC_STUCK_LOG=<absolute JSONL path>`. Default observation radius is 30 m. `end` writes the observation summary to `<path>.summary.json` and closes ongoing episodes as censored (`observation_end`). |
| `fight <npc\|all> <target\|player> [deed]` | it attacks as the game starts a fight (`ZS_ATTACK`, attack reason to kill: archers keep the bow beyond 9 m); `deed` uses the nonlethal native `npc_act fight{deed:true}` start for a standing human NPC target |
| `waitdown <npc\|player> [ms] [other npc]` | waits till either participant is down, or times out; writes `waitdown_end` with outcome, elapsed time and HP |
| `npcaim on [second\|arrow] \| off \| sigma <deg\|off> \| lead on\|off` | npc archers aim by hand (game/archerai, `[GAME] npcManualAim`, on by default: `npcaim off` for the dice): as many hits a second as G2's dice (`second`) or as sure an arrow (`arrow`); an error forced, degrees per axis (0: the bow's own spread only); the lead on a runner. Each release logs `arrow aim: at, true <the point meant>, err <right up> deg, sigma, pv <dice %> p <target %> rho, lead, hold, disturb, settled\|waited, by <npc>` |
| `archer <npc\|all> at <npc\|player> [dice] \| archer off` | the archers stand where they are and shoot at the target every time they can, by hand (npcaim on) or by the dice: no fight AI, no perception's wait, arrows topped up (the aim's calibration) |
| `still <npc\|player> [off]` | hit, it neither stumbles nor flinches: a target that keeps its pose (as its zones were dumped) |
| `dodge [<npc>] back\|left\|right\|forward\|none [before <attacker> <ms>]` | the melee dodge (`[GAME] melee_dodge`, `Npc::meleeDodge`) that way relative to its facing, now, or as the attacker's next blow comes within `<ms>` of its contact (`Pose::nextMeleeContact`, 8 s at most); `bench.log`: `melee dodge <npc>: <way>, armour <class>, <move> <animation> [(no <missing>)], cost <n>, window <a>-<b> ms` or why there is none, and `... hit by <npc> at <ms> ms of <move> <a>-<b> ms: did not pass` for a blow, arrow or spell inside the window. `melee-dodge.bench`: the four ways by the dodge key, then by the jump key (the default binding; with the strafe and the turn key), one under the bandit's blow, medium and heavy armour; the same file with `MELEE_DODGE=0` checks the flag off (`tap jump` jumps: `anims` logs `bs=8`) |
| `aimat <npc\|nearest> [head\|body\|limb\|leg] [raw]`, `aimat <x> <y> <z>`, `aimat ahead [dist] [height]`, `aimat off` | the manual aim aims at the middle of that zone of the npc (every frame) or at a point, instead of the mouse; the hero faces it. Over it by the drop of the arrow at full draw, as an archer holds over (`raw`: right at it). `limb` is the middle of all the limbs, which lies inside the trunk; `leg`: the limb capsules below the knees. A last word `mouse`: as the player does, the game's camera turns so that the center of the screen falls on it, the hero turned with the camera, and the shot goes from the center of the screen (the game's own aim: what the camera sees, the lean by where the hero looks) |
| `open [dist cm]` | how far it is open around the hero at head height and at the knees every 15 degrees from the facing: where to put a target |
| `volley <shots> [hold ms] [draw 0..1] [jitter ms]` | shots by hand in a row: press `action`, draw that far (1: full), hold (up to `jitter` longer at random: the sway is a function of time), release, wait for the next arrow; every arrow is in the log (`arrow shot: ..., by <npc>`, `arrow hit <npc>: <zone>, hp a -> b, draw, flew, at, by <npc>` or `arrow missed: ..., by <npc>`). With an NPC aim target, `aim zone ... hero ... target ...` records the aim point and both root positions in cm at each release. `volley phase` notes add press, draw-limit, release and reload-end positions and animation names; `shot pose` records the real projectile release. |
| *(log)* | the dice of npc shots: `dice shot: by <npc> at <target>`, `arrow dice <victim>: dist <cm> path <cm> talent <%> chance <%> roll <0..99>, hit\|miss, by <npc>`; every blow: `damage <victim>: raw <n> zone <z> k <x> prot <n> net <n> hit 0\|1, hp <n>, precise\|dice\|melee\|spell, by <npc>`; every note also in `events.jsonl` (`kind: note`, with the time) |
| `immortal <npc\|nearest> [off]` | arrows hit it, it does not die: counting hits |
| `zones <npc\|nearest\|player>` | the hit zones of the npc now (`graphics/mesh/hitzones`): each capsule (radius, the guess by the bone's name) and how well they cover its body: of each zone, the points of the body in it / only in another zone / in none, and the body's mesh. The report sums them up (`Hit zones`) |
| `zones fit on\|off` | the hit zones as thick as the body's mesh (on, the default) or guessed by the bone names and the creature's size; the hero refitted at once, the others when they change their body (`transform`) |
| `showzones <npc symbol>\|player\|off`, `overlay zones [off]` | the overlay draws the hit zones of that npc (the head red, the trunk yellow, the limbs blue); `overlay zones`: nothing else (no legs, no text), and turns the overlay on. To film the zones of a distant target, stand it with `put` (`pin` floats it into a jump pose). `zones.bench`: 31 creatures with the guess and fitted |
| `corner <left\|right> [count]`, `corner <left\|right> go <i>` | corners of walls near the waypoints within 80 m: a wall right in front of the head, open 6 m ahead leaning out to that side (the manual aim's peek); lists them nearest first, or puts the hero at the i-th facing along it |
| `aim-over-edge.bench` | the manual aim down over an edge, from the game's camera (`aimat ... mouse`): a wolf below the roof of Lobart's barn and below the gallery in Xardas' tower; the trunk bends over the edge, the camera over the shoulder goes ahead and down with the head, the wolf under the crosshair, not the roof or the wall. On the roof the aim goes level and back down (the lean eases out and in), then the shot and the bow lowered |
| `aim-stance.bench`, `aim-corner.bench` | the manual aim's stance: crouched (the sneak key, `ik_crouch`), leaning out (the side steps held, `a_side`, room checked by a ray beside the head); from the side, from behind and from the game's camera; at a corner of a house in the harbour. CSV `a_side ik_crouch m_crouch m_peek` |
| `archery-smoke.bench` | the stage 4 tools: who shot, the dice and the damage lines, `seed` twice the same, `volley` with jitter, an npc's `stats`/`talent`, `arc`, `fight` (by the dice), `waitdown` |
| `npc-today.bench` | npc archers by the dice as the game had them (`npcaim off`): 4 bandits with bows, addon bandits with crossbows, paladins, at 11 / 13.5 / 16 m set out (the fight AI moves them), the hero immortal and still; the Dice table checks the throw against G2's formula |
| `npc-mech.bench` | npc archers aiming by hand where they stand (`archer`): the bow's own spread (`npcaim sigma 0`), with G2's dice as aim, by the dice; a fight; one archer filmed close up |
| `hero-balance.bench` | the hero by hand against G2's dice: four tiers as G2 has them (talent 10/30/60/100 with its bow, strength and dexterity, the trained animations), a farmer at 10, 20 and 30 m, body and head, 30 shots a cell with a random hold and a seed each; the player root is pinned and NPCs within 200 m are cleared before inserting the target; the Archery accuracy table gives the hits and shots a minute |
| `npc-lead.bench` | npc archers at a runner: the hero runs sideways to and fro 12 m in front of a bandit and a paladin, by hand with the lead, without it, by the dice |
| `npc-fight.bench` | fights with 4 npc archers of a kind (bandit bow, addon crossbow, paladin): the dice, by hand per second, per arrow; damage a minute and the time to kill from the Damage table; bandits set out at 25 m running in; a real duel at the end |
| `aim-walk.bench` | steps with the bow drawn (forward and back while aiming by hand): the legs sneak under the aim, the hero moves as they step; on open grass from the side (no overlay), in front, behind, the game's camera with shots on the way, crouched; the hits at 10 m standing and stepping. CSV `m_walk aw_w aw_speed aw_twist aw_chest aw_drop` |
| `aim-cant.bench` | the bow canted for a quick shot (the mouse wheel, `wheel <delta>`: down cants, up straightens): round the arrow, the top limb toward the drawing hand; from the side, in front, as the target sees it, behind, the game's camera, aimed up and down; the hits upright and canted at 8 and 25 m. CSV `m_cant bd_cant` |
| `crossbow.bench` | the crossbow by hand: a weak and a strong shooter (`stats ... reload x`: the reload by strength), lowered loaded and raised, lowered right after a shot; the bolt lies in the left hand, the aim lines it up (`bowgeom` prints where it lies) |
| `humiliate-look.bench` | the humiliation after a surrender staged without the AI server: the hero kneels, Lobart 100 cm in front plays `T_STAND_2_PEE` and the aimed stream `PFX_GAI_PEE` arcs to the hero's chest; stills round them and a 2 s video from the side. Judge it at `TEMPEST_WINDOW_SIZE=1600x1000`: at 800x600 the stream is a few pixels |
| `gi-clips.bench` | reflected light (`-gi 0/1/2`): shots and clips of the places where it shows, for step 2 of the ray-tracing revisit — the harbour house by candle and torch at night, the plank hut of the poor quarter at noon and at night (and the clip `hut-response`, 22:00 → 12:00), Onar's stable and Lobart's house at noon, a harbour street at midnight, the forest at noon, the bandit cave at noon and at night, the market at noon. Every shot `X` is taken twice: as the player sees it and `X-bare` without the hero, the npc and the particles (`view hide animated morph pfx`; the ray scene and so the reference lack them). The views `leak-*` test light through thin plank walls (thinner than a probe cell of lod 0): `leak-lamp-night` (a lamp inside hut 1 behind its north wall, the camera outside), `leak-fire-night` (the fireplace of hut 3 behind its north wall, the camera in the alley), `leak-sun-day` (the west wall of hut 6 in the noon sun, the camera inside). `uv run scripts/gi_compare.py <root> --variants gi,bare <shots>` compares them with `gi-reference.bench` cell by cell ([step 2b](../../reports/render-features/2026-11-08-rt-revisit/step-2b.md#2б-2-сделано)) |
| `gi-reference.bench` | the path-traced reference of the shots of `gi-clips.bench` (the same views, leaks included; 12 s to accumulate each; `OPENGOTHIC_RT=1 OPENGOTHIC_GI=0`): change both together. Its shadow rays skip the back faces of triangles (`rayQueryProceedShadow`): a one-sided surface that faces a lamp lets the lamp's light through in the reference too ([report](../../reports/render-features/2026-11-10-lamp-one-sided/README.md)); alpha-tested triangles skip their back faces in `rayQueryProceedAlphaTest` besides, and the reflected light lights places a lamp does not reach: judge lamp shadows against direct light alone ([report](../../reports/render-features/2026-11-11-lamp-objects-two-sided/README.md)) |
| `gi-turn.bench` | reflected light (`-gi 1`) through a turn of the camera: the plank hut of `gi-clips` at noon (view `hut-day-2`), clip `hut-turn` — the camera stands 1 s, turns 180° round the hero at once (`cam cut`, `cam world` at +180°), stands 3 s, turns back, stands 2 s. `uv run scripts/gi_compare.py --turn <out>/hut-turn.mp4` prints the largest and the median cell change between the frame before the turn and the first frame after the return (8×4 cells without the two middle columns). Run it with `OPENGOTHIC_RT=1 OPENGOTHIC_GI=1 TEMPEST_WINDOW_SIZE=2560x1440 BENCH_HOST=ryzen` ([step 2b](../../reports/render-features/2026-11-08-rt-revisit/step-2b.md)) |
| `gi-accept.bench` | the acceptance clips of reflected light (step 2 of the ray-tracing revisit): the user judges «без GI \| GI» by eye, so the scenario records clips only, no shots. The places and camera set-ups of `gi-clips.bench`; each clip is a slow turn of the held camera, 1 s still, 9 s turn (`cloth camera-turn`), 1 s still, after a turn of 1 ms that sets the first view and 10 s for the probes to light: `interior-night` (the harbour house at 22:00, cabinet → candle and lit wall → shelves in the dark corner), `hut-day` (the plank hut at noon, sunlit wall → dark corner), `harbour-night` (the street with lamp posts at midnight), `forest-day` (rock → sunlit valley → slope in the shade), `cave-day` and `cave-night` (the bandit cave: the mouth → the fire). `harbour-night-walk` follows the hero 5 m behind down the same street (`route 120` PATH_04 → PATH_06, 9 s; the hero and the npcs hidden, he would slide without steps; `route` lifts him 50 cm to the waypoint on its first step, so the camera jumps at 1.0 s and the side-by-side starts at frame 31): probes are placed by the distance to the camera, so light popping in shows there. The fixed step makes both runs frame for frame alike. The clips of the eye's adaptation under GI ([step 2](../../reports/render-features/2026-11-08-rt-revisit/step-2-shade.md#привыкание-глаза-сделано-12112026)): `forest-walk` (the hero runs from the open slope at `NW_FOREST_PATH_67` under the crowns to `PATH_66` and back, the camera following), `cave-walk` (from the path outside the bandit cave to the fire and out), `cave-mouth-turn` and `forest-turn` (360° in 12 s at the cave's mouth and under the crowns), `market-day` (the open sun, the control). Run it with `OPENGOTHIC_RT=0`, with `OPENGOTHIC_RT=1 OPENGOTHIC_GI=1 OPENGOTHIC_EYE=0` and with `OPENGOTHIC_RT=1 OPENGOTHIC_GI=1` (the eye on), `TEMPEST_WINDOW_SIZE=2560x1440 BENCH_HOST=ryzen`, and put the thirds side by side («без GI \| GI \| GI + привыкание») |
| `gi-decay.bench` | how the light of `-gi 1` dies out in the chains of probes: the plank hut and the bandit cave of `gi-clips` (views `hut-night-1`, `cave-night-1`), clips `hut-decay` and `cave-decay` — 1 s of noon, then `set time 22 0` and 14 s of night, the camera still. A jump of the time of day relights the probes from nothing; `OPENGOTHIC_GI_JUMP_RESET=0` turns that off, and the clip shows the day's light dying out as after a fire put out. `uv run scripts/gi_compare.py --decay <out>/hut-decay.mp4` prints how many times brighter than its end (night) the brightest and the median cell are 0.5, 1 and 2 s after the change, and when every cell is within 10% of it; cells that flicker at the end by more than 10% (flames) are left out, each cell is the median of 15 frames (sparks). Run it with `OPENGOTHIC_GI_JUMP_RESET=0 OPENGOTHIC_RT=1 OPENGOTHIC_GI=1 TEMPEST_WINDOW_SIZE=2560x1440 BENCH_HOST=ryzen` ([step 2b](../../reports/render-features/2026-11-08-rt-revisit/step-2b.md#2б-1-пересмотр)) |
| `facet-shards.bench` | the shadow test on a dense mesh's facets ([report](../../reports/render-features/2026-11-11-facet-shards/README.md)): the hero's own head (`OPENGOTHIC_CONTENT` with it) close to the camera under the noon sun of the market and the candle of the harbour house, then the market and the harbour at a low sun and the lamps at night; every view as stills of one camera — `-old` (`vsm study 1` or `2`), `-new`, `-old2`, `-class`, for the sun `-diff`. Run with `OPENGOTHIC_LIGHT_FREEZE=1`; `reports/render-features/2026-11-11-facet-shards/tools/pairs.py <out>` counts the changed pixels against the noise of the two old shots |
| `give <item> [count]`, `equip <item>` | into the hero's inventory; `equip` puts it on whatever the strength (`ItRw_Bow_L_01`, `ItRw_Arrow`, `ItRw_Crossbow_L_01`, `ItRw_Bolt`, `ItRu_FireBolt`) |
| `put <npc\|nearest> <dist> <angle>` | the npc stands there on the ground from the hero (angle from the hero's facing), frozen |
| `pin <npc\|nearest\|player> <dist> <angle> <height>`, `pin off` | hold the root at the computed point every frame; NPCs face the hero, while `pin player 0 0 0` keeps normal player input, animation and aiming |
| `anims` | what the watched one (or the hero) plays, its body state, weapon and target |
| `cansee <npc>` | can the hero see it: from the head, leaning over an edge (the aim's lean); what the rays hit; the bow's focus range |
| `sight <npc\|off> [fan cm]` | each recorded frame and `shot`, the hero's lines of sight to it on the picture: `events.jsonl` kind `sight` (`shot` names the still), points `[x, y]` in pixels of the frame (null behind the camera): `h0` the head when `sight` was given (upright) and `c0` where its ray to the npc's centre hits (null: nothing; `c0d` cm of `d0`); `e` the head leaned over an edge as the focus puts it (`room`), `ec` where its ray hits; `h` the head now, `c` where its ray hits (`cd` of `d`, `down` degrees); `t` the npc's centre; the bow's focus fan from `h0`: `f` level, `g` at `FOCUS_RANGED`'s `elevdo` down (`elev`), `fan` cm long; `seen`, `over`: the focus sees it from the head, over the edge. For lines drawn over a cut |
| `views <npc> [count] [max dist] [from <min dist>] [shoot <prefix>]` | where a camera sees the hero (head and chest: a wall's top may hide the legs) and the npc in one frame from open air (nothing within 40 cm, a clear line to the hero's head): `cam follow` spots with the `cam look` halfway between them, the nearest of each side (`from`: no nearer, for a wide shot); `shoot`: a shot from each (`<prefix><dist>_<height>_<angle>`) goes into the scenario right after it, with `cam lift off` |
| `hp <npc> [full]` | hit points of the 3 nearest npcs of that script symbol and how far they are (did the arrow hit); `full`: the nearest gets them all back first |
| `walls [count]` | spots at the edge of a wall (from a waypoint walking to where the ground falls away) with its foot 2.5..10 m below, 1 m out, hidden from the head and seen leaning over: the waypoint and the yaw for `goto`, `edge` |
| `perches [radius 3000] [count 15] [min drop 250] [max drop 1000]` | like `walls` on any top around the hero, waypoint or not (a roof, a rock, a wall's top): a grid of the highest surfaces you can stand on (slope up to 35 degrees, 1.9 m free above) at an edge with the foot 1 m out hidden from the head, seen leaning over, the sky above it; one a 2 m cell, the nearest first, as `goto pos <x y z>`, `face <yaw>` |
| `npcgo <npc\|nearest> <waypoint> [walk\|run]` | the npc goes there by the AI path, running by default (`freeze` it first so its routine does not take it back) |
| `lookat <npc\|nearest> <npc\|player\|off>` | as the script `AI_LookAtNpc` / `AI_StopLookAt` |
| `gazeat <npc\|nearest\|player> <yaw> <pitch> <ms> [dist]` | it looks at a point that many degrees aside and up from its facing, `dist` cm (100) from its eyes (`mapEyes`), for `ms`, before anything else it would look at |
| `nearby [radius]` | the npcs around the hero to `bench.log`: distance, where the hero is from their facing, where they look and why |
| `pair <a> <b>` | how two npcs stand to each other, to `bench.log`: the distance; for each, where it faces (`at_other`, 0: right at the other), where it looks and why (`look`, `at`), its script state and for how long (`state`, `t` ms), body state, whether it uses a mob, how much of its AI runs (`ai`: `normal`, `far` beyond 30 m from the hero, `far2` beyond 60 m) |
| `where <npc> [waypoint]` | where it is, how far from the hero (and from the waypoint), its state, whether its queue is busy (walking there), how much of its AI runs |
| `state <npc> <ZS_*> <waypoint> [minutes]` | it does that at that place now, as a routine entry would (`TA_*`), for that many game minutes (60), then its routine goes on (`npc_act start_activity`) |
| `come <npc>` | it walks up to the hero and turns to him (`npc_act go_to_npc`); a free one goes back to its routine after |
| `cam face <dist> [angle] [height]` | close to the face of the watched one, looking at the mouth; `angle` from its facing |
| `speak <npc\|nearest\|player> <output name\|$SVM> [to]` | it says a voiced line of the game (`DIA_Addon_Xardas_Hello_14_00`), like `AI_Output` in a script, or an SVM (`$SMALLTALK01`, like `AI_OutputSvm`); to the hero, or to the npc `to`: it turns to it first, as a line of the AI said to an npc does (`say.to`) |
| `dialog <npc\|nearest>` | the hero talks to it, as with the action key: the dialog window opens |
| `choose <n>` | runs the n-th choice (from 1) of the dialog with the watched npc, as if the player picked it |
| `faceanim <npc\|nearest\|player> <anim> [ms] [intensity]` | a face animation of the head (`S_ANGRY`, `S_FRIENDLY`, `T_HURT`...) |
| `facecue <npc\|nearest\|player> <gesture> [L\|R]` | a facial gesture of its own head (`flash`, `doubt`, `smirk`...: the `cues` of its face profile); a one-sided one without a side goes on a side by the seed; columns `cue_*` show the gestures |
| `faceleak <npc\|nearest\|player> <felt> <upper\|lower>` | a leak of the felt emotion (`friendly`, `angry`, `hostile`, `afraid`, `pain`) on half of its own head's face (the `leaks` of its face profile) now, as a line that hides a lie: with the liar's two blinks in the 2 s after it; column `cue_leak` shows its strongest weight |
| `lips on\|off`, `lips param <name> <value>`, `lips params` | lip sync (off: the old talk loop) and its parameters |
| `gestures on\|off`, `gestures param <name> <value>`, `gestures params` | the hands while one speaks (`TalkGestures`; off: a random gesture when a line starts) and its parameters: `Tail`, `Wait`, `MinLeft`, `Slack` (ms), `Long`, `RestScale`; the head: `Nod` (degrees), `NodHz`, `Stress`, `NodGap` (ms), `SwayYaw`, `SwayRoll` (degrees), `SwayTime`, `SwayLag` (ms); listening and thinking: `ListenSway`, `ListenNod` (degrees), `ListenHz`, `ListenGap` (ms), `ListenProb`, `DoubleNod`, `ThinkDelay` (ms), `ThinkYaw`, `ThinkPitch`, `ThinkTilt` (degrees), `AsideTime` (ms) |
| `blink on\|off`, `blink param <name> <value>`, `blink params` | blinking (`EyeBlink`; off: nobody blinks) and its parameters: `MinGap`, `RestMean`, `SpeakMean`, `MaxGap`, `Hold`, `PauseMs`, `SoonMs`, `DoubleGap` (ms), `PauseProb`, `ShiftProb`, `DoubleProb` |
| `hear <npc\|nearest\|player> <ms> [every ms]` | the hero speaks to it that long, his words come every so often (600 ms), as the bridge passes the partial transcripts of his voice: it listens, nods now and then; `player`: the hero listens to the nearest npc |
| `think <npc\|nearest\|player> <ms>` | it thinks of an answer that long, as on the AI server's `thinking` status; `player`: the hero, of an answer to the nearest npc |
| `ani <npc\|nearest\|player> <anim>` | it plays an animation of its model script (`T_YES`, `T_DIALOGGESTURE_03`) |
| `foreach slope <min deg> <max deg> <count>` ... `end` | the body runs on that many slopes all over the world, after a teleport; `$I`, `$NAME`, `$ANGLE` are replaced |
| `foreach stairs <count>`, `foreach wp <name part> <count>` | the same for stairs and named waypoints |
| `slopes [min] [max]`, `stairs`, `findwp <name part>`, `pos` | print spots / state to `bench.log` |
| `edge <yaw> [max cm] [margin cm]` | the hero is put that way to where the ground falls away, `margin` (35) from the edge facing it (closer the body slips over it); prints the drop past it (a wall, a ship's side) |
| `ledges [min drop] [max drop] [count] [steep]` | waypoints at an edge with open ground that much lower 1.5..14 m away, seen from there (a target below): the yaw to `face`, the distance, the drop, the angle down; nearest first or the steepest |
| `say <text>`, `quit` | |

Talking (needs the AI server; the video tool lives in the private `kaifaty/gothic-shorts` repository):

| Command | |
|---|---|
| `ptt <wav> [text]` | the hero holds the talk key while the recording is said (`ai ptt start <wav>`: the AI server takes the file instead of the microphone, `--voice-files`) |
| `waitreply [timeout ms] [quiet ms]`, `waitquiet [quiet ms] [timeout ms]` | until an npc of the AI has answered and has been quiet that long; until none speaks. An npc thinking of its answer is not quiet: the "Хм..." it says at once is not the answer |
| `waitai <key> <stage> [timeout ms]` | until a thing of the AI bridge has got that far: `waitai necro fallen` (the necromancer's answer: cast, fallen, raise, risen, card; `ai necro <npc> [spell] [picture]` starts it without the server) |
| `near <npc> [dist] [deg]`, `approach <npc>`, `face npc <npc>` | the hero in front of an npc (teleport), walks up to it, turns to it |
| `cam shoulder\|face\|two <npc> [dist] [height] [side\|deg]`, `cam cut`, `cam smooth <k>` | shots of the hero and an npc; they glide from one to the next unless `cut`. `cam face` with an npc's name is this shot, with a number the close-up above. `cam two arena`: the two participants of a body arena, a yellow dot over the primary (`bodyreplay`, `scripts/body_replay_film.py`) |
| `caption <text>`, `mark <text>` | events for the cut of a short; `mark stream_warm` and `mark stream_warm_5s` also record load-probe memory after landscape warmup and five real seconds later |
| `ai <...>` | the AI bridge's console: `ai attach <npc>`, `ai skip <npc>` (its important dialogs count as told: it does not stop the hero), `ai ptt start [<wav>]\|stop\|cancel` |

Every talking command writes `events.jsonl` (what was said when, in frames of the recording). A line of the
AI is `npc line <npc>: <text>` in `bench.log`, `npc line <npc> -> <to>: <text>` when it is said to another npc
(its `npc_line` event has `to`); the quick model's first words said at once while the conversational model thinks («Хм...», «Что?!») have `"filler": true`. What the npcs say through the game's own path, lines of the game and SVMs
(`AI_Output`, `AI_OutputSvm`, the chatter of two in `ZS_SMALLTALK` too), is `output <npc> -> <target> <name>
<ms>` (0 ms when the engine did not look the length up; `far` or `far2` after it when the npc's AI is cut down
by its distance to the hero: such a line is never heard), and an `output` event.

Environment of `scripts/bench.sh`: `GOTHIC_DIR`, `SAVE` (default: `save_slot_1.sav` of the main
checkout), `WORLD` (a fresh game in that world instead), `GOTHIC_INTRO=1` (the game starts with
`-nointro` by default, as in e2e: no new-game intro and no script videos; Marvin `playvideo` still
plays), `NO_BUILD=1`, `TIMEOUT` (seconds, default 300), `KEEP=1`, `OPENGOTHIC_TEXTURES` (sharper
textures; by default those of `scripts/hd-textures.sh` when they have been made, as the launcher does;
empty: the game's own), `SOUND` and `MUSIC` (volumes 0..1, default 0; with `TEMPEST_AUDIO_LOOPBACK=48000` a
segment's sound goes into `<segment>.wav`).

## Face cues clip

The acceptance clip of the facial gestures and the leak of a lie in the game
([face-cues.md](../../reports/content/2026-11-hero-head/face-cues.md), "Приёмка", "В игре"): Vino with an own head
says four lines with gesture tags and one lie through the real path — the server's parsing (`MoodTags`,
`say.cues`, `say.leak`), the voice of `tts-service` with its word times (`say_audio.words`), the bridge's
`LineCues`. The server's scenario `face_cues_demo` (`server/src/gothic_ai/scenarios/face_cues.py`) puts Vino on open
ground in Lobart's yard (`NW_FARM1_PATH_01`, out of the field's crops) at noon, takes him over, puts the hero
2 m in front of him and asks him 8 s later; `face-cues-clip.bench` films his face
close up without the overlay (`overlay off`: its text would be in the clip) from 5.5 s on until he has been
quiet 3 s. The leak is the server's trial (`p` at most 0.6); for a sure
one the server runs with the debug flag `--debug-leak [upper|lower]` (every `[felt:…]` leaks, on that half).

Vino needs an own head with `cues` and `leaks` in its face profile: a content copy whose `characters.json` lists
`BAU_952_VINO` (e.g. the hero head of `hero-head-2` with the sections of
[`test-head.face.json`](../../reports/content/2026-11-hero-head/mpfb/test-head.face.json)). On the Mac, from the
repository root (Ryzen gets the server through the bench's tunnel):

```bash
W=/tmp/face-cues-clip; mkdir -p $W
(cd tts-service && uv run gothic-tts serve --port 47920 --provider inworld --fallback none) > $W/tts.log 2>&1 &
(cd server && uv run gothic-ai serve --port 47921 --scenario face_cues_demo --llm off --exit-after --no-console \
  --no-voice --no-brain --no-quick --tts-url ws://127.0.0.1:47920 --debug-leak upper \
  --memory-db $W/memory.sqlite3 --trace-dir $W/traces) > $W/server.log 2>&1 &
GOTHIC_AI_URL=ws://127.0.0.1:47921 OPENGOTHIC_CONTENT=<content root> TEMPEST_WINDOW_SIZE=1280x720 \
  OPENGOTHIC_BENCH_REALTIME=1 TEMPEST_AUDIO_LOOPBACK=48000 SOUND=1 MUSIC=0 BENCH_HOST=ryzen \
  scripts/bench.sh scripts/bench/face-cues-clip.bench
uv run --with pillow python reports/content/2026-11-hero-head/cues/ingame_clip.py .bench/face-cues-clip-<time> <out>
```

The gestures of the eyes (glance, roll) take the same bench with `--scenario face_eyes_demo` (four lines with
`glance`, `roll`, `doubt`, `frown`, no lie, no `--debug-leak`) and a face profile with `eyes`.
`--provider inworld` spends four paid Inworld requests a take; `--provider fake` (a tone with word times) checks
the staging for free. `ingame_clip.py` writes into `<out>`:
- `clip.mp4` — the recording with its sound and "Реплика N" over each line;
- `key.txt` — for each line its text and mood, each gesture with its word, start, apex and the weight of its
  `cue_*` column at the apex, the lie with its felt mood, half and `cue_leak`, the liar's blinks (eyes shut by
  `R_EYESBLINK` while `bk_n`, the ordinary blinks, stays);
- `sheet.jpg` — the frame before and the apex of each gesture and of the leak.
The gestures of each line come from its `[ai] cues` line in `game.log`, the times from `metrics.csv`.

## Face brows clip

The acceptance clip of the brows on the accents of the voice
([eyes.md](../../reports/content/2026-11-hero-head/eyes.md), "План: брови по тону"): Vino with an own head says three
lines of the AI server through the real voice path (`face_brows_demo` in
`server/src/gothic_ai/scenarios/face_cues.py`: the Russian yes-no question «Ты его видел вчера?», a long statement, an
angry line; no gesture tags), then `face-brows-clip.bench` has him say two long lines of his original voice
(`DIA_Vino_HALLO_05_02`, `DIA_Vino_PERM_05_04`); the scenario holds him 30 s after his answer for them. Staging as in
[Face cues clip](#face-cues-clip); the face profile needs `speech_brows` (e.g. the hero head of `hero-head-2` with the
sections of [`test-head.face.json`](../../reports/content/2026-11-hero-head/mpfb/test-head.face.json)). The same
commands with `--scenario face_brows_demo`, without `--debug-leak`, and the bench `face-brows-clip`;
`OPENGOTHIC_SPEECH_BROWS=0` films the same without the layer. `--provider inworld` spends three paid requests a take.
`reports/content/2026-11-hero-head/cues/brows_clip.py <bench out> <out>` writes `clip.mp4`, `key.txt` (each line's
text and voice, each accent with its time, the word it falls on — an estimate by characters — the rise of the tone and
up, down or no move, the start and apex of `br_w`) and `sheet.jpg` (before and apex of the strongest moves); with
`--off` only `clip-off.mp4`. The accents come from the `[face] accent` lines of `game.log`, the times from `lip_t` and
`br_w` of `metrics.csv`.

## Face listen clip

The acceptance clip of the face of the one listening
([eyes.md](../../reports/content/2026-11-hero-head/eyes.md), "План: брови по тону и лицо слушающего", item 3): Vino with
an own head listens. First Lobart says two lines to him through the real voice path (`face_listen_demo` in
`server/src/gothic_ai/scenarios/face_cues.py`: Vino on open ground, the hero 2 m and Lobart 1.5 m in front of him;
Vino nods on the accents of Lobart's voice, his face goes by his attitude to Lobart), then `face-listen-clip.bench`
has the hero speak to him as the bridge does with his voice (`hear BAU_952_VINO 16000 600 friendly`, then `angry`:
the last word sets his attitude to the hero); the scenario holds him 45 s after Lobart's lines. Staging as in
[Face cues clip](#face-cues-clip); the face profile needs `speech_brows` and `listen_face` (the sections of
[`test-head.face.json`](../../reports/content/2026-11-hero-head/mpfb/test-head.face.json)). The same commands with
`--scenario face_listen_demo`, without `--debug-leak`, and the bench `face-listen-clip`; `OPENGOTHIC_LISTEN_FACE=0`
films the same without the layer (the nods stay). `--provider inworld` spends two paid requests a take.
`reports/content/2026-11-hero-head/cues/listen_clip.py <bench out> <out>` writes `clip.mp4`, `key.txt` (the parts,
Lobart's lines and the accents Vino heard, each nod with its time and the face that came with it or none, the apex of
`ls_w`) and `sheet.jpg` (before and apex of the faces); with `--off` only `clip-off.mp4`. The faces come from the
`[face] listen` lines of `game.log`, the nods from `ge_lnods` of `metrics.csv`, the accents heard from
`[ai] ... hears accent`.

## Native NPC calibration duels

### Как работает сейчас

`npcduel <attacker instance> <defender instance> <seed> [timeout_ms]` creates two fresh
script NPCs, 3 m apart and 12 m in front of the stationary immortal observer, as in
`bodyduel`. Only these two copies retain `AiNormal`, the near-hero contact rig and
character controller (respecting the physics flags), including swing magnetism, at any
distance from the observer via `NavDemos::fullSimulation` / `forceCharacterVirtual`.
Constructors keep their attributes, talents, tactics,
weapons and armour; only immortality is removed and its original value is recorded.
The attacker starts the same `ZS_ATTACK` / `AR_NONE` / `setNonlethalFight` path as
`npc_act fight{deed:true}`. The defender responds through native perceptions.
The command waits for the first `isDown()` or the timeout (default 180000 game ms),
writes one `kind: npc_duel` row in `events.jsonl`, then removes the copies and their
dropped items. `bodyduel` retains its separate mortal, normalized reference setup.

Each row contains `seed`, `duration_ms`, `winner`/`loser` instance names or `timeout:true`,
and `attacker`/`defender` objects. Those retain raw script `DAMAGE[8]`, `PROTECTION[8]`,
`DAMAGETYPE`, `HITCHANCE` (fist/1H/2H), all 22 talent skills/values, level, STR/DEX,
HP before/after/max, guild/tactic, melee weapon instance/two-handedness and original
immortality. `weapon_damage[8]`, `weapon_damage_total` and `weapon_damagetype` are the separate equipped-item
script fields: consumers can use the same inputs as `story_eval`, without a precombined
NPC/weapon damage array. `hits` counts this side's positive HP-loss combat callbacks;
`damage_dealt` sums actual lost HP (including the final clamp to 1), with no frame sampling.
`hp_one`, `unconscious` (`ZS_UNCONSCIOUS`) and `weapon_dropped` retain the terminal
body state. Drop detection requires a dynamic world item with the NPC's owner and
original weapon instance. An unarmed loser has `weapon_dropped:false`.

Each positive HP-loss hit is also stored in `hit_log`: `time_ms` since duel start,
`attacker`/`victim` instance names, `hp_before`/`hp_after`, `base_damage` from
`DamageCalculator::damageValue` (after its minimum of 5, before impact multipliers),
`critical` from the original damage roll, `melee_scale`, `stun_bonus` (1 or 1.5),
`backstab_mult` (1 or 3) and actual `hp_loss`, including the nonlethal HP clamp.
No damage formula is repeated in the bench. Each side records `swings` at native
attack-window start, `blocked` for completed parried/guarded swings and `misses`
for completed windows without a hit or block (including evades, walls and interrupted
swings). Counters use the selected hit path: physical outcomes or native legacy
results (`applied` is a hit), and both paths share the HP-loss hit logger. An active
window at duel end is counted as a swing, without inventing an outcome. `defender_first_swing_ms` is null until the defender starts its first swing;
the first HP-loss response is available separately from `hit_log`.

The [15 pairs](../../data/npc/far-fight-pairs.json) cover Lobart's equal peasants,
peasant/militia, militia/Onar mercenary, unarmed/armed, level disparity and 1H/2H.
All are ambient humans from save 1's world. The 12 v1 pairs are training, including
the three already used v1 holdouts. Three new pairs use six instances absent from v1:

| New holdout | Native weapons | Talent tiers |
|---|---|---|
| `SLD_818_SOELDNER` / `SLD_827_SOELDNER` | 2H axe / 2H axe | 1 / 1 |
| `BAU_901_BAUER` / `MIL_323_MILIZ` | 1H axe / 1H sword | 0 / 1 |
| `SLD_817_SOELDNER` / `MIL_326_MILIZ` | 1H axe / 1H sword | 1 / 1 |

Names, `type=0` (ambient), `guild < 16`, equipment and talent skills were checked
by executing the base's original `GOTHIC.DAT` constructors through ZenKit;
`STARTUP_NEWWORLD` inserts all six at `BIGFARM` or `NW_CITY_ENTRANCE_01`. Equal
peasants retain their different native mace/axe. No talents or equipment are normalized.

```bash
# Only the smoke suite runs here: 2 pairs × both roles × seeds 17,18 = 8 duels.
python3 scripts/far_fight_duels.py smoke
# Prepare the full party without running it: 15 × 2 × 10 = 300 duels, 120 s each.
python3 scripts/far_fight_duels.py plan --out .bench/far-fight-plan
# The lead runs this after merging:
python3 scripts/far_fight_duels.py run .bench/far-fight-plan --out .bench/far-fight-run
python3 scripts/far_fight_duels.py report .bench/far-fight-run/duels-* --out .bench/far-fight-summary
```

`plan` budgets each duel's full timeout plus one second, and 60 seconds per chunk;
by default at most 59 duels (120 s timeout) fit a two-hour batch request. `run` calls
`BENCH_HOST=ryzen scripts/bench.sh` per chunk, which builds there through `gq quick`
and launches through `gq batch` (`smoke`: `quick`). It reads the prepared base directly
from the cache or NAS via `OPENGOTHIC_BASE`. It uses absolute paths, including
base cache/store below `~/gothic-ai/far-fight/`. Bench's existing output is collected
back to the supplied output directory. `RYZEN_BENCH_ARCHIVE` gives the game request
an `--archive` source at `~/gothic-ai/far-fight/<output-name>/<chunk>`; before finishing,
it copies its result there. `gq` verifies and moves it to
`/mnt/shared/gothic-ai/far-fight/<output-name>/<chunk>`. Use a fresh output name per run.
The queue's `--archive` argument is the **local source**, not the NAS destination.
Full batches are never started by `plan`, `smoke` or the test suite.

`smoke` rejects missing/extra/duplicate duels, incomplete fields, and resolved losers
without 1 HP and `ZS_UNCONSCIOUS`. It requires the hit log and counters, while the reader still accepts v1 records
without them. A timeout remains a timeout. `report` writes
`far-fight.json` and `far-fight.md` and prints the table: n, attacker wins divided by
all duels (timeouts stay in the denominator), timeout count, median resolved winner
HP loss, median duration across all duels, and HP-loss hits per full duel second
pooled by weapon type, own talent tier and opponent tier. The same buckets include
swings, blocked swings and misses per second. Missing legacy counters are `n/a`,
with their exposure reported separately. By weapon type, `hit_multipliers` contains
the full empirical distribution of `hp_loss / base_damage` plus min/median/max,
stun and backstab fractions; terminal clamping is retained in that ratio. Response
tables give the median first defender swing and first HP-loss hit since duel start,
with observed/responding counts (no response is null, not zero). These are empirical
observations for the separate v2 model task; no coefficient fitting happens here.

The v1 fitting tool [`far_fight_fit.py`](../far_fight_fit.py),
[weapon table](../../data/npc/far-fight-weapons.json) and
[rejected calibration report](../../reports/Дальний%20бой%20NPC%20—%20калибровка.md)
are retained. `gamedata/far_fight.json` stays `uncalibrated`.

### План и журнал

- 2026-11-02: v2 bench keeps only its two copies in full simulation at any observer
  distance; records native hit factors and swing outcomes; adds three fresh holdouts
  and a 300-duel default plan. Full calibration is a separate task. The eight v1
  zero-hit replays on Mac before #751 produced hits in 3/8 duels and five zero-hit timeouts:
  unarmed attacker seeds 17–20 (281 swings total), 2H attacker seed 18 (29 swings).
  The attacker retained `AiNormal`. The replay's `run/melee-blade-shadow.jsonl`
  shows 245/281 unarmed swings using `T_FISTATTACKMOVE`. Across all 281 swings,
  median starting distance is 233 cm (112–316); the running punch reaches about
  58 cm. The target moves away only 8 cm per swing (median); 244 swings stop with
  `no_preview`.
  Legacy reports `applied` for 68/281, but physical HP-loss hits are zero. The
  unarmed initiator attacks from too far away while its target has not responded;
  the physical punch falls short and the magnet usually has no contact plan.
  This does not establish that the target is leaving on its routine.
  For 2H seed 18, 29 swings comprise 22 `S_2HATTACK`, four `T_2HATTACKL` and three
  `T_2HATTACKR`. Median starting distance is 201 cm (195–346), versus median reach
  125 cm (125–143); target retreat is 21 cm (14–80). All 29 swings have a preview
  and stop at `predicted`; median forward magnet feed is 11.7 cm. Legacy reports
  `applied` for 24/29, but there are no physical HP-loss hits; minimum blade/body
  clearance is 4.9 cm. Unlike the running punch, this magnet engages, yet the
  actual strike still falls short. The lead accepted the bench with these five
  empty duels; native reach, magnetism and AI attack choice belong to a separate
  melee fix. The defender's native behaviour stays unchanged.
  Before #751, the eight Mac smoke cases yielded 135 HP-loss hits; this is
  historical data, not validation of the #751 integration.
- 2026-11-02, after merging #751 (`54d65a79d`): replayed the same eight commands
  from `.bench/far-fight-v2-zero-hit-replay/duels-000.bench` on Ryzen, with absolute
  output `.bench/far-fight-v2-zero-hit-replay-ryzen-p751-20261102-141051` under the
  worktree. All eight records validated: **4/8 duels with hits, 28 HP-loss hits,
  four zero-hit timeouts and zero deaths**, versus **3/8, 12 hits and five zero-hit
  timeouts** on Mac before #751. All four 2H duels now have hits: seeds 17/18/19/20
  have 9/1/6/12 hits respectively; seed 18 no longer times out. The four unarmed
  initiator duels still have zero hits (70/79/66/72 swings). Host and code revision
  both differ, so this is a new measurement, not an isolated effect estimate for #751.
  All 352 shadow windows selected physical hits; their applied damage agrees with
  the 28 hit-log entries (2067 HP total). Results were archived through `gq`.
  A separate one-duel check with existing `NPC_CHARACTER_VIRTUAL=0` exercised the
  fallback: three legacy windows, two HP-loss hits (159 HP); the attacker has two
  swings, zero misses, and the defender one swing, one miss. This confirms that
  native `applied` is not counted as a miss and both hit paths use the same logger.
  The complete saved check then passed (exit 0): 71 server and 58 tool tests,
  Ruff, Linux and Mac `Gothic2Notr` builds, and CLI smoke on Ryzen. Smoke validated
  eight unique records with 100 HP-loss hits, one timeout and zero deaths; it keeps
  timeouts in the data rather than treating them as invalid records.
- 2026-11-01: added the native nonlethal calibration bench and plan/run/smoke/report
  tools for user decision Б3 in [Дальний бой NPC](../../reports/Дальний%20бой%20NPC.md).

## Live P30 capture on Mac

`scripts/body_live_capture.py` starts a local game with the production `GameServer` and
`BodyPilot`, using `[llm].body_url` from the usual launcher settings. It uses the existing
remote P30 service without starting or changing it. `LLM` must be `off`; `ASR` and `TTS` must be `fake`;
no clients for these models are created. Build the engine first. Each capture checks
`pgrep -fl Gothic2Notr` immediately before launch and refuses to share the Mac with another game.

```bash
LLM=off ASR=fake TTS=fake uv run --project server python scripts/body_live_capture.py \
  live-takeover --output "$HOME/.local/share/gothic-ai/explain/combat-ai/footage/live-takeover"
```

Run each scene separately into an empty absolute output directory: `live-takeover` (one
bandit and the hero answering with player attack input), `live-group` (three bandits), or
`live-passive` (one bandit, no hero attacks). These are native fights in the normal world;
the pilot must obtain the leases itself. The hero has immortality and 100000 HP; each bandit
has 1800 HP. Default duration is 25 seconds (`--duration` accepts 20–30). Capture uses Metal,
1920×1080 at 30 FPS, game sound at full volume, music and world ambience off
(`SOUND=1 MUSIC=0 WORLD_SOUNDS=0`), and a separate 48 kHz `shot.wav` beside `shot.mp4`.
The folder also contains the scene, engine events, frame metrics and `server.log`.
The readiness file releases scene preparation first; the pilot starts after the recording's
`rec` event, so the fixture's native `fight` commands cannot revoke an earlier lease.
A take counts only when it is finished: one `rec`/`rec_stop` pair for `shot` with
`duration × 30` frames and non-empty `shot.mp4` and `shot.wav`. A game that disconnects
after that is fine; an earlier disconnect fails the capture even with a zero exit code.

Export labels after the pilot has finished and written its `neural-body end` line:

```bash
uv run --project body-service python scripts/body_live_film.py labels /absolute/take \
  --npc DIR_NPC_LIVE_BANDIT_1 --min-decisions 10 --output /absolute/take/actions.json
```

For `live-group`, export each `DIR_NPC_LIVE_BANDIT_1`, `_2`, `_3` to a separate
`actions-<npc>.json`, each with `--min-decisions 10`. For `live-passive`, use
`--min-decisions 5 --consecutive-wait 5` to require five consecutive `wait_200` choices.
The JSON schema matches replay labels: `fps`, `frames`, `size`, `track`, `hp`, `actions`.
Head coordinates are in captured pixels (`null` when outside the frame); HP is sampled per
frame. Each action has `frame`, `t`, `action`, `id`, mapped to the engine's capture frame;
`t` is that frame's video timestamp.

Live labels describe accepted P30 choices, including `continue` and commands cancelled
before execution. They do not assert that an animation started; replay labels use `started`
instead. The exporter requires one lease per NPC, complete per-frame samples, and exactly
one end summary per NPC. It reconciles all accepted choices in the session with the pilot's
`decisions=` count, then exports only choices inside `shot`. It prints both counts and the
longest consecutive `wait_200` run. Keep recordings, JSON and preview PNGs outside git.

For the Bullet → Jolt stage 1 oracle, `physics dump [count=100000] [seed=73013] [input.tsv]`
casts that many rays **and** spheres through `DynamicWorld` in the loaded world. It writes
`physics-queries.tsv` (centimeters: from xyz, to xyz, radius) and `physics-results.jsonl` (hit,
point, normal, material, sector, fraction). Half the generated queries go vertically down near
waypoints; the rest go obliquely nearby or between waypoints all over the world. Replay the exact
Bullet input file with Jolt: waypoint ground adjustment itself can differ between backends.
The log reports p50/p95 of warmed batches of 256 queries, excluding result/file writes.
`scripts/physics_compare.py <bullet-output> <jolt-output> --output <comparison-dir>` checks
the identical inputs and reports mismatches. `scripts/physics_suite.py --help` runs the same
eleven stage 1 scenarios without rendering. Temporary frame timing instrumentation belongs only
in isolated source copies (`scripts/physics_profile.py`); its output is not a renderer benchmark.
Frame timing sums public physics work across worker threads, so it can exceed elapsed frame time.
For a hotspot diagnostic, `--method-breakdown` additionally writes per-method inclusive and outer
work to `OPENGOTHIC_PHYSICS_TRACE_METHODS`. Keep this extra instrumentation out of formal timing runs.

`body-impact.bench` checks body impact damage: knockdown falls from 2/5/10 m
require actual torso contact speed within 10% of `sqrt(2gh)`. Ragdoll and
animated-fall HP loss of the same guild are recorded as references, without an
HP parity threshold; ragdoll damping stays 0.12. It also checks knockdown-ragdoll
wall impacts at 6/10/14 m/s, a real hero WindFist into a wall through the animated
capsule path, and unconscious, dead and immortal controls. Ragdolls count only
pelvis, spine and head contacts with landscape, static objects or movable furniture. Spell
capsule wall impacts and their existing landing damage share one 300 ms maximum
speed window per NPC. Damage uses the unchanged guild fall formula; fatal
impacts retain the attacker or caster, including the hero's kill attribution.
Two NPC-culprit controls use the same 14 m/s wall impact: a nonlethal pair must
enter native unconsciousness at 1 HP, drop its weapon, notify a witness of defeat,
leave no murder or death trace, and recover through `AI_StandUp`; a lethal pair
must die with murder attribution and a death trace. `body-impact standup` runs
the unconscious End hook and queues native standup; `body-impact recovered`
checks the completed handoff. With damage disabled it checks posture recovery.

Run `scripts/bench.sh body-impact <absolute-output-directory>` and repeat with
`BODY_IMPACT_DAMAGE=0`; the disabled run retains the previous animated landing
behavior and adds no body impact damage. `[GAME] body_impact_damage` defaults to
1; Marvin `toggle body_impact_damage` changes it during play. The native contract
is `gothic-physics-contract`. Acceptance numbers and the ON/OFF duel and Mac
physics timing comparison belong in the [body impact report](../../reports/physics/2026-11-01-body-impact/README.md).

`body-traces.bench` checks bodies as physical traces (stage 8, step 4b) on a fresh
`WORLD=NEWWORLD.ZEN` in the Xardas valley. The native fixture `body_traces`
(`common/game/bodytracesbench.cpp`) spawns and kills, by the hero's hand, a person,
a wolf and three people the lifetime rule keeps (an `ITM_MISSION` key, `IMMORTAL`,
`NPCTYPE_MAIN`), and parks a living observer with perceptions off. `view` reads the
production `npc_view` through the bridge's writer (`TestBench::npcView`): the person
is a `body` trace with `npc`, `age` and a stable id, the wolf is none; OFF there are
no body rows. `advance <minutes>` moves the clock alone; `sleep <hours>` sleeps like
a bed (`World::setDayTime`); `hero near|far-visible|far-hidden` places the hero
within 30 m, beyond 30 m with an open ray from his eyes to a body sight point, or
beyond 30 m with none; `check <label>` fails the run on the first unmet rule.
`save created` writes `session-created.sav`. A complete run ends with the
`check sleep-far` sample: `scripts/bench.sh` does not fail a game killed by `TIMEOUT`.

```bash
WORLD=NEWWORLD.ZEN scripts/bench.sh body-traces /abs/out/on
WORLD=NEWWORLD.ZEN BODY_TRACES=0 scripts/bench.sh body-traces /abs/out/off
SAVE=/abs/out/on/session-created.sav scripts/bench.sh body-traces-reload /abs/out/reload
python3 scripts/body_traces_compare.py /abs/out/on /abs/out/reload
```

`body-traces-legacy-make.bench` uses only generic commands: run it with a build of
main from before body traces to get `session-legacy.sav` (a dead person and a wolf);
`SAVE=<that>/session-legacy.sav scripts/bench.sh body-traces-legacy` loads it with
the current build: old bodies get an id, `age: null`, and a lifetime from the load.

`blood-stains.bench` checks blood stains as records and `blood` traces (stage 8, step 4c)
on a fresh `WORLD=NEWWORLD.ZEN` in the Xardas valley. The native fixture `blood_stains`
(`common/game/bloodstainsbench.cpp`) kills a person 7 m ahead of the hero and lays his
pool with a splash beside it, two splashes 1.2 m apart, one 9 m away, a meatbug's yellow
stain with a golem's stone chips 1 m from it, and a splash recorded indoors. `check
<label>` reads `World::physicsTraces` and fails the run on the first unmet rule: the pair
is one trace named by its older stain, placed at its larger and aged by its fresher; the
body's blood and the stone chips are no trace; `body gone` removes the body, and its pool
leaves with it while the splash beside it becomes a trace; `advance <minutes>` moves the
clock alone: after a game day only the indoor stain is left, after three days none. OFF
(`BLOOD=0`) there are no stains and no traces. `save created` writes `session-created.sav`;
`blood-stains-reload.bench` loads it in a fresh process, and
`scripts/blood_stains_compare.py` checks the same ids, positions, birth times, trace ids
and an age that continues from the saved clock. Marvin `blood <waypoint> [pool|splash]
[radius_cm]` lays a stain by hand; `[GAME] blood` defaults to 1, Marvin `toggle blood`
changes it during play.

```bash
WORLD=NEWWORLD.ZEN scripts/bench.sh blood-stains /abs/out/on
WORLD=NEWWORLD.ZEN BLOOD=0 scripts/bench.sh blood-stains /abs/out/off
SAVE=/abs/out/on/session-created.sav scripts/bench.sh blood-stains-reload /abs/out/reload
python3 scripts/blood_stains_compare.py /abs/out/on /abs/out/reload
```

How blood looks: `blood-probe.bench` lays a pool by hand on Onar's field at 06:00 and shoots it at
noon and at 18:00 (does a decal draw, and how it dries). `blood-look.bench` lays a pool and a splash on
the field, then a paladin kills a bandit there and on the harbour pier: a clip of each fight, a clip of
the pool spreading (`cam follow` keeps the dead bandit by pointer; `cam two`/`cam face` look for a
living npc of that name and lose him), stills 5 min, 1 h, 6 h and 10 h later in daylight. A `shot`
is written at the end of its frame, so each one waits before the clock moves. `blood-cost.bench` lays
256 stains (`blood_stains fill 256 125`, a 20 m grid round the hero) and measures 30 s of normal app
cadence (`blood_stains measure` starts the window); it needs a build with
`OPENGOTHIC_ENABLE_APP_CADENCE=ON`, and `OPENGOTHIC_GPU_PASSES=1` on a frame-profile build gives the
"Blood decals" pass alone.

```bash
WORLD=NEWWORLD.ZEN BENCH_HOST=local scripts/bench.sh blood-look /abs/out/look
GAME_BIN=OpenGothic/build-profile-on/opengothic/Gothic2Notr BLOOD=1 WORLD=NEWWORLD.ZEN BENCH_HOST=local scripts/bench.sh blood-cost /abs/out/on
GAME_BIN=OpenGothic/build-profile-on/opengothic/Gothic2Notr BLOOD=0 WORLD=NEWWORLD.ZEN BENCH_HOST=local scripts/bench.sh blood-cost /abs/out/off
python3 scripts/blood_cost_read.py /abs/out/on -- /abs/out/off
```

For stage 2, `ragdoll.bench` exercises death, sleeping/wake, controlled unconscious recovery,
slopes, stairs, a cliff and the human/Wolf/Scavenger/Molerat/Orc models. Run it with
`OPENGOTHIC_RAGDOLL_TRACE=1` to append `ph_ms,ph_bodies,ph_rays,rd_mode,rd_weight,rd_sleep`
to the existing metric columns and write every watched actor's final world bone positions and
native part state to `events.jsonl`. Without this flag the established CSV schema stays unchanged.
`scripts/ragdoll_compare.py --help` describes the required segment, archive and removal controls;
missing coverage and command errors fail the comparison. `--toggle before_label:after_label` compares
the full sleeping state before a 600 ms feature-off interval with the automatic on restoration,
before the fixture manually restores any NPC archive.

`immortal-return.bench` covers quest NPC protection: real native KO falls from a cliff and into
water, NPC save/load while the return is pending, and normal AI get-up at the last dry support.
The mortal and feature-off controls get up at their fallen positions. The feature defaults on;
disable it with `[GAME] ragdoll_quest_guard=0` or Marvin `toggle ragdoll_quest_guard`.
Run with `OPENGOTHIC_RAGDOLL_TRACE=1`, then check the output with
`python3 scripts/immortal_return_compare.py <output-directory> --output <result.json>`.
The `ig_*` columns record eligibility, anchor, latched hazards and actual returns. `ph_ms` records
the native physics step; observer wall throughput is informative and is not ordinary-game FPS.

| Command | |
|---|---|
| `ragdoll on\|off` | Set the same `GAME/ragdoll` switch as the Marvin toggle. Enabled by default; set `[GAME] ragdoll=0` or use Marvin `toggle ragdoll` to disable it. |
| `down <npc> dead\|ko` | Invoke the normal NPC down lifecycle and prepare its body. |
| `push <npc> <x> <y> <z> <impulse>` | Apply an impulse in Newton seconds along a finite nonzero direction. Requires a physical body. |
| `waitragdoll <npc> sleep\|recovery\|getup [timeout ms]` | Wait for the actual body/animation state; a bounded timeout is an explicit command error. |
| `standup <npc>` | End the controlled unconscious state and queue the normal AI stand-up action. The test controls the state end; this does not measure the natural script loop's timing. |
| `ragdoll queued-standup <npc> <wait_ms>` | Retain the unconscious script state and enqueue normal `AI_Wait` followed by `AI_StandUp`. The separate midair fixture checks the observed deadline and handoff before the normal state-end cleanup. |
| `ragdoll save\|restore <npc> <label> [old]` | Save/load the actual NPC through `Serialize` in the same world and tick. `old` removes just the optional body/recovery entries. This is an NPC archive control, not a complete game reload. |
| `ragdoll session-save <npc> <label>`, `ragdoll session-snapshot <npc> <label>` | Write a complete `GameSession` archive plus a full corpse snapshot, or the snapshot only. Files are `session-<label>.sav`/`.json`. |
| `ragdoll session-initial <npc> <label>` | As the first scenario command only, snapshot the loaded actor at `dt=0` before any positive physics step. A late/positive-dt invocation fails. |
| `ragdoll loot <npc>` | Require normal player focus to select that exact corpse, then open its real ransack inventory. Trace records the selected and opened NPC ids; the test closes the menu immediately and transfers no items. |
| `ragdoll remove <npc>` | Remove that NPC through the world and verify that all its native parts disappear. |
| `physics stats` | Report actual physics step time, native body count and query count. |

An NPC selector can be `watched`, `player`, `nearest`, a script instance symbol, or `id:<uint32>`.
`watched` keeps the selected corpse when duplicate symbols exist; `id:` selects an exact persisted
world NPC slot, including a dead actor. A fresh process has no watched subject until one is selected.

`scripts/ragdoll_session_probe.py --help` runs the fixed sleeping-human full save and fresh load in
two processes from the normal built executable path, preserving an immutable copy and checking
the executable hash before and after each process. It enables `GAME/ragdoll=1` in an isolated INI before each
session is constructed, verifies the initial archive NPC id and saved clock at `dt=0`, then checks
one second of ordinary sleeping continuation. `scripts/ragdoll_session_compare.py` requires the full
game/world/NPC ZIP entries and compares every final bone, native frame, constant affine reference,
velocity, sleep flag, ancestor topology and inventory item counts. Native COM, quaternion and velocity
float values must match exactly. Bounds and scope are recorded in its result; it does not imply successful
world transitions, mid-return saves, old full-save fallback or inventory interaction.

The asset-free native contract also has `--replay-creation captured.jsonl`: it reconstructs the
actual constructor's captured hierarchy, checks every bind matrix float, then replays the exact
world pose/object/velocity. Full proprietary model hierarchies stay in ignored local evidence;
the maintained test does not embed asset geometry. It checks cold native-world reconstruction of
moving and sleeping actors, explicit teleports and the actual Human knee/elbow limits under yaw.
State 3 retains native COM/quaternion/velocity float values and immutable initial affine references,
preserving positive nonuniform NPC scale and carrying nonphysical ancestor bones with a native child.

The stage-2 game FPS check uses `ragdoll20-natural-{off,on}.bench` with
`OPENGOTHIC_ENABLE_APP_CADENCE=ON` and `OPENGOTHIC_ENABLE_FRAME_PROFILE=OFF`.
Twenty NPCs die together; the next 30 real seconds include body creation, falling and natural sleep.
The ordinary application clock and render loop are used. There is no forced-awake mode or warm-up
after death. Run three OFF/ON pairs and use `scripts/ragdoll_cadence_read.py` for the median results.
The median mean FPS may fall by at most 5% relative to OFF; report both ON and OFF median p95 frame intervals for the same scene.
Check `pgrep Gothic2Notr` before starting so another game does not overlap the measurement.

The older `ragdoll20-{disabled,active,sleeping}.bench` scenarios are supplementary stress tools.
They use the same 20 actors and camera, with an accelerated fixed simulation clock.
Run with `OPENGOTHIC_RAGDOLL_TRACE=0 OPENGOTHIC_RAGDOLL_PERF=1`: lightweight counters and actual
monotonic full-cycle/native-step times are buffered, then written to `ragdoll-performance.csv` at
the end. Full pose JSON and ordinary per-frame metric writes are disabled during this measurement.
`scripts/ragdoll_performance.py --help` checks every frame's actor/visibility/awake coverage and
the declared 120 warm-up plus 900 measured frames. Fixed simulation `dt` is not an FPS measurement.
Each physical actor must have the complete 24-part Human schema, matching its peers. The pre-record
bulk-down event proves all 20 individual schemas; every warm-up and measured row must retain exactly
480 parts (zero in the disabled control). Aggregate counts alone do not establish complete bodies.
`ragdoll-queued-ai.bench` and `scripts/ragdoll_queue_compare.py` separately check a knockout after an
ordinary airborne tick, native settling, the full observed `AI_Wait` interval and normal wounded-to-stand
recovery. This controlled queue test preserves the main acceptance fixture.
The current stage-2 result is in
[`validation-notes.md`](../../reports/physics/2026-10-14-jolt-stage2/validation-notes.md).

Stage 4 movable vobs use the real Coragon tavern in a Khorinis save. The fixture finds
Coragon and the nearest original map table, adds six real beer-mug meshes on its tabletop,
a floor mug on a supported walking lane, and a real barrel only if none is nearby. The
original tavern population keeps its routines during the untouched 60 simulation seconds.
The seven mugs are explicit test bodies created through `addMovableVob`; they are not
authored map props and do not add unused item-visual rules to the stock-world allowlist.
The setup records map/fixture counts separately, plus actual map `cd_dynamic=0` bodies by class.
Native NPC contacts, including their propagation through other movable bodies, mark the
items excluded from spontaneous drift. After that observation actors stop for a controlled
walking test and an actual equipped instant fireball (`spellId=23`). No fixture command
forces a dynamic body asleep. The 15-second fireball checkpoint requires all fixture vobs
to be sleeping, at least five displaced over 20 cm, including a table or barrel.

Configure the build with `-DOPENGOTHIC_ENABLE_APP_CADENCE=ON`. Run one game at a time,
checking `pgrep -fl Gothic2Notr` before each launch. A complete example (from the repository):

```sh
MOVABLE_VOBS=1 NO_BUILD=1 SAVE="$PWD/.bench/movable-input.sav" scripts/bench.sh movable-vobs .bench/movable-functional
python3 scripts/movable_vobs_compare.py functional .bench/movable-functional --output .bench/movable-functional/result.json
MOVABLE_VOBS=1 NO_BUILD=1 SAVE="$PWD/.bench/movable-functional/session-scattered.sav" scripts/bench.sh movable-vobs-reload .bench/movable-reload
python3 scripts/movable_vobs_compare.py reload .bench/movable-reload --saved .bench/movable-functional --output .bench/movable-reload/result.json
MOVABLE_VOBS=0 NO_BUILD=1 SAVE="$PWD/.bench/movable-input.sav" scripts/bench.sh movable-vobs-off .bench/movable-performance/off
MOVABLE_VOBS=1 NO_BUILD=1 SAVE="$PWD/.bench/movable-input.sav" scripts/bench.sh movable-vobs-on .bench/movable-performance/on
python3 scripts/movable_vobs_compare.py performance .bench/movable-performance --output .bench/movable-performance/result.json
```

`movable-census.bench` checks every movable vob of a map against the level, on a fresh world
(`WORLD` starts a new game in that world instead of loading a save). `movable clamps [margin cm]`
writes `movable-clamps.jsonl`: for each movable hull at its ZEN pose, every static or movable body
it touches or penetrates, with the simulation's own contact settings, grouped by body and side
(0 below, 1 above, 2..5 = +x, -x, +z, -z towards the other body). `movable wake` wakes every
movable body without an impulse; `movable drift` writes how far each moved (`movable-drift.jsonl`).

```sh
for w in NEWWORLD OLDWORLD ADDONWORLD DRAGONISLAND; do
  WORLD=$w.ZEN MOVABLE_VOBS=1 NO_BUILD=1 scripts/bench.sh movable-census .bench/movable-census-$w
done
```

`MOVABLE_VOBS` writes the initial `[GAME] movable_vobs` setting before constructing the world;
toggling after load cannot measure disabled world-loading cost. Each performance run records
30 seconds of ordinary app-frame completion cadence, with the real fireball at the start,
plus buffered native physics-step timings. Per-frame vob snapshots and recording are disabled
in the measured window. The reader requires p95 physics ≤2 ms and mean FPS loss ≤5%, and
reports world-constructor duration beside both modes (GameSession deserialization is excluded).
The functional run starts from the supplied old save, writes a complete `GameSession` archive
with scattered original furniture, and the separate fresh process must load those exact bytes
and continue for a second. Authored map vobs may return to their initial transforms; temporary
fixture mugs are outside the map archive. Moved-vob persistence belongs to stage 4, point 2.

`own-mesh-physics.bench` compares collisions of original content and the procedural
barrel from `scripts/meshes/make_test_barrel.py <empty directory> --radius-scale K` (default 1).
K changes X/Z about the barrel axis, preserving Y and origin. Use a fresh `NEWWORLD.ZEN`,
absolute content/output paths, and `MOVABLE_VOBS=0` / `1` before loading; the scenario keeps
destruction off. `movable-census` and `own-mesh-*` skip the new-game intro.

The fixture is authored ZEN id 24244 at `(10248.6309, 326.437866, 2359.78076)` cm.
With K=1.5 it remains enabled/`ok`; the nearest wall is 51.081 cm away.
The accepted fixture uses free directions: the hero and arrow approach from +X, and the hero
pushes along -X, away from the wall. Nine 200 cm segments check both Jolt and visible geometry:
approach and push at four heights, plus the half-height arrow path. The free-side camera
at `(10137.3, 485.94, 2775.03)` cm frames the whole barrel. Nearby geometry stays in place;
the fixture does not require a clear metre in every direction.

Run each of the three content variants (no mesh index, K=1, K=1.5) with both mobility values
through the standard launcher only:

```bash
BENCH_HOST=ryzen WORLD=NEWWORLD.ZEN MOVABLE_VOBS=0 CHARACTER_VIRTUAL=1 \
  OPENGOTHIC_CONTENT=/absolute/temporary/content \
  scripts/bench.sh own-mesh-physics /absolute/output
```

Use an empty temporary content directory for the original variant, and generated directories
for K=1 and K=1.5; repeat with `MOVABLE_VOBS=1`. The launcher stages game runs under
`~/gothic-ai/e2e/<worktree>` and obtains the `gq` window slot itself. Separate test builds and
temporary preparation files use `~/gothic-ai/own-meshes/`. Do not call the remote launcher directly or
start an extra game outside `bench.sh`. The working content directory is not used by this stand.

The narrow `own-mesh setup <zen id> <approach x> <approach z>` command selects an existing
barrel, checks the ground along the approach, and places only the hero/camera. `own-mesh arrow`
launches a real native arrow toward half-height; `own_mesh_arrow_hit` records the actual
collision point, ZEN id, radial distance, and height error. `own-mesh walk` resets only the
hero to the straight approach. While forward is held, yaw steering corrects cross-track error;
only the native character controller supplies positions and velocities. `own-mesh checkpoint`
records the maximum cross-track deviation, hero stop radius and final
500 ms speed while forward is held, body displacement/NPC contact, hull volume in m³,
mass, material density, class multiplier, and disposition. In ON mode the hero radius is
relative to the original axis; use body displacement for the push. Native clearance contacts
are diagnostic and available only in ON mode; the directional rays validate the fixture.
The authored barrel is never inserted or teleported.

The original barrel reports UNDEF / 2000 kg/m³ and the test barrel WOOD / 700 kg/m³;
this explains their different masses at the same bbox. K=1.5/K=1 hull volume is 2.25.
The report contains all direct snapshot fields, per-instance census changes, stop/hit/push
measurements, and three screenshots. Seven mobility changes follow the new hull profile;
the visual-name CSV rules and fracture protection are preserved. Mesh/hull caches are in
memory, and the disk navmesh key already includes the selected geometry.
[Results](../../reports/content/2026-10-own-meshes-physics/README.md).

Film mode (what `gothic-shorts film` sets): `OPENGOTHIC_BENCH_REALTIME=1` keeps the fixed steps to the wall
clock and draws only the captured frame; `TEMPEST_AUDIO_LOOPBACK=48000` records the game's sound into
`<segment>.wav` instead of playing it; `TEMPEST_WINDOW_SIZE=<w>x<h>` (points) sets the frame;
`OPENGOTHIC_BENCH_ENCODER` replaces the x264 options (e.g. `-c:v h264_videotoolbox -b:v 24M`).

Recording in real time on Ryzen (clips that judge loading and streaming at the player's pace):
`OPENGOTHIC_BENCH_REALTIME=1 TEMPEST_WINDOW_SIZE=1920x1080 "OPENGOTHIC_BENCH_ENCODER=-c:v h264_nvenc -preset p4 -b:v 24M"
BENCH_HOST=ryzen scripts/bench.sh <scenario> <out>`. Read `[bench] rec stop … speed=` and `[bench] realtime:`
in `game.log`: a take that kept pace says `speed=` 95 % or more and `kept to the wall clock` (or a lag under
1000 ms). A 127 s city run on the x4 texture set gave 99 % and a 0.6–0.7 s lag three times in a row with
NVENC, 96 % and 4 s with the default x264. A frame of the capture costs about 8 ms (the picture 4.5 ms,
reading it back 1.7 ms, the bench 1 ms) out of the step's 33 ms; filming draws every frame into one image
and reads it through one cached staging buffer. [Measurements](../../reports/engine/2026-11-11-realtime-capture/README.md).

Good spots near the default save (Xardas' tower): `NW_XARDAS_TOWER_VALLEY_05` (27 degree slope in a
forest), `NW_XARDAS_TOWER_IN1_*` (ramp inside the tower), `NW_XARDAS_STAIRS_01`..`_04`.

## NPC day parity

`npc-day-parity` (world streaming, step 1, item 6) runs one continuous game day from a fresh `NEWWORLD`
(day 0, 08:00) and journals every NPC every 5 game minutes. No `set time`: it calls
`resetPositionToTA` and teleports everyone. The hero stands, immortal, on open ground below Xardas'
tower (the valley's rats and bloodflies attack him all day). Start it once per NPC simulation tier and
compare two runs:

```sh
OPENGOTHIC_NPC_SIM_TIER=full BENCH_HOST=dev-ubuntu \
  scripts/bench.sh npc-day-parity /absolute/out/full-1
OPENGOTHIC_NPC_SIM_TIER=simplified BENCH_HOST=dev-ubuntu \
  scripts/bench.sh npc-day-parity /absolute/out/simplified-1
scripts/npc_day_parity.py /absolute/out/full-1 /absolute/out/simplified-1 --label-a full --label-b simplified \
  --tolerances scripts/bench/npc-day-parity.tolerances-6c.json --out /absolute/out/report.md --json /absolute/out/report.json
```

The scenario carries `# @headless`: it defaults to `WORLD=NEWWORLD.ZEN`, `TIMEOUT=3600` and
`RYZEN_MAX=75m`, with a `batch` request and no window or GPU reservation. `--game-parity` supplies
the AI bridge and the window's 40 m effect radius around the camera, and disables body traces by
default. With `BENCH_HOST=auto`, the host order is dev-ubuntu → Ryzen → Mac; pin
`BENCH_HOST=dev-ubuntu` to keep a comparison there. `DEV_UBUNTU_HOST` changes its SSH alias.
`BENCH_HEADLESS=0` restores the window for a same-build comparison on Ryzen or Mac;
`BENCH_HOST=dev-ubuntu` then exits with code 2 because that host has no window.

The [headless measurements](../../reports/compute/2026-11-02-headless-bench/README.md) on Ryzen
show that a day through `scripts/bench.sh npc-day-parity` took 1553 s (25,9 min) without a window,
as a `batch` request (`gq`: `4t 6G gpu=0G окно=0`); windowed days took 38–43 min.
The window and `--game-parity` runner days from the same build and environment passed
the historical numeric bench tolerances (exit 0): position differences 0 m,
ZS/TA/HP/inventory matches 100%, and `npc-day-events.csv` byte-identical. Only `queue`/`head`
differ for Lester (`PC_PSIONIC`) when he approaches the hero to talk: `AI_ProcessInfo` is handled
by the dialogue menu in the window; the runner has no menu. The current exact-journal control
also checks those fields, so window/runner comparisons need to report this difference explicitly.
Compare only runs from the same build,
base and environment. Days of one build are byte-identical on a host and between dev-ubuntu and Ryzen, so a difference is a real change, not noise; nothing in the simulation may depend on heap addresses ([why](../../docs/world-streaming.md#почему-сутки-перестали-повторяться-фаза-lod-анимации-2026-11-07)). A day writes about 55 MB.
With `OPENGOTHIC_NPC_LOGIC_AUDIT=1`, `npc-day-heights.csv` compares sampled Simplified positions with a ground ray; movement itself does not cast these verification rays. The comparer splits position errors into `stationary_both` and `moving_either` from the existing body-state column, and accepts final-HP, matched-death and stationary-p95 guards.

Use `npc-day-parity.tolerances.json` for Full and `OPENGOTHIC_NPC_SIMPLE_MOVE=0` controls against
the current-main noise floor. It requires identical nonhero sample fields except `tier` and all
nonhero events, including the final post-sample tick. Active Simplified/auto use
`npc-day-parity.tolerances-6c.json`: TA ≥99.5%, ZS and standard start coverage ≥99%, stationary
position p95 ≤1 m and mutual-nearest start shift p95 ≤8 game seconds. Deaths, final HP and NPC
presence remain strict. At most three NPCs may have different final inventory, and every case
needs an exact attested script-created/random-item hash pair; unexplained differences fail.
Moving positions and standard greedy offsets are report-only. The
[acceptance decision and measurements](../../docs/world-streaming.md#шаг-1-пункт-6в--графовое-движение-и-повороты)
explain the bounds and remaining combat/body work. `npcday` files:

| File | Contents |
|---|---|
| `npc-day.json` | Tier mode, seed, private random streams, period, start time and hero; at the end the sample/event counts, wall time and the NPC tick time per tier |
| `npc-day.csv` | Per NPC and sample: `t_min` (scheduled, minutes since the start), key (`SYMBOL#ordinal` by start position), alive, HP, routine entry in force (`index hh:mm-hh:mm ZS waypoint`), ZS state and its time, position (cm), nearest waypoint and distance, AI queue size and head, body state, inventory hash and item count, mobsi scheme, tier, policy, hero flag |
| `npc-day-events.csv` | Polled every world tick: `zs_start`/`zs_end`, `ta` (routine entry change), `death`/`revive`, `insert`/`remove`, with game minutes and position |
| `npc-day-cost.csv` | Per sample: wall seconds, NPCs within 30/60 m of the hero, world ticks, NPCs per tier, tier transitions, `Npc::tick` milliseconds and NPC-tick counts per tier, plus Simplified combat ticks using the native executor since the previous sample |
| `npc-day-arrivals.csv` | Audit-only exact terminal arrivals, observed movement start, final goal identity, actor/target position, ZS, queue and executor; it adds no records to the strict samples/events files. |

The society stand's core is checked against the same journal: `scripts/society_parity.py` compares its marks
with `npc-day.csv` and the `ta` events of `npc-day-events.csv` (people only; [how](../../docs/society-sim.md#проверка-самого-стенда)).

Every NPC gets a private script random stream (`GameScript::isolateRandom`, seed from `seed`, stream from
the key), so one NPC's extra `Hlp_Random` or dice roll does not shift every later NPC's numbers; `shared`
keeps the game's single stream. The journal drops exactly the streams it registered when it finishes or
stops, so commands after `npcday` use the shared stream again. Do not combine `npcday` with the body arena
in one scenario: the arena isolates its actors and clears all streams on reset. `std::rand` (sound variants, gestures, parry variant) is not reseeded.
States that start and end within one world tick are not seen by the event poll.

`scripts/npc_day_parity.py` matches samples by time and key and leaves the hero out, with his events
(`--min-hero-distance <m>` also leaves out NPCs near him). Completeness gates come first, because the metrics
only see the samples both runs share: `require_done` (both `end_reason` are `done`), `require_full_length`
(both reach their requested minutes and end at the same time), `max_only_samples` (samples present in one run
only) and `max_missing_end` (an NPC at the end of one run and not the other; such an NPC also counts for the
end-of-run alive and inventory checks). It reports the share of NPC-samples in the same
ZS state and routine entry, HP and inventory, the position error (p50/p95/max, straight line, metres),
matched ZS starts and their offset (greedy, same state, `--window` game minutes), deaths, alive and
inventory differences at the end, the same split by run B's tier and by game hour, and the NPCs that
diverge most. Tolerances come from `--tolerances` (JSON) or `--min-same-zs` and similar options
(`--require-done false` turns a gate off); a failed check exits with 1. Twin monsters that spawn at the same point keep their order of the start array.

## Engine side

### Placed world Items (physics stage 8, step 5)

`placed-items-census` checks the immutable authored-geometry census and wakes
every admitted Item for five seconds, in 12 m sections. Each section advances
at least 300 real fixed native steps synchronously, sampling after every tick;
rendering and game-clock time do not pace the physical interval. All admitted
neighbouring supports within 22 m are installed asleep before the section wakes.
It never captures images.
Run all four fresh worlds through Ryzen's queue:

```bash
WORLD=NEWWORLD.ZEN PLACED_ITEMS=1 RYZEN_CLEAN_IDLE_HOURS=0 \
  RYZEN_LOCK_WAIT=1200 TIMEOUT=3600 RYZEN_MAX=75m \
  scripts/bench.sh placed-items-census "$PWD/.bench/placed-items-NEWWORLD"
# Repeat with OLDWORLD.ZEN, ADDONWORLD.ZEN and DRAGONISLAND.ZEN.
python3 scripts/placed_items_report.py \
  .bench/placed-items-{NEWWORLD,OLDWORLD,ADDONWORLD,DRAGONISLAND} \
  --known-exceptions reports/physics/2026-10-31-placed-items/known-exceptions.csv
```

`placed-items-census.jsonl` records each ZEN-ID, original pose, instance, visual,
exclusive exclusion reason, support gap and penetration depths. The census event
adds classification time, admitted gap buckets (0–1, 1–2, 2–4 cm) and the densest
20 m neighbourhood among all source Items (ties prefer more admitted Items).
After the original geometric reasons, `rooted` excludes `ItPl_*` on landscape or
`C_Object`; `slope` excludes static supports steeper than 15°. Its normal comes
from a downward ray at the collision shape's lowest point, rather than the
nearest overlap contact. `irregular` rejects static support when this ray misses
a face; it follows `slope`. `no_ground` then rejects Items on static objects or
movable furniture when a downward COM ray, excluding that support, finds no
landscape or other static object within 5 m below the hull bottom. Penetration
into static geometry, furniture or another Item is limited to 2 cm; the support
gap remains 4 cm. `on_excluded` propagates rejected Item supports through
whole stacks. Plants on furniture or admitted Items use the ordinary rules.
`placed-items-probe.jsonl` records maximum and final travel
for every admitted Item. The reader requires at least 95% of the combined sample
to move less than 5 cm, maximum travel at most 100 cm, and zero `item recovered`
messages. Every displacement over 10 cm remains in the output with its original
contact measurements. These limits are fixed; a failed 95% gate requires a decision.
For linked recoveries the reader also measures the logged pre-reset pose, so a
reset inside a native tick cannot hide an excursion from the regular samples.
Pass the previous four run directories with `--previous-runs` to include the
new facet-slope distributions of their >10 cm outliers, alongside admitted Items,
in 5° bins. A missing facet normal is counted separately. Recovery messages name
the ZEN-ID whenever the recovered Item has an authored link.
`--known-exceptions` reads `world,zen_id,reason` CSV rows and prints those Items
separately. They still count toward the 95% fraction, but are omitted from the
maximum-travel and recovery gates. Raw maximum/recovery totals remain visible
beside `gate_max_shift_cm` and `gate_recovered`. The agreed file contains exactly
NEWWORLD 18261 (the rolling flask); every other excess still fails.

`placed-items diagnose <ZEN-ID> [control]` advances that Item's original 12 m
section for 300 native steps and writes `placed-items-diagnose.jsonl`. Each step
records center, velocity, nearby native contacts, triangle indices, normals and
edge lengths, plus the recovery ray result. `solver_contacts` records the real
`OnContactAdded`/`Persisted` manifold separately from geometric snapshots. `control` removes other Items and
movable furniture while retaining landscape and static objects. The command
requires an authored Item and uses `tick(16)` to prevent batched native steps.
For an excluded Item it records the verdict and verifies that neither wake nor
activation can create a body, without attempting to simulate that Item.

`placed-items-diagnose-1644` / `placed-items-diagnose-1644-control` use
`WORLD=OLDWORLD.ZEN`; `placed-items-diagnose-6863` /
`placed-items-diagnose-6863-control` use `WORLD=ADDONWORLD.ZEN`. Set
`PLACED_ITEMS=1 ITEM_DEEPEST_CONTACT=1` for the fixed manifold or
`ITEM_DEEPEST_CONTACT=0` for the upstream control, for example:

```bash
WORLD=OLDWORLD.ZEN PLACED_ITEMS=1 ITEM_DEEPEST_CONTACT=1 \
  scripts/bench.sh placed-items-diagnose-1644 "$PWD/.bench/thin-blades/1644-on"
```

`[GAME] item_deepest_contact=1` retains the EPA pair in manifolds involving
`C_Item` when it is more than 1 mm deeper than every clipped pair; coplanar
face contacts keep their original support corners. Marvin
`toggle item_deepest_contact` applies on the next
native step. OFF uses Jolt's original clipping. See the
[thin-blade results](../../reports/physics/2026-10-31-thin-blades/README.md).

`PLACED_ITEMS=0` selects the existing body-creation paths. `[GAME] placed_items`
defaults to 1; Marvin `toggle placed_items` changes it during play. The census is
computed even when the feature starts disabled, so enabling it later uses the
same original geometry. Dropped Items retain their existing physics path.
In OFF, the existing CharacterVirtual path creates bodies within its nearby Item
radius and can push them; the walk check records this observed behavior.

`placed-items` runs the native hero passage, arrow, tavern-table and owned pickup
checks in NEWWORLD with `PLACED_ITEMS=1` and `=0`. Both modes choose the same
authored floor food and supported lane; nearby NPCs are removed using the
`item-wake` fixture pattern. The arrow aims at native collision bounds. The
Dead Harpy table tests admitted Items; `owned-table` also selects a real mixed
table with excluded Items and native private-room ownership for the theft check.
Ownership fields are never rewritten. Failed result events preserve the final
FAIL while allowing later independent checks to run. It snapshots ownership before
the checks and saves after pickup (including removal/reordering of world Items):
`session-placed-items.sav` and `placed-items-saved.jsonl` describe the same Items.
Start `placed-items-reload` in two successive new processes, using the preceding
process's archive as `SAVE` each time (leave `WORLD` unset). Its first command
captures every Item before the first native step, then saves again. Verify both
cycles with:

```bash
python3 scripts/placed_items_report.py --save-chain \
  .bench/placed-items-on .bench/placed-items-reload-1 .bench/placed-items-reload-2
```

The reader checks saved poses within 0.01 cm, ZEN linkage, instance, owner,
flags and count; linked native bodies must enter asleep and excluded Items must
have no body. Also load an actual main save with no `placed-items` entry through
`placed-items-reload`: only unchanged instance/pose matches acquire a link.

`placed-items-performance` selects the densest source-Item 20 m neighbourhood from
the census, measures entry frames and ten steady seconds after two seconds of
warmup. Run three fresh processes per mode on the M1 Pro with `BENCH_HOST=local`,
`WORLD=NEWWORLD.ZEN`, `RYZEN_CLEAN_IDLE_HOURS=0` and `PLACED_ITEMS=0`/`1`.
Before each pair, record `uptime` and `pgrep -fl '[G]othic2Notr' || echo absent`.
Wait up to 30 minutes for load average to fall to 12 or below, and always wait
for any existing game. If physics p95 exceeds 2 ms in both modes, repeat the
pair in the same scene on a clean `origin/main`. ON–OFF is the main comparison
under high background load. `placed-items-performance.csv` contains native physics
time, frame time, total/placed body counts and `proximity_created`. The latter
counts only the new proximity budget; the existing `item_wake` budget is separate.
Acceptance is physics p95 ≤2 ms and ON–OFF ≤0.2 ms. The report also records the
worst entry frame, body counts and complete world loading time including census.
Compare complete NEWWORLD loading time using the median of three runs per mode.

[Recorded results](../../reports/physics/2026-10-31-placed-items/README.md): after
the 2 cm penetration limit and `no_ground`, the four-world probe reached
636/660 (96.363636%) below 5 cm, maximum 123.417038 cm and two recoveries.
The three agreed exceptions preserve that fraction and raw evidence; with the
CSV the reader passes, with a gated maximum of 74.7259445 cm and zero other
recoveries. The two swords remain admitted; their thin-body support crossing
will be investigated separately.
The corrected functional ON/OFF passes all five result checks per mode. Mac
ON p95 is 1.394707/1.581916/1.402624 ms; each pair is below OFF and within budget.
Both cold cycles now pass on Ryzen with all 2330 saved links after pickup,
including authored Item 2839 below mover 2838. The load-time catalogue retains
all 2331 authored Items independently of inherited trigger flags. The old main
save restores all 2331 links with no duplicate Items; maximum cold pose error is
0.00007079 cm. Version 55 and placement-entry schema 1 remain unchanged.
The native placed-Item contract also moves the mover-child Item before saving
and checks its identity and exact matrix on reload. Linux builds and all four
physics contracts pass after explicit signed Guild comparisons in the fixture.

`OpenGothic/common/testbench.cpp` and `testbench_<family>.cpp` (including healing in
`testbench_healing.cpp`, cloth in `testbench_cloth.cpp`, water film in `testbench_water_film.cpp`, water / corpse-water in `testbench_water.cpp` and hero / human NPC
CharacterVirtual in `testbench_character.cpp` and ground monsters in `testbench_monster.cpp`, destruction in `testbench_destruction.cpp`, physics-trace debug commands in `testbench_traces.cpp`, movable objects/candles, physics noise and spells in `testbench_movables.cpp`, item wake in `testbench_item_wake.cpp` and living bodies in `testbench_living.cpp`, ragdolls in `testbench_ragdoll.cpp` and own meshes / chests in `testbench_own_mesh.cpp`, archery in `testbench_archery.cpp`, melee in `testbench_melee.cpp` and routine return / guard in `testbench_routine.cpp`, manual-aim spot probes in `testbench_aim_spots.cpp`, hero / NPC commands in `testbench_npc_commands.cpp`, physics / world / time in `testbench_world.cpp`, scenario control in `testbench_control.cpp` and conversation / AI probes in `testbench_dialog.cpp`), inert unless `OPENGOTHIC_BENCH` is set. Hooks: the fixed time step and
the capture in `MainWindow` (in film mode the captured frame replaces the window's), the AI bridge's lines
and voices (`TestBench::aiLine`, `aiVoiceEnd`, `aiHeard`, `aiBusy`), an npc's lines of the game and SVMs
(`TestBench::npcOutput` in `Npc::nextAiAction`), the OpenAL loopback device in Tempest's
`SoundDevice`, `TEMPEST_WINDOW_SIZE` in Tempest's macOS and X11 backends, `FootIk::playerProbe` (legs before and after IK),
`Pose::playerProbe` (the animated pose, what plays, `AnimBlend`; `OPENGOTHIC_ANIBLEND_LOG=1` logs the hero's
switches bone by bone),
`LipSync::subjectProbe` (the mouth), `Flinch::subjectProbe` (blows), `TalkGestures::subjectProbe` (the hands), `EyeBlink::subjectProbe` (blinks), `Camera::setNearClip`
(`view clean`), `DrawCommands::hiddenTypes` (`view hide`), `Npc::transformInto` (`transform`), and
`TEMPEST_BACKGROUND` in Tempest's macOS backend (no focus, no Dock icon).

### Stage 5, step 1: hero CharacterVirtual

`character_virtual` is on by default since the manual trial (2026-10-16). The comparison uses two fresh processes and the same
stock-world geometry, with `movable_vobs=1` in both. Check `pgrep -fl Gothic2Notr` before each
run and wait for any other game's process to finish.

```sh
NO_BUILD=1 MOVABLE_VOBS=1 CHARACTER_VIRTUAL=0 scripts/bench.sh character-virtual-probe .bench/character-probe
NO_BUILD=1 MOVABLE_VOBS=1 CHARACTER_VIRTUAL=0 OPENGOTHIC_BENCH_TAKE=off scripts/bench.sh character-virtual .bench/character-off
NO_BUILD=1 MOVABLE_VOBS=1 CHARACTER_VIRTUAL=1 OPENGOTHIC_BENCH_TAKE=on scripts/bench.sh character-virtual .bench/character-on
```

Preparation searches actual supported geometry deterministically, in waypoint-name order. Nearby NPCs use
the existing bench freeze policy before each route’s first native tick, so automatic important dialogs
cannot consume held movement; this fixture does not change production NPC behavior. It
prints every selected waypoint, start XYZ, goal XYZ and yaw as `character_route_setup` in
`events.jsonl`. Missing geometry produces `character_error`; the probe visits all eight
preparations before ending, while the actual scenario stops on a missing fixture. Routes are
Khorinis stairs up/down, downhill movement from a measured slope above the hero guild’s legacy slide threshold (up to 65°), a harbour lane within 95 cm of a drop, a narrow
opening, Coragon's furnished tavern, a 55–110 cm ledge and a waterfront with a swimming destination (the quay may enter via falling).
Stairs additionally require at least three distinct 6–32 cm risers and level treads, excluding bed waypoints.
The port lane stays within 30 cm of its starting height. These coordinates are also the teleport points for a manual trial. Only the initial placement
teleports the hero; route movement uses the real player's held keys. The jump and water routes
exercise the legacy-path handoff.

Held movement is released as soon as the destination is reached; the remainder of the video records the natural stop.
Each route has its own MP4 and ordinary IK metrics; `foot-ik-stairs.png` is captured on the first
stair route. `character-frames.csv` additionally records the exact physical root and the actual
model translation (`visual_x_cm`, `visual_y_cm`, `visual_z_cm`), held movement, falling,
swimming, actual CV use, support velocity and native physics cost after each world tick. A route
succeeds only after crossing its destination plane within the stated lateral/height tolerance;
water entry additionally requires the real swimming state. Route-end `saw_slide` establishes whether the
legacy slope take entered the actual slide state. The analyzer counts continuous held
input with less than 5 cm of horizontal progress for more than one second, maximum absolute Y
change per frame and native falls more than 100 cm below the last supported root (planned ledge
jumps and swimming are excluded). Physical and visual maximum Y changes are reported separately;
older CSVs without visual columns leave that measurement empty. Unmatched falls are compared
within a 1 m spatial radius. `character-virtual-review` retakes stairs up/down, the tavern
and the steep slope after support-filter or visual-height changes, using the same OFF/ON
environment above. `character-virtual-step-blend` retakes only the steep slope and stairs
down with ON. The 100 ms model-height blend applies only to Run height discontinuities
beyond support-plane travel; Slide clears it, and offsets over 10 cm catch up immediately.

Run `character-virtual-props` with the same OFF/ON environment in separate output directories for
four equal-duration native contacts: a real potion Item, a beer mug Item, a stock barrel mesh and
a stock table mesh. Only the current test prop occupies the lane; temporary props from the general
tavern fixture are removed. Checkpoints include native contact attribution, mass, horizontal
position, velocity and displacement. Reported push speed uses the actual held-input duration,
excluding the 700 ms settling interval. Table passage is checked against the current transformed
far face after straight forward input. The same scenario records dynamic table, kinematic mesh
and any exposed standable world mover. A missing standable mover is explicitly reported;
insufficient support motion or falling needs review of its video. The real world mover is the
accepted support case for this step; artificial tavern supports remain diagnostic, including
the known kinematic drift left outside this PR's scope.

`character-virtual-save` writes `session-standing.sav` at an identical freshly placed stair point.
Load each result through `SAVE=<path>/session-standing.sav` and `character-virtual-reload`, with
that mode's initial `CHARACTER_VIRTUAL`. Its first command snapshots the loaded root before the
first world tick. The comparator checks saved-versus-loaded and OFF-versus-ON positions to 1 cm.

Performance uses `character-virtual-performance` in two more fresh processes, with a build
configured with `-DOPENGOTHIC_ENABLE_APP_CADENCE=ON`. It records an uncaptured 30-second normal app-clock
window of walking to and fro; `appcadence.csv` supplies complete app-frame intervals and
`character-frames.csv` supplies native physics-step cost. Video capture runs at a fixed 30 Hz and
must never be presented as game FPS. Limits are +0.3 ms native physics p95 and 5% mean FPS loss.

`character on|off`, `character route <name>`, `character begin|end`, `character prop <item|mug|barrel|table>`,
`character prop-checkpoint`, `character ride <dynamic|kinematic|mover>`, `character ride-start`,
`character ride-checkpoint`, `character cleanup`, `character save <label>` and the first-command
`character initial` are test-only commands. `CHARACTER_VIRTUAL=1` also works with `scripts/e2e.sh`
for the single requested `SCENARIO=m5b` check, without changing the user's Gothic.ini.

### Nearby human NPC CharacterVirtual comparison

`NPC_CHARACTER_VIRTUAL=0|1` sets the independent `[GAME] npc_character_virtual` before loading
in both `bench.sh` and `e2e.sh`. The production default is **1** since 2026-10-16; the hero's setting is independent.
`OPENGOTHIC_BENCH_TAKE=off|on` selects the same NPC flag inside the comparison scripts.

`npc-character-virtual` sends its own human NPC through the stock forest stairs, doorway and
Coragon tavern using native `AI_GoToPoint`, then records native bench sitting and standing.
`npc-character-virtual-seat` pins one free authored bench at `NW_CITY_HABOUR_06_C`, with a fixed
camera checked for visibility of the approach, feet and seated pose. `npccharacter seat <waypoint>`
selects the nearest authored BENCH assigned to that exact waypoint; occupied or invalid fixtures
fail rather than silently choosing a different bench. Without an argument, the full route stand
searches free authored benches, preferring the harbour and farms. Actors use the bench's external-control policy so routines cannot
replace a queued test leg; ordinary observation windows do not change NPC control or goals.
The mug must settle on the floor before recording. `npc-character-frames.csv` and
`npc_character_route_end` contain actual NPC root/model heights, controller activity, arrival,
stalls, prop mass/contact/displacement and the maximum frame-to-frame height change.
Sitting also records the authored animation-socket alignment, which is included in that maximum.

`OPENGOTHIC_NPC_CHARACTER_TRACE=1` enables opt-in support/contact diagnostics for the route actor.
On a physical or model height change above 10 cm, `npc_character_support_trace` records that frame
and five preceding frames, with native body ID, layer, mass, visual/class, support normal/velocity
and all active contacts. Each sample distinguishes the state before movement, after native update
and floor sticking, and after the separate world-stair recipe. The output threshold only selects
diagnostic records; it never changes movement. Ordinary runs allocate no native diagnostic snapshot.

`npc-character-virtual-performance` keeps twenty human actors walking in supported market
lanes for a normal, uncaptured 30-second window. It checks all twenty remain inside 20 m;
limits are +0.5 ms physics p95 and 5% mean app FPS loss. `npc-character-virtual-radius`
checks the 20/22 m hysteresis and live toggles. `npc-character-virtual-save` writes a complete
`session-standing.sav`; load it with the matching initial flag and `npc-character-virtual-reload`.

`scripts/npc-character-virtual-windows.sh` runs the eight passive morning/evening windows from
the navigation baseline in both modes. Set absolute `RUNNER`, `SAVE`, `GOTHIC_DIR` for the
existing CPU-only body runner, or omit `RUNNER` to use the Mac game. Completed windows are
preserved on restart after validating the complete interval and native/stuck evidence;
`NPC_CV_MODES='on off'` changes run order. `NPC_CV_TIMEOUT=600` limits each process, including loading.
The default covers all sixteen runs; `NPC_CV_SPOTS='market harbour'` selects the four city pairs.
Use a fresh output directory after rebuilding. Native observation counts
unique humans reaching a goal separately from the stuck detector's all-NPC walking population.
Run `scripts/npc_character_virtual_compare.py .bench/npc-character-virtual --output
.bench/npc-character-virtual/comparison.md` to compare the raw data. Missing evidence stays
incomplete; the analyzer also writes JSON beside the Markdown output.
For a deliberately reduced series, repeat `--window-spot market --window-spot harbour` in the
analyzer; the report then explicitly states the selected coverage.

### Nearby ground-monster CharacterVirtual comparison

`monster-character-virtual` is the first numerical stand for the independent
`[GAME] monster_character_virtual` switch (production default **0**). It records no
video. Run the same binary and save in separate fresh OFF/ON processes:

```sh
NO_BUILD=1 MOVABLE_VOBS=1 MONSTER_CHARACTER_VIRTUAL=0 OPENGOTHIC_BENCH_TAKE=off scripts/bench.sh monster-character-virtual .bench/monster-character-off
NO_BUILD=1 MOVABLE_VOBS=1 MONSTER_CHARACTER_VIRTUAL=1 OPENGOTHIC_BENCH_TAKE=on scripts/bench.sh monster-character-virtual .bench/monster-character-on
```

The wolf follows the actual hero with native `AI_GoToNpc` across stock city stairs
(preferring the forest stair waypoint) and a measured stock-world slope. A continuous
15 cm support profile validates a hero destination beyond the obstacle using the
monster's actual step, slope and stopping distance. The hero stands beyond each obstacle so the
monster's ordinary attack-distance stopping rule cannot finish before crossing it.
A troll uses native `AI_GoToPoint` through a body-size lane in Coragon's tavern or
adjacent courtyard, with a real stock barrel in its way. The same wolf enters water
along a continuous stock bank, then returns to its dry start without teleporting.
Preparation logs the selected points and MDH radius/height, guild step/slope limits,
water depths and mass in `monster_character_body` and route-setup events. Missing
geometry or animation is an explicit preparation error.

`monster-character-frames.csv` contains physical and visual positions after every
native tick, actual CV use, airborne/falling/swimming state and playing animations.
`monster_character_route_end` reports arrival and elapsed time, stalls with less
than 5 cm horizontal progress for more than 1000 ms, maximum physical and visual
vertical change per frame, InAir/Falling drops more than 100 cm below the last supported root (including the landing frame),
CV/swimming frames and barrel displacement. Arrival uses the native attack range
for a chase, or 60 cm horizontal / 80 cm vertical tolerance for point routes. Water
entry additionally requires actual swimming; the exit requires returning to land.
Falls exclude the deliberate attack root and swimming transition. A legacy barrel
stall is retained as a measured result; route summaries do not silently turn it
into successful arrival.

Two separate authored `T_FISTATTACKMOVE` / `BS_HIT` clips measure wolf lunge and
field-raider charge through the production animation and movement paths. The
field-raider starts the native `S_FISTRUNL` base before calling the ordinary
running-attack entry point; that moving base remains underneath the rootless
layer-2 attack clip, with the real hero 350 cm ahead.
The stand records maximum horizontal travel, upward root travel, observed clip duration
and actual charge collision callbacks. Compare ON/OFF distance and duration to
10%; the field-raider case also requires the native callback to its real target.
The stand supplies no synthetic movement or contact response.

After the comparison windows, the stand enables only the monster flag and checks
20/22 m hysteresis, both live Marvin toggles and exclusion of Bloodfly, Harpie,
Demon and Wisp on dry ground. `monster_character_expect` records expected and actual
CV activity. It also saves `session-monster.sav` and performs a native `Npc::save` /
`Npc::load` round trip with immediate and first-two-tick position checks to 0.5 cm.
For a full session reload, use `SAVE=<absolute output>/session-monster.sav`,
`MONSTER_CHARACTER_VIRTUAL=1`, and a small scenario whose first command is
`monstercharacter initial`, followed by `wait 100` and
`monstercharacter snapshot loaded`. The initial event precedes the first positive
world step; compare its actor IDs and roots against the `saved` snapshot to 0.5 cm.
Temporary barrel fixtures belong only to their measured route and are not saved.

## NPC movement keys (CPU runner)

`keys` is a test-bench command, available in `gothic-body-runner`; it does not change the AI bridge.
Targets are a fixture index, an existing script instance such as `NONE_100_XARDAS`, or
`id:<world NPC index>` for duplicate instances. Existing NPCs need no `keys setup`; enabling
them preserves their current AI queue, interaction and routine.
The stand enables individual actors and suspends their native AI while enabled. A command replaces
all previous keys. TTL uses simulation milliseconds; expiration releases input while retaining stand
control. On a slope, CharacterVirtual can drift with zero requested movement after release:
the player and NPC both moved 4.514 cm in the same one-second idle window. This is a shared
controller limitation, distinct from held input; see the
[validation report](../../reports/navigation/2026-10-16-keys/README.md).
`off` restores native control and the previous walk mode without clearing
the queued destination. Airborne movement finishes naturally after release.

```text
keys setup flat 1                   # flat|jump|climb|market|void|void-replay, 1..10 human actors
keys place 0                        # actor index, or hero; same stored start and facing
keys 0 on
# actor forward strafe turn walk|run jump climb head_yaw head_pitch ttl_ms
keys 0 1 0 0 walk 0 0 60 0 3000
wait 3500
keys sample released
keys 0 off
```

Axes are digital: only their sign is used. Positive strafe/turn means right; positive head yaw
means left, positive pitch means up, in degrees relative to the body. Climb means forward+jump,
using the same `tryJump`/`startClimb` tests as the player. `keys hero ...` sends the same digital
movement through the real `PlayerControl`; head input is measured on the NPC.

`keys trace <tag> on|off` writes per-frame `keys-frames.jsonl`; `keys sample <tag>` writes an explicit
checkpoint. `keys eye <index> <tag>` writes all rays of a fresh existing `NavigationEye` sensor to
`keys-eye-snapshots.jsonl`. `keys routine <index>` queues a native `AI_GoToPoint` to the fixture's
recorded destination. `keys stress begin <seed>` after `keys setup market 10` sends random keys
with TTL 100 ms; `keys stress end` records executor timing, per-actor travel and sequential-tick
observations in `keys-events.jsonl`. Setup records the selected stock waypoint and exact geometry.
The fixture removes its spawned bandit's `Humans_Relaxed.mds` when absent on the hero, then checks
walk/run/strafe clip equality before comparing motion. The executor itself preserves each NPC's
authored animation set. `keys info` records clip parameters for diagnosing fixture differences.

Frame journals distinguish the physical feet (`physical_root_cm`), gameplay position
(`npc_root_cm`) and displayed model (`visual_root_cm`). `support_physical` and `support_npc`
contain the downward terrain/object ray result, or `null` for a miss. Flat/obstacle/void scenes
also record native CV requests, accepted positions, contacts and support; market profiling leaves
that detailed CV instrumentation disabled. `keys setup void 1` finds a supported approach to the
stock-world support gap near `[6754, 262, -6115]` and records an 81-point support grid. Its goal
lies beyond the gap and is intentionally not required to have floor.
`keys setup void-replay 1` uses the supported idle pose recorded before the stress actor's
takeoff. The report script's `void-replay` case replays that actor's last 54 decisions through
the real player's input and the NPC executor; it records differing contact trajectories and falls.

Repeat the checks with [`reports/navigation/2026-10-16-keys/run.py`](../../reports/navigation/2026-10-16-keys/run.py).
Use absolute runner/save/assets/output paths and the shared Ryzen simulation lock; the script takes
and releases it separately for each case. Run both `npc_character_virtual` settings in fresh processes.

## Navigation eye (CPU runner)

The `eye` commands are an independent navigation sensor fixture. They leave combat's 16 body rays
and bridge observations unchanged. Run with `gothic-body-runner --mode lockstep`; all output paths
must be absolute. The runner also builds on macOS (POSIX target).
For timing, set `OPENGOTHIC_BODY_TRACE=0` to suppress the runner's default combat journal;
`run.sh` below sets this explicitly. Eye timing and snapshots use their own journals.

```text
eye setup market 20                 # market|lobart|lobart-fence; 1, 10, 20 or 50 frozen human NPCs
wait 2000
eye begin 500 100 off baseline       # layout 250|500|1000, center range 20|50|100 m, on|off, tag
wait 12000                          # first 2000 ms excluded; remaining ~10 s measured
eye end
eye begin 500 100 on eye-500-100
wait 12000
eye end
eye snapshot market-20 0             # fresh full eye of actor 0, outside measurement
eye wall                            # bounded real finite-wall fixture: back 0 hits, front >0
```

`setup` uses supported roots near the market or Lobart's field, freezes nearby residents,
and enables the existing native living rigs. `begin` rejects overlapping spans. Only `tick` samples
the eye, after world movement and animation. Near segments (up to 10 m) update at 10 Hz; unobstructed
continuations at 2 Hz. Cached continuations retain their original head pose and timestamp.

Output: `eye-spans.jsonl` (all/near/full cost aggregates, wall and process CPU time, actual Hz and rig
count), `eye-timing.jsonl` (each measured NPC update), `eye-snapshots.jsonl` (ray geometry, distance,
hit kind, signed normal Y and timestamps), `eye-events.jsonl` (fixture geometry and wall checks).
Timing rows are buffered and written after the measured span; snapshots are explicit. No eye data
enters the AI bridge. Repeat the full matrix with
[`reports/navigation/2026-10-16-eye/run.sh`](../../reports/navigation/2026-10-16-eye/run.sh).
`lobart-fence` places the calibration actor outside the pasture, near Maleth's previously confirmed
collision with the fence; the performance pool `lobart` stays inside the field at `NW_FARM1_PATH_01`.

### Cached destruction

Build the local stock-world cache once with `scripts/fracture.sh`; `OPENGOTHIC_FRACTURE` overrides
`~/.local/share/gothic-ai/fracture`. `scripts/fracture.sh verify` repeats the cut with seed 601 and
compares bytes, closed surfaces, convexity, UVs and shell volume. Game assets stay outside git.

`destruction` runs a real fireball against a crate and barrel in Coragon's tavern, native arrows,
one/five metre drops and NPC/hero walking. `destruction-fireball` is the shorter fireball/hearing
reproducer. Both require the same old Khorinis save as `movable-vobs`. The listener stands beyond
ordinary furniture-impact range so the existing per-NPC noise cooldown cannot consume the break.

```sh
SOUND=1 TEMPEST_AUDIO_LOOPBACK=48000 scripts/bench.sh destruction .bench/destruction-functional
python3 scripts/destruction_compare.py functional .bench/destruction-functional
DESTRUCTION=0 scripts/bench.sh destruction-off .bench/destruction-performance/off
DESTRUCTION=1 scripts/bench.sh destruction-on .bench/destruction-performance/on
python3 scripts/destruction_compare.py performance .bench/destruction-performance
DESTRUCTION=0 scripts/bench.sh destruction-disabled .bench/destruction-disabled
OPENGOTHIC_FRACTURE= scripts/bench.sh destruction-disabled .bench/destruction-no-cache
python3 scripts/destruction_compare.py disabled .bench/destruction-disabled
python3 scripts/destruction_compare.py disabled .bench/destruction-no-cache
```

Use the same host and binary for both performance runs. They measure thirty seconds of completed app
frames with the normal game clock, including the exact frame containing five native breaks, and
native physics-step costs. Configure local builds with `-DOPENGOTHIC_ENABLE_APP_CADENCE=ON`;
the Ryzen runner enables it automatically. Capture cadence is not game FPS.
The five-body comparison uses an open stock-world arena with native floor and projectile/blast
visibility checks; the crate/barrel acceptance pair stays in Coragon's tavern.

`destruction on|off`, `destruction setup fireball|five|arrows|drop1|drop5|walk-npc|walk-hero`,
`destruction begin` and `destruction checkpoint` control only the fixture. Geometry, body replacement,
spell/arrow contacts and hearing use production paths. `destruction_*` events retain health, body
identity, real drop trajectories, piece movement/sleep and break wall time. The scenario writes PNGs,
six videos with optional WAV audio, and `session-destruction-scattered.sav` beneath its output directory.
The save uses the previous format; transient fixture props and fragments are not saved.


Cloth acceptance uses authored level surfaces and mesh-vobs. Each functional
scenario records the original OFF and Jolt ON from the same fixed camera,
including calm and smoothly increasing gusts. Ship sails are the furled cloth
already present in the level. The harbour scenario includes the whole ship,
a close main-mast flag with a native Fireball, and a close furled sail. The city
scenario fires an arrow through an extracted banner. The Valley of Mines
scenarios show the native `S_WINDY` orc standard and the Old Camp banner; the
latter also records both 40 m crossings and exports a moving-cloth save.
`cloth-arrow-npc` checks the harbour wind and two single-step native arrow paths:
cloth before the NPC must receive an impulse; cloth behind the stopping NPC must
not. A fixture NPC is held only for these checks; production NPC logic is unchanged.
Use the separate `cloth-on`/`cloth-off` scenarios for a clean-world timing pair without the fixture NPC.

```sh
WORLD=NEWWORLD.ZEN scripts/bench.sh cloth-harbour /absolute/path/cloth/harbour
WORLD=NEWWORLD.ZEN scripts/bench.sh cloth-city /absolute/path/cloth/city
BENCH_HOST=ryzen RYZEN_BENCH_CLASS=quiet WORLD=NEWWORLD.ZEN scripts/bench.sh cloth-arrow-npc /absolute/path/cloth/review-arrow-npc
WORLD=OLDWORLD.ZEN scripts/bench.sh cloth-orc /absolute/path/cloth/orc
WORLD=OLDWORLD.ZEN scripts/bench.sh cloth /absolute/path/cloth/oldcamp
SAVE=/absolute/path/cloth/oldcamp/session-cloth-moving.sav scripts/bench.sh cloth-reload /absolute/path/cloth/reload
CLOTH=0 SAVE=/absolute/path/older-save.sav scripts/bench.sh cloth-legacy-load /absolute/path/cloth/legacy-load
BENCH_HOST=ryzen RYZEN_BENCH_CLASS=quiet WORLD=NEWWORLD.ZEN scripts/bench.sh cloth-off /absolute/path/cloth/performance/off
BENCH_HOST=ryzen RYZEN_BENCH_CLASS=quiet WORLD=NEWWORLD.ZEN scripts/bench.sh cloth-on /absolute/path/cloth/performance/on
```

Videos that verify Metal/MoltenVK run on Mac with `BENCH_HOST=local`; put their
output beneath `~/.local/share/gothic-ai/saves/cloth/` and check that no other game
is running first. Other benches use Ryzen by default. `cloth initial` observes
the untouched loaded world before its first native step: no active soft bodies,
zero vertex speeds and zero transient hit/recovery counters. Saves use the
ordinary GameSession archive and can be started in a separate process.
`CLOTH=0` writes the ini switch before world loading. The legacy-load fixture
also checks Marvin `toggle cloth` in both directions before enabling simulation.

Use the same binary, world and host for both performance runs. `densest` searches
cloth centers and local cluster centroids, counts the unchanged 3D 40 m sphere,
and chooses the observer with the greatest real count. Compare the actual count
in the Khorinis and Valley fixtures before selecting the final timed scene.
Supported native floor is preferred. For ship flags more than 40 m above deck,
the fixture can hold the actual hero in static flight near the mast; this is
recorded as `observer_support: fixture-static-flight` and changes no production
activation rule. The camera remains independent of that observer.

`cloth-on` and `cloth-off` use the native game clock for 30 seconds, without
video or per-frame mesh snapshots. `appcadence.csv` contains completed app frame
intervals; `cloth-physics.csv` contains native physics step costs.
`cloth_measure_before` and `cloth_measure_after` include actual soft-body counts
and physics p95. Capture cadence is not game FPS. Local performance builds need
`-DOPENGOTHIC_ENABLE_APP_CADENCE=ON`; Ryzen enables this option automatically.
`RYZEN_BENCH_CLASS=quiet` waits for the other queued work to finish and reserves
the machine for the timing run; ordinary benches retain `quick`. Set
`BENCH_HOST=ryzen` for the complete ON/OFF pair. A local cadence scenario refuses
to start when its build lacks this option, including after an automatic fallback.

| Command | Effect |
|---|---|
| `cloth on\|off` | Set the normal `[GAME] cloth` option, also available as Marvin `toggle cloth`. |
| `cloth setup <zen-id\|visual\|densest> <label>` | Select an authored vob or level cloth island (for example `6367`, `NW_CITY_FLAG_01:1` or `NW_SHIP_FLAG_01:1`), place and hold the observer, and frame the entire cloth from a clear side. Test visible simulated and native-reference vertices against foreground geometry; high ship cloth can be viewed from below its yard. Exact visual labels take precedence over prefix matches. |
| `cloth camera harbour` | Frame all ten authored ship cloth islands without changing the observer or selected physics target. |
| `cloth camera-turn <degrees> <duration_ms>` | Smoothly turn a held camera in place, retaining its pitch, then hold the new view. `cloth-visibility` records an authored flag leaving and returning to the camera; checkpoints include `visible` and `simulation_steps` to verify that hidden time is frozen. |
| `cloth camera-range <cm>` | Move a held cloth camera horizontally to the requested distance from the selected cloth, retain its height and clear side, and verify the whole cloth is unobstructed and framed. Run before recording the turn. |
| `cloth wind calm\|natural\|gust` | Smoothly set the common world wind scale to 0, 1 or 2. |
| `cloth checkpoint <label>` | Record physical/rendered vertex counts, anchor error, current and lifetime maximum vertex speed, recovery count, projectile counters and the selected cloth's rendered vertices in cm. |
| `cloth arrow-npc behind\|ahead` | Invoke one synchronous production `moveBullet` segment with a native precise arrow and a fixture NPC behind or ahead of the selected authored banner. Verify the native bone hit and cloth arrow-pass delta 1 or 0. |
| `cloth shoot fireball\|wind\|arrow` | Launch a real production Bullet at an exposed cloth face. The fixture does not inject a synthetic impulse or introduce an NPC target. |
| `cloth verify anchors\|fireball\|wind\|arrow` | Require anchors within 0.5 cm, finite vertices, bounded speeds and no solver recovery; require the selected spell hit; or require an arrow contact and its actual Bullet trajectory at least 30 cm beyond the face. |
| `cloth verify harbour-wind` | At the densest ship observer require all five active furled sails and four active flags to be outdoors and exposed to the common wind. Record separate total and exposed counts. |
| `cloth room-check <x> <y> <z> indoor\|outdoor` | Check production room classification at an authored world position, recording the point, legacy BSP room name, actual classification and expected result. This adds no cloth or other fixture object. |
| `cloth distance <cm>` | Move the actual hero to the requested 3D distance, retaining the camera and existing native-support or static-flight fixture mode. |
| `cloth reset-stats` | Reset transient solver measurements. |
| `cloth save <label>` | Write `session-cloth-<label>.sav` as a complete ordinary GameSession archive under the output directory. |
| `cloth initial <label>` | First command of a fresh process: observe and verify transient cloth rest before the first native world step. |
| `cloth measure on\|off`, `cloth report` | Start and report the normal-clock timing window after `cadence normal`; complete it with `cadence wait`. |

Functional `cloth_sample` events include the maximum rendered vertex displacement
since the preceding frame. Active-state changes include the complete rendered
vertex list; explicit checkpoints and the named before/after PNGs retain both
sides of each radius crossing. `transition_blend_delta_cm` isolates the blend's
contribution from natural movement. A calm checkpoint is taken after 10 seconds
at zero wind following the wind ramp. The arrow's native post-face positions are
retained as `cloth_arrow_trajectory` events.

Monster CharacterVirtual videos: `scripts/monster-character-virtual-videos.sh OUT` records
six independent fresh-world OFF/ON pairs (seed 73024, noon). It first asks Ryzen through
`bench.sh`; exit 75 selects Mac for the remaining series, with the Mac game/pytest wait.
`SCENE=wolf_terrain|wolf_pack|troll_props|cliff|wolf_water|fieldraider_bloodfly` selects one
pair. Build `Gothic2Notr` first (`NO_BUILD=0` enables local incremental builds).
The candidate `OUT/pairs/*.mp4` keep OFF on the left and ON on the right; the accompanying
`*-frames.png` sheets show OFF above ON once a second. Each scene follows its monster
with matching `watch id`, `cam follow` and `view clean` settings; the pack follows its
middle wolf. Review framing and visible geometry before accepting a candidate. The
[validation report](../../reports/physics/2026-10-18-monster-character-virtual/validation-notes.md#шаг-4-часть-2--парные-видео)
lists accepted pairs and skipped preparations. Composition can also be repeated
with `uv run scripts/monster_character_virtual_videos.py compose --off OFF.mp4 --on ON.mp4
--out PAIR.mp4`; `metrics --off OFF_DIR --on ON_DIR` reads native movement evidence.
`--crop X:Y:W:H` applies the same fixed crop to both videos and their contact sheet.
Metrics include actual settled route starts and reject ON ground actors with no CV frames;
the bloodfly control must stay on the legacy path.
`monster-character-virtual-attacks` checks wolf lunge and fieldraider charge numerically;
`monster-character-virtual-performance` measures twenty ground monsters for 30 seconds
without capture. Run performance on Mac with `BENCH_HOST=local` and a build configured
with `-DOPENGOTHIC_ENABLE_APP_CADENCE=ON`. The opt-in flag remains OFF by default.

`monster-character-virtual-stairs` pins the part-1 chase to `NW_CITY_TO_FARM2_09`
and records a twelve-second `wolf_stairs` take with `watch id`, `cam follow` and
`view clean`. Its `monster_character_stair_step` events include body state, signed
physical/visual Y changes, requested animation movement and separate CV
ExtendedUpdate/WalkStairs positions. A recorded take still needs framing review;
the report documents the accepted pair and the earlier obstructed diagnostics.
For the stair pair, `uv run scripts/monster_character_virtual_videos.py foliage --out DIR`
creates twelve transparent leaf/grass PNG overrides. Pass the same absolute
`OPENGOTHIC_TEXTURES=DIR` to both fresh processes: ground, trunks and all collision
meshes remain intact. The video title discloses the render-only foliage removal.
The monster stair policy uses only the remaining horizontal request instead of
Jolt's extra 2 cm minimum advance. It retains the full guild step (wolf 60 cm,
troll 100 cm) for a single ledge. A blocking steep polygon with more than the guild step plus contact padding
exposed above the physical feet is treated as extended steep terrain and retains
a 12 cm rise budget. The decision stays with the same body/subshape until the
blocking contact ends; teleportation clears it. Buried portions below the feet
are excluded. Unknown/non-polygonal shapes retain the full guild step.
Player/human paths retain the native policy.
The contract covers repeated ten-centimetre risers, isolated ledges, and a
40 cm ledge whose vertical face continues 100 cm below the approach floor.
Presentation lag stays within 10 cm; a high single ledge may visibly snap as in
legacy movement.

`monster-character-virtual-ledges` compares six isolated ledges: wolf 20/40/55 cm,
troll 40/80 cm, and wolf 40 cm with a face bottom at -100 cm.
`monstercharacter ledge wolf|troll HEIGHT [BOTTOM]` creates one static
source mesh 100 m above a verified stock anchor, checks three support rays and
the vertical face, and runs production `Npc::goToPosition`/MoveAlgo over a 6 m
approach to a 15 m landing. OFF/ON use the same native running animation and
geometry. Each eight-second window reports arrival, time, stalls, root/model
height changes and falls. Arrival requires 60 cm horizontal / 5 cm vertical
tolerance and actual support; all six results are retained even after a
mechanical failure. The synthetic geometry supplies no movement or response.
`monster-character-virtual-stair-attack` enters `T_FISTATTACKMOVE` while a stair
presentation correction is pending, without resetting that correction. It tests
the grounded attack handoff separately from physical lunge range. The numerical
wolf lunge and fieldraider charge use the verified open lunge lane; the latter
also retains its Bloodfly control and requires the native NPC collision callback.
