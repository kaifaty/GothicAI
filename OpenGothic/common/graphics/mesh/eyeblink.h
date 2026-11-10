#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Blinking. Every head of the game has R_EYESBLINK (the eyes closed, in and out in 40 ms), but OpenGothic
// never played it: nobody blinked. People blink every few seconds at random, more often while they
// speak, in the pause after a phrase and when the eyes go to another place; now and then twice. A
// frightened face blinks more, a hostile stare less. Only decides: the caller shows the blink.
class EyeBlink final {
  public:
    struct Input final {
      uint64_t now      = 0;
      uint64_t dt       = 0;
      bool     allowed  = false;  // awake, the eyes open, near enough to be seen
      bool     speaking = false;  // its voice (or line) goes on
      float    voice    = 0;      // how loud the voice is now, 0..1
      bool     shift    = false;  // the eyes go to another place now
      float    rate     = 1;      // more than 1: blinks more often (a frightened face)
      };

    // State for the test bench
    struct Probe final {
      bool     valid   = false;
      bool     enabled = false;
      uint32_t blinks  = 0;  // since the start
      uint32_t pauses  = 0;  // of them in a pause of the voice
      uint32_t shifts  = 0;  // ...when the eyes went elsewhere
      uint32_t doubles = 0;  // ...the second of a double blink
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);
    static bool    setParam(std::string_view name, float value);
    static auto    paramList() -> std::string;

    // per frame: true when a blink starts now, closed for `hold` ms (between fading in and out)
    bool           update(const Input& in, uint64_t& hold);
    // a blink the caller starts itself at that tick (the liar's blinks of a line): none of its own comes closer
    // than ReserveGapMs to it
    void           reserve(uint64_t at);
    // the tick of its last blink, or of the second of a double one still coming; 0: none
    uint64_t       lastBlink() const { return std::max(last,second); }

    static constexpr uint64_t ReserveGapMs = 500;
    void           probe(Probe& p) const;

    static Probe       subjectProbe;  // the npc the test bench watches
    static const void* probeNpc;

  private:
    uint64_t       interval(bool speaking, float rate);
    uint32_t       random();

    std::vector<uint64_t> reserved;  // ticks of the caller's blinks to keep away from
    uint64_t       next      = 0;  // the next blink at random
    uint64_t       last      = 0;
    uint64_t       second    = 0;  // the second blink of a double one, 0: none
    uint64_t       quiet     = 0;  // ms the voice has been quiet while it speaks
    bool           inPause   = false;
    bool           started   = false;
    uint32_t       rnd       = 0;
    uint32_t       blinks    = 0;
    uint32_t       pauses    = 0;
    uint32_t       shifts    = 0;
    uint32_t       doubles   = 0;
  };
