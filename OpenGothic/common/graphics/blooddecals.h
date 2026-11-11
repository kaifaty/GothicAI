#pragma once

#include <Tempest/Encoder>
#include <Tempest/CommandBuffer>
#include <Tempest/StorageBuffer>
#include <Tempest/Texture2d>
#include <Tempest/Attachment>
#include <Tempest/ZBuffer>
#include <Tempest/Pixmap>
#include <Tempest/Vec>

#include <vector>

#include "resources.h"
#include "bloodstain.h"

class World;
class BloodDecalSource;
class SceneGlobals;

// Blood on the world as projected decals on the GBuffer (physics stage 8, step 4c).
// The stains themselves live in World (World::bloodStains); this draws them after the GBuffer
// and before lighting, so blood is lit, shadowed and, with material maps, glossy while wet.
// Research, targets and acceptance: reports/physics/2026-11-02-blood/README.md.
class BloodDecals final {
  public:
    BloodDecals();

    // Per-frame instance data of the stains near the camera; false: nothing to draw.
    bool prepare(uint8_t fId, const World* world, const SceneGlobals& scene, const Tempest::Vec3& camera);
    void draw   (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, const SceneGlobals& scene,
                 Tempest::Attachment& gbufDiffuse, Tempest::Attachment* gbufOrm,
                 const Tempest::Attachment& gbufNormal, const Tempest::ZBuffer& zbuffer);

    size_t drawCount() const { return count; }

    // Visual curves of a stain over game time (pure, for the contract and the bench).
    struct Look {
      float growth  = 1; // pool spread 0..1
      float wetness = 1; // gloss 0..1
      float dryness = 0; // fresh red 0 .. brown 1
      float opacity = 1; // thins out at the end of the lifetime
      };
    static Look look(const BloodStains::Stain& s, int64_t now);

    // Atlas of stain shapes: R shape, G thickness, B pool arrival time. 4x4 tiles: 0..11 splashes,
    // 12..15 pools. Procedural and deterministic, so no binary art goes into git.
    static Tempest::Pixmap buildAtlas(uint32_t tile = 256);
    static constexpr uint32_t AtlasTiles  = 4;
    static constexpr uint32_t SplashTiles = 12;
    static constexpr uint32_t PoolTiles   = 4;

  private:
    bool prepare(uint8_t fId, const BloodDecalSource& world, const SceneGlobals& scene, const Tempest::Vec3& camera);

    struct Gpu {
      Tempest::Vec4 world, center, axisU, axisV, axisW, uvRect, look, tint;
      };

    Tempest::Texture2d          atlas;
    Tempest::StorageBuffer      ssbo[Resources::MaxFramesInFlight];
    struct Near {
      float                     dist  = 0;
      const BloodStains::Stain* stain = nullptr;
      };
    std::vector<Near>           near;
    std::vector<Gpu>            data;
    size_t                      count = 0;
  };
