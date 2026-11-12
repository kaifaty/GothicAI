#pragma once

#include "benchprobe.h"

#include <functional>
#include <Tempest/Vec>

namespace Tempest {
class Pixmap;
}
class PlayerControl;
class World;
struct PhysicsNoise;

class BenchHooks : public BenchProbe {
  public:
    static void install(BenchHooks* hooks) { BenchProbe::install(hooks); }
    // Both interfaces share the sole process pointer, always installed as BenchHooks.
    static BenchHooks* get() { return static_cast<BenchHooks*>(BenchProbe::get()); }

    virtual bool strictGraphMovement(const Npc& npc) = 0;
    virtual void npcDuelSwingStart(const Npc& npc) = 0;
    virtual void npcDuelSwingEnd(const Npc& npc, std::string_view outcome) = 0;
    virtual void npcDuelHit(const Npc& victim, const Npc& by, int32_t hpBefore, int32_t hpAfter, int32_t baseDamage,
                            bool critical, float meleeScale, float stunBonus, float backstabMult) = 0;
    virtual uint64_t stepCount() = 0;
    virtual uint64_t frameDt(uint64_t dt) = 0;
    virtual bool deterministic() = 0;
    virtual bool simulationFrozen() = 0;
    virtual bool takeRenderReset() = 0;
    virtual void beforeWorldTick(uint64_t dt) = 0;
    virtual void beforeNpcTick(Npc& npc) = 0;
    virtual bool tick(uint64_t dt, PlayerControl& player) = 0;
    virtual void sampleStreaming(const World& world) = 0;
    virtual void capture(Tempest::Pixmap& frame, float uiScale) = 0;
    virtual void note(const char* text) = 0;
    virtual void physicsNoise(const PhysicsNoise& noise, std::string_view sound, bool played) = 0;
    virtual void physicsNoiseHeard(const Npc& npc, const PhysicsNoise& noise, bool callbackDispatched) = 0;
    virtual void destructionMeleeHit(const Npc& actor, size_t affected, float damage) = 0;
    virtual void movementImpulseHandoff(const Npc& actor, std::string_view backend, uint32_t fromState, uint32_t toState,
                                        const Tempest::Vec3& velocity) = 0;
    virtual void monsterCharacterCollision(const Npc& actor, const Npc* target) = 0;
    virtual bool isPhysicsNoiseDebug() = 0;
    virtual void physicsNoiseBridgeEvent(const Npc& listener, uint64_t tick) = 0;
    virtual bool physicsNoiseDebug(World& world, Npc& player, std::string_view command, std::string& value,
                                   std::string& error) = 0;
    virtual bool physicsTracesDebug(World& world, Npc& player, std::string_view command, std::string& value,
                                    std::string& error) = 0;
    virtual bool destructionTracesDebug(World& world, Npc& player, std::string_view command, std::string& value,
                                        std::string& error) = 0;
    virtual void arrowAt(const void* arrow, float x, float y, float z) = 0;
    virtual void ownMeshArrowHit(uint32_t zenId, const Tempest::Vec3& point, const Tempest::Vec3& normal) = 0;
    virtual bool bodyTracing() = 0;
    virtual void bodyEvent(const Npc& actor, const Npc* target, std::string_view phase, uint64_t gameTime,
                           const std::string& fields, std::string_view animation) = 0;
    virtual void bodyForget(const Npc* npc) = 0;
    virtual void npcOutput(const Npc& npc, std::string_view name, const Npc* target, uint64_t ms) = 0;
    virtual bool filming() = 0;
    virtual bool withUi() = 0;
    virtual bool ragdollLootOpened(const Npc& player, const Npc& target) = 0;
    virtual void aiLine(std::string_view npc, std::string_view name, std::string_view text, uint64_t ms,
                        std::string_view to, bool filler) = 0;
    virtual void aiVoiceEnd(std::string_view npc, uint64_t ms) = 0;
    virtual bool voiceStems() = 0;
    virtual std::function<void(const int16_t*,size_t)> aiVoiceStem(std::string_view npc, std::string_view line,
                               uint32_t rate) = 0;
    virtual void aiHeard(std::string_view text, std::string_view to) = 0;
    virtual bool bodySweepAllowed() = 0;
  };

namespace Bench {
  inline bool isActive() {
    if(auto* hooks = BenchHooks::get())
      return hooks->active();
    return false;
    }

  inline bool strictGraphMovement(const Npc& npc) {
    if(auto* hooks = BenchHooks::get())
      return hooks->strictGraphMovement(npc);
    return false;
    }

  inline void npcDuelSwingStart(const Npc& npc) {
    if(auto* hooks = BenchHooks::get())
      hooks->npcDuelSwingStart(npc);
    }

  inline void npcDuelSwingEnd(const Npc& npc, std::string_view outcome) {
    if(auto* hooks = BenchHooks::get())
      hooks->npcDuelSwingEnd(npc,outcome);
    }

  inline void npcDuelHit(const Npc& victim, const Npc& by, int32_t hpBefore, int32_t hpAfter, int32_t baseDamage,
                         bool critical, float meleeScale, float stunBonus, float backstabMult) {
    if(auto* hooks = BenchHooks::get())
      hooks->npcDuelHit(victim,by,hpBefore,hpAfter,baseDamage,critical,meleeScale,stunBonus,backstabMult);
    }

  inline uint64_t stepCount() {
    if(auto* hooks = BenchHooks::get())
      return hooks->stepCount();
    return 0;
    }

  inline uint64_t frameDt(uint64_t dt) {
    if(auto* hooks = BenchHooks::get())
      return hooks->frameDt(dt);
    return dt;
    }

  inline bool deterministic() {
    if(auto* hooks = BenchHooks::get())
      return hooks->deterministic();
    return false;
    }

  inline bool simulationFrozen() {
    if(auto* hooks = BenchHooks::get())
      return hooks->simulationFrozen();
    return false;
    }

  inline bool takeRenderReset() {
    if(auto* hooks = BenchHooks::get())
      return hooks->takeRenderReset();
    return false;
    }

  inline bool orderedVisibility() {
    if(auto* hooks = BenchHooks::get())
      return hooks->orderedVisibility();
    return false;
    }

  inline void beforeWorldTick(uint64_t dt) {
    if(auto* hooks = BenchHooks::get())
      hooks->beforeWorldTick(dt);
    }

  inline void beforeNpcTick(Npc& npc) {
    if(auto* hooks = BenchHooks::get())
      hooks->beforeNpcTick(npc);
    }

  inline bool tick(uint64_t dt, PlayerControl& player) {
    if(auto* hooks = BenchHooks::get())
      return hooks->tick(dt,player);
    return false;
    }

  inline void sampleStreaming(const World& world) {
    if(auto* hooks = BenchHooks::get())
      hooks->sampleStreaming(world);
    }

  inline void capture(Tempest::Pixmap& frame, float uiScale) {
    if(auto* hooks = BenchHooks::get())
      hooks->capture(frame,uiScale);
    }

  inline void note(const char* text) {
    if(auto* hooks = BenchHooks::get())
      hooks->note(text);
    }

  inline void physicsNoise(const PhysicsNoise& noise, std::string_view sound, bool played) {
    if(auto* hooks = BenchHooks::get())
      hooks->physicsNoise(noise,sound,played);
    }

  inline void physicsNoiseHeard(const Npc& npc, const PhysicsNoise& noise, bool callbackDispatched = false) {
    if(auto* hooks = BenchHooks::get())
      hooks->physicsNoiseHeard(npc,noise,callbackDispatched);
    }

  inline void destructionBreak(uintptr_t body, size_t pieces, float milliseconds, bool remnant = false) {
    if(auto* hooks = BenchHooks::get())
      hooks->destructionBreak(body,pieces,milliseconds,remnant);
    }

  inline void destructionRetire(uintptr_t body, size_t piece, bool expired, float observerDistanceCm) {
    if(auto* hooks = BenchHooks::get())
      hooks->destructionRetire(body,piece,expired,observerDistanceCm);
    }

  inline void destructionMeleeHit(const Npc& actor, size_t affected, float damage) {
    if(auto* hooks = BenchHooks::get())
      hooks->destructionMeleeHit(actor,affected,damage);
    }

  inline void movementImpulseHandoff(const Npc& actor, std::string_view backend, uint32_t fromState, uint32_t toState,
                                     const Tempest::Vec3& velocity) {
    if(auto* hooks = BenchHooks::get())
      hooks->movementImpulseHandoff(actor,backend,fromState,toState,velocity);
    }

  inline void monsterCharacterCollision(const Npc& actor, const Npc* target) {
    if(auto* hooks = BenchHooks::get())
      hooks->monsterCharacterCollision(actor,target);
    }

  inline bool isPhysicsNoiseDebug() {
    if(auto* hooks = BenchHooks::get())
      return hooks->isPhysicsNoiseDebug();
    return false;
    }

  inline void physicsNoiseBridgeEvent(const Npc& listener, uint64_t tick) {
    if(auto* hooks = BenchHooks::get())
      hooks->physicsNoiseBridgeEvent(listener,tick);
    }

  inline bool physicsNoiseDebug(World& world, Npc& player, std::string_view command, std::string& value,
                                std::string& error) {
    if(auto* hooks = BenchHooks::get())
      return hooks->physicsNoiseDebug(world,player,command,value,error);
    return false;
    }

  inline bool physicsTracesDebug(World& world, Npc& player, std::string_view command, std::string& value,
                                 std::string& error) {
    if(auto* hooks = BenchHooks::get())
      return hooks->physicsTracesDebug(world,player,command,value,error);
    return false;
    }

  inline bool destructionTracesDebug(World& world, Npc& player, std::string_view command, std::string& value,
                                     std::string& error) {
    if(auto* hooks = BenchHooks::get())
      return hooks->destructionTracesDebug(world,player,command,value,error);
    return false;
    }

  inline void arrowAt(const void* arrow, float x, float y, float z) {
    if(auto* hooks = BenchHooks::get())
      hooks->arrowAt(arrow,x,y,z);
    }

  inline void ownMeshArrowHit(uint32_t zenId, const Tempest::Vec3& point, const Tempest::Vec3& normal) {
    if(auto* hooks = BenchHooks::get())
      hooks->ownMeshArrowHit(zenId,point,normal);
    }

  inline bool bodyTracing() {
    if(auto* hooks = BenchHooks::get())
      return hooks->bodyTracing();
    return false;
    }

  inline void bodyEvent(const Npc& actor, const Npc* target, std::string_view phase, uint64_t gameTime,
                        const std::string& fields = {}, std::string_view animation = {}) {
    if(auto* hooks = BenchHooks::get())
      hooks->bodyEvent(actor,target,phase,gameTime,fields,animation);
    }

  inline void bodyForget(const Npc* npc) {
    if(auto* hooks = BenchHooks::get())
      hooks->bodyForget(npc);
    }

  inline std::string bodyJson(std::string_view text) {
    if(auto* hooks = BenchHooks::get())
      return hooks->bodyJson(text);
    return {};
    }

  inline int bodyBlockVariant(const Npc& npc, int fallback) {
    if(auto* hooks = BenchHooks::get())
      return hooks->bodyBlockVariant(npc,fallback);
    return fallback;
    }

  inline std::string_view who(const Npc* npc) {
    if(auto* hooks = BenchHooks::get())
      return hooks->who(npc);
    return "none";
    }

  inline void npcOutput(const Npc& npc, std::string_view name, const Npc* target, uint64_t ms) {
    if(auto* hooks = BenchHooks::get())
      hooks->npcOutput(npc,name,target,ms);
    }

  inline bool filming() {
    if(auto* hooks = BenchHooks::get())
      return hooks->filming();
    return false;
    }

  inline bool withUi() {
    if(auto* hooks = BenchHooks::get())
      return hooks->withUi();
    return false;
    }

  inline bool ragdollLootOpened(const Npc& player, const Npc& target) {
    if(auto* hooks = BenchHooks::get())
      return hooks->ragdollLootOpened(player,target);
    return false;
    }

  inline void ragdollBirthPeak(const Npc& npc, const Npc* culprit, const std::string& fields) {
    if(auto* hooks = BenchHooks::get())
      hooks->ragdollBirthPeak(npc,culprit,fields);
    }

  inline void aiLine(std::string_view npc, std::string_view name, std::string_view text, uint64_t ms, std::string_view to,
                     bool filler = false) {
    if(auto* hooks = BenchHooks::get())
      hooks->aiLine(npc,name,text,ms,to,filler);
    }

  inline void aiVoiceEnd(std::string_view npc, uint64_t ms) {
    if(auto* hooks = BenchHooks::get())
      hooks->aiVoiceEnd(npc,ms);
    }

  inline bool voiceStems() {
    if(auto* hooks = BenchHooks::get())
      return hooks->voiceStems();
    return false;
    }

  inline std::function<void(const int16_t*,size_t)> aiVoiceStem(std::string_view npc, std::string_view line, uint32_t rate) {
    if(auto* hooks = BenchHooks::get())
      return hooks->aiVoiceStem(npc,line,rate);
    return {};
    }

  inline void aiHeard(std::string_view text, std::string_view to) {
    if(auto* hooks = BenchHooks::get())
      hooks->aiHeard(text,to);
    }

  inline bool bodySweepAllowed() {
    if(auto* hooks = BenchHooks::get())
      return hooks->bodySweepAllowed();
    return false;
    }

  // Set by the bridge; queried by bench commands in the main thread.
  extern std::function<bool()> aiBusy;
  extern std::function<bool(std::string_view key, std::string_view stage)> aiReached;
  extern std::function<bool(World&,std::string_view,float)> stuckBegin;
  extern std::function<bool(World&)> stuckEnd;
  extern std::function<std::string(World&,std::string_view npc)> npcView;
  // Set by the main window to open a menu by its script name.
  extern std::function<void(std::string_view menu)> openMenu;
  }
