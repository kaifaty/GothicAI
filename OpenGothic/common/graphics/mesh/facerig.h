#pragma once

#include <Tempest/Vec>

#include <cstdint>
#include <memory>
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

  std::vector<Anim>    anims;
  std::vector<uint8_t> mouth;  // per head target: a mouth shape (its name starts with a "mouth" prefix)

  const Anim* find(std::string_view name) const;
  size_t      index(std::string_view name) const;  // size_t(-1): none

  // Throws std::runtime_error with the reason: broken JSON, a wrong field, a target the head does not have.
  static std::unique_ptr<FaceProfile> parse(std::string_view json, const GltfHeadMesh& head);
  };

// The face of an own head: the old face animations through the profile make one vector of the head's shape
// weights, it deforms the head on the CPU (positions and normals of every drawn vertex, for setDeformation).
// Time as VisualObjects plays the morphs of the original heads: a slot per anim, fade in and out, frames in time
// or set by hand (VISEME), one anim per layer giving way to the next one.
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
      std::string shown;            // the active anims and their strength: "S_ANGRY 0.82 R_EYESBLINK 1.00"
      };

    static constexpr float    Changed     = 1e-4f; // a weight change that deforms the head again
    static constexpr float    SpeechMouth = 0.7f;  // a voice takes that much of the mouth from the other layers
    static constexpr uint64_t ShapeHold   = 250;   // ms a shape set by hand stays without being set again
    // A target delta below both is not seen and not blended: exports carry normal noise on most of the mesh
    // (the MPFB test head: 357 thousand normal deltas, 104 thousand above 1e-3)
    static constexpr float    StillCm     = 1e-4f; // cm of position
    static constexpr float    StillNormal = 1e-3f; // of a normal's component

    void   setup(const GltfHeadMesh* head, const FaceProfile* profile);
    bool   isActive() const { return head!=nullptr && profile!=nullptr; }
    // the view of the head was made again: the deformation goes to it on the next update
    void   invalidate() { dirty = true; }

    // as VisualObjects::startMMAnim: timeUntil uint64_t(-1) - its own duration or until stopped, below now - stop
    void   startAnim(std::string_view name, float intensity, uint64_t timeUntil, uint64_t now);
    // as VisualObjects::setMMShape: two frames mixed by alpha, set every frame by the owner (lip sync)
    void   setShape (std::string_view name, uint32_t frame0, uint32_t frame1, float alpha, float intensity, uint64_t now);
    // as shown now: intensity and fade
    float  weight   (std::string_view name, uint64_t now) const;
    // per frame of the anim: how far it moves the vertices, 0..1 of its strongest (LipSync::setShapes)
    std::vector<float> strength(std::string_view name) const;

    Result update(uint64_t now);
    const std::vector<float>&         weights()     const { return applied; }
    const std::vector<Tempest::Vec4>& deformation() const { return deform; }
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

    bool   isOn(const Slot& s, uint64_t now) const { return s.on && s.timeUntil>=now; }
    float  fade(const Slot& s, uint64_t now) const;
    void   fadeOut(Slot& s, uint64_t now);
    Slot&  freeSlot(uint64_t now);
    void   mix(uint64_t now, std::vector<float>& out) const;
    void   blend();
    void   prepare();
    void   write(uint32_t v);

    const GltfHeadMesh*        head    = nullptr;
    const FaceProfile*         profile = nullptr;
    int32_t                    speechLayer = -1;  // the layer of VISEME

    std::vector<Slot>          slots;
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
    float                      blendUs = 0;
    uint32_t                   blends  = 0;
  };
