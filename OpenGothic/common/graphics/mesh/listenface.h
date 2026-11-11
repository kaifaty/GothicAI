#pragma once

#include "facerig.h"
#include "talkgestures.h"

#include <cstdint>
#include <string_view>

// The face of the one listening (listen_face of the face profile; reports/content/2026-11-hero-head/eyes.md, "План:
// брови по тону и лицо слушающего", item 3): while the other one speaks, now and then a nod of it listening
// (TalkGestures, as words come) comes with a face. Its mood decides which: a friendly one smiles a little (or smiles
// and flashes its brows), an angry or hostile one frowns a moment, any other one flashes its brows, smaller and shorter
// than the flash gesture. Not on every nod: a gap after each face and a chance by the seed of the npc, the same every
// time; none while the face is taken (a gesture, a leak, its own voice, an expression stronger than the smile).
// Only decides: FaceRig shows the faces.
class ListenFace final {
  public:
    enum class React : uint8_t { None, Flash, Smile, SmileFlash, Frown };

    // OPENGOTHIC_LISTEN_FACE=0 switches it off (a profile without listen_face has none anyway)
    static bool isEnabled();
    static void setEnabled(bool e);
    static auto name(React r) -> std::string_view;  // "flash", "smile", "smile+flash", "frown"; "none"

    void     seed(uint32_t s) { rnd = s*2654435761u | 1u; }
    bool     isSeeded() const { return rnd!=0; }
    // a nod of it listening starts now; busy: the face is taken (FaceRig::listenBusy, or it speaks)
    React    nod(const FaceProfile::Listen& l, TalkGestures::Mood mood, bool busy, uint64_t now);
    // a face into FaceRig, from now
    static void play(FaceRig& face, React r, uint64_t now);

    uint32_t nods()  const { return nodCount; }
    uint32_t faces() const { return faceCount; }

  private:
    float    random();

    uint32_t rnd       = 0;
    uint64_t last      = 0;  // tick the last face started at
    bool     any       = false;
    uint32_t nodCount  = 0;
    uint32_t faceCount = 0;
  };
