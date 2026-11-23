#pragma once

// The island's hero (engine layers, step 8.6): a man of Gothic's measure as a capsule, without a skeleton. The
// ground of the island is made for these numbers (the way up from the beach stays under WalkSlope), and the
// program's capsule walks by them. Metres, seconds, degrees, kilograms.

namespace island {

// The man: Gothic's navigation takes him 180 cm tall (common/game/navigationmemory.h, bodyHeightCm).
constexpr float BodyHeight   = 1.80f;
// Half the breadth of the shoulders of a man of 1.8 m (about 0.46 m, anthropometric tables) with a little clothing.
constexpr float BodyRadius   = 0.25f;
// The highest ledge he steps onto without a jump: the human guild's step in Gothic, 50 cm as MoveAlgo::stepHeight
// falls back to and navigation keeps (navigationmemory.h, stepCm).
constexpr float StepHeight   = 0.50f;
// The steepest ground he walks up: the angle of repose of dry sand is about 34 degrees, and the island's hill keeps
// a way from the beach to the summit under it (tests/island_content_test.cpp). Steeper ground he slides down.
constexpr float WalkSlope    = 35.f;
// Gothic's hero walks at the root motion of S_WALKL, 1.37 m/s (people's preferred speed is 1.3-1.4 m/s), and runs at
// that of S_RUNL, 3.45 m/s.
constexpr float WalkSpeed    = 1.37f;
constexpr float RunSpeed     = 3.45f;
// A standing jump of an untrained man: the centre of mass rises 0.4-0.6 m; the capsule rises by the middle of it.
constexpr float JumpHeight   = 0.50f;
constexpr float Gravity      = 9.81f;
// He wades into the sea to about his hips and goes no deeper: there is no swimming on the island.
constexpr float WadeDepth    = 0.80f;
constexpr float BodyMass     = 75.f;
// The camera follows his pelvis, as Gothic's follows the root bone BIP01.
constexpr float PelvisHeight = 0.95f;

}
