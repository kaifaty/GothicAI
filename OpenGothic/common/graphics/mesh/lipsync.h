#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace Tempest {
class Sound;
}

// Lip sync: the mouth of a speaking character follows the loudness of its voice.
// Gothic heads have no mouth shape per sound, only VISEME: 16 shapes from a nearly closed mouth to a
// wide open one (the original game loops through them while a line lasts). Here every syllable of the
// voice picks a shape that opens about as wide as the syllable is loud, and the shape follows the
// loudness from moment to moment: the mouth closes in pauses and opens wide on loud vowels.
class LipSync final {
  public:
    // Loudness of a voice line, a value per 10 ms. Filled at once from a sound file, or bit by bit
    // from a voice that is still arriving (the bridge's streamed TTS)
    class Track final {
      public:
        explicit Track(uint32_t rate);
        // nullptr if the sound has no 16-bit samples
        static std::shared_ptr<Track> fromSound(const Tempest::Sound& snd);

        void     append(const int16_t* pcm, size_t count, uint16_t channels = 1);
        void     finish();
        // a streamed voice tells how much of it has been played (from the audio thread)
        void     setPlayed(uint64_t ms) { played.store(int64_t(ms)); }
        int64_t  playedMs() const { return played.load(); }  // -1: it plays from the start of the line

        float    level(uint64_t ms) const;                 // 0..1: how wide the voice opens the mouth
        float    peak (uint64_t from, uint64_t to) const;  // the largest level in [from,to]
        uint64_t durationMs() const;
        bool     isFinished() const;
        // the first stretch of at least minMs after `from` where the level stays below `below`: [start, end) in ms,
        // end - where the voice is back or the end of what has arrived. A stretch already going on at `from` is
        // not one (the gestures want a pause after their word, not the one before it)
        bool     quietRun(uint64_t from, float below, uint64_t minMs, uint64_t& start, uint64_t& end) const;

      private:
        struct Biquad final {
          float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
          float z1 = 0, z2 = 0;
          float run(float x);
          };
        float    implLevel(size_t step) const;

        mutable std::mutex   sync;
        uint32_t             rate     = 0;
        size_t               perStep  = 0;
        Biquad               hp, lp;        // speech band: rumble and hiss do not open the mouth
        double               acc      = 0;  // sum of squares in the current step
        size_t               accN     = 0;
        std::vector<float>   db;            // loudness per step
        float                peakDb   = -100;
        bool                 finished = false;
        std::atomic<int64_t> played{-1};
      };

    // what to show: two VISEME frames mixed, then scaled
    struct Shape final {
      uint32_t frame0    = 0;
      uint32_t frame1    = 0;
      float    alpha     = 0;
      float    intensity = 0;
      };

    // State for the test bench
    struct Probe final {
      bool     valid     = false;  // the head has mouth shapes
      bool     active    = false;
      float    level     = 0;      // of the voice at this moment
      float    mouth     = 0;      // smoothed: how wide the voice wants the mouth open
      float    open      = 0;      // how wide it is shown, relative to the widest shape of the head
      Shape    shape;
      uint32_t picks     = 0;      // shapes picked since the line started
      uint64_t time      = 0;      // ms into the line
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);
    static bool    setParam(std::string_view name, float value);
    static auto    paramList() -> std::string;

    // how wide each VISEME frame of the head opens the mouth, 0..1; empty: the head has none
    void           setShapes(std::vector<float> open);
    bool           hasShapes() const { return !open.empty(); }

    void           start(std::shared_ptr<Track> track, uint64_t now);
    void           stop();                          // the line is cut off: the mouth closes
    bool           isSpeaking(uint64_t now) const;  // a voice line moves the mouth
    uint64_t       voiceLeft(uint64_t now) const;   // ms of the voice left, 0: none or not known yet (streaming)
    uint32_t       syllableCount() const { return syllables; }  // syllables of voices started so far
    float          syllablePeak()  const { return sylLoud; }    // how loud the latest one gets, 0..1
    float          voiceLevel()    const { return lvl; }        // how loud the voice is now, 0..1
    // per frame; false: nothing to show
    bool           update(uint64_t now, uint64_t dt, Shape& out);
    void           probe(Probe& p) const;

    static Probe       subjectProbe;  // the npc the test bench watches
    static const void* probeNpc;

  private:
    uint64_t       lineTime(uint64_t now) const;
    void           pick(float need, uint64_t now);

    std::vector<float>     open;
    std::shared_ptr<Track> track;
    uint64_t               startTick = 0;
    uint64_t               time      = 0;  // ms into the line, debug
    bool                   active    = false;
    float                  lvl       = 0;  // level of the voice now
    float                  mouth     = 0;  // smoothed
    float                  sylMax    = 0;  // the loudest moment of the current syllable so far
    float                  sylPeak   = 1;  // how loud it gets (looked ahead): the shape is shown fully there
    float                  dipMin    = 0;  // the quietest moment after it
    bool                   inDip     = true;
    uint32_t               cur       = 0;
    uint32_t               prev      = 0;
    float                  blend     = 1;  // 0..1 from prev to cur
    uint64_t               lastPick  = 0;
    uint32_t               picks     = 0;
    uint32_t               syllables = 0;
    float                  sylLoud   = 0;
    uint32_t               rnd       = 1;
    Shape                  shown;
    float                  shownOpen = 0;
  };
