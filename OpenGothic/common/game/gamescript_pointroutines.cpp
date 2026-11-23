#include "gamescript.h"

#include <array>

#include "game/npcactivityhost.h"
#include "world/objects/npc.h"
#include "world/world.h"

namespace {
using ActivityDefinitions::Phase;

enum class PointRoutine { GuardPassage, Pick, PracticeMagic, PracticeSword, Pray, Study, Unknown };

PointRoutine pointRoutine(std::string_view name) {
  if(name=="ZS_GUARD_PASSAGE")  return PointRoutine::GuardPassage;
  if(name=="ZS_PICK_FP")        return PointRoutine::Pick;
  if(name=="ZS_PRACTICE_MAGIC") return PointRoutine::PracticeMagic;
  if(name=="ZS_PRACTICE_SWORD") return PointRoutine::PracticeSword;
  if(name=="ZS_PRAY_INNOS_FP")  return PointRoutine::Pray;
  if(name=="ZS_STUDY_WP")       return PointRoutine::Study;
  return PointRoutine::Unknown;
  }

constexpr int32_t PositionStage = 19;
constexpr int32_t InPose = 0, NeedsPose = 1, WalkingToPost = 2;
constexpr int32_t ApproachDistance = 500;
}

bool GameScript::runPointRoutine(Npc& npc, const daedalus::DaedalusSymbol& symbol, int& result) {
  std::string_view name = symbol.name();
  Phase phase = Phase::Init;
  if(name.ends_with("_LOOP")) {
    name.remove_suffix(5);
    phase = Phase::Loop;
    }
  else if(name.ends_with("_END")) {
    name.remove_suffix(4);
    phase = Phase::End;
    }
  const auto kind = pointRoutine(name);
  if(kind==PointRoutine::Unknown)
    return false;

  result = LOOP_CONTINUE;
  auto& stage = npc.handle().aivar[PositionStage];
  const auto gotoWaypoint = [&] {
    NpcEngineCalls::gotoWaypoint(world(),npc,npc.handle().wp);
    };
  const auto awayFromWaypoint = [&] {
    return NpcEngineCalls::distanceToWaypoint(world(),npc,npc.handle().wp)>ApproachDistance;
    };
  const auto awayFromHero = [&] {
    const auto hero = std::static_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
    return npc_getdisttonpc(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()),hero)>ApproachDistance;
    };
  const auto stopLookAt = [&] {
    if(npc.lookAtTarget()!=nullptr)
      npc.aiPush(AiQueue::aiStopLookAt());
    };

  if(phase==Phase::Init) {
    vm.call_function<void>(findSymbol("PERCEPTION_SET_NORMAL"));
    stopLookAt();
    npc.aiPush(AiQueue::aiRemoveWeapon());
    if(kind==PointRoutine::GuardPassage) {
      npc.setPerceptionTime(100);
      npc.aiPush(AiQueue::aiStandup());
      }
    NpcEngineCalls::setWalkMode(npc,1);
    if(kind==PointRoutine::GuardPassage || kind==PointRoutine::Study || awayFromWaypoint())
      gotoWaypoint();
    if(kind==PointRoutine::Study || kind==PointRoutine::PracticeSword ||
       (kind==PointRoutine::GuardPassage && awayFromHero()))
      npc.aiPush(AiQueue::aiAlignToWp());
    if(kind==PointRoutine::PracticeSword)
      npc.aiPush(AiQueue::aiEquipBestMeleeWeapon());
    if(kind==PointRoutine::PracticeSword || kind==PointRoutine::Study)
      stage = NeedsPose;
    return true;
    }

  if(phase==Phase::End) {
    if(kind==PointRoutine::PracticeSword) {
      npc.aiPush(AiQueue::aiStandup());
      npc.aiPush(AiQueue::aiRemoveWeapon());
      }
    else if(kind==PointRoutine::Pray) {
      npc.stopAnim("T_PRAY_RANDOM");
      npc.aiPush(AiQueue::aiPlayAnim("T_PRAY_2_STAND"));
      }
    else if(kind==PointRoutine::Study) {
      const auto scroll = findSymbol("FAKESCROLL_ADDON")->index();
      // The old End removes from Self the number of scrolls held by Other.
      if(auto* other = findNpc(vm.global_other())) {
        const auto count = other->itemCount(scroll);
        if(count>0)
          npc.delItem(scroll,uint32_t(count));
        }
      }
    return true;
    }

  const auto seconds = int32_t(npc.stateTime()/1000);
  const auto gotoAndAlign = [&](const WayPoint& point) {
    npc.aiPush(AiQueue::aiGoToPoint(point));
    npc.aiPush(AiQueue::aiStandup());
    npc.aiPush(AiQueue::aiAlignToFp());
    };
  const auto seated = [&] {
    return ActivityDefinitions::bodyStateContains(int32_t(npc.bodyState()),int32_t(BS_SIT));
    };
  switch(kind) {
    case PointRoutine::GuardPassage:
      if(seconds>=3 && awayFromHero()) {
        npc.aiPush(AiQueue::aiAlignToWp());
        npc.setStateTime(0);
        }
      break;
    case PointRoutine::Pick: {
      const auto move = rollRandom(2,&npc,true)!=0;
      const auto onPoint = npc.isOnFreePoint("PICK");
      if(seconds>=7 && onPoint) {
        const bool takeNext = move && world().findFreePoint(npc,"PICK")!=nullptr;
        if(takeNext)
          npc.aiPush(AiQueue::aiGoToNextFp("PICK"));
        npc.aiPush(AiQueue::aiPlayAnim("T_PLUNDER"));
        if(move)
          npc.setStateTime(0);
        if(!takeNext)
          NpcEngineCalls::setWalkMode(npc,1);
        }
      if(!onPoint) {
        if(const auto* point = world().findFreePoint(npc,"PICK"))
          npc.aiPush(AiQueue::aiGoToPoint(*point));
        }
      break;
      }
    case PointRoutine::PracticeMagic: {
      if(npc.isOnFreePoint("STAND")) {
        npc.aiPush(AiQueue::aiAlignToFp());
        if(stage==WalkingToPost)
          stage = NeedsPose;
        }
      else if(const auto* point = world().findFreePoint(npc,"STAND")) {
        gotoAndAlign(*point);
        stage = WalkingToPost;
        }
      else {
        npc.aiPush(AiQueue::aiAlignToWp());
        if(stage==WalkingToPost)
          stage = NeedsPose;
        }
      if(stage==NeedsPose && seconds>7) {
        const auto roll = int32_t(rollRandom(100,&npc,true));
        findSymbol("ZS_PRACTICE_MAGIC_LOOP.ZUFALL")->set_int(roll);
        constexpr std::array gestures{
          std::pair{25,"T_PRACTICEMAGIC"}, std::pair{50,"T_PRACTICEMAGIC2"},
          std::pair{70,"T_PRACTICEMAGIC3"}, std::pair{90,"T_PRACTICEMAGIC4"},
          std::pair{99,"R_SCRATCHHEAD"},
          };
        for(const auto& [max,animation] : gestures) {
          if(roll<=max) {
            npc.aiPush(AiQueue::aiPlayAnim(animation));
            break;
            }
          }
        npc.setStateTime(0);
        }
      break;
      }
    case PointRoutine::PracticeSword:
      if(stage==NeedsPose) {
        if(npc.currentMeleeWeapon()==nullptr)
          storeItem(npc.addItem(findSymbol("ITMW_1H_BAU_MACE")->index(),1));
        npc.aiPush(AiQueue::aiEquipBestMeleeWeapon());
        npc.aiPush(AiQueue::aiReadyMeleeWeapon());
        stage = InPose;
        }
      npc.aiPush(AiQueue::aiPlayAnim("T_1HSFREE"));
      gotoWaypoint();
      npc.aiPush(AiQueue::aiAlignToWp());
      break;
    case PointRoutine::Pray:
      if(npc.isOnFreePoint("PRAY")) {
        if(seated())
          npc.aiPush(AiQueue::aiPlayAnimBs("T_PRAY_RANDOM",BS_SIT));
        else {
          npc.aiPush(AiQueue::aiAlignToFp());
          npc.aiPush(AiQueue::aiPlayAnimBs("T_STAND_2_PRAY",BS_SIT));
          }
        }
      else if(const auto* point = world().findFreePoint(npc,"PRAY"))
        gotoAndAlign(*point);
      break;
    case PointRoutine::Study: {
      // The previous roll supplies the threshold. The VM locals are shared across the
      // NPCs in this state and persist in the save along with the queued commands.
      auto* random = findSymbol("ZS_STUDY_WP_LOOP.RANDY");
      const auto deadline = 25 + random->get_int();
      findSymbol("ZS_STUDY_WP_LOOP.RANDYSTATETIME")->set_int(deadline);
      const auto roll = int32_t(rollRandom(10,&npc,true));
      random->set_int(roll);
      const auto eventRoll = int32_t(rollRandom(100,&npc,true));
      findSymbol("ZS_STUDY_WP_LOOP.EVENTRANDY")->set_int(eventRoll);
      const auto wait = 5 + roll;
      findSymbol("ZS_STUDY_WP_LOOP.WAIT")->set_int(wait);
      findSymbol("ZS_STUDY_WP_LOOP.WAITTIME")->set_float(float(wait));
      if(awayFromWaypoint()) {
        gotoWaypoint();
        npc.aiPush(AiQueue::aiAlignToWp());
        }
      if(seconds>=deadline) {
        if(seated()) {
          npc.aiPush(AiQueue::aiStandup());
          npc.aiPush(AiQueue::aiTurnToNpc(findNpc(vm.global_hero())));
          npc.aiPush(AiQueue::aiRemoveWeapon());
          }
        constexpr std::array<std::string_view,3> poses = {
          "T_LGUARD_SCRATCH", "T_LGUARD_STRETCH", "T_LGUARD_CHANGELEG",
          };
        if(roll<int32_t(poses.size()))
          npc.aiPush(AiQueue::aiPlayAnim(poses[size_t(roll)]));
        else if(roll>3) {
          npc.aiPush(AiQueue::aiPlayAnim("T_LGUARD_2_STAND"));
          stage = NeedsPose;
          const auto scroll = findSymbol("FAKESCROLL_ADDON")->index();
          if(npc.itemCount(scroll)==0)
            storeItem(npc.addItem(scroll,1));
          stopLookAt();
          npc.aiPush(AiQueue::aiUseItemToState(int32_t(scroll),1));
          npc.aiPush(AiQueue::aiWait(uint64_t(wait)*1000));
          npc.aiPush(AiQueue::aiUseItemToState(int32_t(scroll),-1));
          npc.setStateTime(0);
          const auto level = findSymbol("CURRENTLEVEL")->get_int();
          if(findSymbol("RAVENISDEAD")->get_int()==0 && npc.handle().guild==GIL_KDW &&
             ((level==4 && eventRoll==1) || (level==1 && eventRoll<5)))
            vm.call_function<void>(findSymbol("B_EVENT_PORTAL_EARTHQUAKE"));
          }
        }
      if(stage==NeedsPose) {
        npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_LGUARD"));
        stage = InPose;
        }
      break;
      }
    case PointRoutine::Unknown:
      return false;
    }
  return true;
  }
