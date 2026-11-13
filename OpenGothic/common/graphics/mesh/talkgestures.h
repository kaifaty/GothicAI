#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The hands of a character that speaks: the game's dialog gestures (T_DIALOGGESTURE_xx, 2..10 s of
// moving hands each). The original starts a random one when a line starts and none plays: after a
// short one the hands hang still while the voice goes on (the AI server says a reply in several
// lines, the next one may start only seconds later), a long one goes on while the other one speaks.
// Here one gesture follows another while the voice lasts, with a rest between them, the next one
// starting on a syllable; the mood of the line decides how lively they are and how long the rests;
// when the voice is over, the hands rest. Only decides: the caller plays and stops the animations.
//
// The head goes with the voice too (Gaze turns it): a small nod down on a syllable clearly louder
// than the ones before it (a stressed word), a slow sway aside while the voice lasts.
//
// And while the other one speaks (the hero's voice): it listens, the head sways a little, now and then
// it nods as the words come. Then it thinks of its answer (the AI server's model takes a second or
// three): the head tilts, the eyes go aside and up from the other one, and come back as it answers.
class TalkGestures final {
  public:
    enum class Mood : uint8_t { Neutral, Friendly, Angry, Hostile, Afraid, Pain };

    struct Input final {
      uint64_t now      = 0;
      bool     allowed  = false;  // it stands with its hands free
      bool     speaking = false;  // its line is being said
      bool     voiced   = false;  // a voice moves the mouth: a gesture starts on a syllable
      bool     syllable = false;  // a syllable of the voice starts this frame
      bool     playing  = false;  // the gesture started last still plays
      uint64_t left     = 0;      // ms the line still lasts, as far as known (0: not known)
      bool     more     = false;  // another line of it is waiting
      };

    enum class Act : uint8_t { None, Start, Stop };
    struct Decision final {
      Act     act     = Act::None;
      uint8_t gesture = 0;  // to start: 1 is T_DIALOGGESTURE_01
      };

    // how the voice turns the head now, radians
    struct Head final {
      float pitch = 0;  // + up
      float yaw   = 0;  // + to the left
      float roll  = 0;
      };

    // where the eyes go while it thinks: this far aside (+ to the left) and up from the other one,
    // radians, as much as `weight` (0..1, it comes and goes)
    struct Aside final {
      float yaw = 0, pitch = 0, weight = 0;
      };

    // State for the test bench
    struct Probe final {
      bool     valid    = false;
      bool     enabled  = false;
      bool     engaged  = false;  // gestures go with its lines now
      bool     speaking = false;
      uint8_t  gesture  = 0;      // playing, 0: none
      Mood     mood     = Mood::Neutral;
      uint32_t starts   = 0;      // gestures started
      uint32_t cuts     = 0;      // started over one that still played
      uint32_t stops    = 0;      // stopped before their end (the voice is over)
      uint32_t nods     = 0;      // nods on stressed syllables
      uint32_t listenNods = 0;    // nods as the other one speaks
      bool     listening = false;
      bool     thinking  = false;
      Head     head;
      Aside    aside;
      };

    static bool    isEnabled();
    static void    setEnabled(bool e);
    static bool    setParam(std::string_view name, float value);
    static auto    paramList() -> std::string;
    // for the test bench: the gestures to start next, in turn and over again (1 is T_DIALOGGESTURE_01), instead of
    // ones picked at random; none: at random again
    static void    setPicks(std::vector<uint8_t> gestures);
    static Mood    moodOf(std::string_view mood);  // the bridge's say.mood; unknown: neutral
    static auto    moodName(Mood m) -> std::string_view;

    // how long each gesture of the model plays, ms (0: it has not this one); the liveliness of the
    // Gothic II ones is known, others count as lively
    void           setGestures(std::vector<uint32_t> durationMs);
    void           setMood(Mood m) { mood = m; }
    bool           isIdle(uint64_t now) const {
      return !engaged && current==0 && !headMoves && now>=listenUntil && now>=thinkUntil && aside.weight<=0.f;
      }
    void           lineStart(uint64_t now);  // a line starts: the gestures go with it
    void           stop();                   // cut off: the other one speaks, a weapon is drawn...
    Decision       update(const Input& in);

    // per frame: whether it speaks, whether a syllable starts now and how loud it gets (0..1)
    void           updateHead(uint64_t now, uint64_t dt, bool speaking, bool syllable, float loud);
    const Head&    head() const { return headNow; }

    // the other one speaks to it until then (renewed while the voice goes on)
    void           listen(uint64_t until) { listenUntil = std::max(listenUntil,until); }
    void           stopListening(uint64_t now) { listenUntil = std::min(listenUntil,now); }
    void           heard(uint64_t now);  // words of the other one came: now and then a nod
    // it thinks of an answer until then; 0: no more (its answer starts)
    void           think(uint64_t now, uint64_t until);
    bool           isListening(uint64_t now) const { return now<listenUntil; }
    uint32_t       listenNodCount() const { return listenNods; }  // nods as the other one speaks, so far
    bool           isThinking (uint64_t now) const { return now<thinkUntil; }

    const Aside&   eyesAside() const { return aside; }

    // the original way (switched off): a random gesture started with a line
    void           started(uint8_t gesture, bool overPlaying);
    void           probe(Probe& p) const;

    static Probe       subjectProbe;  // the npc the test bench watches
    static const void* probeNpc;

  private:
    enum Kind : uint8_t { Calm, Lively, Sharp };

    uint8_t        pick(uint64_t left, bool more);
    uint64_t       rest();
    uint32_t       random();

    std::vector<uint32_t> duration;
    std::vector<uint8_t>  kind;
    Mood                  mood       = Mood::Neutral;
    bool                  engaged    = false;
    bool                  speaking   = false;
    uint8_t               current    = 0;
    uint8_t               recent[2]  = {};
    uint64_t              lastSpeech = 0;
    uint64_t              nextAt     = 0;
    uint32_t              starts     = 0;
    uint32_t              cuts       = 0;
    uint32_t              stops      = 0;
    uint32_t              rnd        = 0;

    Head                  headNow;
    bool                  headMoves  = false;
    float                 talkW      = 0;  // the sway fades in and out with the voice
    float                 nod        = 0;  // radians, and its speed: a critically damped spring
    float                 nodSpeed   = 0;
    float                 sway[2]    = {}; // yaw, roll
    float                 swayGoal[2] = {};
    uint64_t              swayLeft   = 0;
    float                 sylMean    = 0;  // how loud the syllables were so far
    uint64_t              sinceNod   = 0;
    uint32_t              nods       = 0;
    uint64_t              lastNow     = 0;
    uint64_t              listenUntil = 0;
    uint64_t              thinkUntil  = 0;
    uint64_t              thinkFrom   = 0;
    uint64_t              lastHeardNod = 0;
    uint64_t              secondNod  = 0;  // a double nod: the second one then
    float                 wantNod    = 0;  // a nod asked for by heard(), its strength
    float                 nodHz      = 2.5f;  // of the nod now: listening nods are slower
    float                 tiltSign   = 1;
    float                 tilt       = 0;  // radians, the head tilted while it thinks
    Aside                 aside;
    uint32_t              listenNods = 0;
  };
