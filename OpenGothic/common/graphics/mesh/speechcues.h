#pragma once

#include "lipsync.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

struct FaceProfile;

// When the facial gestures of a line (say.cues, v61) play (reports/content/2026-11-hero-head/face-cues.md):
// a gesture of the upper face starts a moment before its word, a mouth gesture waits for the next pause of the voice
// after its word and is squeezed into it (cues.py fit_mouth), or comes after the end of the sound. The time of a
// word is its time in the line's sound (say.words, say_audio.words, v62), else an estimate by its characters. Time
// runs from the playback of the line's sound, as LipSync's.
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

      explicit Plan(std::string_view text);  // the words of the text: split by whitespace, as Python's split()
      void setWord(size_t index, float start, float end);  // a wrong or out of range one is left out

      std::vector<Cue>      cues;
      std::vector<uint32_t> wordChar;  // the character each word starts at
      std::vector<Word>     words;
      uint32_t              chars    = 0;  // of the text
      bool                  streamed = false;  // its voice arrives in pieces: the estimate goes by a speech rate
      uint64_t              silentMs = 0;      // a line without a voice: its time on screen
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

    static uint32_t seedOf(std::string_view s);  // FNV-1a
    static char     sideOf(uint32_t seed) { return (seed>>7)&1 ? 'R' : 'L'; }

    // the line starts; voiced: its voice starts too (before or after this, through voice())
    void     start(std::shared_ptr<const Plan> plan, bool voiced, uint64_t now);
    // a voice of the npc starts (LipSync::start)
    void     voice(std::shared_ptr<LipSync::Track> track, uint64_t now);
    void     stop();
    // the gestures to start now
    void     update(const FaceProfile& profile, uint64_t now, std::vector<Start>& out);

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

    std::shared_ptr<const Plan>     plan;
    std::shared_ptr<LipSync::Track> track;
    uint64_t                        planTick  = 0;
    uint64_t                        trackTick = 0;
    bool                            trackUsed = true;   // the voice belongs to a line (or to none that has cues)
    bool                            bound     = false;  // the plan has its voice (or needs none)
    std::vector<uint8_t>            fired;
  };
