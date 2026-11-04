// C ABI of the nav-sim stand library (docs/nav-sim.md, tasks S1 and S2): the
// static world from NAVSTAT1 in Embree and N agents, each with the engine's
// NavigationEye core (OpenGothic/common/game/navigationeye.cpp, compiled as is)
// and a Jolt CharacterVirtual body on the same dump (motion.cpp).
// Units are world centimetres, Y up; game time is milliseconds. Every call
// returns a negative value on error; navsim_last_error() names the cause.
// One world is driven from one caller thread; the library parallelises inside.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct navsim_world navsim_world;

// Eye output slots per agent, agent-major, in the layout of the gpumem eye
// inputs (scripts/navsim/gpumem/memory_kernels.h, Ctx::rayOrigin..rayTime):
// R = navsim_eye_slots() slots, the fresh rays followed by the cached far rays.
// Any pointer may be null; that field is then skipped.
typedef struct navsim_slots {
  float* origin;      // [N*R*3]
  float* point;       // [N*R*3]
  float* distance;    // [N*R]
  float* normal_y;    // [N*R]
  uint8_t* kind;      // [N*R] NavigationEye::HitKind
  uint8_t* hit;       // [N*R]
  uint32_t* time;     // [N*R] sampled game ms (gpumem keeps 32 bits)
  // Not read by gpumem; for checks against the engine.
  float* direction;   // [N*R*3]
  uint8_t* complete;  // [N*R]
} navsim_slots;

// 238 rays (layout 250, walking gaze) and 476 memory slots per agent.
int32_t navsim_eye_rays(void);
int32_t navsim_eye_slots(void);

// static_bin: NAVSTAT1 (scripts/navsim/formats.py). Categories 1 landscape ->
// World, 2 static object -> StaticVob, 3 water -> Water, 4 kinematic object in
// its dump pose -> MovableVob (the engine's kind for movable C_Object bodies).
// threads: worker threads of the update pool, also used to build the BVH.
// On failure returns null and writes the reason to error (may be null).
navsim_world* navsim_world_create(const char* static_bin, int32_t agents, int32_t threads,
                                  char* error, size_t error_size);
void navsim_world_destroy(navsim_world* world);
int32_t navsim_world_agents(const navsim_world* world);
int64_t navsim_world_triangles(const navsim_world* world);
const char* navsim_last_error(const navsim_world* world);

// Rebuilds the pool. cpus (count entries, may be null) pins worker i to cpus[i] (Linux).
int32_t navsim_set_threads(navsim_world* world, int32_t threads, const int32_t* cpus);

// Head frame (origin, forward, right, up), feet position and game time of
// agents [first, first+count). Vectors are float[count*3].
int32_t navsim_set_poses(navsim_world* world, int32_t first, int32_t count,
                         const float* origin, const float* forward, const float* right,
                         const float* up, const float* position, const uint64_t* game_ms);
// The neutral head frame of a body yaw in degrees, built like the engine's
// perception replay: forward (cos, 0, sin), right (-sin, 0, cos), up +Y.
int32_t navsim_set_poses_yaw(navsim_world* world, int32_t first, int32_t count,
                             const float* origin, const float* position, const float* yaw_deg,
                             const uint64_t* game_ms);
// Forgets the eye history (cadence, far cache, gaze) of agents [first, first+count).
int32_t navsim_reset_eyes(navsim_world* world, int32_t first, int32_t count);

// Updates every agent's eye at its pose and time on the pool. An agent whose
// near period has not passed keeps its slots. out (may be null) receives all
// agents' slots, written by the workers. updated, far_updated and casts are
// optional [N] outputs. Returns the casts of this call.
int64_t navsim_eye_update(navsim_world* world, const navsim_slots* out,
                          uint8_t* updated, uint8_t* far_updated, uint32_t* casts);
// Copies the current slots of agents [first, first+count) to out (indexed from first).
int32_t navsim_eye_read(const navsim_world* world, int32_t first, int32_t count, const navsim_slots* out);

// ---- Ground motion (task S2) ----------------------------------------------
// A decision's keys pick the 100 ms translation from the root-motion table
// (scripts/navsim/motion_table.py), the turn is code (90 deg/s unarmed), and
// Jolt CharacterVirtual with the NPC capsule moves the feet over the static
// dump in three 33/33/34 ms ticks, in the engine's order. Vertical motion is
// physics only. Positions are feet in cm; yaw in degrees with forward
// (cos, 0, sin), turn < 0 increasing it (npckeys.cpp).

// The human NPC of the engine: capsule from the HUMANS bbox (dynamicworld.cpp:419),
// guild step_height 60 and slide_angle 50, NavigationMemory's default body
// height 180 for pose_limits, 90 deg/s turn without a weapon.
typedef struct navsim_motion_params {
  float radius_cm;
  float height_cm;
  float step_cm;
  float slide_angle_deg;
  float body_cm;
  float turn_deg_s;
  float mass_kg;  // pushes dynamic bodies only; the dump has none
} navsim_motion_params;

// Per-tick flags.
enum {
  NAVSIM_MOTION_SUPPORTED = 1,     // on walkable ground (CharacterVirtual OnGround)
  NAVSIM_MOTION_BLOCKED = 2,       // advanced <= 10% of the request, or no floor below (DynamicWorld::moveVirtual)
  NAVSIM_MOTION_FALLING = 4,       // neither walkable nor steep support
  NAVSIM_MOTION_STEEP = 8,         // on too steep ground: slides
  NAVSIM_MOTION_UNKNOWN_KEYS = 16, // the table has no entry for the keys: no translation
  NAVSIM_MOTION_STEPPED = 32,      // climbed a step this tick
};

// Tick outputs of agents, right-aligned in 3 slots per agent; the fields and
// units are those of StepBatch pose_* (scripts/navsim/stand/contract.py). Any
// pointer may be null. Slots before an agent's first tick are zeroed.
typedef struct navsim_ticks {
  float* position;  // [N*3*3] feet, cm
  float* forward;   // [N*3*3] body forward
  float* limits;    // [N*3*3] step cm, max slope deg, body cm
  uint32_t* ms;     // [N*3] game ms after the tick (low 32 bits)
  uint8_t* flags;   // [N*3] NAVSIM_MOTION_*
  float* yaw;       // [N*3] body yaw, degrees (not in StepBatch)
} navsim_ticks;

void navsim_motion_default_params(navsim_motion_params* out);
// Builds the shared Jolt meshes from the world's static.bin (categories 1, 2, 4;
// water is not solid) and `shards` PhysicsSystems with their own static bodies;
// the agents are split into contiguous shards, each stepped by one worker at a
// time. params may be null (defaults). Call once per world.
int32_t navsim_motion_init(navsim_world* world, const navsim_motion_params* params, int32_t shards);
int32_t navsim_motion_shards(const navsim_world* world);
// One level (-1..3) of the translation table: packed keys as motion_table.key_ids
// and the forward/right centimetres per 100 ms ([count*2]).
int32_t navsim_motion_table(navsim_world* world, int32_t level, int64_t count, const int64_t* keys,
                            const float* forward_right);
// Places agents [first, first+count), also mid-episode: feet, yaw, game ms and
// the key history. history (may be null: none) is int8 [count*3*4], the previous
// three decisions newest first as forward, strafe, turn, walk; walk < 0 marks a
// missing decision. held (may be null: 0) is the age, in decisions, that the
// newest previous keys had at their own decision (motion_table.key_ids).
int32_t navsim_motion_set_state(navsim_world* world, int32_t first, int32_t count, const float* position,
                                const float* yaw_deg, const uint64_t* game_ms, const int8_t* history,
                                const int32_t* held);
// The NPC's pace against the table, float [count*2]: run, walk (default 1, kept by
// set_state). Table translations are multiplied by it: NPCs walk and run at their
// own speeds (their animation sets), the table holds a pooled one.
int32_t navsim_motion_set_speed(navsim_world* world, int32_t first, int32_t count, const float* run_walk);
// Overrides the yaw only, keeping the body and its contacts.
int32_t navsim_motion_set_yaw(navsim_world* world, int32_t first, int32_t count, const float* yaw_deg);
// Keys of the next decision, int8 [count*4]: forward, strafe, turn in -1/0/+1, walk 0/1.
// They stay until replaced; every step is one decision.
int32_t navsim_motion_set_keys(navsim_world* world, int32_t first, int32_t count, const int8_t* keys);
// One decision of agents [first, first+count) on the pool: ticks (may be null:
// 3 each) is uint8 [count] in 0..3. out is indexed from first.
int32_t navsim_motion_step(navsim_world* world, int32_t first, int32_t count, const uint8_t* ticks,
                           const navsim_ticks* out);
// The current state: feet [count*3], yaw, game ms, the last tick's flags, and the
// last decision's table translation [count*2] and level (-2: none). Any may be null.
int32_t navsim_motion_read(const navsim_world* world, int32_t first, int32_t count, float* position,
                           float* yaw_deg, uint64_t* game_ms, uint8_t* flags, float* table_cm, int8_t* level);

#ifdef __cplusplus
}
#endif
