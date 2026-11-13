#pragma once

#include "asset/modeltypes.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

// AnimSample: the pose of a clip between two of its keys (reports/engine/2026-11-13-smooth-sampling).
//
// Gothic stores many clips with fewer keys than their source had: 10 a second for the dialog gestures, the
// stances with a weapon, sitting, work at objects; 15 for dances and many moves of creatures; 5 for a few
// (FPS:10 in the model script, a saving of 2001). Between two keys the pose went on a straight line: a joint
// kept one speed for the whole 0.1 s and changed it in one frame at the key - at 30 frames a second, steps of
// three frames in the arms and the trunk. Such a clip now goes on a cubic curve through its keys (Hermite, the
// tangent at a key from its two neighbours, Catmull-Rom), so the speed has no jump at a key.
//
// The tangent at a key is not longer than three times the shorter of the two chords at it (the bound of
// Fritsch and Carlson for a monotone cubic, on the length of the whole vector): next to a hold it is zero, so a
// hold stays a hold, and a joint that stops sharply does not swing past its key (plain Catmull-Rom took a hand
// up to 2.8 cm past it, this one 1.7 at most). The pose at a key is the key, as it was.
//
// Clips keyed 25 times a second or more stay on the straight line: at 30 frames a second nearly every frame
// crosses a key there and the curve changes nothing (measured), and blows and walks keep their frames.
// OPENGOTHIC_ANIM_SMOOTH=0 (the bench: `sample off`) brings the straight line back for all.
namespace AnimSample {

using Sample = asset::AnimationSample;

// keys a second: a clip keyed slower than this goes on the curve
constexpr float SparseRate = 25.f;

bool enabled();
void setEnabled(bool e);

// A clip of this key rate and length is one for the curve (its data, whatever the switch says)
bool sparse(float fpsRate, uint32_t numFrames);

// The keys around a time of a clip, in the order they play
struct Keys final {
  size_t a = 0, b = 0;            // the pose is between these, t of the way from a to b
  size_t before = 0, after = 0;   // the key played before a and the one played after b
  bool   hasBefore = false;       // a clip that is no loop has no key before its first
  bool   hasAfter  = false;       // nor one after its last
  // a loop: how many cycles of the clip lie between a and the other key (the root of a loop that carries the
  // body is that many travels further on)
  int    cycBefore = 0, cycB = 0, cycAfter = 0;
  float  t = 0;
  };

// ms - the time since the clip began; count - the frames of one cycle of a loop (AnimData::loopFrames), else
// the frames of the clip
Keys   keys(uint64_t ms, float fpsRate, size_t count, bool loop, bool reverse);

// The pose t of the way from a to b on the curve; before and after are the keys around them as played, null
// where the clip has none: its outer chord then goes on as it is
Sample mix(const Sample* before, const Sample& a, const Sample& b, const Sample* after, float t);

// How far the curve through these positions is from the straight line from a to b at t. The body travels along
// that line (Animation::Sequence::translateXZ), the legs are on the curve: the pose's root is shifted by this,
// or a planted foot would slide by it
asset::Vec3 offLine(const asset::Vec3* before, const asset::Vec3& a, const asset::Vec3& b,
                    const asset::Vec3* after, float t);

}
