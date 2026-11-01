# Our stack: starting point for a physics-driven character and physics-based melee (Gothic AI: OpenGothic + Jolt)

Source convention: all paths are relative to the main checkout `/Volumes/DevSSD/Dev/projects/gothic` (branch `main`, read 2026-10-23). `file:N` = line number. Engine code is under `OpenGothic/`. Docs are mostly in Russian; numbers quoted from them as-is. Web sources are given as URLs.

## 1. How animation and melee work now (skeletons, MDS animations, combos, hit frames, hit decision, damage, stumble, block)

### Takeaway
Melee in our OpenGothic is fully animation-driven and "abstract": a hit is a single scripted moment (`OPTIMAL_FRAME` event of the attack sequence) at which the engine checks **distance between skeleton roots ≤ guild range + weapon `range`** and **facing within ±30°** against the one `currentTarget`; the weapon mesh has no collision at all. Damage is the G2 formula `max(STR + weapon damage − protection, 0)` per damage type, crit roll vs talent, min 5. Block/evade are pose-state windows (`BS_PARADE` + combo/def windows) plus facing tests, not geometry.

### Cited Findings
**Animation data and playback**
- Sequence times are converted from frames: `totalTime = numFrames × 1000 / fpsRate`; layer effective time = `start + (t_game − start) × speed`. — [docs/npc-body-combat-contract.md:108](docs/npc-body-combat-contract.md)
- On load, MDS event tags become per-sequence time arrays: `HIT_END` → `defHitEnd`, `PARRY_FRAME` → `defParFrame`, `COMBO_WINDOW` → `defWindow`; only the earliest `OPTIMAL_FRAME` tag is kept. — [OpenGothic/common/graphics/mesh/animation.cpp:613-637](OpenGothic/common/graphics/mesh/animation.cpp)
- Per tick, `Sequence::processEvents` scans frames crossed since the last barrier and counts `OPTIMAL_FRAME` crossings into `ev.def_opt_frame`. — [animation.cpp:418-460](OpenGothic/common/graphics/mesh/animation.cpp)
- Pose is a stack of layers; `Pose::update` samples each layer (`updateFrame`), then `AnimBlend::apply` (inertialization), then `mkSkeleton`. Transition layers that finish get replaced via `solveNext`; stumble/parade body states are reset to `BS_NONE` on next. — [OpenGothic/common/graphics/mesh/pose.cpp:426-515](OpenGothic/common/graphics/mesh/pose.cpp)
- Procedural post-pass order over world-space bone matrices: `walk → lean → FootIk → BowDraw::hold → Aim → BowDraw → Gaze → Flinch → ragdoll pose (or ragdoll return) → aim.finish`. — [pose.cpp:574-597](OpenGothic/common/graphics/mesh/pose.cpp)
- Weapon/animation names are solved by `AnimationSolver`: 1H/2H front attack = `S_%sATTACK` (one long combo sequence), from run `T_%sATTACKMOVE`, side `T_%sATTACKL/R`, block in G2 = random of `T_%sPARADE_0 / _0_A2 / _0_A3` (uses `std::rand()`), finish `T_%sSFINISH`; fist `S_FISTATTACK`, `T_FISTPARADE_0`; jump back `T_%sPARADEJUMPB` / `T_%sJUMPB`; stumble `T_STUMBLE` / `T_STUMBLEB`; death/KO `T_DEAD(B)`, `T_STAND_2_WOUNDED(B)`, `T_WOUNDED_2_DEAD`. — [OpenGothic/common/graphics/mesh/animationsolver.cpp:114-165, 244-257, 363-392](OpenGothic/common/graphics/mesh/animationsolver.cpp)
- Measured asset windows: `T_1HPARADE_0*` 25 fps, 600 ms total, window [0,480]; `S_1HATTACK` 4560 ms total with several hit/combo windows; observed `prehit` intervals 66–1134 ms, `defence` 466–467 ms, `def_window` 33–767 ms (30 Hz resolution). — [docs/npc-body-combat-contract.md:222](docs/npc-body-combat-contract.md)

**Combos**
- `Pose::continueCombo`: if the current layer has no `defWindow`/`defHitEnd`, start normally; hit→block is a forced start; hit→hit is allowed only if elapsed time lies inside `defWindow[2·comboLen .. 2·comboLen+1]`; continuing the same sequence rewinds its start (`sAnim = tick − defHitEnd[comboLen]`) and increments combo length; pressing outside the window sets a combo "break". — [pose.cpp:1131-1190](OpenGothic/common/graphics/mesh/pose.cpp)
- Predicates: `isAttackAnim` (sequence has any OPTIMAL_FRAME), `isPrehit` (an OPTIMAL_FRAME still ahead), `isDefWindow`, `isDefence` = `BS_PARADE` layer + def window, `isJumpBack` = `BS_PARADE` with empty or active defWindow, `isFinished` = elapsed > `defHitEnd[combo]` or > totalTime. — [docs/npc-body-combat-contract.md:110-121](docs/npc-body-combat-contract.md); code [pose.cpp:1008-1090](OpenGothic/common/graphics/mesh/pose.cpp)
- Player input path: `PlayerControl` calls `swingSword()`, `swingSwordL/R()` per keys. — [OpenGothic/common/game/playercontrol.cpp:903, 934-937](OpenGothic/common/game/playercontrol.cpp)

**NPC fight AI**
- Native path: Daedalus `AI_Attack` → `FightAlgo::fetchInstructions` (tactic table + RNG) → `nextFromQueue` (expands to `MV_*` queue of 6) → `Npc::implAttack` (gates, turn, checks) → `Pose` start/continue → `OPTIMAL_FRAME` → `commitDamage` (re-check range/angle) → `takeDamage` (block/evade or damage). — [docs/npc-body-combat-contract.md:9-24](docs/npc-body-combat-contract.md)
- `implAttack` gates: target down, non-empty script queue, `BS_HIT` melee lock (only turns/moves), `BS_LIE`, stumble/fall/air, `faiWaitTime`/`waitTime`; MV_WAIT promoted to jump-back in closeup+focus30. Successful attack sets `faiWaitTime = t + atkTotalTime + 1`. — [OpenGothic/common/world/objects/npc.cpp:2431-2560](OpenGothic/common/world/objects/npc.cpp); [combat-contract.md:123-130](docs/npc-body-combat-contract.md)
- Primitive vocabulary: `MV_MOVE, JUMPBACK, ATTACK, ATTACKL, ATTACKR, STRAFEL/R, BLOCK, WAIT(200 ms), WAITLONG(300 ms), TURN2HIT, TURN`; script intents `ATTACK_TRIPLE/WHIRL/MASTER` expand to sequences of side/front attacks. — [docs/npc-body-combat-contract.md:34-75](docs/npc-body-combat-contract.md); [OpenGothic/common/game/fightalgo.h](OpenGothic/common/game/fightalgo.h)

**How a melee hit is decided**
- `Npc::tickAnimationTags` calls `commitDamage()` when `ev.def_opt_frame>0`. — [npc.cpp:3723-3752](OpenGothic/common/world/objects/npc.cpp)
- `commitDamage`: (a) if destruction is on, sweeps rays for movable/destructible vobs (see §2); (b) if `currentTarget` is null → no hit; else requires `isInAttackRange` and `isInFocusAngle` (±30°); then `currentTarget->takeDamage(*this,nullptr)`. Only the current target can be hit — no multi-target, no swept weapon. — [npc.cpp:3267-3314](OpenGothic/common/world/objects/npc.cpp)
- Range: `fight_range_base[tg.guild] + fight_range_base[npc.guild] + weaponRange`, where `weaponRange = fight_range_1ha/2ha[guild] + item.range` (fist: `fight_range_fist`); G-range adds `fight_range_g`; closeup = 0.75 × base sum. Distance is 3D between transformed skeleton roots (`fightDistanceTo`, root of `rootTr`, not BIP01 as the scripts say). Angle test uses `collosionCenter` vectors vs `rotationRad`. Comment in code: rusty sword range 70 hits a bloodfly, 60 does not. — [OpenGothic/common/game/fightalgo.cpp:259-380](OpenGothic/common/game/fightalgo.cpp); [npc.cpp:1540-1556](OpenGothic/common/world/objects/npc.cpp)
- `Item::swordLength()` is just the script `range` field. — [OpenGothic/common/world/objects/item.cpp:375-377](OpenGothic/common/world/objects/item.cpp)

**Damage (G2 formula)**
- `DamageCalculator::swordDamage` (G2): for each damage type in the weapon mask, `vd = max(STR + damage[i] − protection[i], 0)`; if `hitchance[talent] <= rand(100)` (i.e. non-critical) `vd = (vd−1)/10`; monsters without weapon always "crit"; protection <0 = immune. G1 branch uses `criticalDamageMultiplyer`. — [OpenGothic/common/game/damagecalculator.cpp:183-243](OpenGothic/common/game/damagecalculator.cpp)
- G2 minimum damage `MinDamage = 5` applied to any non-invincible hit. — [damagecalculator.cpp:36-38](OpenGothic/common/game/damagecalculator.cpp); [damagecalculator.h:14](OpenGothic/common/game/damagecalculator.h)
- Arena reference profile: both fighters 300 HP, STR/DEX 60, 1H chance 60 %, master 1H anims, `ItMw_1h_Common_01`, edge/blunt protection 30. — [docs/npc-body-arena.md](docs/npc-body-arena.md)

**Defence and hit reactions**
- `takeDamage(other,b,ignoreDefence)`: evade if `pose.isJumpBack` and attacker within ±90°; block if attacker is not a weaponless monster, attacker within ±30° focus and `pose.isDefence`. Blocked/evaded melee calls `emitBlockEffect` and skips damage entirely. — [npc.cpp:3316-3347](OpenGothic/common/world/objects/npc.cpp)
- On damage: `lastHitType` 'A'/'B' = from front/back; `FightAlgo::onTakeHit` clears the fight queue, `implFaiWait(0)`; if body state is interruptible (or RUN/NONE) and `bodystate_interruptable_override==0`, `visual.interrupt()` and stumble `T_STUMBLE`/`T_STUMBLEB` by side; then `hitReaction` (procedural Flinch); FLY damage → `mvAlgo.accessDamFly` knockback. — [npc.cpp:3364-3470](OpenGothic/common/world/objects/npc.cpp); `onTakeHit` [fightalgo.cpp:248-253](OpenGothic/common/game/fightalgo.cpp)
- `Npc::hitReaction` builds a `Flinch::Hit` (direction from arrow or from attacker + side swing for ATTACKL/R, height, twist); Flinch rotates waist/chest/head on top of any animation. — [npc.cpp:3497-3530](OpenGothic/common/world/objects/npc.cpp); [OpenGothic/common/graphics/mesh/flinch.h:1-40](OpenGothic/common/graphics/mesh/flinch.h)
- Physics doc (decisions table): "Бой — трогаем последним: физика ломает тайминги комбо". Stage 7 "Бой — под вопросом: отталкивание оружием, физический блок, удары по направлению клинка. Только после оценки баланса." — [docs/physics.md:69, 323-325](docs/physics.md)

### Inferences
- A physics melee would replace exactly two points: the hit test at `commitDamage` (range+angle → weapon sweep vs HitZones/ragdoll bodies) and the defence test in `takeDamage` (pose windows → blade-vs-blade contact). Everything upstream (FightAlgo queue, combo windows, `faiWaitTime`) is time-based and could be kept initially.
- Because hits are a single frame and target-only, anything like glancing hits, multi-target swings, wall bounce or blade-on-blade parry has no existing data path; the `HIT_LIMB`, `HIT_DIRECTION`, `DAMAGE_MULTIPLIER` MDS events are parsed but ignored (`npc.cpp:3685-3692` are empty `case` branches) — possible hooks.

### Gaps
- Exact per-swing OPTIMAL_FRAME/combo-window frames for every 1H/2H skill level were not extracted (only the S_1HATTACK / T_1HPARADE metadata in the melee reference report).
- Hero-side combo/turn speed details in `playercontrol.cpp` were only skimmed.

## 2. What physics already exists for characters

### Takeaway
A lot: Jolt v5.6.0 replaces Bullet; every human/beast within 20 m has a **kinematic capsule rig (24 parts for a human)** that follows the final animation pose; arrows switch the hit limb chain to **dynamic + Jolt SwingTwist position motors** that drive it back to the animation (an own variant of `DriveToPoseUsingMotors`); death/KO give full ragdolls with save/load, buoyancy and get-up via AnimBlend; the hero, human NPCs and ground monsters within 20 m move with **`CharacterVirtual`** that only resolves collisions for the animation's root motion. The weapon itself is a visual mesh bound to a slot bone, with no body.

### Cited Findings
- Stack summary and toggles (`ragdoll, living_bodies, arrow_physics, arrow_sticking, spell_physics, movable_vobs, destruction, physics_noise, buoyancy, character_virtual, npc_character_virtual, monster_character_virtual, cloth`), all on by default. — [docs/physics.md:5-23, 303](docs/physics.md)
- Jolt vendored at `e77f1755…` (v5.6.0), MIT. — [OpenGothic/lib/VENDORED.md:14](OpenGothic/lib/VENDORED.md)
- Fixed step 1/60 s with accumulator, max 2 steps per frame; with ragdolls present each step uses 2 internal collision steps (1/120 s) because CCD stopped a shin later than the thigh. — [docs/physics.md:130, 243](docs/physics.md); [OpenGothic/common/physics/collisionworld.cpp:2100-2115](OpenGothic/common/physics/collisionworld.cpp)
- **Ragdoll schema** (`common/physics/ragdoll.cpp`): parts = StaticCompound of HitZones capsules per bone; joints `SwingTwistConstraint` (knee/elbow as hinge, shoulder/hip cone, spine/neck small cone); mass split by capsule volume of a species mass (human 75 kg, orc 150, troll 600, wolf 40, shadowbeast 250…); friction 0.55, restitution 0, damping lin 0.12/ang 0.3, `LinearCast` CCD, inertia ×2, 12 velocity / 10 position steps per part and joint. — [OpenGothic/common/physics/ragdoll.cpp:400-480, 1521-1537](OpenGothic/common/physics/ragdoll.cpp); design [docs/physics.md:132-154](docs/physics.md)
- Root that carries several limbs gets its own torso part (bloodfly fix 2026-10-22). — [docs/physics.md:9, 33](docs/physics.md)
- **Living (kinematic) rigs**: `kinematicSettings` copies parts as Kinematic, layer `C_LivingBody`, no gravity, no constraints; created <20 m, removed >22 m; follow pose after IK/aim/flinch via `MoveKinematic`; push items and lying bodies; not part of gameplay rays; absent during interactive-object use. — [ragdoll.cpp:604-620, 1110-1147](OpenGothic/common/physics/ragdoll.cpp); [npc.cpp:1304](OpenGothic/common/world/objects/npc.cpp); [docs/physics.md:265](docs/physics.md); test scene "10 тел / 240 частей" [reports/physics/2026-10-15-living-npcs/validation-notes.md](reports/physics/2026-10-15-living-npcs/validation-notes.md)
- **Active ragdoll for arrow hits**: on first hit `ensureMotors` swaps the kinematic rig for the full constrained ragdoll (constraints disabled); `startReaction` makes the hit chain (arm/leg/head chain; torso hit = spine only, pelvis hits redirected to nearest spine) Dynamic on layer `C_ActiveBody`, enables only those joints; `driveMotors` sets swing+twist motors to `EMotorState::Position`, spring `FrequencyAndDamping` 4→6 Hz, damping 1.0, torque limit `partMass × 8 × (1 − 0.9·strength·(1−recovery))` N·m, target = parent⁻¹·child from animation targets ("same body-relative convention as DriveToPoseUsingMotors"); reaction holds 0.12 s, fades over 1.1 s (smoothstep), then the chain snaps back to kinematic. Pelvis/gameplay root never dynamic. — [ragdoll.cpp:622-747, 1149-1166](OpenGothic/common/physics/ragdoll.cpp)
- Arrow impulse = arrow mass × speed into the HitZones bone; displayed physical pose kept separate from the query pose, so `aim`/`manual-aim` hits unchanged; chest test SPINE2 13.165° → 0.16° before reset at 4.01 N·s. — [docs/physics.md:266](docs/physics.md); [reports/physics/2026-10-15-arrow-reaction/validation-notes.md:5-34](reports/physics/2026-10-15-arrow-reaction/validation-notes.md); call site `Npc::queueArrowPhysics` [npc.cpp:1339-1373](OpenGothic/common/world/objects/npc.cpp)
- Pose blending of physics: `Pose::setRagdollPose` with mask and weight, `applyRagdollPose` replays the procedural state at origin to avoid precision loss; `clearRagdollPose(blendFromPhysical)` and `applyRagdollReturn` for return to animation. — [pose.cpp:600-840](OpenGothic/common/graphics/mesh/pose.cpp)
- Death/KO ragdoll creation carries the root velocity of the last live sample (capped 3000 cm/s). — [npc.cpp:954-980](OpenGothic/common/world/objects/npc.cpp)
- Get-up: settle → pelvis axis decides back/belly → `S_WOUNDEDB`/`S_WOUNDED` with AnimBlend from body pose → `T_WOUNDED_2_STAND`; quest guard returns living humans to last stable dry point after >10 m fall/water. — [docs/physics.md:149-153, 264](docs/physics.md)
- Spells: wind fist impulse to the hit NPC, whirlwind lift, fireball explosion, ice block holds kinematic; mass/inertia respect species and scale (wind fist does not throw a troll). — [docs/physics.md:268](docs/physics.md)
- **HitZones**: capsules along bones built from bind pose by bone names, thickness fitted to skinned mesh; 31 creatures; zones None/Body/Head/Limb. — [OpenGothic/common/graphics/mesh/hitzones.h:1-40](OpenGothic/common/graphics/mesh/hitzones.h); [docs/physics.md:96](docs/physics.md); [docs/manual-aim.md](docs/manual-aim.md)
- **CharacterVirtual**: capsule from MDH size, padding 1 cm, predictive contact 5 cm, enhanced internal edge removal; `ExtendedUpdate` with patched `WalkStairs` (up to 4 intermediate probes); push strength `min(mass·g, 450 N)`; `MoveAlgo` keeps states/animations and gives desired displacement from root motion (`animMoveSpeed`); jumps, falls, climb, swim keep the old path; NPC–NPC blocking via old ellipsoids, no mutual impulses; moving supports carry the character (error ≤0.011 cm). — [collisionworld.cpp:850-918](OpenGothic/common/physics/collisionworld.cpp); [OpenGothic/common/game/movealgo.cpp:30, 974](OpenGothic/common/game/movealgo.cpp); [docs/physics.md:295-303](docs/physics.md)
- **AnimBlend** = inertialization (Bollo, GDC 2018) with velocity kept, one blend time per switch frame. — [OpenGothic/common/graphics/mesh/animblend.h:14-28](OpenGothic/common/graphics/mesh/animblend.h)
- **FootIk** (ground rays under ankle/toe, crouch, stance floor), **Gaze**, **Lean**, **Aim**, **BowDraw** are procedural passes in `Pose`. — [OpenGothic/common/graphics/mesh/footik.h:21-65](OpenGothic/common/graphics/mesh/footik.h); [pose.cpp:574-597](OpenGothic/common/graphics/mesh/pose.cpp)
- **Weapon attach**: `MdlVisual::updateWeaponSkeleton` binds the sword mesh to `ZS_RIGHTHAND` in fight mode, else `ZS_SWORD`/`ZS_LONGSWORD`; `AttachBinder` is just a node-index map for a mesh on a skeleton. No collision shape for weapons. — [OpenGothic/common/graphics/mdlvisual.cpp:460-461, 757-770](OpenGothic/common/graphics/mdlvisual.cpp); [OpenGothic/common/graphics/mesh/attachbinder.h](OpenGothic/common/graphics/mesh/attachbinder.h)
- **Melee vs vobs at OPTIMAL_FRAME**: `DynamicWorld::meleeMovableVobs` casts a ray fan (yaw ±60° for weapons / ±20° fists in 10° steps, pitch −40..+20 in 20° steps) to range `base + 1h/2h range + weapon range`; first hit per ray; damage from `destructible-vobs.csv` rule; impulse = damage × 0.02 at hit point; NPC damage path untouched and uses the same RNG. — [OpenGothic/common/physics/dynamicworld.cpp:1363-1405](OpenGothic/common/physics/dynamicworld.cpp); [npc.cpp:3267-3292](OpenGothic/common/world/objects/npc.cpp); [docs/physics.md:310](docs/physics.md)
- Open, not done: cloaks/robes cloth with HitZones collisions; "бой" stage 7 not started; open questions include fall/wall damage of bodies and dragging corpses. — [docs/physics.md:321-325, 359-364](docs/physics.md)

### Inferences
- The kinematic→motor-driven swap already exists per chain; extending it to "whole body powered ragdoll except pelvis" is an incremental step, but the current design deliberately keeps the gameplay root and pelvis kinematic and lets `CharacterVirtual` own locomotion. A fully physics-driven character (balance, root from physics) is not started anywhere.
- Weapons need a new body (e.g., kinematic capsule/convex on `ZS_RIGHTHAND`) before any blade-direction hit or physical block can be computed; HitZones capsules are a ready target set.

### Gaps
- No measurements of a full-body motor-driven rig (all 24 parts dynamic) exist in our reports.

## 3. The NPC combat body model P30 (GRU, PPO)

### Takeaway
P30 is a recurrent policy that chooses **one of 12 discrete native combat primitives at 20 Hz** (`continue, approach, turn_to_target, strafe_left/right, jump_back, attack_front/left/right, block, wait_200, wait_300`); the engine executes animation, movement, collisions and damage. It was trained in a deterministic headless arena of the real engine on Ryzen (BC → self-imitation → recurrent PPO with self-play), and its features/masks are built from exactly the current abstract rules (range/angle booleans, `prehit`/`defence`/`def_window` flags, gates). Any change of combat mechanics changes the environment and requires re-collecting, re-training and re-gating.

### Cited Findings
- Action IDs 0–11 as above; pure `FightAlgo` resolver builds masks (sword/target, gates, anim availability, angle, range, collision ray); one executor `Npc::implAttack`. — [docs/npc-body-api.md, "Примитивы, masks и события"](docs/npc-body-api.md); [body-service/src/gothic_body/schema.py:10](body-service/src/gothic_body/schema.py)
- Actor features: self hp, weapon state/length, skill, dead/down/air/swim, `attack_anim, prehit, defence_active, jumpback_active, def_window, par_window, interruptible`; target visible, yaw error, `fight_distance_cm`, `attack_range_cm, g_range_cm, closeup_range_cm, in_w_range, in_g_range, in_closeup, focus_30, focus_5, melee_line_clear`, target hp/weapon/attack/prehit/defence; velocities; gates `fai/ani/ai/output_wait_remaining_ms`; 16 local obstacle rays. — [scripts/body_trajectory_dataset.py:155-215](scripts/body_trajectory_dataset.py)
- Architecture: encoder MLP-256 → GRU-256 → MLP-128 → 12 masked logits (BC); PPO adds MLP critic over features + GRU state. — [docs/npc-body-bc.md:23](docs/npc-body-bc.md); [reports/npc-body/2026-10-16-ppo/README.md:29-34](reports/npc-body/2026-10-16-ppo/README.md)
- Reward: episode score = win +1 / loss −1 + 0.5 × (own HP − opponent HP fraction) − 0.2 × duration on win; PPO distributes it per decision, γ = 0.99/s, λ = 0.95, clip 0.2, entropy 0.003, KL 0.02 to BC. — [reports/npc-body/2026-10-15-self-imitation/README.md:17-18](reports/npc-body/2026-10-15-self-imitation/README.md); [reports/npc-body/2026-10-16-ppo/README.md:29-34](reports/npc-body/2026-10-16-ppo/README.md)
- P30 = PPO iteration 30 of 40 (1024 duels/iteration, half vs native AI, half vs own pool, half near a wall); 40 iterations = 73 min on 10 workers, 40 960 duels, 826 765 decisions. 2048/2048 wins, HP 294.5 open / 281.4 near wall; made working model by the user 2026-10-17. — [docs/npc-body-control-plan.md:540-554](docs/npc-body-control-plan.md); [reports/npc-body/2026-10-16-ppo/README.md:59-64](reports/npc-body/2026-10-16-ppo/README.md)
- Arena: bandit `BDT_1003_BANDIT_M` vs `PAL_255_RITTER`, 1H, 30 Hz fixed sim, observations 20 Hz, lockstep or realtime. — [docs/npc-body-arena.md](docs/npc-body-arena.md)
- Throughput work: frozen world + isolated script RNG, fork-prefix resets 2–3 ms, ~790 decisions/s ceiling on Ryzen 3950X; runner Jolt physics on its own thread (`OPENGOTHIC_PHYSICS_THREADS=0`) for fork-reset; Jolt built `CROSS_PLATFORM_DETERMINISTIC`, AVX2 build for runner. — [docs/npc-body-control-plan.md:491-503, 556-590](docs/npc-body-control-plan.md)
- **Sensitivity to engine physics, measured**: merging Jolt + `npc_character_virtual` made only 13/64 open and 8/64 wall duels replay tick-for-tick; strength unchanged (2048/2048); but runner step +4.6 ms (foot-IK rays via SSE2 Jolt) caused 100/2361 stale commands and failed the 4-NPC load gate (0.918 < 0.95) until fixed with x86-64-v3 (0.985). A new native baseline had to be created. — [docs/npc-body-control-plan.md:556-590](docs/npc-body-control-plan.md)
- Lessons: lockstep must equal realtime schedule (one-tick offset flipped conclusions); geometry mix matters (open-arena rounds lost the wall sidestep); after iteration 30 wall play degraded. — memory notes in [docs/npc-body-control-plan.md:518-532](docs/npc-body-control-plan.md)
- Live pilot: up to 4 NPC with 1H weapons fighting the hero/companion; 2H, bows, magic, monsters stay native; known weakness: loops `wait_200` vs a passive hero. — [docs/npc-body-control-plan.md:5-12, 156](docs/npc-body-control-plan.md)
- Roadmap 9.4 lists draw/sheath weapon, bows, memory tasks; "полное управление суставами, новый физический движок … не входят в первую версию". — [docs/npc-body-control-plan.md:198, 614-627](docs/npc-body-control-plan.md)

### Inferences
- Physics melee would break P30 at three levels: (1) the action space (no direction/strength of swing, no physical block), (2) features that encode the old rules (`in_w_range`, `focus_30`, `def_window` become meaningless or need new physical analogues), (3) environment dynamics (hits/blocks probabilistic by contact). Keeping the 12 primitives and only changing hit resolution would need a fresh native baseline, re-run of `body_schedule_equivalence.py`, re-collection and at least a PPO fine-tune plus full gate.
- Runner cost is critical: training relies on ~2 ms ticks and determinism. Extra dynamic bodies per fighter (2 × 24 parts plus weapons, 1/120 s substeps) will reduce decisions/s and must stay deterministic (`CROSS_PLATFORM_DETERMINISTIC`, single-threaded physics in runner).
- The existing pipeline (headless deterministic runner, fork resets, self-play PPO, gates) is a reusable asset for training any future lower-level physics controller too.

### Gaps
- Exact parameter count of P30 not found in the notes read.
- No experiment yet of P30 under any changed hit rule.

## 4. Performance already measured and budgets

### Takeaway
Budget for physics is **p95 ≤ 2 ms per frame on the M1 Pro**, FPS loss ≤5 % vs OFF in the same scene. Measured: 10 living kinematic rigs (240 parts) cost p95 ≈ 0.29 ms; 20 simultaneous deaths (480 dynamic parts) were within budget; arrow active reaction raised step p95 to 0.68 ms; NPC CharacterVirtual for 20 NPC +0.18 ms p95 / −3.6 % FPS. Typical scene FPS on the M1 Pro is ~55–80.

### Cited Findings
- Budget: "Бюджет физики — p95 не больше 2 мс на кадр, максимум справочный" (M1 Pro). — [docs/physics.md:353](docs/physics.md)
- Stage 2 (20 NPC die at once, 30 s): ON 71.67 FPS / frame p95 20.16 ms vs OFF 66.59 / 23.29 ms; 20 corpses, 480 parts, all slept naturally. — [reports/physics/2026-10-14-jolt-stage2/validation-notes.md:9-16](reports/physics/2026-10-14-jolt-stage2/validation-notes.md)
- Living rigs, 10 NPC (240 parts), M1 Pro 3 pairs: median OFF 73.61 FPS, ON 77.39; physics ON mean/p95/max 0.148–0.169 / 0.281–0.338 / up to 35.7 ms. — [reports/physics/2026-10-15-living-npcs/validation-notes.md:13-21](reports/physics/2026-10-15-living-npcs/validation-notes.md)
- Arrow active reaction bench: full `CollisionWorld::tick` ON 0.320 / 0.684 / 1.986 ms vs OFF 0.210 / 0.258 / 0.403 (mean/p95/max). — [reports/physics/2026-10-15-arrow-reaction/validation-notes.md:19](reports/physics/2026-10-15-arrow-reaction/validation-notes.md)
- Hero CharacterVirtual: FPS −1.17 %, p95 physics +0.0148 ms; NPC CV (20 NPC): p95 +0.18 ms, FPS −3.56 %; 20 monsters: p95 −0.32 ms, FPS +9.8 % (preliminary). — [docs/physics.md:295-303](docs/physics.md)
- Bodies in water (10 bodies): p95 0.731 / 0.584 ms ON/OFF, FPS 132.9 / 143.3 (−7.26 %); cloth on Mac 59.30 → 54.27 FPS (−8.49 %), accepted by the user; physics noise p95 0.378 ms. — [docs/physics.md:183-184, 14, 282](docs/physics.md)
- Runner (Ryzen): Jolt code per step 6.1 ms (was 8.4 SSE2; Bullet 7.7), full step 22.9 ms CPU in the full-world runner; foot-IK rays +1.7 ms, pose +0.6 ms, world +1.0 ms after Jolt merge. — [docs/npc-body-control-plan.md:576-590](docs/npc-body-control-plan.md)
- Mac bench runs at 73–77 FPS on M1 Pro (≈2.5× realtime with 33 ms steps). — [docs/navigation.md:202](docs/navigation.md)
- P30 latency budgets: model step p95 ≤5 ms, engine snapshot → arbitration p95 ≤100 ms; service p95 ≤20 ms in group e2e. — [docs/npc-body-combat-contract.md:209-216](docs/npc-body-combat-contract.md); [docs/npc-body-control-plan.md:62](docs/npc-body-control-plan.md)
- Rules: models never run on the Mac; heavy runs on Ryzen under a shared lock. — [AGENTS.md](AGENTS.md)

### Inferences
- Roughly: a living kinematic human ≈ 0.03 ms p95 in our scenes; one active chain on motors adds ≈0.4 ms p95 in the bench. A duel with two fully motor-driven 24-part rigs at 120 Hz internal substeps is unmeasured; extrapolation from arrow reaction suggests ~1 ms p95 per active rig, which would consume half the 2 ms budget with 2 fighters — needs measurement.

### Gaps
- No per-ragdoll isolated cost breakdown (all numbers are scene totals); no measurement of motors on all joints; no M1 Pro measurement of the runner.

## 5. Jolt capabilities relevant to a physics character / melee

### Takeaway
Jolt (we vendor v5.6.0) has everything needed for an active ragdoll: `Ragdoll` with `DriveToPoseUsingMotors` and `DriveToPoseUsingKinematics`, `SkeletonMapper` (high-detail animation skeleton ↔ low-detail ragdoll), SwingTwist motors with Position/Velocity modes and frequency-damping or stiffness springs plus torque limits, `GroupFilterTable` for intra-ragdoll collision filtering, and cross-platform determinism (~8 % slower). Jolt's own perf scenes use 160 ragdolls (3680 bodies), but no timings are published in the docs I found.

### Cited Findings
- `Ragdoll` API: `SetPose`, `GetPose`, `ResetWarmStart`, `DriveToPoseUsingKinematics` ("setting velocities on each of the bodies so that it will reach inPose in inDeltaTime"), `DriveToPoseUsingMotors` ("activating the motors on each constraint"); `RagdollSettings::Stabilize`, `DisableParentChildCollisions` (GroupFilterTable, unique GroupID per ragdoll), additional non parent-child constraints. — vendored header [OpenGothic/lib/JoltPhysics/Jolt/Physics/Ragdoll/Ragdoll.h:20-186](OpenGothic/lib/JoltPhysics/Jolt/Physics/Ragdoll/Ragdoll.h)
- Motor states Off/Velocity/Position/PositionAndVelocity; Position: "force = stiffness * (target_position - current_position) - damping * current_velocity"; default spring FrequencyAndDamping 2 Hz, damping 1; separate min/max force and torque limits. Spring modes include MassNormalizedStiffnessAndDamping ("acceleration mode"). — [OpenGothic/lib/JoltPhysics/Jolt/Physics/Constraints/MotorSettings.h:18-79](OpenGothic/lib/JoltPhysics/Jolt/Physics/Constraints/MotorSettings.h); [SpringSettings.h:14-51](OpenGothic/lib/JoltPhysics/Jolt/Physics/Constraints/SpringSettings.h)
- `SkeletonMapper`: "map a low detail (ragdoll) skeleton to a high detail (animation) skeleton and vice versa", with 1-to-1 joints, chains and unmapped joints. — [OpenGothic/lib/JoltPhysics/Jolt/Skeleton/SkeletonMapper.h:12-52](OpenGothic/lib/JoltPhysics/Jolt/Skeleton/SkeletonMapper.h); [Jolt docs](https://jrouwe.github.io/JoltPhysics/index.html)
- Samples: "Kinematic Ragdoll", "Ragdoll Driven to Animated Pose" (motors match a sprint animation), "Skeleton Mapper", "160 Ragdolls Driven to Pose" (gives them "stiffness"). — [Jolt Samples.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/Samples.md)
- Perf test scene `Ragdoll`: 16 piles of 10 ragdolls (3680 bodies, i.e. 23 bodies per ragdoll) with motors active; `RagdollSinglePile` 160 ragdolls. No published timing in the doc. — [Jolt PerformanceTest.md](https://raw.githubusercontent.com/jrouwe/JoltPhysics/master/Docs/PerformanceTest.md)
- Determinism requires same binary and same API call order; `CROSS_PLATFORM_DETERMINISTIC` "approximately 8% slower"; listener callbacks and `GetActiveBodies` order are non-deterministic; constraints need unique `mConstraintPriority`. — [Jolt docs](https://jrouwe.github.io/JoltPhysics/index.html)
- Our choice of Jolt was explicitly motivated by `Ragdoll::DriveToPoseUsingMotors`, multithreading, soft bodies, `CharacterVirtual`, buoyancy and determinism. — [docs/physics.md:65](docs/physics.md)
- We already use our own motor drive instead of `DriveToPoseUsingMotors` to avoid "another SkeletonPose solve" and only on the active chain. — [ragdoll.cpp:706-727](OpenGothic/common/physics/ragdoll.cpp)

### Inferences
- Jolt motors are pose-tracking PD controllers without balance; a standing physics character would need either root/pelvis kept kinematic (our current pattern, "soft keyframed"), or an added balance controller/learned policy driving motor targets. Jolt does not provide balance or locomotion control.
- Our ragdoll human (24 parts) is close to Jolt's perf-test ragdoll (23 bodies), so Jolt's 160-ragdoll scene is a reasonable stress reference if we benchmark it ourselves.

### Gaps
- No authoritative published per-ragdoll timings for Jolt found; Jolt's multicore-scaling numbers were not retrieved.
