#pragma once

#include "asset/modeltypes.h"

#include <Tempest/Vec>
#include <memory>

class Npc;
class MdlVisual;
class Interactive;
class World;

class Animation final {
  public:
    enum AnimClass : uint8_t {
      UnknownAnim=0,
      Transition,
      Loop
      };

    struct EvTimed final {
      asset::MdsEventType def = asset::MdsEventType::UNKNOWN;
      std::string_view     item;
      std::string_view     slot[2] = {};
      uint64_t             time    = 0;
      };

    struct EvMorph final {
      std::string_view node;
      std::string_view anim;
      };

    struct EvCount final {
      uint8_t              def_opt_frame = 0;
      uint8_t              groundSounds = 0;
      asset::MdsFightMode weaponCh = asset::MdsFightMode::INVALID;
      std::vector<EvTimed> timed;
      std::vector<EvMorph> morph;
      };

    struct MdsParticleEffect : asset::MdsParticleEffect {
      bool pfxStop = false;
      };

    struct AnimData final {
      Tempest::Vec3                               translate={};
      Tempest::Vec3                               moveTr={};

      std::vector<asset::AnimationSample>        samples;
      std::vector<uint32_t>                       nodeIndex;
      std::vector<Tempest::Vec3>                  tr;
      bool                                        hasMoveTr=false;

      uint32_t                                    firstFrame=0;
      uint32_t                                    lastFrame =0;
      float                                       fpsRate   =60.f;
      uint32_t                                    numFrames =0;

      std::vector<asset::MdsSoundEffectGround>   gfx;
      std::vector<asset::MdsSoundEffect>         sfx;
      std::vector<MdsParticleEffect>              pfx;
      std::vector<asset::MdsModelTag>            tag;
      std::vector<asset::MdsEventTag>            events;

      std::vector<asset::MdsMorphAnimation>      mmStartAni;

      std::vector<uint64_t>                       defHitEnd;   // hit-end time
      std::vector<uint64_t>                       defParFrame;
      std::vector<uint64_t>                       defWindow;

      void                                        setupMoveTr();
      void                                        setupEvents(float fpsRate);
      };

    struct Sequence final {
      Sequence()=default;
      Sequence(const asset::MdsAnimation& hdr, std::string_view name);

      bool                                   isRotate() const { return bool(flags & asset::AnimationFlags::ROTATE); }
      bool                                   isMove()   const { return bool(flags & asset::AnimationFlags::MOVE);   }
      bool                                   isFly()    const { return bool(flags & asset::AnimationFlags::FLY);    }
      bool                                   isIdle()   const { return bool(flags & asset::AnimationFlags::IDLE);   }
      bool                                   isFinished(uint64_t now, uint64_t sTime, uint16_t comboLen) const;
      float                                  atkTotalTime(uint16_t comboLen) const;
      bool                                   canInterrupt(uint64_t now, uint64_t sTime, uint16_t comboLen) const;
      bool                                   isInComboWindow(uint64_t t, uint16_t comboLen) const;
      bool                                   isDefParWindow(uint64_t t) const;
      bool                                   isDefWindow(uint64_t t) const;
      float                                  totalTime() const;

      bool                                   isAttackAnim() const;
      bool                                   isPrehit(uint64_t sTime, uint64_t now) const;
      void                                   processEvents(uint64_t barrier, uint64_t sTime, uint64_t now, EvCount& ev) const;
      void                                   processSfx   (uint64_t barrier, uint64_t sTime, uint64_t now, Npc* npc, Interactive* mob) const;
      void                                   processPfx   (uint64_t barrier, uint64_t sTime, uint64_t now, MdlVisual& visual, World& world) const;
      void                                   processPfx   (const MdsParticleEffect& p, MdlVisual& visual, World& world) const;

      Tempest::Vec3                          speed(uint64_t at, uint64_t dt) const;
      Tempest::Vec3                          translateXZ(uint64_t at) const;
      void                                   schemeName(char buf[64]) const;

      std::string                            name, askName;
      const char*                            shortName = nullptr;
      uint32_t                               layer     = 0;
      asset::AnimationFlags                 flags     = asset::AnimationFlags::NONE;
      uint64_t                               blendIn   = 0;
      uint64_t                               blendOut  = 0;
      AnimClass                              animCls   = Transition;
      bool                                   reverse   = false;

      std::string                            next;
      const Sequence*                        nextPtr = nullptr;
      const Animation*                       owner   = nullptr;

      std::vector<const Sequence*>           comb;
      std::shared_ptr<AnimData>              data;

      private:
        static void                          processEvent(const asset::MdsEventTag& e, EvCount& ev, uint64_t time);
        bool                                 extractFrames(uint64_t &frameA, uint64_t &frameB, bool &invert, uint64_t barrier, uint64_t sTime, uint64_t now) const;
      };


    Animation(asset::ModelScript &p, std::string_view name, bool ignoreErrChunks);

    const Sequence*    sequence(std::string_view name) const;
    const Sequence*    sequenceAsc(std::string_view name) const;
    void               debug() const;
    std::string_view   defaultMesh() const;

  private:
    Sequence&          loadMAN(const asset::MdsAnimation& hdr, std::string_view name);
    void               setupIndex();

    mutable const Sequence*                  sqHot = nullptr;
    std::vector<Sequence>                    sequences;
    std::vector<asset::MdsAnimationAlias>   ref;
    std::vector<std::string>                 mesh;
    asset::MdsSkeleton                      meshDef;
  };
