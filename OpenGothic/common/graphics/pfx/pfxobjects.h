#pragma once

#include <functional>
#include <Tempest/Matrix4x4>
#include <Tempest/UniformBuffer>

#include <memory>
#include <list>

#include "graphics/visualobjects.h"
#include "graphics/pfx/pfxslot.h"
#include "graphics/pfx/pfxbucket.h"

class SceneGlobals;
class ParticleFx;
class WorldView;

class PfxObjects final {
  public:
    PfxObjects(WorldView& world, const SceneGlobals& scene, VisualObjects& visual);
    ~PfxObjects();

    static constexpr const float viewRage = 4000.f;

    void       setViewerPos(const Tempest::Vec3& pos);
    // the world's wind this frame, cm/ms, and how open a place is to it (0..1; nullptr: open) - particles.d "wind"
    void       setWind(const Tempest::Vec3& wind);
    void       setShelter(std::function<float(const Tempest::Vec3& at, const Tempest::Vec3& wind)> open);
    // where the way from one point to another first meets the world's static geometry (particles.d "land");
    // nullptr: particles meet nothing
    void       setGround(std::function<bool(const Tempest::Vec3& from, const Tempest::Vec3& to, Tempest::Vec3& hit)> ray);

    void       resetTicks();
    void       tick(uint64_t ticks);
    bool       isInPfxRange(const Tempest::Vec3& pos) const;

    void       preFrameUpdate(uint8_t fId);

    void       drawGBuffer    (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, bool historyOwner = false);
    void       drawShadow     (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId, int layer);
    void       drawMultiply   (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);
    void       drawTranslucent(Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);
    bool       hasHaze        (uint8_t fId) const;
    void       drawHaze       (Tempest::Encoder<Tempest::CommandBuffer>& cmd, uint8_t fId);

    // Published buckets of the frame being recorded: tick writes `bucket`, recording reads pubBucket.
    // Rebuilt at the end of preFrameUpdate.
    struct PubBucket {
      PfxBucket*         b = nullptr;
      PfxBucket::PubDraw d;
      };
    const std::vector<PubBucket>& published() const { return pubBucket; }

  private:
    struct SpriteEmitter {
      asset::SpriteAlignment     visualCamAlign = asset::SpriteAlignment::NONE;
      int32_t                     zBias          = 0;
      Tempest::Vec2               decalDim = {};
      std::unique_ptr<ParticleFx> pfx;
      };

    PfxBucket&                    getBucket(const ParticleFx& decl);
    PfxBucket&                    getBucket(const Material& mat, const asset::VirtualObject& vob);

    WorldView&                    world;
    const SceneGlobals&           scene;
    VisualObjects&                visual;
    std::recursive_mutex          sync;

    std::list<PfxBucket>          bucket;
    std::vector<PubBucket>        pubBucket;
    std::vector<SpriteEmitter>    spriteEmit;

    Tempest::Vec3                 viewerPos={};
    Tempest::Vec3                 wind={};
    std::function<float(const Tempest::Vec3&,const Tempest::Vec3&)> shelter;
    std::function<bool(const Tempest::Vec3&,const Tempest::Vec3&,Tempest::Vec3&)> ground;

    // particles.d "land": a particle that met the ground asks for its effect there; the effects are put after the
    // tick of all buckets, and their emitters go when they have emitted (their particles live on)
    struct Landing {
      PfxSlot  slot;
      uint64_t until = 0;
      };
    std::vector<std::pair<const ParticleFx*,Tempest::Vec3>> landAsked;
    std::vector<Landing>          landings;
    void       putLandings();
    uint64_t                      lastUpdate=0;
    uint64_t                      now=0;  // the world tick of the update: the curl field's clock and the flicker's

  friend class PfxSlot;
  friend class PfxBucket;
  friend class TrlObjects;
  };
