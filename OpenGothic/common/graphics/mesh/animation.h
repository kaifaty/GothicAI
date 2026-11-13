#pragma once

#include "asset/modeltypes.h"

#include <Tempest/Vec>
#include <memory>

class AnimationGroundSound;
class AnimationSound;
class AnimationPfx;

class Animation final {
  public:
    size_t probeCpuBytes() const;

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

    // Sequence-local event clock for the logical NPC body. Times are relative
    // to the layer start, before its playback speed is applied.
    struct LogicEvent final {
      enum Kind : uint8_t { Tag, Ground, Morph, Sound, Particle, HitEnd, ParFrame, Window };
      Kind                kind = Tag;
      asset::MdsEventType  def = asset::MdsEventType::UNKNOWN;
      uint64_t            time = 0;
      uint64_t            frame = 0;
      size_t              index = 0;
      std::string_view    item;
      std::string_view    slot[2] = {};
      // Only these events are delivered by processEvents; sounds, particles
      // and the queried combat-window boundaries have separate consumers.
      bool                delivered = false;
      };

    struct LogicEventObserver final {
      void* context = nullptr;
      void (*fired)(void*, const LogicEvent&) = nullptr;
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
      uint32_t                                    restFrame=0; // the root stays put from this frame on (StopFoot::restFrame)

      uint32_t                                    firstFrame=0;
      uint32_t                                    lastFrame =0;
      float                                       fpsRate   =60.f;
      uint32_t                                    numFrames =0;
      bool                                        sparse    =false; // few keys a second: on a curve (animsample.h)

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
      // Frames of one cycle when the sequence loops. A loop that carries the body (walk, run, strafe, swim) ends
      // on the first frame of its next cycle moved by the travel: that is how Gothic's loops are cut and how moveTr
      // and tr count them (numFrames-1 steps per cycle). Its pose then wraps over numFrames-1 frames too, or it
      // would stand still on the repeated frame once per cycle while the body moves on. Loops in place wrap over
      // all frames, as before.
      uint32_t                                    loopFrames() const;
      // the frame of the sequence an optimal-frame tag (DEF_OPT_FRAME) points at
      uint32_t                                    optimalFrame(int32_t frame) const;
      };

    struct Sequence final {
      Sequence()=default;
      Sequence(const asset::MdsAnimation& hdr, std::string_view name);

      bool                                   isRotate() const { return bool(flags & asset::AnimationFlags::ROTATE); }
      bool                                   isMove()   const { return bool(flags & asset::AnimationFlags::MOVE);   }
      bool                                   isFly()    const { return bool(flags & asset::AnimationFlags::FLY);    }
      bool                                   isIdle()   const { return bool(flags & asset::AnimationFlags::IDLE);   }
      bool                                   isSettle() const { return bool(flags & asset::AnimationFlags::SETTLE); }
      bool                                   isFinished(uint64_t now, uint64_t sTime, uint16_t comboLen) const;
      float                                  atkTotalTime(uint16_t comboLen) const;
      bool                                   canInterrupt(uint64_t now, uint64_t sTime, uint16_t comboLen) const;
      bool                                   isInComboWindow(uint64_t t, uint16_t comboLen) const;
      bool                                   isDefParWindow(uint64_t t) const;
      bool                                   isDefWindow(uint64_t t) const;
      float                                  totalTime() const;

      bool                                   isAttackAnim() const;
      bool                                   isPrehit(uint64_t sTime, uint64_t now) const;
      void                                   processEvents(uint64_t barrier, uint64_t sTime, uint64_t now, EvCount& ev,
                                                           const LogicEventObserver* observer = nullptr) const;
      void                                   logicEvents(std::vector<LogicEvent>& out) const;
      LogicEvent                             logicTag(size_t index) const;
      void                                   processSfx   (uint64_t barrier, uint64_t sTime, uint64_t now, AnimationGroundSound* npc, AnimationSound* mob) const;
      void                                   processPfx   (uint64_t barrier, uint64_t sTime, uint64_t now, AnimationPfx& world) const;
      void                                   processPfx   (const MdsParticleEffect& p, AnimationPfx& world) const;

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
      bool                                   ownReload = false; // the reload from the quiver (BowDraw::QuiverReloadAnim)

      std::string                            next;
      const Sequence*                        nextPtr = nullptr;
      const Animation*                       owner   = nullptr;

      std::vector<const Sequence*>           comb;
      std::shared_ptr<AnimData>              data;

      private:
        LogicEvent                           logicEvent(LogicEvent::Kind kind, size_t index, uint64_t frame) const;
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
