#pragma once

#include <Tempest/Vec>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct GltfHeadMesh;

// The face profile of an own head (characters.json "face"): the old face animation names of Gothic heads
// (S_ANGRY, R_EYESBLINK, VISEME...) as weights of the head's shapes, with the timing of the original .MMB.
struct FaceProfile final {
  struct Anim final {
    std::string name;
    int32_t     layer    = 0;
    float       blendIn  = 0;  // weight per ms, 0: at once (as ProtoMesh::Morph)
    float       blendOut = 0;
    uint64_t    duration = 0;  // ms of its own, 0: until stopped or the time given
    uint64_t    frameMs  = 1;
    std::vector<std::vector<std::pair<uint32_t,float>>> frames;  // per frame: head target -> weight
    std::vector<float> strength;  // per frame: how far it moves the vertices on average, 0..1 of the strongest
    };

  // A facial gesture ("cues", reports/content/2026-11-hero-head/face-cues.md): a short curve of shape weights over
  // the other layers. Each part rises from 0 to its weights over on_ms (a smooth step), holds, falls over off_ms.
  struct Cue final {
    struct Part final {
      std::vector<std::pair<uint32_t,float>> weights;   // head target -> weight at the apex
      std::vector<std::pair<uint32_t,float>> mirrored;  // the same on the other side (Left <-> Right)
      float delayMs = 0, onMs = 0, holdMs = 0, offMs = 0;
      };
    std::string       name;
    bool              mouth = false;  // zone "mouth": into the pauses of the voice; else "upper": at its word
    char              side  = 0;      // 'L'/'R' of a one-sided gesture as written, 0: both sides
    std::vector<Part> parts;

    // ms from its start to its end; hold: the first part's hold instead of its own (below 0: its own)
    float span(float hold = -1) const;
    };

  // A leak of a felt emotion through a lie ("leaks", face-cues.md step 4): for a moment one half of the face shows
  // what the speaker feels. Its shapes at the apex are the felt-minus-neutral Audio2Face runs of that half
  // (reports/content/2026-11-hero-head/cues/leak_table.py); it rises over on_ms, holds, falls over off_ms as a cue.
  struct Leak final {
    std::string felt;           // friendly, angry, hostile, afraid, pain (the moods of say)
    bool        lower = false;  // the half: the expression shapes of the mouth, cheeks and nose; else brows and lids
    std::vector<std::pair<uint32_t,float>> weights;  // head target -> weight at the apex
    float       peak  = 0;      // the largest of them
    };
  struct LeakTime final {
    float onMs = 0, holdMs = 0, offMs = 0;
    float span() const { return onMs+holdMs+offMs; }
    };
  static constexpr std::string_view Felt[] = {"friendly","angry","hostile","afraid","pain"};
  static bool isFelt(std::string_view felt);

  // The eyeballs ("eyes"): two head parts turned as rigid balls around their centres, the lids following them.
  // Angles in the frame of the head bone: x up, y across (+ to the character's left), z forward.
  struct Eyes final {
    enum Dir : uint8_t { Down, Up, In, Out };
    struct Lid final {
      uint32_t target = 0;
      float    from = 0, to = 1, max = 0;  // weight = max*clamp((angle-from)/(to-from), 0, 1), degrees
      };
    struct Eye final {
      size_t           part    = 0;
      uint32_t         first   = 0, end = 0;  // its logical vertices
      Tempest::Vec3    center;                // the sphere fitted to them
      float            radius  = 0;
      float            outSign = 1;           // +1: a turn to +y is outwards (away from the nose)
      std::vector<Lid> lids[4];               // per Dir
      };
    Eye   eye[2];            // the character's own left eye, the right one
    float limit[4] = {};     // in the socket per Dir, radians
    };


  std::vector<Anim>    anims;
  std::vector<Cue>     cues;
  std::vector<Leak>    leaks;     // none: the profile has no "leaks"
  LeakTime             leakTime;
  std::vector<uint8_t> mouth;  // per head target: a mouth shape (its name starts with a "mouth" prefix)
  std::optional<Eyes>  eyes;   // none: the eyes stay as modelled

  const Anim* find(std::string_view name) const;
  size_t      index(std::string_view name) const;  // size_t(-1): none
  const Cue*  findCue(std::string_view name) const;
  const Leak* findLeak(std::string_view felt, bool lower) const;

  // Throws std::runtime_error with the reason: broken JSON, a wrong field, a target the head does not have.
  static std::unique_ptr<FaceProfile> parse(std::string_view json, const GltfHeadMesh& head);
  };

// The face of an own head: the old face animations through the profile make one vector of the head's shape
// weights, it deforms the head on the CPU (positions and normals of every drawn vertex, for setDeformation).
// Time as VisualObjects plays the morphs of the original heads: a slot per anim, fade in and out, frames in time
// or set by hand (VISEME), one anim per layer giving way to the next one. With "eyes" in the profile the eyeballs
// turn as Eyes says, inside the same deformation (no bones), and their lids add to the weights.
class FaceRig final {
  public:
    enum class Result : uint8_t {
      Same,      // nothing to send
      Deformed,  // deformation() holds the new vertices
      Rest,      // all weights are zero: the neutral head (setDeformation({}))
      };

    struct Probe final {
      bool        valid   = false;  // the head has a profile
      uint32_t    anims   = 0;      // active anims
      uint32_t    targets = 0;      // targets with a weight above 0
      float       blendUs = 0;      // the last blend, µs
      uint32_t    blends  = 0;      // blends so far
      float       uploadUs = 0;     // the last setDeformation of the head, µs (filled by its owner)
      uint32_t    cues    = 0;      // active gestures
      // the active anims and gestures, their strength, and a leak with its strongest weight as shown:
      // "S_ANGRY 0.82 doubt:R 0.64 leak:afraid:upper 0.45"
      std::string shown;
      };

    static constexpr float    Changed     = 1e-4f; // a weight change that deforms the head again
    static constexpr float    SpeechMouth = 0.7f;  // a voice takes that much of the mouth from the other layers
    static constexpr uint64_t ShapeHold   = 250;   // ms a shape set by hand stays without being set again
    static constexpr uint64_t PosedFadeMs = 300;   // the squint a posed smile cuts comes back over this
    // A target delta below both is not seen and not blended: exports carry normal noise on most of the mesh
    // (the MPFB test head: 357 thousand normal deltas, 104 thousand above 1e-3)
    static constexpr float    StillCm     = 1e-4f; // cm of position
    static constexpr float    StillNormal = 1e-3f; // of a normal's component
    static constexpr float    EyeChanged  = 1e-4f; // radians of an eye's turn that deforms the head again

    // the turn of an eyeball in its socket, radians: yaw + to the head's +y (the character's left), pitch + up
    struct EyeTurn final {
      float yaw = 0, pitch = 0;
      };

    void   setup(const GltfHeadMesh* head, const FaceProfile* profile);
    bool   isActive() const { return head!=nullptr && profile!=nullptr; }
    bool   hasEyes() const { return isActive() && profile->eyes.has_value(); }
    const FaceProfile::Eyes* eyes() const { return hasEyes() ? &*profile->eyes : nullptr; }
    // the view of the head was made again: the deformation goes to it on the next update
    void   invalidate() { dirty = true; }

    // as VisualObjects::startMMAnim: timeUntil uint64_t(-1) - its own duration or until stopped, below now - stop
    void   startAnim(std::string_view name, float intensity, uint64_t timeUntil, uint64_t now);
    // as VisualObjects::setMMShape: two frames mixed by alpha, set every frame by the owner (lip sync)
    void   setShape (std::string_view name, uint32_t frame0, uint32_t frame1, float alpha, float intensity, uint64_t now);
    // a gesture of the profile's cues from `start` (it may lie a little in the past): mirror - the other side of
    // a one-sided one, hold - ms of the first part's hold instead of its own (below 0: its own); false: no such cue
    bool   startCue (std::string_view name, bool mirror, float hold, uint64_t start, uint64_t now);
    // a leak of the profile's table from `start` (it may lie a little in the past), over the other layers; it takes
    // the place of one still going (a line has one at most); false: the profile has no such leak
    bool   startLeak(std::string_view felt, bool lower, uint64_t start, uint64_t now);
    // the leak shown now: its strongest weight as shown (at its apex, the table's largest), 0: none
    float  leakWeight(uint64_t now) const;
    // a smile put on (a friendly line that hides a lie): while on, the expression's squint - its eyeSquint* and
    // cheekSquint* - is cut; after it, the squint comes back over PosedFadeMs. The gestures and a leak keep theirs
    void   setPosed (bool on, uint64_t now);
    // as shown now: intensity and fade
    float  weight   (std::string_view name, uint64_t now) const;
    // how far the gesture is shown now, 0..1 (the strongest of its parts and of its runs)
    float  cueWeight(std::string_view name, uint64_t now) const;
    const FaceProfile* faceProfile() const { return profile; }
    // per frame of the anim: how far it moves the vertices, 0..1 of its strongest (LipSync::setShapes)
    std::vector<float> strength(std::string_view name) const;
    // the eyeballs turned so (Eyes): the balls are turned in the deformation, the lids follow by the profile's table
    void   setEyes(const EyeTurn& left, const EyeTurn& right);

    Result update(uint64_t now);
    const std::vector<float>&         weights()     const { return applied; }
    const std::vector<Tempest::Vec4>& deformation() const { return deform; }
    // the drawn vertices [first, end) the last Deformed changed; the whole head after a rest or a new view
    std::pair<uint32_t,uint32_t>      changed()     const { return {changedFirst,changedEnd}; }
    void   probe(Probe& p, uint64_t now) const;

    static Probe       subjectProbe;  // the npc the test bench watches
    static const void* probeNpc;

  private:
    struct Slot final {
      size_t   anim      = 0;
      uint64_t timeStart = 0;
      uint64_t timeUntil = 0;
      float    intensity = 0;
      bool     on        = false;
      bool     manual    = false;  // frames set by the owner instead of played in time
      uint32_t frame0    = 0;
      uint32_t frame1    = 0;
      float    alpha     = 0;
      };

    struct CueRun final {
      size_t   cue    = 0;
      bool     mirror = false;
      float    hold   = -1;
      uint64_t start  = 0;
      };

    struct LeakRun final {
      size_t   leak  = 0;
      uint64_t start = 0;
      bool     on    = false;
      };

    bool   isOn(const Slot& s, uint64_t now) const { return s.on && s.timeUntil>=now; }
    float  leakEnvelope(uint64_t now) const;
    float  posedCut(uint64_t now) const;
    float  cueEnvelope(const CueRun& r, size_t part, uint64_t now) const;
    float  cueStrength(const CueRun& r, uint64_t now) const;
    float  fade(const Slot& s, uint64_t now) const;
    void   fadeOut(Slot& s, uint64_t now);
    Slot&  freeSlot(uint64_t now);
    void   mix(uint64_t now, std::vector<float>& out) const;
    void   blend();
    void   turnEyes();
    void   writeEyes();
    void   prepare();
    void   write(uint32_t v);

    const GltfHeadMesh*        head    = nullptr;
    const FaceProfile*         profile = nullptr;
    int32_t                    speechLayer = -1;  // the layer of VISEME

    std::vector<Slot>          slots;
    std::vector<CueRun>        runs;      // the gestures shown now
    LeakRun                    leakRun;   // the leak shown now
    bool                       posed    = false;
    uint64_t                   posedEnd = 0;      // when the posed smile ended (0: never)
    std::vector<uint8_t>       squint;            // per head target: eyeSquint*, cheekSquint* (a posed smile cuts them)
    std::vector<float>         want;      // the weights now
    std::vector<float>         applied;   // the weights the deformation shows
    bool                       deformed = false;  // the head is not neutral
    bool                       dirty    = true;   // the view does not have the deformation yet

    // the head as the deformation shows it: logical vertices, the neutral ones, the drawn ones by logical vertex
    std::vector<Tempest::Vec3> pos, nrm, basePos, baseNrm;
    std::vector<uint32_t>      drawnFirst, drawn;  // drawn[drawnFirst[v] .. drawnFirst[v+1]) of logical vertex v
    std::vector<uint32_t>      moved;             // logical vertices off the neutral head now
    struct Delta final {
      uint32_t      vertex = 0;
      Tempest::Vec3 position, normal;
      };
    std::vector<std::vector<Delta>> deltas;       // per target: the deltas worth blending
    std::vector<uint8_t>       mark;
    std::vector<Tempest::Vec4> deform;
    EyeTurn                    eyeWant[2], eyeShown[2];
    float                      eyeTurn[2][3][3] = {};  // the rotation of each ball as shown, row-major
    uint32_t                   changedFirst = 0, changedEnd = 0;
    float                      blendUs = 0;
    uint32_t                   blends  = 0;
  };
