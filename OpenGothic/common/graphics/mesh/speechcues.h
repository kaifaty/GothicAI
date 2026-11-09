#pragma once

#include "lipsync.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct FaceProfile;

// When the facial gestures of a line (say.cues, v61) play (reports/content/2026-11-hero-head/face-cues.md):
// a gesture of the upper face starts a moment before its word, a mouth gesture waits for the next pause of the voice
// after its word and is squeezed into it (cues.py fit_mouth), or comes after the end of the sound. The time of a
// word is its time in the line's sound (say.words, say_audio.words, v62), else an estimate by its characters. Time
// runs from the playback of the line's sound, as LipSync's.
// A line that hides a lie (say.leak, v61, face-cues.md step 4) also has a leak of the felt emotion, which starts as an
// upper gesture does, two more blinks (the liar's) in its sound, and a friendly one has its smile put on: the
// expression without its squint while the line goes.
class SpeechCues final {
  public:
    // The gestures of one line and the time of its words. The bridge makes it when the line arrives and adds the
    // words as they come (in any order, by index); the speaker reads it while the line is said.
    struct Plan final {
      struct Cue final {
        std::string name;
        uint32_t    word = 0;
        char        side = 0;  // 'L'/'R': the side a one-sided gesture goes on (by the seed of the npc and line)
        };
      struct Word final {
        float start = -1;  // s from the start of the line's sound; below 0: not known (yet)
        float end   = -1;
        };
      struct Leak final {
        std::string felt;           // friendly, angry, hostile, afraid, pain
        bool        lower = false;  // the half of the face
        uint32_t    word  = 0;
        };

      explicit Plan(std::string_view text);  // the words of the text: split by whitespace, as Python's split()
      void setWord(size_t index, float start, float end);  // a wrong or out of range one is left out

      std::vector<Cue>      cues;
      std::vector<uint32_t> wordChar;  // the character each word starts at
      std::vector<Word>     words;
      uint32_t              chars    = 0;  // of the text
      bool                  streamed = false;  // its voice arrives in pieces: the estimate goes by a speech rate
      uint64_t              silentMs = 0;      // a line without a voice: its time on screen
      std::optional<Leak>   leak;              // say.leak: one a line at most
      bool                  friendly = false;  // the mood of the line
      uint32_t              seed     = 0;      // of the npc and the line: when the liar's blinks come

      bool posed() const { return leak.has_value() && friendly; }  // a smile put on over a lie
      };

    struct Start final {
      std::string_view name;           // the profile's cue
      bool             mirror = false; // on the other side than written
      float            hold   = -1;    // ms of the first part's hold instead of its own (below 0: its own)
      uint64_t         at     = 0;     // tick it starts at (at most now)
      };

    static constexpr uint64_t UpperLeadMs   = 100;   // an upper gesture starts this much before its word
    static constexpr uint64_t PauseMs       = 250;   // a pause of the voice: words this far apart or as long quiet
    static constexpr float    QuietLevel    = 0.02f; // LipSync level of a quiet voice (about 22 dB below its peak)
    static constexpr uint64_t PauseBeforeMs = 100;   // a mouth gesture may start this much before its pause...
    static constexpr uint64_t PauseAfterMs  = 150;   // ...and end this much after it
    static constexpr float    MinHoldMs     = 100;   // a pause that leaves less hold does not take the gesture
    static constexpr float    CharsPerS     = 14;    // a streamed voice without word times: its speech rate
    static constexpr uint64_t BindMs        = 100;   // a line and a voice that start this close are one line
    static constexpr uint64_t LateMs        = 50;    // a gesture found later than this starts now, not in the past
    static constexpr uint32_t LiarBlinks    = 2;     // the blinks a line with a leak has on top of the ordinary ones,
    static constexpr uint64_t BlinkGapMs    = 500;   // at least this far from each other and from ordinary blinks,
    static constexpr uint64_t LeakClearMs   = 150;   // and this far outside the leak
    static constexpr uint64_t BlinkMs       = 160;   // a blink from its start to its end, for the room it takes
    static constexpr uint64_t LiarBlinkHold = 80;    // ms the liar's blink holds the R_EYESBLINK anim (+ its fade out)

    // what to start now
    struct Out final {
      std::vector<Start>    cues;
      bool                  leak   = false;  // the leak of the line (leakFelt, leakLower) from leakAt
      std::string           leakFelt;
      bool                  leakLower = false;
      uint64_t              leakAt = 0;
      uint32_t              blinks = 0;      // liar's blinks
      std::vector<uint64_t> reserve;         // the ticks the liar's blinks are planned for, once: ordinary ones keep away
      bool                  posed  = false;  // the posed smile of the line goes on

      void clear();
      };

    static uint32_t seedOf(std::string_view s);  // FNV-1a
    static char     sideOf(uint32_t seed) { return (seed>>7)&1 ? 'R' : 'L'; }

    // the line starts; voiced: its voice starts too (before or after this, through voice())
    void     start(std::shared_ptr<const Plan> plan, bool voiced, uint64_t now);
    // a voice of the npc starts (LipSync::start)
    void     voice(std::shared_ptr<LipSync::Track> track, uint64_t now);
    void     stop();
    // the gestures, the leak and the liar's blinks to start now; lastBlink: the tick of the last ordinary blink, or of
    // one coming (the second of a double), 0: none
    void     update(const FaceProfile& profile, uint64_t now, uint64_t lastBlink, Out& out);
    // the line ms of the liar's blinks, once planned (the test)
    const std::vector<int64_t>& liarBlinks() const { return blinks; }

    // ms into the line's sound where the word starts, as known now; below 0: not known
    int64_t  wordMs(size_t word) const;

  private:
    struct Slot final {
      int64_t start = -1;  // ms of the line; below 0: not known yet
      int64_t limit = -1;  // ms of the line its first part must end by (the pause), below 0: none
      float   hold  = -1;
      };
    uint64_t lineTime(uint64_t now) const;
    uint64_t heardMs() const;   // of the sound so far
    bool     complete() const;  // all of the sound is here
    Slot     mouthSlot(const Plan::Cue& c, float delay, float on, float hold, float off) const;
    bool     fits(int64_t from, int64_t to, float delay, float on, float hold, float off, Slot& s) const;
    bool     updateLeak(const FaceProfile& profile, uint64_t now, uint64_t t, uint64_t lastBlink, Out& out);
    void     planBlinks(int64_t from, int64_t lastBlink, float leakMs);

    std::shared_ptr<const Plan>     plan;
    std::shared_ptr<LipSync::Track> track;
    uint64_t                        planTick  = 0;
    uint64_t                        trackTick = 0;
    bool                            trackUsed = true;   // the voice belongs to a line (or to none that has cues)
    bool                            bound     = false;  // the plan has its voice (or needs none)
    std::vector<uint8_t>            fired;
    bool                            leakFired = false;
    int64_t                         leakStart = -1;   // ms of the line the leak started at
    bool                            blinksPlanned = false;
    std::vector<int64_t>            blinks;           // ms of the line of the liar's blinks
    uint32_t                        blinksFired = 0;
  };
