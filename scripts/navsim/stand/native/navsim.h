// C ABI of the nav-sim stand library (docs/nav-sim.md, task S1): the static
// world from NAVSTAT1 in Embree and N agents, each with the engine's
// NavigationEye core (OpenGothic/common/game/navigationeye.cpp, compiled as is).
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

#ifdef __cplusplus
}
#endif
