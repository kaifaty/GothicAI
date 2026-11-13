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
// The track also knows the tone of the voice per 10 ms (the brows of the speaker follow it, SpeechBrows).
class LipSync final {
  public:
    // Loudness of a voice line, a value per 10 ms. Filled at once from a sound file, or bit by bit
    // from a voice that is still arriving (the bridge's streamed TTS).
    // And its tone (fundamental frequency) per 10 ms, found as the sound arrives: YIN (de Cheveigne, Kawahara 2002)
    // on 30 ms of the sound brought down to 8 kHz, 60..400 Hz, a step without a clear period has none. A step's tone
    // is known once 25 ms of the sound after it is here (or the sound is finished).
    class Track final {
      public:
        explicit Track(uint32_t rate);
        // nullptr if the sound has no 16-bit samples; name: of the line (the seed of what the face does with it)
        static std::shared_ptr<Track> fromSound(const Tempest::Sound& snd, std::string_view name = {});

        void     append(const int16_t* pcm, size_t count, uint16_t channels = 1);
        void     finish();
        // a streamed voice tells how much of it has been played (from the audio thread)
        void     setPlayed(uint64_t ms) { played.store(int64_t(ms)); }
        int64_t  playedMs() const { return played.load(); }  // -1: it plays from the start of the line
        // A live microphone has no future audio: sample its played time without the recorded voice lead.
        // Its loudness is measured against a speech level of its own (LiveRef), not only the loudest moment so
        // far: a breath before the first word is not stretched to a wide mouth; a step not here yet holds the last
        void     setLiveInput() { liveInput = true; }
        bool     isLiveInput() const { return liveInput; }
        // dB added to the loudness of the steps appended from now on (a live track: an npc's voice is in the
        // microphone, LiveDuck), 0: as it is
        void     setLiveGain(float db);

        float    level(uint64_t ms) const;                 // 0..1: how wide the voice opens the mouth
        float    peak (uint64_t from, uint64_t to) const;  // the largest level in [from,to]
        float    loudestDb(uint64_t from, uint64_t to) const;  // the loudest step in [from,to], dB of the speech band
        uint64_t durationMs() const;
        bool     isFinished() const;
        // the first stretch of at least minMs after `from` where the level stays below `below`: [start, end) in ms,
        // end - where the voice is back or the end of what has arrived. A stretch already going on at `from` is
        // not one (the gestures want a pause after their word, not the one before it)
        bool     quietRun(uint64_t from, float below, uint64_t minMs, uint64_t& start, uint64_t& end) const;

        // the tone at `ms`: Hz, 0 - none there or not known yet
        float    toneHz(uint64_t ms) const;
        // the tone at `ms` in semitones from the median of the line's toned steps so far; false: none
        bool     tone(uint64_t ms, float& semitones) const;
        // ms of the line whose tone is known (the steps before it)
        uint64_t toneKnownMs() const;
        // the name of the line, if its maker gave one (a sound file, a line id of the bridge)
        void     setName(std::string_view n);
        auto     name() const -> std::string;
        // it is the voice of that line: its name is the line's id or the id with ".WAV" (case aside, as the game names
        // sounds; the bridge's streamed voice has the id, a sound file of it "<id>.WAV")
        bool     isOf(std::string_view line) const;

      private:
        struct Biquad final {
          float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
          float z1 = 0, z2 = 0;
          float run(float x);
          };
        float    implLevel(size_t step) const;
        void     pushTone(float x);
        void     findTones(bool flush);
        float    yin(size_t at) const;
        float    implMedian() const;

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
        bool                 liveInput = false;
        float                liveGain  = 0;

        // the tone: the sound without what lies above 1 kHz, brought down to 8 kHz
        Biquad               toneLp[2];
        double               decStep  = 1;  // input samples per sample at 8 kHz
        double               decAt    = 1;  // where the next one lies, in input samples from the last one
        float                decLast  = 0;
        std::vector<float>   dec;           // at 8 kHz, from sample decBase on (the older ones are dropped)
        size_t               decBase  = 0;
        std::vector<float>   hz;            // per step, 0: none
        mutable float        medianSt = 0;  // log2 of the median tone of the toned steps, in semitones
        mutable size_t       medianOf = 0;  // ...of so many toned steps
        size_t               toned    = 0;
        std::string          lineName;
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
    float    tone      = 0;      // semitones from the median of the line, NaN: none here
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
    // the live microphone (PlayerVoice): ms its clock stays behind what has arrived, dB its loudness is lowered by
    // while an npc's voice sounds near the hero (the speakers' sound in the microphone)
    static uint64_t liveBufferMs();
    static float    liveDuckDb();

    // how wide each VISEME frame of the head opens the mouth, 0..1; empty: the head has none
    void           setShapes(std::vector<float> open);
    bool           hasShapes() const { return !open.empty(); }

    void           start(std::shared_ptr<Track> track, uint64_t now);
    void           stop();                          // the line is cut off: the mouth closes
    bool           isSpeaking(uint64_t now) const;  // a voice line moves the mouth
    uint64_t       voiceLeft(uint64_t now) const;   // ms of the voice left, 0: none or not known yet (streaming)
    // the rule a syllable starts by (as update finds them): the voice rises `rise` above the quietest moment after the
    // last one, which ended when the voice fell below `dip` of its loudest moment
    static void    syllableRule(float& dip, float& rise);
    uint32_t       syllableCount() const { return syllables; }  // syllables of voices started so far
    // the syllables judged so far, how loud the latest one is (dB of the speech band, its loudest 10 ms): a syllable
    // is judged on JudgeMs of its sound (LiveJudgeMs of a live voice, which has none ahead) once that has arrived
    static constexpr uint64_t JudgeMs     = 150;
    static constexpr uint64_t LiveJudgeMs = 80;
    uint32_t       judgedCount()   const { return judged; }
    float          judgedDb()      const { return judgedLoud; }
    bool           isLive()        const { return live; }       // the voice now (or last) is a live microphone
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
    bool                   live      = false;  // the voice is a live microphone (its own times, LiveAttack...)
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
    uint64_t               sylAt     = 0;      // ms of the line the latest syllable starts at...
    bool                   sylPending = false; // ...not judged yet
    uint32_t               judged    = 0;
    float                  judgedLoud = -100;
    uint32_t               rnd       = 1;
    Shape                  shown;
    float                  shownOpen = 0;
  };
