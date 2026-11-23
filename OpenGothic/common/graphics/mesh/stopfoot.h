#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Which foot a transition out of a moving loop starts from.
//
// Gothic names its moving loops by the left foot: S_RUNL, S_WALKL. A loop begins with the left foot down ahead, and
// half a cycle later the right foot is there. The model scripts name transitions out of a loop for both feet -
// T_RUNL_2_RUN and T_RUNR_2_RUN, T_WALKL_2_WALK and T_WALKR_2_WALK - and the left one begins on the loop's first
// frame, the right one half a cycle on. Pose::startAnim asks here which of the two is nearer to where the loop is.
//
// Only a right transition of our own content is taken - one that carries the flag SETTLE. The base has a single
// right clip, T_WALKR_2_WALK, and it comes from another file than the walk loop (HUM_WALKLOOPEND_A01 against
// HUM_WALKLOOP_A02): played at the loop's half it jumps the pose by 28 cm where the left clip jumps it by 13
// (bench hero-run-startstop, take walk_stops). So with the base's clips alone nothing changes.
//
// OPENGOTHIC_STOPFOOT=0 turns the choice and the early way out of a stop off: the left transition plays from any
// phase and to its end, as before.
namespace StopFoot {
  // The loop's phase, 0..1 from its first frame: has it gone a quarter past its start and not yet a quarter short
  // of it? Then the right foot's frame is the nearer one.
  bool        isRight(float phase);

  // The phase of a loop of `frames` frames at `fps` that has played `elapsedMs`.
  float       phase(uint64_t elapsedMs, float fps, uint32_t frames);

  // "RUNL" -> "RUNR", "1HWALKL" -> "1HWALKR"; empty for a name that is not a left loop's.
  std::string rightName(std::string_view leftLoop);

  // A transition that stops the body (flag SETTLE, own content only): the first frame from which the root moves
  // slower than 35 cm/s along the ground to the clip's end. `way` is the root's place on the ground at every frame
  // but the last, counted from the first frame; `endX`, `endZ` - at the last. From that frame on the clip only
  // straightens the body up (it may rock back a few cm), and Sequence::canInterrupt lets any other animation in:
  // the player runs on, turns or draws a weapon without waiting for the pose to settle. A clip that never rests
  // gives the number of its frames.
  uint32_t    restFrame(const std::vector<std::array<float,2>>& way, float endX, float endZ, float fps);

  bool        enabled();
  }
