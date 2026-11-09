#include "testbench_internal.h"
#include "testbench.h"

#include <Tempest/SoundDevice>

#include <algorithm>
#include <cmath>

#include "appcadence.h"
#include "frameprofile.h"
#include "navbench.h"
#include "navdemos.h"
#include "npcdayjournal.h"
#include "camera.h"
#include "game/archery.h"
#include "game/archerai.h"
#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "game/bodyobservations.h"
#include "game/navigationeyebench.h"
#include "game/navigationmemorybench.h"
#include "game/knownroadsbench.h"
#include "game/npckeysbench.h"
#include "graphics/mesh/footik.h"
#include "graphics/mesh/pose.h"
#include "graphics/worldview.h"
#include "world/chunkresidency.h"
#include "world/aiqueue.h"
#include "world/objects/npc.h"
#include "world/objects/item.h"
#include "world/objects/sound.h"
#include "world/world.h"
#include "gothic.h"

// resources.cpp: texture residency of a deterministic bench (`deterministic on`)
extern bool benchTexturesFrozen;

using namespace Tempest;
using namespace testbench_detail;

namespace {

// `deterministic on`: steps between the end of `streaming wait` and the next command; the TAA history of the shot
// holds all its jitter phases twice
constexpr uint32_t DeterministicSettleSteps = 16;

void tickRoute(Npc& player, uint64_t dt) {
  if(!bench.routeActive)return;
  if(bench.routeNext==bench.routePoints.size()) {
    // Keep the endpoint for one complete update/render before perf stop advances the DSL.
    bench.routeActive=false;player.setExternalControl(bench.routeWasExternal);
    event("route_end","\"speed_cm_s\":"+std::to_string(bench.routeSpeed)+",\"length_cm\":"+std::to_string(bench.routeLength)+
      ",\"elapsed_ms\":"+std::to_string(bench.time-bench.routeStarted)+",\"pass\":true");
    return;
    }
  float remaining=bench.routeSpeed*float(dt)/1000.f;
  Vec3 direction;
  while(remaining>0 && bench.routeNext<bench.routePoints.size()) {
    const auto delta=bench.routePoints[bench.routeNext]-bench.routePosition;
    const float distance=delta.length();
    direction=delta;
    if(distance<=remaining) {
      bench.routePosition=bench.routePoints[bench.routeNext++];remaining-=distance;
      } else {
      bench.routePosition+=delta*(remaining/distance);remaining=0;
      }
    }
  player.setPosition(bench.routePosition);face(player,direction);player.updateTransform();
  }

void tickVolley(PlayerControl& ctrl) {
  const auto& mp = Archer::playerProbe;
  const auto  t  = bench.time - bench.npcCommands.volleyT;
  auto stop = [&](const char* why) {
    say(std::string("volley: ") + why);
    release(ctrl,KeyCodec::ActionGeneric);
    bench.npcCommands.volleyLeft = 0;
    };
  switch(bench.npcCommands.volleyPhase) {
    case 0:
      press(ctrl,KeyCodec::ActionGeneric);
      bench.npcCommands.volleyPhase = 1;
      bench.npcCommands.volleyT     = bench.time;
      break;
    case 1: {
      if(mp.valid && mp.stage==Archer::Held && mp.draw<Archery::MinDraw) {
        stop("the draw limit is below the release threshold");
        break;
        }
      // Held means the archer reached its own limit, which can be below the requested draw.
      const bool drawn = mp.valid && (mp.stage==Archer::Held || (mp.stage==Archer::Drawing && mp.draw>=bench.npcCommands.volleyDraw));
      if(drawn) {
        bench.npcCommands.volleyPhase   = 2;
        bench.npcCommands.volleyT       = bench.time;
        // held at its limit it waits; drawn as far as asked short of it, it goes at once: the button held
        // draws on (the hero has no partial hold)
        bench.npcCommands.volleyHoldNow = mp.stage==Archer::Held ? bench.npcCommands.volleyHold : 0;
        if(bench.npcCommands.volleyJitter>0 && mp.stage==Archer::Held) {
          bench.lcg = bench.lcg*1664525u + 1013904223u;
          bench.npcCommands.volleyHoldNow += (bench.lcg>>8)%(bench.npcCommands.volleyJitter+1);
          }
        }
      else if(t>10000)
        stop("the string is not drawn");
      break;
      }
    case 2:
      if(t>=bench.npcCommands.volleyHoldNow) {
        if(bench.archery.aimNpc!=nullptr) {
          // where it aims, the middle of the zone: the log compares the arrow's flight with it
          Vec3 c = bench.archery.aimNpc->collosionCenter();
          aimPoint(*bench.archery.aimNpc,bench.archery.aimZone,bench.archery.aimLeg,c);
          char buf[160];
          std::snprintf(buf,sizeof(buf),"aim zone %.1f %.1f %.1f",c.x,c.y,c.z);
          TestBench::note(buf);
          }
        release(ctrl,KeyCodec::ActionGeneric);
        bench.npcCommands.volleyPhase = 3;
        bench.npcCommands.volleyT     = bench.time;
        }
      break;
    case 3:
      if(mp.valid && mp.stage==Archer::Nocked && t>200) {
        --bench.npcCommands.volleyLeft;
        bench.npcCommands.volleyPhase = 0;
        }
      else if(t>8000)
        stop("no next arrow");
      break;
    }
  }

// the hero's phrase is said: the talk key goes up
void endSpeaking() {
  std::string reply;
  aiCommand("ai ptt stop",reply);
  bench.speaking  = false;
  bench.heroEndAt = bench.time;
  event("hero_end");
  }

bool waitingForReply() {
  if(!bench.waitReply)
    return false;
  const bool replied = !bench.replyNeed || bench.lastLineAt>bench.replyFrom;
  const bool quiet   = bench.time>=bench.busyAt+bench.replyQuiet;
  if(replied && quiet) {
    bench.waitReply = false;
    return false;
    }
  if(bench.time<bench.replyEnd)
    return true;
  say(std::string(bench.replyNeed ? "waitreply" : "waitquiet") + ": timeout" + (replied ? "" : ", nobody answered"));
  event("timeout");
  bench.waitReply = false;
  return false;
  }

bool waitingForAi() {
  if(bench.waitAiKey.empty())
    return false;
  const bool signalled=bench.waitAiKey=="broken" && bench.brokenSignals.count(bench.waitAiStage)>0;
  if(signalled || (TestBench::aiReached && TestBench::aiReached(bench.waitAiKey,bench.waitAiStage))) {
    say("waitai " + bench.waitAiKey + " " + bench.waitAiStage);
    bench.waitAiKey.clear();
    return false;
    }
  if(bench.time<bench.waitAiEnd)
    return true;
  say("waitai " + bench.waitAiKey + " " + bench.waitAiStage + ": timeout");
  bench.waitAiKey.clear();
  return false;
  }

// One frame of `streaming wait`; true while the scenario stays in it. In deterministic mode the world stands
// still from the first frame that finds the landscape unready (MainWindow::tick takes no step, this runs with
// dt 0); readiness then also means no texture demotion or reload is pending. Once it is ready, the renderer drops
// its temporal state on this very frame, texture residency stands still, and the scenario takes a fixed number of
// steps with visibility in cluster order; both last until the next shot.
// Neither the frame count of the wait nor its wall time then reach the picture.
bool streamingWaitTick(World& w) {
  if(bench.time<bench.waitUntil)
    return true;
  if(!w.landscapeReady()) {
    if(std::chrono::steady_clock::now()<bench.world.streamingDeadline) {
      bench.frozen=bench.world.deterministic;
      return true;
      }
    bench.world.waitStreaming=false;
    bench.frozen=false;
    arenaAbort("streaming wait timed out before all Near/Far landscape and vob visuals were published ("+(w.view() ? w.view()->landscapeWaitState() : std::string("no view"))+")");
    return true;
    }
  bench.world.waitStreaming=false;
  bench.frozen=false;
  std::string publications="[";
  for(size_t i=0;i<bench.streamingWarmupFarMs.size();++i) {
    if(i!=0)publications+=',';
    publications+=std::to_string(bench.streamingWarmupFarMs[i]);
    }
  publications+=']';
  event("streaming_ready","\"world\":"+jsonStr(w.name())+",\"pass\":true,\"far_publication_ms\":"+publications+
    ",\"far_max_per_frame\":"+std::to_string(bench.streamingWarmupFarMaxPerFrame)+
    ",\"queue_max_ms\":"+std::to_string(bench.streamingWarmupQueueMaxMs)+
    ",\"vob_pending\":"+std::to_string(w.chunkResidency() ? w.chunkResidency()->stats().vobPending : 0)+
    ",\"vob_near_missing\":"+std::to_string(w.chunkResidency() ? w.chunkResidency()->stats().vobNearMissing : 0));
  bench.streamingWarmupFarMs.clear();
  bench.streamingWarmupFarMaxPerFrame=0;
  bench.streamingWarmupQueueMaxMs=0;
  if(bench.world.deterministic) {
    bench.renderReset=true;
    bench.ordered=true;
    benchTexturesFrozen=true; // the levels stay as they are now until the shot
    bench.settleSteps=DeterministicSettleSteps;
    }
  return false;
  }

}

namespace testbench_detail {

void run(World& w, Npc& pl, PlayerControl& ctrl) {
  if(npcTierTestTick(bench.npcTierTest,w,pl,bench.frameMs))return;
  if(npcSpeedTick(bench.npcSpeedTest,w))return;
  if(bench.routeActive || bench.placedItems.probing)return;
  if(bench.routine.routinePhase) {routineReturnTick(w,pl,ctrl);if(bench.routine.routinePhase)return;}
  if(NavBench::running() || NavDemos::running() || NpcDayJournal::running()) return;
  if(bench.movables.spellEquipPending) {
    if(pl.drawSpell(bench.movables.spellEquipPending)) {bench.movables.spellCastingId=bench.movables.spellEquipPending;bench.movables.spellEquipPending=0;}
    else if(w.tickCount()<bench.movables.spellEquipUntil) return;
    else {bench.movables.spellEquipPending=0;arenaAbort("real spell draw animation did not finish");return;}
    }
  if(bench.movables.spellWaitFrozen>=0) {
    const bool frozen=!bench.movables.spellActors.empty() && bench.movables.spellActors[0].npc->spellFrozen();
    const bool ready=bench.movables.spellWaitFrozen==2 ? !bench.movables.spellActors.empty() && bench.movables.spellActors[0].npc->isInAir() :
                     bench.movables.spellWaitFrozen==3 ? w.spellFieldCount()>0 :
                     bench.movables.spellWaitFrozen==4 ? !bench.movables.spellActors.empty() &&
                       bench.movables.spellActors[0].npc->ragdollQuestReturnCount()==1 &&
                       bench.movables.spellActors[0].npc->isStanding() && !bench.movables.spellActors[0].npc->isInAir() &&
                       !bench.movables.spellActors[0].npc->isDown() && !bench.movables.spellActors[0].npc->ragdollQuestReturnState().armed :
                       int(frozen)==bench.movables.spellWaitFrozen;
    if(!ready && w.tickCount()<bench.movables.spellWaitUntil) return;
    event("spell_physics_freeze_wait","\"frozen\":"+std::string(frozen?"true":"false")+",\"pass\":"+(ready?"true":"false"));
    bench.movables.spellWaitFrozen=-1;
    if(!ready) {arenaAbort(bench.movables.spellGuardTrace?"real wind throw did not return standing":"real freeze state did not reach requested transition");return;}
    }
  if(!bench.npcCommands.wanderPoints.empty())
    for(auto* n:bench.npcCommands.wanderers) {
      if(n->isDown() || n->isAiBusy())
        continue;
      bench.lcg = bench.lcg*1664525u + 1013904223u;
      n->aiPush(AiQueue::aiGoToPoint(*bench.npcCommands.wanderPoints[(bench.lcg>>8) % bench.npcCommands.wanderPoints.size()]));
      ++bench.npcCommands.wanderLegs;
      }
  if(bench.melee.meleeFixture && !bench.melee.meleeBrawl) {
    size_t actor=0;
    for(auto& walker:bench.living.livingWalkers) {
      auto& npc=*walker.npc;
      const auto index=actor++;
      const int phase=int(bench.melee.meleeSteady ? index%3 : ((w.tickCount()-bench.melee.meleeMotionFrom)/4000+index)%3);
      if(!npc.drawWeaponMelee()) continue;
      if(phase!=walker.meleePhase) {
        npc.clearAiQueue();npc.clearGoTo();npc.stopWalking();npc.setAnim(Npc::Anim::Idle);
        walker.queued=false;walker.meleePhase=phase;walker.meleeAttackAt=0;
        }
      if(phase==0) {
        const auto& target=walker.ends[walker.next?1:0];
        if(!walker.queued || ((npc.position()-target.pos).length()<35 && npc.isAiQueueEmpty())) {
          if(walker.queued) walker.next=!walker.next;
          npc.aiPush(AiQueue::aiGoToPoint(walker.ends[walker.next?1:0]));walker.queued=true;
          }
        }
      else if(phase==2 && w.tickCount()>=walker.meleeAttackAt) {
        const bool started=npc.setAnim(Npc::Anim::Attack);
        if(bench.melee.meleeMeasuring && started) ++bench.melee.meleeAttackStarts;
        walker.meleeAttackAt=w.tickCount()+1500;
        }
      }
    }
  if(bench.living.livingMotion)
    for(auto& walker:bench.living.livingWalkers) {
      if(walker.npc->isDown()) continue;
      const auto& target=walker.ends[walker.next?1:0];
      if(!walker.queued || ((walker.npc->position()-target.pos).length()<120 && walker.npc->isAiQueueEmpty())) {
        if(walker.queued) walker.next=!walker.next;
        walker.npc->aiPush(AiQueue::aiGoToPoint(walker.ends[walker.next?1:0]));walker.queued=true;
        }
      }
  for(auto& walker:bench.character.npcCharacterCrowd) {
    if(walker.npc->isDown())continue;
    const auto& target=walker.ends[walker.next?1:0];
    if(!walker.queued || ((walker.npc->position()-target.pos).length()<90 && walker.npc->isAiQueueEmpty())) {
      if(walker.queued)walker.next=!walker.next;
      walker.npc->aiPush(AiQueue::aiGoToPoint(walker.ends[walker.next?1:0]));walker.queued=true;
      }
    }
  if(bench.routine.questNpc!=nullptr && !bench.routine.questCase.empty()) questFields(w,*bench.routine.questNpc,false);
  if(bench.routine.questStandingWait) {
    const auto* npc=bench.routine.questNpc;
    const bool ready=npc && npc->isStanding() && !npc->isDown() && !npc->ragdollBody() &&
                     !npc->isRagdollBlending() && !npc->ragdollQuestReturnState().armed;
    if(!ready && bench.time<bench.routine.questDeadline) return;
    bench.routine.questStandingWait=false;
    event("immortal_guard_standing","\"case\":"+jsonStr(bench.routine.questCase)+",\"standing\":"+(ready?"true":"false"));
    if(!ready) {arenaAbort("real get-up did not finish standing");return;}
    }
  if(bench.routine.questWait) {
    const bool observed=bench.routine.questWaterCase?bench.routine.questSawWater:bench.routine.questSawDrop;
    const uint8_t wanted=bench.routine.questWaterCase ? RagdollSave::QuestReturn::water : RagdollSave::QuestReturn::fell;
    const bool guarded=bench.routine.questNpc->isHuman() && !bench.routine.questNpc->isPlayer() && Gothic::settingsGetI("GAME","ragdoll_quest_guard");
    const bool reached=observed && (!guarded || (bench.routine.questNpc->ragdollQuestReturnState().hazards & wanted));
    if(!reached && bench.time<bench.routine.questDeadline) return;
    bench.routine.questWait=false;
    if(!reached) {arenaAbort("quest guard physical hazard was not reached");return;}
    const auto* npc=bench.routine.questNpc;
    const auto* body=npc->ragdollBody();
    auto pelvis=npc->position();
    if(body) {const auto tr=body->pelvisTransform();pelvis={tr.at(3,0),tr.at(3,1),tr.at(3,2)};}
    const auto water=w.physic()->waterRay(pelvis,0);
    event("immortal_guard_hazard","\"case\":"+jsonStr(bench.routine.questCase)+",\"pelvis_cm\":"+ragdollVector(pelvis)+
      ",\"water_y_cm\":"+(water.hasCol?ragdollNumber(water.wdepth):"null")+
      ",\"unconscious\":"+(npc->isUnconscious()?"true":"false")+
      ",\"parts\":"+std::to_string(body?body->partCount():0)+",\"hazards\":"+std::to_string(npc->ragdollQuestReturnState().hazards));
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  if(bench.world.cadenceWait) { if(!AppCadence::done()) return;bench.world.cadenceWait=false; }
#endif
  if(bench.observeActive) {
    const auto now = w.tickCount();
    bool invalid = false;
    for(const auto* actor:bench.observeActors)
      invalid = invalid || actor->isDown() || actor->isImmortal() || actor->isExternalControl() ||
                actor->processPolicy()!=NpcProcessPolicy::AiNormal || actor->qDistTo(pl)>3000.f*3000.f;
    if(invalid) {
      event("body_observe_error","\"reason\":\"native actor escaped, died or lost active native processing\"");
      bench.observeActive = false;
      bench.pc = bench.prog.size();
      }
    else if(now>=bench.observeEnd) {
      event("body_observe_end","\"t_game_ms\":"+std::to_string(now)+",\"elapsed_ms\":"+
            std::to_string(now-bench.observeFrom)+",\"actors\":4");
      bench.observeActive = false;
      }
    }
  if(bench.npcCommands.volleyLeft>0) {
    tickVolley(ctrl);
    if(bench.npcCommands.volleyLeft>0)
      return;
    }
  while(bench.pc<bench.prog.size() || bench.speaking || bench.waitReply || !bench.waitAiKey.empty() || !bench.bodyLiveWait.empty() ||
        bench.npcCommands.downNpc!=nullptr || bench.ragdoll.waitRagdollNpc!=nullptr || bench.duelWaiting || bench.npcDuel.active || bench.world.waitStreaming ||
        std::chrono::steady_clock::now()<bench.realWaitUntil) {
    if(bench.speaking) {
      if(bench.time<bench.speakUntil)
        return;
      endSpeaking();
      }
    if(waitingForReply() || waitingForAi() || waitingForBodyLive())
      return;
    if(bench.world.waitStreaming && streamingWaitTick(w))
      return;
    if(bench.ragdoll.waitRagdollNpc!=nullptr) {
      const auto* npc=bench.ragdoll.waitRagdollNpc;
      const auto* body=npc->ragdollBody();
      const auto& quest=npc->ragdollQuestReturnState();
      const bool guardedReturn=quest.armed && quest.hazards!=0;
      const bool ready=bench.ragdoll.waitRagdollState=="knockdown" ? npc->isMeleeKnockedDown() :
                       bench.ragdoll.waitRagdollState=="sleep" ? body!=nullptr && body->isSleeping() :
                       bench.ragdoll.waitRagdollState=="recovery" ? npc->isRagdollBlending() :
                       body==nullptr && !npc->isDown() && !npc->isMeleeKnockedDown() && !npc->isRagdollBlending() &&
                       (!guardedReturn || (npc->isStanding() && !quest.armed));
      if(!ready && bench.time<bench.ragdoll.waitRagdollEnd)
        return;
      say("waitragdoll "+bench.ragdoll.waitRagdollState+(ready ? " reached" : " timeout"));
      event(ready ? "ragdoll_wait" : "ragdoll_error","\"state\":"+jsonStr(bench.ragdoll.waitRagdollState)+
            ",\"reached\":"+(ready ? "true" : "false"));
      bench.ragdoll.waitRagdollNpc=nullptr;
      }
    if(tickNpcDuel(bench.npcDuel,w))return;
    if(bench.duelWaiting) {
      Npc& first = *bench.duelA;
      Npc& second = *bench.duelB;
      const uint64_t now = w.tickCount();
      if(!first.isDown() && !second.isDown() && now<bench.duelEnd)
        return;
      const std::string outcome = first.isDown() ? (second.isDown() ? "both_down" : "actor_down") :
                                  (second.isDown() ? "target_down" : "timeout");
      endAnimationControl();
      TestBench::bodyEvent(first,&second,"episode_end",now,
                           "\"outcome\":"+jsonStr(outcome)+",\"elapsed_ms\":"+std::to_string(now-bench.duelFrom)+
                           ",\"actor_hp\":"+std::to_string(first.attribute(ATR_HITPOINTS))+
                           ",\"target_hp\":"+std::to_string(second.attribute(ATR_HITPOINTS))+
                           ",\"actor_dead\":"+(first.isDead() ? "true" : "false")+
                           ",\"target_dead\":"+(second.isDead() ? "true" : "false"));
      say("bodyduel outcome "+outcome+" after "+std::to_string(now-bench.duelFrom));
      for(Npc* n:{bench.duelA,bench.duelB}) {
        n->clearAiQueue();
        n->clearState(true);
        n->setExternalControl(true);
        }
      bench.duelWaiting = false;
      }
    if(bench.npcCommands.downNpc!=nullptr) {
      Npc* down = bench.npcCommands.downNpc->isDown() ? bench.npcCommands.downNpc :
                  bench.npcCommands.downOther!=nullptr && bench.npcCommands.downOther->isDown() ? bench.npcCommands.downOther : nullptr;
      if(down==nullptr && bench.time<bench.npcCommands.downEnd)
        return;
      event("waitdown_end","\"t_game_ms\":"+std::to_string(w.tickCount())+
            ",\"elapsed_ms\":"+std::to_string(bench.time-bench.npcCommands.downFrom)+
            ",\"outcome\":"+jsonStr(down!=nullptr ? "knockout" : "timeout")+
            ",\"down\":"+jsonStr(symName(w,down))+
            ",\"actor_hp\":"+std::to_string(bench.npcCommands.downNpc->attribute(ATR_HITPOINTS))+
            ",\"other_hp\":"+(bench.npcCommands.downOther!=nullptr ? std::to_string(bench.npcCommands.downOther->attribute(ATR_HITPOINTS)) : "null"));
      char buf[160];
      if(down!=nullptr)
        std::snprintf(buf,sizeof(buf),"fight over: %s down after %llu",symName(w,down).c_str(),
                      (unsigned long long)(bench.time-bench.npcCommands.downFrom)); else
        std::snprintf(buf,sizeof(buf),"waitdown: timeout, %s hp %d",symName(w,bench.npcCommands.downNpc).c_str(),
                      bench.npcCommands.downNpc->attribute(ATR_HITPOINTS));
      TestBench::note(buf);
      bench.npcCommands.downNpc = bench.npcCommands.downOther = nullptr;
      }
    if(bench.pc>=bench.prog.size())
      break;
    if(bench.time<bench.waitUntil || std::chrono::steady_clock::now()<bench.realWaitUntil || bench.npcCommands.volleyLeft>0 ||
       bench.settleSteps>0)
      return;
    if(bench.waitArrive) {
      if(!pl.isAiQueueEmpty() && bench.time<bench.arriveEnd)
        return;
      if(!pl.isAiQueueEmpty())
        say("waitarrive: timeout");
      bench.waitArrive = false;
      }
    if(!bench.holdCommand.empty() && bench.prog[bench.pc].rfind(bench.holdCommand,0)==0) {
      bench.heldNow = true;
      return;
      }
    const std::string line = bench.prog[bench.pc++];
    exec(line,w,pl,ctrl);
    if(bench.routeActive || bench.placedItems.probing || bench.world.waitStreaming || NavBench::running() || NavDemos::running() ||
       NpcDayJournal::running() || bench.npcTierTest.npc || bench.npcSpeedTest.npc) return;
    if(bench.movables.spellWaitFrozen>=0 || bench.movables.spellEquipPending) return;
    if(bench.routine.questWait || bench.routine.questStandingWait || bench.routine.routinePhase) return;
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
    if(bench.world.cadenceWait) return;
#endif
    }
  if(bench.time>=bench.waitUntil && std::chrono::steady_clock::now()>=bench.realWaitUntil && !bench.waitArrive && bench.npcCommands.volleyLeft==0 && bench.npcCommands.downNpc==nullptr && bench.ragdoll.waitRagdollNpc==nullptr && !bench.duelWaiting && !bench.npcDuel.active && !bench.world.waitStreaming && bench.settleSteps==0 && bench.bodyLiveWait.empty())
    finish();
  }

}

namespace {

// the game's sound for `dt` ms of game time, into the recording when there is one
void renderSound(uint64_t dt) {
  if(Resources::isHeadless())
    return;
  const uint32_t rate = SoundDevice::loopbackRate();
  if(rate==0 || dt==0)
    return;
  const bool slowed = bench.slow>1.f && bench.frameMs>0;
  bench.audioAcc += uint64_t(rate)*dt;
  const size_t n = size_t(bench.audioAcc/1000);
  bench.audioAcc -= uint64_t(n)*1000;
  bench.mix.resize(n*2);
  SoundDevice::renderLoopback(bench.mix.data(),n);
  if(bench.audio==nullptr)
    return;
  if(!slowed) {
    std::fwrite(bench.mix.data(),sizeof(int16_t),bench.mix.size(),bench.audio);
    bench.audioFrames += n;
    return;
    }
  // slowed: the mixer has played dt of game time (the sounds go on), the recording gets the frame's
  // length of silence, so that the sound stays in step with the picture
  const size_t quiet = size_t(uint64_t(rate)*bench.frameMs/1000);
  bench.mix.assign(quiet*2,0);
  std::fwrite(bench.mix.data(),sizeof(int16_t),bench.mix.size(),bench.audio);
  bench.audioFrames += quiet;
  }

}

bool TestBench::tick(uint64_t dt, PlayerControl& ctrl) {
  FrameProfile::Scope timing(FrameProfile::Zone::Bench);
  if(scenarioPath==nullptr || bench.done)
    return false;
  if(!bench.loaded)
    loadScenario();

  if(!bench.world.perfSegment.empty())FrameProfile::sample(bench.world.perfSegment,bench.world.perfFrame);

  auto world  = Gothic::inst().world();
  auto player = Gothic::inst().player();
  KnownRoadsBench::tick(Gothic::inst().checkLoading()==Gothic::LoadState::Idle?world:nullptr);
  if(world==nullptr || player==nullptr || Gothic::inst().checkLoading()!=Gothic::LoadState::Idle) {
    renderSound(dt);
    return false;
    }
  if(!bench.started) {
    bench.started = true;
    bench.heartbeatStart = std::chrono::steady_clock::now();
    FootIk::setProbing(true);
    say("start");
    if(std::string_view(scenarioPath).find("placed-items")!=std::string_view::npos)
      event("placed_items_loaded","\"world\":"+jsonStr(world->name())+",\"world_load_ms\":"+
        ragdollNumber(float(world->loadMilliseconds()))+",\"classification_ms\":"+
        ragdollNumber(float(world->placedItemValidationMilliseconds()))+",\"enabled\":"+
        (Gothic::physicsFlags().placedItems?"true":"false"));
    if(std::string_view(scenarioPath).find("furniture-parts")!=std::string_view::npos)
      event("furniture_parts_loaded","\"world\":"+jsonStr(world->name())+",\"world_load_ms\":"+
        ragdollNumber(float(world->loadMilliseconds()))+",\"enabled\":"+
        (Gothic::settingsGetI("GAME","furniture_parts")?"true":"false")+
        ",\"peak_rss_bytes\":"+std::to_string(furniturePeakMemory()));
    if(std::string_view(scenarioPath).find("movable-vobs")!=std::string_view::npos)
      event("movable_loaded","\"world\":"+jsonStr(world->name())+",\"world_load_ms\":"+
        ragdollNumber(float(world->loadMilliseconds()))+",\"enabled\":"+
        (Gothic::settingsGetI("GAME","movable_vobs")?"true":"false"));
    // A reload observer can enable god mode before the untouched initial
    // snapshot without allowing a native world step between those commands.
    if(bench.pc+1<bench.prog.size()) {
      const auto prelude=split(bench.prog[bench.pc]);
      const auto initial=split(bench.prog[bench.pc+1]);
      if(prelude.size()==2 && prelude[0]=="cheat" && prelude[1]=="god" &&
         initial.size()==3 && (initial[0]=="water" || initial[0]=="water-bodies") && initial[1]=="initial")
        exec(bench.prog[bench.pc++],*world,*player,ctrl);
      }
    if(bench.pc<bench.prog.size()) {
      const auto args=split(bench.prog[bench.pc]);
      if(args.size()>1 && args[0]=="ragdoll" && args[1]=="session-initial") {
        const auto line=bench.prog[bench.pc++];
        try {
          ragdollSessionArchive(args,*world,*player,dt);
          } catch(const std::exception& error) {
          say("initial session snapshot failed: "+std::string(error.what()));
          event("ragdoll_error","\"command\":"+jsonStr(line)+",\"reason\":"+jsonStr(error.what()));
          }
        }
      else if(args.size()==2 && args[0]=="placed-items" && args[1]=="initial") {
        ++bench.pc;placedItemsSnapshot(*world,"initial",dt);
        }
      else if(args.size()==2 && args[0]=="item-save" && args[1]=="initial") {
        ++bench.pc;
        try {savedItemInitial(*world,*player,dt);}
        catch(const std::exception& error) {arenaAbort(error.what());}
        }
      else if(args.size()==2 && args[0]=="monstercharacter" && args[1]=="initial") {
        ++bench.pc;monsterCharacterSnapshot(*world,*player,"initial");
        characterIdleGuards(*world,*player);
        }
      else if(args.size()==2 && args[0]=="npccharacter" && args[1]=="initial") {
        ++bench.pc;npcCharacterSnapshot(*world,*player,"initial");
        characterIdleGuards(*world,*player);
        // The first native step recreates the transient controller. Observe it
        // before normal command dispatch can advance past that initial contact.
        std::string trace="npccharacter trace";
        for(uint32_t id=0;id<world->npcCount();++id) {
          const auto* npc=world->npcById(id);
          if(npc && npc!=player && npc->isHuman() && !npc->isDown() &&
             (npc->position()-player->position()).length()<=2300.f)
            trace+=' '+std::to_string(id);
          }
        if(trace!="npccharacter trace")exec(trace,*world,*player,ctrl);
        }
      else if(args.size()==2 && args[0]=="character" && args[1]=="initial") {
        ++bench.pc;characterSnapshot(*world,*player,"initial");
        characterIdleGuards(*world,*player);
        exec("npccharacter trace player",*world,*player,ctrl);
        }
      else if(args.size()==3 && args[0]=="movable" && args[1]=="initial") {
        ++bench.pc;
        movableSaveSnapshot(*world,args[2]);
        // External control is a transient fixture setting. Restore it before
        // the first native step, after observing the unmodified loaded state.
        if(args[2]=="restored")
          exec("movable observe",*world,*player,ctrl);
        }
      else if(args.size()==3 && args[0]=="cloth" && args[1]=="initial") {
        ++bench.pc;clothInitial(clothContext(),*world,args[2]);
        }
      else if(args.size()==3 && args[0]=="destruction2" && args[1]=="initial") {
        ++bench.pc;destructionSessionSnapshot(*world,args[2]);
        }
      else if(args.size()==3 && args[0]=="water" && args[1]=="initial") {
        ++bench.pc;waterInitial(bench.water,waterContext(),*world,*player,ctrl,args[2]);
        }
      else if(args.size()==3 && args[0]=="water-bodies" && args[1]=="initial") {
        ++bench.pc;waterBodiesInitial(bench.water,waterContext(),*world,*player,args[2]);
        }
      }
    renderSound(dt);
    return false;
    }
  writeHeartbeat(*world);
  if(dt==0) {
    // the world stands still (`deterministic on`): only the streaming wait runs
    if(bench.frozen)
      streamingWaitTick(*world);
    return false;
    }
  if(bench.settleSteps>0)
    --bench.settleSteps;
  NavBench::tick(*world);
  NavDemos::tick(*world,dt);
  NavigationEyeBench::tick(world->tickCount());
  NavigationMemoryBench::tick(*world);
  NpcKeysBench::tick(*world,*player,ctrl,dt);
  arrowFields(*world);
  stickingFields(*world);
  spellFields(*world);
  bodyImpactTick(*world);
  healingObserve(bench.healing,*world,*player);
  if(auto& drift=bench.npcCommands.stunDrift;drift.target!=nullptr) {
    const uint64_t elapsed=world->tickCount()-drift.start;
    const uint64_t requested=drift.next*100;
    if(elapsed>=requested) {
      stunDriftSample(*world,elapsed,requested);
      if(++drift.next==17) drift={};
      }
    }
  destructionFields(bench.destruction,destructionContext(),*world);
  if(!bench.movables.movableMeasuring)fracturePiecesTick(bench.fracturePieces,bench.destruction,destructionContext(),*world,*player,ctrl);
  if(bench.destruction.destructionSupportTrace && bench.destruction.destructionTrace &&
     world->tickCount()-bench.destruction.destructionStart>=bench.destruction.destructionSupportNext) {
    bench.destruction.destructionSupportNext=world->tickCount()-bench.destruction.destructionStart+1000;
    destructionSupportSample(bench.destruction,destructionContext(),*world);
    }
  destructionShoot(bench.destruction,*world,*player);
  destructionMeleeTick(bench.destruction,destructionContext(),*world);
  if(bench.destruction.destructionFarmNpc) {
    bench.destruction.destructionFarmNpcTravel=std::max(bench.destruction.destructionFarmNpcTravel,
      (bench.destruction.destructionFarmNpc->position()-bench.destruction.destructionFarmNpcStart).length());
    if(bench.destruction.destructionFarmNpcGoal)
      bench.destruction.destructionFarmNpcBest=std::min(bench.destruction.destructionFarmNpcBest,
        (bench.destruction.destructionFarmNpc->position()-bench.destruction.destructionFarmNpcGoal->pos).length());
    }
  if(bench.furnitureParts.walking) {
    auto& fixture=bench.furnitureParts;
    auto& actor=fixture.walker?*fixture.walker:*player;
    const auto position=actor.position(),delta=position-fixture.walkStart;
    fixture.walkMaxStep=std::max(fixture.walkMaxStep,std::abs(position.y-fixture.walkPrevious.y));
    fixture.walkPrevious=position;
    fixture.walkMaxCrossTrack=std::max(fixture.walkMaxCrossTrack,
      std::abs(delta.x*fixture.walkDirection.z-delta.z*fixture.walkDirection.x));
    const float progress=Vec3::dotProduct(delta,fixture.walkDirection);
    const float length=Vec3::dotProduct(fixture.walkGoal-fixture.walkStart,fixture.walkDirection);
    if(!fixture.walkArrived && progress>=length-25.f) {
      fixture.walkArrived=world->tickCount();
      if(!fixture.walker) {
        release(ctrl,KeyCodec::Forward);
        bench.holds.erase(std::remove_if(bench.holds.begin(),bench.holds.end(),[](const Hold& hold){return hold.a==KeyCodec::Forward;}),bench.holds.end());
        player->stopWalking();player->clearSpeed();
        }
      }
    if(!fixture.walker && !fixture.walkArrived)player->setDirection(fixture.walkDirection);
    }

  meleeDodgeFields(*world);
  meleeAiResponseFields(*world);
  placedItemsFields(bench.placedItems,*world,*player,ctrl,bench.holds);
  movableFields(*world);
  ownMeshFields(*world,*player);
  ownMeshChestFields(*world,*player);
  waterFields(bench.water,waterContext(),*world);
  itemWakeFields(*world);
  savedItemSample(*world);
  waterFilmTick(waterFilmContext(),*world);
  clothFields(clothContext(),*world);
  movableCandleFields(*world);
  noiseFields(*world);
  if(bench.movables.spellReleaseAt && world->tickCount()>=bench.movables.spellReleaseAt) {
    player->endCastSpell(false);bench.movables.spellReleaseAt=0;
    event("spell_physics_release","\"id\":"+std::to_string(bench.movables.spellCastingId));
    }
  if(ragdollPerf) {
    const auto now=std::chrono::steady_clock::now();
    FrameProfile::observer(dt,now);
    FrameProfile::setActors(bench.crew);
    bench.ragdoll.perfWallMs=bench.ragdoll.hasPerfFrame ? std::chrono::duration<float,std::milli>(now-bench.ragdoll.lastPerfFrame).count() : 0;
    bench.ragdoll.lastPerfFrame=now; bench.ragdoll.hasPerfFrame=true;
    }
  // Independent privileged oracle at the same post-world/animation phase as the observer.
  // Setup commands run below and may leave real root motion for the next native substep; comparing
  // their pre-substep coordinates to a later observation would incorrectly reject correct data.
  if(!bench.observeProbeCase.empty()) {
    const auto actors = bodyObservationActors();
    if(actors.size()>=2) {
      const auto& first = *actors[0]; const auto& second = *actors[1];
      const auto self = first.position(), target = second.position();
      const auto center = world->physic()->ray(first.mapHeadBone(),second.collosionCenter());
      const auto head = world->physic()->ray(first.mapHeadBone(),second.mapHeadBone());
      event("body_observe_probe_sample","\"case\":"+jsonStr(bench.observeProbeCase)+
            ",\"t_game_ms\":"+std::to_string(world->tickCount())+
            ",\"sim_step\":"+std::to_string(BodyObservations::currentStep())+
            ",\"actor_id\":"+std::to_string(BodyObservations::entityId(first))+
            ",\"target_id\":"+std::to_string(BodyObservations::entityId(second))+
            ",\"visible\":"+(first.canSeeNpc(second,false) ? "true" : "false")+
            ",\"center_ray_hit\":"+(center.hasCol ? "true" : "false")+
            ",\"head_ray_hit\":"+(head.hasCol ? "true" : "false")+
            ",\"truth_self_hp\":"+std::to_string(first.attribute(ATR_HITPOINTS))+
            ",\"truth_self_yaw_rad\":"+std::to_string(first.rotationRad())+
            ",\"truth_self_position_cm\":["+std::to_string(self.x)+","+std::to_string(self.y)+","+std::to_string(self.z)+"]"+
            ",\"truth_target_hp\":"+std::to_string(second.attribute(ATR_HITPOINTS))+
            ",\"truth_target_position_cm\":["+std::to_string(target.x)+","+std::to_string(target.y)+","+std::to_string(target.z)+"]");
      }
    }
  characterFields(bench.character,characterContext(),*world,*player,dt,ctrl);
  monsterCharacterFields(bench.monster,monsterCharacterContext(),*world,dt);
  npcCharacterFields(bench.character,characterContext(),*world,*player,dt);
  npcCharacterMotionTrace(bench.character,characterContext(),*world,dt);
  NpcDayJournal::tick(*world);
  if(bench.inputTraceUntil!=0 && bench.time<=bench.inputTraceUntil) {
    const auto animation = player->animNames();
    if(animation!=bench.inputAnimation) {
      bench.inputAnimation = animation;
      say("input animation PC_HERO at " + std::to_string(world->tickCount()) + " ms: " + animation);
      }
    }
  bench.time += dt;
  if(aiBusy && aiBusy())
    bench.busyAt = bench.time;

  for(auto k:bench.taps)
    release(ctrl,k);
  bench.taps.clear();
  for(size_t i=0; i<bench.holds.size();) {
    if(bench.holds[i].until<=bench.time) {
      release(ctrl,bench.holds[i].a);
      bench.holds.erase(bench.holds.begin()+long(i));
      } else {
      ++i;
      }
    }

  bench.ctrl = &ctrl;
  if(bench.ragdoll.birthActor!=nullptr && !bench.ragdoll.birthActor->isDown()) {
    const Bench::BirthLive live{bench.ragdoll.birthActor->position(),world->tickCount(),bench.ragdoll.birthActor->isInAir()};
    if(bench.ragdoll.birthHistory.empty() || live.tick>bench.ragdoll.birthHistory.back().tick) {
      bench.ragdoll.birthHistory.push_back(live); if(bench.ragdoll.birthHistory.size()>4) bench.ragdoll.birthHistory.pop_front();
      event("ragdoll_birth_live","\"label\":"+jsonStr(bench.ragdoll.birthLabel)+",\"actor_id\":"+std::to_string(world->npcId(bench.ragdoll.birthActor))+
            ",\"t_game_ms\":"+std::to_string(live.tick)+",\"in_air\":"+(live.inAir ? "true" : "false")+
            ",\"position_cm\":"+ragdollVector(live.position));
      }
    }
  if(bench.ragdoll.birthContinuation!=nullptr && world->tickCount()>bench.ragdoll.birthTick) {
    const auto* body=bench.ragdoll.birthContinuation->ragdollBody();
    if(body!=nullptr)
      event("ragdoll_birth_continuation","\"label\":"+jsonStr(bench.ragdoll.birthContinuationLabel)+",\"actor_id\":"+
            std::to_string(world->npcId(bench.ragdoll.birthContinuation))+",\"t_game_ms\":"+std::to_string(world->tickCount())+
            ",\"pelvis_y_cm\":"+std::to_string(body->pelvisTransform().data()[13])+",\"native_parts\":"+std::to_string(body->partCount()));
    else event("ragdoll_error","\"reason\":\"birth body missing on the next native tick\"");
    bench.ragdoll.birthContinuation=nullptr;
    }
  if((ragdollPerf || bench.ragdoll.ragdollArenaCamera)
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
     && !AppCadence::normalClock()
#endif
     ) {
    // Coverage is sampled after the actual world step and BEFORE fixture commands
    // can wake bodies. A post-impulse "awake" flag alone could hide a sleeping tick.
    bench.ragdoll.perfPhysicalBeforeImpulse=0; bench.ragdoll.perfSleepingBeforeImpulse=0;
    for(const auto* npc:bench.crew)
      if(const auto* body=npc->ragdollBody()) {
        ++bench.ragdoll.perfPhysicalBeforeImpulse;
        if(body->isSleeping()) ++bench.ragdoll.perfSleepingBeforeImpulse;
        }
    }
#ifdef OPENGOTHIC_ENABLE_APP_CADENCE
  if(bench.cloth.clothMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    if(bench.cloth.clothPhysics.size()<20000)
      bench.cloth.clothPhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.cloth.clothOverflow=true;
    }
  if(bench.movables.movableMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    bench.movables.noiseMeasuredRays+=stats.rays;
    if(bench.movables.movablePhysics.size()<20000)
      bench.movables.movablePhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.movables.movableOverflow=true;
    }
  if(bench.living.livingMeasuring && !AppCadence::done()) {
    const auto stats=world->physic()->physicsStats();
    if(bench.living.livingPhysics.size()<20000)
      bench.living.livingPhysics.push_back({world->tickCount(),dt,stats.stepMilliseconds,stats.bodies});
    else bench.living.livingOverflow=true;
    }
#endif
  itemHullSample(bench.itemHulls,*world);
  furniturePerformanceSample(bench.furnitureParts,*world,dt);
  tickRoute(*player,dt);
  run(*world,*player,ctrl);
  renderSound(dt);
  if(bench.done)
    return false;
  if(bench.ragdoll.ragdollChurn) {
    for(size_t i=0;i<bench.crew.size();++i)
      if(bench.crew[i]->ragdollBody()!=nullptr) {
        const float sign=((bench.ragdoll.ragdollPulseFrame+2)/5+i)%2==0 ? 1.f : -1.f;
        bench.crew[i]->addRagdollImpulse({50.f*sign,5.f,25.f*sign});
        }
    ++bench.ragdoll.ragdollPulseFrame;
    }
  // a dialog (an npc with an important one comes to the hero) holds the player: say so
  if(Gothic::inst().isInDialog()!=bench.inDialog) {
    bench.inDialog = !bench.inDialog;
    std::string who;
    if(bench.inDialog)
      world->detectNpc(player->position(),2000,[&](Npc& n) {
        if(&n!=player && Gothic::inst().isNpcInDialog(n))
          who += " " + symName(*world,&n);
        });
    say(std::string(bench.inDialog ? "dialog started:" : "dialog ended") + who);
    }
  for(auto& [n,at]:bench.archery.archers) {
    // the archer harness: where it was set, at the target, the bow drawn, arrows; an arrow every time it
    // can (no fight AI, no perception's wait)
    if(n==nullptr || bench.archery.archerAt==nullptr || n->isDown())
      continue;
    const Vec3 d = bench.archery.archerAt->position()-at;
    n->setPosition(at);
    n->setDirection(Vec3(d.x,0,d.z));
    n->updateTransform();
    n->setTarget(bench.archery.archerAt);
    const auto ws = n->weaponState();
    if(auto rw = n->currentRangedWeapon(); rw!=nullptr && n->itemCount(size_t(rw->handle().munition))<5)
      n->addItem(size_t(rw->handle().munition),20);
    if(ws!=WeaponState::Bow && ws!=WeaponState::CBow) {
      n->drawWeaponBow();
      continue;
      }
    if(!bench.archery.archerDice && n->archerAi().isDriving(*n))
      n->archerAi().attack(*n);
    else if(!n->shootBow())
      n->aimBow();
    }
  if(bench.pinned!=nullptr) {
    const Vec3 d = player->position()-bench.pinAt;
    bench.pinned->setPosition(bench.pinAt);
    bench.pinned->setDirection(Vec3(d.x,0,d.z));
    bench.pinned->updateTransform();
    }
  if(bench.archery.aimNpc!=nullptr || (bench.archery.aimMouse && bench.archery.aimFixed)) {
    Vec3 at = bench.archery.aimAt;
    if(bench.archery.aimNpc!=nullptr && !aimPoint(*bench.archery.aimNpc,bench.archery.aimZone,bench.archery.aimLeg,at) &&
       !bench.archery.aimNpc->hitZoneCenter(HitZones::Body,at))
      at = bench.archery.aimNpc->collosionCenter();
    if(bench.archery.aimNpc!=nullptr && !bench.archery.aimRaw) {
      // Aim for the release draw throughout drawing, so the changing trajectory does not shake the aim.
      const auto st = Archery::stats(*player,player->currentRangedWeapon());
      Vec3       nock, dir;
      if(!player->arrowRay(nock,dir))
        nock = player->position() + Vec3(0,145,0);
      const auto& ar = player->archer();
      const float releaseDraw = bench.npcCommands.volleyLeft>0 ? std::min(st.burstDraw,bench.npcCommands.volleyDraw) : st.burstDraw;
      const float draw = st.bowStrength ? (ar.stage()==Archer::Held ? ar.draw() : releaseDraw) : 1.f;
      at = Archery::launch(nock,at,Archery::speedAt(st,draw));
      }
    auto cam = Gothic::inst().camera();
    if(!bench.archery.aimMouse) {
      ctrl.setAimOverride(true,at);
      }
    else if(cam!=nullptr && cam->isAim()) {
      // as the mouse: the center of the screen onto it, the hero turned with the camera. The camera looks
      // through a point over the shoulder that turns about the hero with it: the turn that puts the target in
      // the camera's plane through it (steeply down a turn toward the target from there would swing round)
      Vec3 from, dir;
      cam->aimRay(from,dir);
      Vec3 fw = Vec3(dir.x,0,dir.z);
      if(fw.length()<1e-3f)
        fw = facing(*player);
      fw = Vec3::normalize(fw);
      const Vec3  o    = from - player->position();
      const float ahd  = Vec3::dotProduct(o,fw);
      const float side = Vec3::dotProduct(o,Vec3(fw.z,0,-fw.x));
      const Vec3  t    = at - player->position();
      const float lt   = std::sqrt(t.x*t.x+t.z*t.z);
      const float yaw  = std::atan2(t.z,t.x) + (lt>1.f ? std::asin(std::clamp(side/lt,-1.f,1.f)) : 0.f);
      const Vec3  f    = Vec3(std::cos(yaw),0,std::sin(yaw));
      const Vec3  d    = t - (f*ahd + Vec3(f.z,0,-f.x)*side + Vec3(0,o.y,0));
      const float pit  = std::atan2(d.y,std::max(Vec3::dotProduct(d,f),1.f))*180.f/float(M_PI);
      cam->setSpin(PointF(-pit,yaw*180.f/float(M_PI)));
      player->setDirection(yaw*180.f/float(M_PI));
      }
    }
  if(bench.archery.arrowNpc!=nullptr && !bench.archery.arrowAim.empty()) {
    Vec3 at;
    if(arrowAimPoint(*bench.archery.arrowNpc,at)) {
      Vec3 nock,direction;
      if(!player->arrowRay(nock,direction)) nock=player->position()+Vec3(0,145,0);
      const auto stats=Archery::stats(*player,player->currentRangedWeapon());
      ctrl.setAimOverride(true,Archery::launch(nock,at,Archery::speedAt(stats,bench.archery.arrowDraw)));
      }
    }
  // the hero speaks to an npc (as the bridge does with his voice), it thinks of an answer
  if(bench.hearNpc!=nullptr) {
    if(bench.time<bench.hearUntil) {
      bench.hearNpc->listenTo(300);
      bench.hearNpc->setTalkPartner(player,1000);
      player->setTalkPartner(bench.hearNpc,1000);
      if(bench.time>=bench.hearNext) {
        bench.hearNpc->heardWords();
        bench.hearNext = bench.time + bench.hearEvery;
        }
      } else {
      bench.hearNpc->stopListening();
      bench.hearNpc = nullptr;
      }
    }
  if(bench.thinkNpc!=nullptr) {
    if(bench.time<bench.thinkUntil) {
      bench.thinkNpc->thinkOfAnswer(300);
      bench.thinkNpc->setTalkPartner(player,1000);
      } else {
      bench.thinkNpc->thinkOfAnswer(0);
      bench.thinkNpc = nullptr;
      }
    }
  if(bench.orbitUntil>bench.time) {
    bench.placeAngle += bench.orbitSpeed*float(dt)/1000.f;
    placeHero(*world,*player);
    }

  const Vec3 pos = player->position();
  if(bench.hasPrev) {
    const Vec3 d = pos-bench.prevPos;
    bench.speed  = std::sqrt(d.x*d.x+d.z*d.z)*1000.f/float(dt);
    }
  bench.prevPos = pos;
  bench.hasPrev = true;

  updateCamera(*world,subjectOf(*player));

  if(bench.world.perfArmed)FrameProfile::sample(bench.world.perfSegment,bench.world.perfFrame++);

  if(bench.ragdoll.ragdollCoverageActive) {
    auto sample=ragdollCoverageSample(*world);
    sample.segment=bench.ragdoll.ragdollCoverageMode; sample.frame=bench.ragdoll.ragdollCoverageFrames++;
    for(const auto* npc:bench.crew) arenaModelProof(*world,*npc,size_t(sample.frame));
    if(!arenaModeMatches(sample,bench.ragdoll.ragdollCoverageMode) && !bench.ragdoll.ragdollCoverageFailed) {
      bench.ragdoll.ragdollCoverageFailed=true;
      event("ragdoll_error","\"reason\":\"untimed twenty-body coverage failed\",\"coverage_frame\":"+std::to_string(sample.frame));
      }
    bench.ragdoll.ragdollCoverage.push_back(std::move(sample));
    if(bench.ragdoll.ragdollCoverageFrames==1020) {
      bench.ragdoll.ragdollCoverageActive=false;
      event("ragdoll_coverage_complete","\"mode\":"+jsonStr(bench.ragdoll.ragdollCoverageMode)+",\"frames\":1020,\"pass\":"+
            (bench.ragdoll.ragdollCoverageFailed ? "false" : "true"));
      }
    }

  bool capture = !bench.shot.empty();
  if(bench.rec) {
    if(ragdollPerf)
      sampleRagdollPerformance(*world);
    else
      writeRow(*player);
    capture |= bench.recVideo;
    if(!bench.recVideo)
      ++bench.recFrame;
    }
  return capture;
  }
