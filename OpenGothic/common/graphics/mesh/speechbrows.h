#pragma once

#include "facerig.h"
#include "lipsync.h"
#include "talkgestures.h"

#include <cstdint>
#include <memory>
#include <vector>

// The brows of the speaker follow the tone of its voice (speech_brows of the face profile; reports/content/
// 2026-11-hero-head/eyes.md, "План: брови по тону"): the brows go up on a syllable where the tone rises a few
// semitones and the voice is loud (an accent), from a moment before it; an angry or hostile line lowers them instead.
// Not on every accent: a gap after each move, and a chance by the seed of the npc and the line, the same every time.
// The syllables are found in the line's track as LipSync finds them while it plays, but ahead of the playback: the
// track's loudness and tone are known as its sound arrives (a sound file at once, a streamed voice twice as fast as it
// plays); what has not arrived in time is late and starts as it can. Only decides: FaceRig shows the moves.
class SpeechBrows final {
  public:
    struct Accent final {
      int64_t ms    = 0;      // the start of its syllable, ms of the line's sound
      float   rise  = 0;      // semitones the tone rises there
      float   loud  = 0;      // the syllable's loudest level, 0..1
      bool    moves = false;  // it moves the brows (past the gap, by chance)
      bool    shown = false;  // ...and has started to
      };

    struct Start final {
      uint64_t at       = 0;  // tick it starts at (at most now)
      float    strength = 0;  // 0..1 of the profile's weights
      };

    static constexpr uint64_t LateMs   = 50;  // a move found later than this starts now, not in the past
    static constexpr float    OctaveSt = 9;   // a jump of the tone this large is a mistake of an octave, not a rise

    // OPENGOTHIC_SPEECH_BROWS=0 switches them off (a profile without speech_brows has none anyway)
    static bool isEnabled();
    static void setEnabled(bool e);

    // the accents of a line in this mood lower the brows instead of raising them: angry, hostile
    static bool lowers(TalkGestures::Mood m) { return m==TalkGestures::Mood::Angry || m==TalkGestures::Mood::Hostile; }

    // the line's voice starts (LipSync::start); seed: of the npc and the line
    void     start(std::shared_ptr<LipSync::Track> track, uint32_t seed, uint64_t now);
    void     stop();
    // finds the accents of what has arrived; true: a move to start now in `out` (called until false)
    bool     update(const FaceProfile::Brows& brows, uint64_t now, Start& out);

    const std::vector<Accent>& accents() const { return found; }
    uint32_t moves() const { return moved; }

  private:
    uint64_t lineTime(uint64_t now) const;
    void     scan(const FaceProfile::Brows& brows);
    bool     judge(const FaceProfile::Brows& brows, int64_t syllable);
    float    riseAt(const FaceProfile::Brows& brows, int64_t from, int64_t to) const;
    float    random();

    std::shared_ptr<LipSync::Track> track;
    uint64_t             startTick = 0;
    uint32_t             rnd       = 1;
    size_t               scanned   = 0;     // steps looked at for syllables
    bool                 inDip     = true;  // LipSync::update's rule
    float                dipMin    = 0;
    float                sylMax    = 0;
    std::vector<int64_t> syllables;         // their starts, ms of the line
    size_t               judged    = 0;
    std::vector<Accent>  found;
    size_t               next      = 0;     // the first of found that may still start
    int64_t              lastMove  = 0;     // ms of the line the last move starts at
    bool                 anyMove   = false;
    uint32_t             moved     = 0;
  };
