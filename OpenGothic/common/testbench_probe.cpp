#include "testbench.h"
#include "testbench_internal.h"
#include "world/benchhooks.h"

namespace {
class Probe final : public BenchHooks {
  public:
    bool active() const override { return TestBench::isActive(); }
    bool orderedVisibility() const override { return TestBench::orderedVisibility(); }
    HistoryVerifyRequest historyVerifyRequest() override {
      auto& bench = testbench_detail::bench;
      auto& verify = bench.historyVerify;
      if(verify.finish) {
        HistoryVerifyRequest request;
        request.finish = true;
        request.bough = verify.bough;
        return request;
        }
      if(verify.next==verify.frames)
        return {};
      const auto elapsed = bench.time - verify.start;
      if(verify.duration>0 && elapsed*(verify.frames-1) < verify.duration*verify.next)
        return {};
      return {verify.segment, verify.total++, verify.next++, elapsed, false, verify.bough};
      }
    void historyVerifyComplete(bool passed) override {
      auto& verify = testbench_detail::bench.historyVerify;
      verify.finish = false;
      testbench_detail::event(verify.bough ? "bough_hiz_verify" : "hiz_history_verify", "\"pairs\":"+std::to_string(verify.total)+
                             ",\"passed\":"+(passed ? "true" : "false"));
      if(!passed)
        testbench_detail::say(verify.bough ? "Bough HiZ verification failed. See bough-hiz-verify.csv." :
                                           "HiZ history verification failed. See tree-hidden-3b.csv.");
      verify.total = 0;
      }
    int bodyBlockVariant(const Npc& npc, int fallback) const override {
      return TestBench::bodyBlockVariant(npc,fallback);
      }
    void destructionBreak(uintptr_t body, size_t pieces, float milliseconds, bool remnant) override {
      TestBench::destructionBreak(body,pieces,milliseconds,remnant);
      }
    void destructionRetire(uintptr_t body, size_t piece, bool expired, float observerDistanceCm) override {
      TestBench::destructionRetire(body,piece,expired,observerDistanceCm);
      }
    void ragdollBirthPeak(const Npc& npc, const Npc* culprit, const std::string& fields) override {
      TestBench::ragdollBirthPeak(npc,culprit,fields);
      }
    bool strictGraphMovement(const Npc& npc) override {
      return TestBench::strictGraphMovement(npc);
      }
    void npcDuelSwingStart(const Npc& npc) override {
      TestBench::npcDuelSwingStart(npc);
      }
    void npcDuelSwingEnd(const Npc& npc, std::string_view outcome) override {
      TestBench::npcDuelSwingEnd(npc,outcome);
      }
    void npcDuelHit(const Npc& victim, const Npc& by, int32_t hpBefore, int32_t hpAfter, int32_t baseDamage, bool critical,
                    float meleeScale, float stunBonus, float backstabMult) override {
      TestBench::npcDuelHit(victim,by,hpBefore,hpAfter,baseDamage,critical,meleeScale,stunBonus,backstabMult);
      }
    uint64_t stepCount() override {
      return TestBench::stepCount();
      }
    uint64_t frameDt(uint64_t dt) override {
      return TestBench::frameDt(dt);
      }
    bool deterministic() override {
      return TestBench::deterministic();
      }
    bool simulationFrozen() override {
      return TestBench::simulationFrozen();
      }
    bool takeRenderReset() override {
      return TestBench::takeRenderReset();
      }
    void beforeWorldTick(uint64_t dt) override {
      TestBench::beforeWorldTick(dt);
      }
    BenchNpcTick beforeNpcTick(Npc& npc) override {
      return TestBench::beforeNpcTick(npc);
      }
    bool tick(uint64_t dt, PlayerControl& player) override {
      return TestBench::tick(dt,player);
      }
    void sampleSocietyPose(World& world) override {
      TestBench::sampleSocietyPose(world);
      }
    void sampleStreaming(const World& world) override {
      TestBench::sampleStreaming(world);
      }
    void capture(Tempest::Pixmap& frame, float uiScale) override {
      TestBench::capture(frame,uiScale);
      }
    void note(const char* text) override {
      TestBench::note(text);
      }
    void physicsNoise(const PhysicsNoise& noise, std::string_view sound, bool played) override {
      TestBench::physicsNoise(noise,sound,played);
      }
    void physicsNoiseHeard(const Npc& npc, const PhysicsNoise& noise, bool callbackDispatched) override {
      TestBench::physicsNoiseHeard(npc,noise,callbackDispatched);
      }
    void destructionMeleeHit(const Npc& actor, size_t affected, float damage) override {
      TestBench::destructionMeleeHit(actor,affected,damage);
      }
    void movementImpulseHandoff(const Npc& actor, std::string_view backend, uint32_t fromState, uint32_t toState,
                                const Tempest::Vec3& velocity) override {
      TestBench::movementImpulseHandoff(actor,backend,fromState,toState,velocity);
      }
    void monsterCharacterCollision(const Npc& actor, const Npc* target) override {
      TestBench::monsterCharacterCollision(actor,target);
      }
    bool isPhysicsNoiseDebug() override {
      return TestBench::isPhysicsNoiseDebug();
      }
    void physicsNoiseBridgeEvent(const Npc& listener, uint64_t tick) override {
      TestBench::physicsNoiseBridgeEvent(listener,tick);
      }
    bool physicsNoiseDebug(World& world, Npc& player, std::string_view command, std::string& value,
                           std::string& error) override {
      return TestBench::physicsNoiseDebug(world,player,command,value,error);
      }
    bool physicsTracesDebug(World& world, Npc& player, std::string_view command, std::string& value,
                            std::string& error) override {
      return TestBench::physicsTracesDebug(world,player,command,value,error);
      }
    bool destructionTracesDebug(World& world, Npc& player, std::string_view command, std::string& value,
                                std::string& error) override {
      return TestBench::destructionTracesDebug(world,player,command,value,error);
      }
    void arrowAt(const void* arrow, float x, float y, float z) override {
      TestBench::arrowAt(arrow,x,y,z);
      }
    void ownMeshArrowHit(uint32_t zenId, const Tempest::Vec3& point, const Tempest::Vec3& normal) override {
      TestBench::ownMeshArrowHit(zenId,point,normal);
      }
    bool bodyTracing() override {
      return TestBench::bodyTracing();
      }
    void bodyEvent(const Npc& actor, const Npc* target, std::string_view phase, uint64_t gameTime,
                   const std::string& fields, std::string_view animation) override {
      TestBench::bodyEvent(actor,target,phase,gameTime,fields,animation);
      }
    void bodyForget(const Npc* npc) override {
      TestBench::bodyForget(npc);
      }
    void npcOutput(const Npc& npc, std::string_view name, const Npc* target, uint64_t ms) override {
      TestBench::npcOutput(npc,name,target,ms);
      }
    bool filming() override {
      return TestBench::filming();
      }
    bool withUi() override {
      return TestBench::withUi();
      }
    bool ragdollLootOpened(const Npc& player, const Npc& target) override {
      return TestBench::ragdollLootOpened(player,target);
      }
    void aiLine(std::string_view npc, std::string_view name, std::string_view text, uint64_t ms, std::string_view to,
                bool filler) override {
      TestBench::aiLine(npc,name,text,ms,to,filler);
      }
    void aiVoiceEnd(std::string_view npc, uint64_t ms) override {
      TestBench::aiVoiceEnd(npc,ms);
      }
    bool voiceStems() override {
      return TestBench::voiceStems();
      }
    std::function<void(const int16_t*,size_t)> aiVoiceStem(std::string_view npc, std::string_view line,
                       uint32_t rate) override {
      return TestBench::aiVoiceStem(npc,line,rate);
      }
    void aiHeard(std::string_view text, std::string_view to) override {
      TestBench::aiHeard(text,to);
      }
    bool bodySweepAllowed() override {
      return TestBench::bodySweepAllowed();
      }
  };
}

void TestBench::install() {
  static Probe probe;
  BenchHooks::install(&probe);
  }

std::string_view TestBench::who(const Npc* npc) {
  return Bench::who(npc);
  }
