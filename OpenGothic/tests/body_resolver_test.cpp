#include "game/fightalgo.h"
#include "game/bodycontroller.h"
#include "game/bodyarenageometry.h"
#include "game/bodyarena.h"
#include "world/aiqueue.h"
#include "world/objects/npc.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {
using Policy = FightAlgo::PolicyAction;
using Primitive = FightAlgo::Action;
using State = FightAlgo::ResolverState;
using Mask = std::array<bool,12>;

// Serialized native ordinals are an engine contract, not policy indices.
static_assert(FightAlgo::MV_NULL==0 && FightAlgo::MV_MOVEG==1 && FightAlgo::MV_MOVE==2);
static_assert(FightAlgo::MV_JUMPBACK==3 && FightAlgo::MV_ATTACK==4 && FightAlgo::MV_ATTACKL==5);
static_assert(FightAlgo::MV_ATTACKR==6 && FightAlgo::MV_STRAFEL==7 && FightAlgo::MV_STRAFER==8);
static_assert(FightAlgo::MV_BLOCK==9 && FightAlgo::MV_WAIT==10 && FightAlgo::MV_WAITLONG==11);
static_assert(FightAlgo::MV_TURN2HIT==12 && FightAlgo::MV_TURN==13 && FightAlgo::MV_TURNG==14);
static_assert(FightAlgo::MV_STRAFE_E==15 && FightAlgo::MV_MAX==6);
static_assert(static_cast<unsigned>(Policy::Count)==12);

std::size_t assertions=0;
std::size_t cases=0;
std::vector<const char*> completed;

void emitResult(bool passed, const char* failure=nullptr) {
  rapidjson::Document result(rapidjson::kObjectType);
  auto& allocator=result.GetAllocator();
  result.AddMember("type","body_resolver_test",allocator);
  result.AddMember("passed",passed,allocator);
  result.AddMember("cases",uint64_t(cases),allocator);
  result.AddMember("assertions",uint64_t(assertions),allocator);
  rapidjson::Value checks(rapidjson::kArrayType);
  for(const auto name:completed) {
    rapidjson::Value check(rapidjson::kObjectType);
    check.AddMember("name",rapidjson::Value(name,allocator),allocator);
    check.AddMember("passed",true,allocator);
    checks.PushBack(check,allocator);
    }
  result.AddMember("checks",checks,allocator);
  if(failure!=nullptr)
    result.AddMember("failure",rapidjson::Value(failure,allocator),allocator);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  result.Accept(writer);
  std::cout << buffer.GetString() << '\n';
  }

void expect(bool condition, std::string_view message) {
  ++assertions;
  if(!condition)
    throw std::runtime_error(std::string(message));
  }

State ready() {
  State s;
  s.eligible=s.targetValid=s.sword=true;
  s.inWRange=s.focus5=s.focus30=s.lineClear=true;
  s.hasMove=s.hasLeft=s.hasRight=s.hasBack=true;
  s.hasAttack=s.hasAttackLeft=s.hasAttackRight=s.hasBlock=s.canRotate=true;
  return s;
  }

auto values(const State& s) {
  return std::tuple(s.eligible,s.targetValid,s.sword,s.inWRange,s.inCloseup,
                    s.focus5,s.focus30,s.lineClear,s.parade,s.running,
                    s.hasMove,s.hasLeft,s.hasRight,s.hasBack,
                    s.hasAttack,s.hasAttackLeft,s.hasAttackRight,s.hasBlock,s.canRotate);
  }

void expectMask(const State& s, const Mask& expected, std::string_view name) {
  const auto mask=FightAlgo::actionMask(s);
  for(std::size_t i=0;i<expected.size();++i) {
    const auto label=std::string(name)+" policy index "+std::to_string(i);
    expect(mask[i]==expected[i],label);
    expect(FightAlgo::resolve(Policy(i),s).allowed==mask[i],label+" mask/resolver agreement");
    }
  }

void expectResolution(Policy action, const State& s, Primitive primitive, bool allowed,
                      std::string_view reason) {
  const auto r=FightAlgo::resolve(action,s);
  const auto label="policy index "+std::to_string(static_cast<unsigned>(action));
  expect(r.primitive==primitive,label+" effective primitive");
  expect(r.allowed==allowed,label+" allowed");
  expect(std::string_view(r.reason)==reason,label+" reason");
  }

void masksAndOrdinals() {
  ++cases;
  // A ready, aligned, non-close fighter already in weapon range cannot approach or turn.
  const Mask closeEnough{true,false,false,true,true,true,true,true,true,true,true,true};
  auto s=ready();
  expectMask(s,closeEnough,"aligned weapon range");
  const std::array<Primitive,12> primitives{
    FightAlgo::MV_NULL,FightAlgo::MV_MOVE,FightAlgo::MV_TURN2HIT,
    FightAlgo::MV_STRAFEL,FightAlgo::MV_STRAFER,FightAlgo::MV_JUMPBACK,
    FightAlgo::MV_ATTACK,FightAlgo::MV_ATTACKL,FightAlgo::MV_ATTACKR,
    FightAlgo::MV_BLOCK,FightAlgo::MV_WAIT,FightAlgo::MV_WAITLONG};
  for(std::size_t i=0;i<primitives.size();++i)
    expectResolution(Policy(i),s,primitives[i],closeEnough[i],
                     i==0 ? "continue" : closeEnough[i] ? "allowed" : "masked");

  s.inWRange=false; s.focus5=false; s.focus30=false;
  expectMask(s,Mask{true,true,true,true,true,false,false,false,false,false,true,true},
             "distant unaligned fighter");
  }

void globalGatesAndContinue() {
  ++cases;
  const Mask onlyContinue{true,false,false,false,false,false,false,false,false,false,false,false};
  auto s=ready();
  s.targetValid=false;
  expectMask(s,onlyContinue,"invalid or invisible target");
  expectResolution(Policy::AttackFront,s,FightAlgo::MV_NULL,false,"target_invalid");
  s=ready(); s.sword=false;
  expectMask(s,onlyContinue,"unsupported weapon mode");
  expectResolution(Policy::AttackFront,s,FightAlgo::MV_NULL,false,"weapon_mode");
  s=ready(); s.eligible=false;
  // Live snapshot construction collapses animation locks, water/air and terminal gates here.
  expectMask(s,onlyContinue,"ineligible snapshot");
  expectResolution(Policy::AttackFront,s,FightAlgo::MV_NULL,false,"locked");
  s=State{};
  expectResolution(Policy::Continue,s,FightAlgo::MV_NULL,true,"continue");
  expectResolution(Policy::Count,ready(),FightAlgo::MV_NULL,false,"masked");
  expectResolution(Policy(255),ready(),FightAlgo::MV_NULL,false,"masked");
  }

void individualAnimationAvailability() {
  ++cases;
  constexpr std::array<Policy,8> actions{Policy::Approach,Policy::StrafeLeft,Policy::StrafeRight,
    Policy::JumpBack,Policy::AttackFront,Policy::AttackLeft,Policy::AttackRight,Policy::Block};
  constexpr std::array<bool State::*,8> flags{&State::hasMove,&State::hasLeft,&State::hasRight,
    &State::hasBack,&State::hasAttack,&State::hasAttackLeft,&State::hasAttackRight,&State::hasBlock};
  for(std::size_t i=0;i<actions.size();++i) {
    auto s=ready();
    if(actions[i]==Policy::Approach) s.inWRange=false;
    const auto before=FightAlgo::actionMask(s);
    expect(before[std::size_t(actions[i])],"available animation must permit its action");
    s.*flags[i]=false;
    auto expected=before;
    expected[std::size_t(actions[i])]=false;
    expectMask(s,expected,"missing individual animation");
    }
  auto s=ready(); s.focus5=false;
  expect(FightAlgo::resolve(Policy::TurnToTarget,s).allowed,"rotation starts when unaligned");
  s.canRotate=false;
  expect(!FightAlgo::resolve(Policy::TurnToTarget,s).allowed,"rotation availability gate");
  }

void attackDistanceOrientationAndCollision() {
  ++cases;
  constexpr std::array<Policy,3> attacks{Policy::AttackFront,Policy::AttackLeft,Policy::AttackRight};
  for(auto action:attacks) {
    auto s=ready();
    expect(FightAlgo::resolve(action,s).allowed,"ready attack");
    s.inWRange=false;
    expect(!FightAlgo::resolve(action,s).allowed,"attack beyond weapon range");
    s=ready(); s.focus5=false;
    expect(!FightAlgo::resolve(action,s).allowed,"policy attack needs facing; no hidden turn");
    s=ready(); s.lineClear=false;
    expect(!FightAlgo::resolve(action,s).allowed,"attack collision line blocked");
    s=ready(); s.focus30=false;
    expect(FightAlgo::resolve(action,s).allowed,"attack uses precise focus5 predicate");
    }
  auto s=ready(); s.inWRange=false;
  expect(FightAlgo::resolve(Policy::Approach,s).allowed,"approach beyond weapon range");
  s.inWRange=true;
  expect(!FightAlgo::resolve(Policy::Approach,s).allowed,"approach stops at weapon range");
  }

void defensiveGates() {
  ++cases;
  for(auto action:{Policy::JumpBack,Policy::Block}) {
    auto s=ready();
    expect(FightAlgo::resolve(action,s).allowed,"ready defence");
    s.focus30=false;
    expect(!FightAlgo::resolve(action,s).allowed,"defence focus30 gate");
    s=ready(); s.parade=true;
    expect(!FightAlgo::resolve(action,s).allowed,"no nested defence during parade");
    s=ready(); s.focus5=false; s.inWRange=false; s.lineClear=false;
    expect(FightAlgo::resolve(action,s).allowed,"defence does not inherit attack gates");
    }
  }

void explicitWaitPromotion() {
  ++cases;
  auto s=ready();
  expectResolution(Policy::Wait200,s,FightAlgo::MV_WAIT,true,"allowed");
  s.inCloseup=true;
  expectResolution(Policy::Wait200,s,FightAlgo::MV_JUMPBACK,true,"allowed");
  expectResolution(Policy::Wait300,s,FightAlgo::MV_WAITLONG,true,"allowed");
  s.hasBack=false;
  expectResolution(Policy::Wait200,s,FightAlgo::MV_JUMPBACK,false,"masked");
  s=ready(); s.inCloseup=true; s.parade=true;
  expectResolution(Policy::Wait200,s,FightAlgo::MV_JUMPBACK,false,"masked");
  s.focus30=false;
  expectResolution(Policy::Wait200,s,FightAlgo::MV_WAIT,true,"allowed");
  s=ready(); s.hasBack=false; s.parade=true;
  expectResolution(Policy::Wait200,s,FightAlgo::MV_WAIT,true,"allowed");
  }

void nativeNormalizationPreservesOrdinals() {
  ++cases;
  auto s=ready();
  for(unsigned i=0;i<=15;++i)
    expect(FightAlgo::resolveNative(Primitive(i),s)==Primitive(i),"normal native ordinal preserved");
  s.inCloseup=true; s.focus5=false;
  const std::array<Primitive,16> normalized{
    FightAlgo::MV_NULL,FightAlgo::MV_MOVEG,FightAlgo::MV_MOVE,FightAlgo::MV_JUMPBACK,
    FightAlgo::MV_TURN2HIT,FightAlgo::MV_TURN2HIT,FightAlgo::MV_TURN2HIT,
    FightAlgo::MV_STRAFEL,FightAlgo::MV_STRAFER,FightAlgo::MV_BLOCK,FightAlgo::MV_JUMPBACK,
    FightAlgo::MV_WAITLONG,FightAlgo::MV_TURN2HIT,FightAlgo::MV_JUMPBACK,
    FightAlgo::MV_TURNG,FightAlgo::MV_STRAFE_E};
  for(unsigned i=0;i<normalized.size();++i)
    expect(FightAlgo::resolveNative(Primitive(i),s)==normalized[i],"native promotion contract");
  s.focus30=false;
  expect(FightAlgo::resolveNative(FightAlgo::MV_WAIT,s)==FightAlgo::MV_WAIT,"native wait requires focus30");
  expect(FightAlgo::resolveNative(FightAlgo::MV_TURN,s)==FightAlgo::MV_TURN,"native turn requires focus30");
  expect(FightAlgo::resolveNative(Primitive(255),s)==Primitive(255),"unknown diagnostic native ordinal preserved");
  // Native normalization diagnoses the old executor. It does not acquire policy permission.
  s=State{}; s.inCloseup=s.focus30=true;
  expect(FightAlgo::resolveNative(FightAlgo::MV_WAIT,s)==FightAlgo::MV_JUMPBACK,"native normalization independent of policy gates");
  expect(!FightAlgo::resolve(Policy::Wait200,s).allowed,"policy permission remains separate");
  }

void repeatPurity() {
  ++cases;
  auto s=ready(); s.inCloseup=true; s.focus5=false; s.running=true;
  const auto original=values(s);
  const auto expected=FightAlgo::actionMask(s);
  FightAlgo queue;
  const auto queueBefore=queue.queueSnapshot();
  for(unsigned i=0;i<128;++i) {
    expect(FightAlgo::actionMask(s)==expected,"repeat mask is deterministic");
    for(unsigned a=0;a<12;++a) {
      const auto first=FightAlgo::resolve(Policy(a),s);
      const auto second=FightAlgo::resolve(Policy(a),s);
      expect(first.primitive==second.primitive && first.allowed==second.allowed &&
             std::string_view(first.reason)==second.reason,"repeat resolution is deterministic");
      }
    expect(values(s)==original,"resolver input fields unchanged");
    expect(queue.queueSnapshot()==queueBefore && !queue.hasInstructions(),"native queue unchanged");
    }
  s.running=false;
  expect(FightAlgo::actionMask(s)==expected,"running diagnostic does not add permission");
  }

void actualArbiterFakeStateChecks() {
  ++cases;
  // Executes production value-only command gates, without NPCs, world assets or simulation.
  const auto json=BodyController::selfTestJson();
  rapidjson::Document result;
  result.Parse(json.c_str());
  expect(!result.HasParseError() && result.IsObject(),"arbiter self-test produces JSON");
  expect(result.HasMember("passed") && result["passed"].IsBool() && result["passed"].GetBool(),
         "actual arbiter fake-state checks pass");
  expect(result.HasMember("checks") && result["checks"].IsArray() && result["checks"].Size()>=13,
         "arbiter fake-state checks are present");
  for(const auto& row:result["checks"].GetArray()) {
    expect(row.HasMember("name") && row["name"].IsString(),"named arbiter fake-state check");
    expect(row.HasMember("passed") && row["passed"].IsBool() && row["passed"].GetBool(),
           std::string("arbiter gate: ")+row["name"].GetString());
    }
  }

void nativeCombatAcquisitionGates() {
  ++cases;
  using Acquire = BodyController::AcquireState;
  Acquire state;
  state.targetValid=state.safeBoundary=true;
  const auto reason=[](const Acquire& actual) {
    return std::string_view(BodyController::validateAcquire(actual));
    };
  expect(reason(state)=="allowed","empty arena queue and goto need no native state");
  for(auto field:{&Acquire::nativeCombat,&Acquire::queueCombatOnly,&Acquire::gotoToTarget}) {
    auto diagnostic=state; diagnostic.*field=true;
    expect(reason(diagnostic)=="allowed","unused native provenance cannot alter empty arena acquisition");
    }
  state.nativeCombat=state.queueCombatOnly=state.gotoToTarget=true;
  state.queueEmpty=state.gotoEmpty=false;
  expect(reason(state)=="allowed","safe same-target native combat queue and goto can be replaced");
  auto changed=state; changed.nativeCombat=false;
  expect(reason(changed)=="native_queue_busy","other native states retain their queued actions");
  changed.queueEmpty=true;
  expect(reason(changed)=="goto_busy","other native states retain their goto");
  changed=state; changed.queueCombatOnly=false;
  expect(reason(changed)=="native_queue_busy","mixed or unproven native queue blocks acquisition");
  changed=state; changed.gotoToTarget=false;
  expect(reason(changed)=="goto_busy","goto to a different goal blocks acquisition");
  changed=state; changed.safeBoundary=false;
  expect(reason(changed)=="not_safe_boundary","combat provenance cannot bypass a live wait or segment lock");
  changed=state; changed.overlayEmpty=false;
  expect(reason(changed)=="native_overlay_busy","overlay output remains native work");
  changed=state; changed.targetValid=false;
  expect(reason(changed)=="target_invalid","lease target must remain the actor's live visible target");
  for(auto pair:{std::pair{&Acquire::externalControl,"external_control"},
                 std::pair{&Acquire::player,"player_actor"},std::pair{&Acquire::talking,"talking"},
                 std::pair{&Acquire::terminal,"actor_down"}}) {
    changed=state; changed.*pair.first=true;
    expect(reason(changed)==pair.second,"actor-specific refusal retains its cause");
    }
  const auto target=reinterpret_cast<Npc*>(uintptr_t(1));
  const auto other=reinterpret_cast<Npc*>(uintptr_t(2));
  for(unsigned hint=Npc::GT_No;hint<=Npc::GT_Flee;++hint) {
    expect(BodyController::nativeCombatGoto(uint8_t(hint),target,target,target,false)==
           (hint==Npc::GT_Way || hint==Npc::GT_Enemy),"only real native waypoint/NPC and enemy goto hints can be replaced");
    expect(!BodyController::nativeCombatGoto(uint8_t(hint),target,nullptr,target,false),
           "matching enemy destination without native provenance cannot be replaced");
    expect(!BodyController::nativeCombatGoto(uint8_t(hint),target,target,other,false),
           "every native goto hint remains bound to the requested target");
    }
  expect(BodyController::nativeCombatGoto(Npc::GT_Way,target,target,nullptr,true),
         "proven native path waypoint toward the enemy can be replaced");
  expect(!BodyController::nativeCombatGoto(Npc::GT_Enemy,target,target,nullptr,true),
         "native enemy approach must point to the exact enemy NPC, not a waypoint");
  expect(!BodyController::nativeCombatGoto(Npc::GT_Enemy,target,target,target,true),
         "inconsistent enemy approach cannot smuggle a waypoint goal");
  expect(!BodyController::nativeCombatGoto(Npc::GT_Way,target,other,nullptr,true),
         "native waypoint path toward another enemy is preserved");
  expect(!BodyController::nativeCombatGoto(Npc::GT_Enemy,nullptr,nullptr,nullptr,false),
         "null enemy cannot become a valid goto target");
  }

void nativeCombatQueueProvenance() {
  ++cases;
  // Identity-only sentinel pointers: queue validation never dereferences an NPC.
  const auto target=reinterpret_cast<Npc*>(uintptr_t(1));
  const auto other=reinterpret_cast<Npc*>(uintptr_t(2));
  AiQueue queue;
  expect(queue.nativeCombatOnly(target),"empty native combat queue has no conflicting work");
  expect(!queue.nativeCombatOnly(nullptr),"missing native combat target fails closed");
  for(unsigned ordinal=AI_None;ordinal<=AI_TurnAway;++ordinal) {
    auto action=AiQueue::aiAttack(); action.act=Action(ordinal); action.target=target;
    const bool replaceable=action.act==AI_Attack || action.act==AI_Wait ||
                           action.act==AI_GoToNpc || action.act==AI_TurnToNpc;
    queue.pushBack(std::move(action),nullptr);
    expect(!queue.nativeCombatOnly(target),"untagged script work cannot become native combat merely by action type");
    queue.clear();
    action=AiQueue::aiAttack(); action.act=Action(ordinal); action.target=target; action.nativeCombatTarget=target;
    queue.pushBack(std::move(action),nullptr);
    expect(queue.nativeCombatOnly(target)==replaceable,"native queue whitelist excludes output, animation, state and story work");
    expect(!queue.nativeCombatOnly(other),"native combat provenance is bound to the same target");
    queue.clear();
    }
  for(auto action:{AiQueue::aiAttack(),AiQueue::aiWait(200),AiQueue::aiGoToNpc(target),AiQueue::aiTurnToNpc(target)}) {
    action.nativeCombatTarget=target; queue.pushBack(std::move(action),nullptr);
    }
  const auto first=queue.front();
  for(unsigned repeat=0;repeat<32;++repeat)
    expect(queue.nativeCombatOnly(target) && queue.size()==4 && queue.front()==first,
           "native queue classification preserves every action and order");
  AiQueue::AiAction story; story.act=AI_Output; story.target=target; story.s0="story";
  queue.pushBack(std::move(story),nullptr);
  expect(!queue.nativeCombatOnly(target) && queue.size()==5,"one story action blocks the whole queue without clearing it");
  queue.clear();
  for(auto action:{AiQueue::aiGoToNpc(other),AiQueue::aiTurnToNpc(other)}) {
    action.nativeCombatTarget=target; queue.pushBack(std::move(action),nullptr);
    expect(!queue.nativeCombatOnly(target),"tagged navigation must still explicitly address the lease target");
    queue.clear();
    }
  }

void boundaryRequestLifecycle() {
  ++cases;
  using Request=BodyController::BoundaryRequest;
  BodyController::AcquireState state;
  state.targetValid=state.nativeCombat=state.queueCombatOnly=state.gotoToTarget=true;
  state.queueEmpty=state.gotoEmpty=false;
  Request request;
  expect(!request.begin(0,42,100,1500) && !request.pending,"zero request identity is rejected");
  expect(!request.begin(7,0,100,1500) && !request.pending,"zero target identity is rejected");
  expect(!request.begin(7,42,100,0) && !request.begin(7,42,100,2001),"only a bounded positive wait may be pending");
  expect(request.begin(7,42,100,1500) && request.pending,"a request reserves its identity and wall deadline");
  expect(request.id==7 && request.target==42 && request.deadlineMs==1600,"stored request identity is exact");
  expect(!request.begin(8,43,200,1500),"a second request cannot replace the existing actor slot");
  expect(request.id==7 && request.target==42 && request.deadlineMs==1600,"duplicate submission never extends the window");
  for(uint64_t wall:{101,200,1000,1599})
    expect(std::string_view(request.advance(state,42,wall))=="waiting" && request.pending,
           "locked native combat keeps executing without acquiring before its safe boundary");
  state.safeBoundary=true;
  expect(std::string_view(request.advance(state,42,1599))=="acquired" && !request.pending,
         "the first safe simulation boundary consumes exactly one pending request");
  expect(request.id==7 && request.target==42,"grant preserves identity for the ownership event and cancellation race");
  expect(std::string_view(request.advance(state,42,1599))=="not_pending","one request cannot grant a second lease");
  expect(request.begin(8,42,2000,500),"completed request slot may be reused");
  expect(std::string_view(request.advance(state,42,2500))=="expired" && !request.pending,
         "deadline is wall time and wins over a simultaneous safe boundary");
  expect(!request.cancel(),"expired request cannot be cancelled twice");
  for(const auto reason:{"released","disconnect","loading","destroyed","story_takeover"}) {
    expect(request.begin(9,42,3000,1500),reason);
    expect(request.cancel() && !request.pending,"explicit cancellation consumes the actual pending lifecycle");
    expect(std::string_view(request.advance(state,42,3001))=="not_pending",
           "release, disconnect and world teardown cannot later grant an old request");
    }
  for(const auto reason:{"target_changed","actor_down","talking","native_combat_ended","target_invalid",
                         "external_control","native_overlay_busy","native_queue_busy","goto_busy"}) {
    auto changed=state;
    uint64_t target=42;
    if(std::string_view(reason)=="target_changed") target=43;
    if(std::string_view(reason)=="actor_down") changed.terminal=true;
    if(std::string_view(reason)=="talking") changed.talking=true;
    if(std::string_view(reason)=="native_combat_ended") changed.nativeCombat=false;
    if(std::string_view(reason)=="target_invalid") changed.targetValid=false;
    if(std::string_view(reason)=="external_control") changed.externalControl=true;
    if(std::string_view(reason)=="native_overlay_busy") changed.overlayEmpty=false;
    if(std::string_view(reason)=="native_queue_busy") changed.queueCombatOnly=false;
    if(std::string_view(reason)=="goto_busy") changed.gotoToTarget=false;
    // Cancellation is checked while a physical segment is still locked as well.
    changed.safeBoundary=false;
    expect(request.begin(10,42,4000,1500),"new cancellation case starts in the same actual lifecycle");
    expect(std::string_view(request.advance(changed,target,4001))==reason && !request.pending,
           std::string("pending request cancels promptly on ")+reason);
    expect(std::string_view(request.advance(state,42,4002))=="not_pending",
           "restoring the target/state after cancellation does not resurrect the lease");
    }
  }

void optionalWallNormalPlacement() {
  ++cases;
  using Tempest::Vec3;
  using namespace BodyArenaGeometry;
  // A wall roughly 33 degrees off the search ray is the actual regression geometry.
  const Vec3 normal(-0.822759271f,0.159460396f,0.545563519f), search(1,0,0);
  const auto basis=horizontalBasis(normal,search);
  expect(bool(basis),"reported tilted wall normal produces a placement basis");
  expect(std::abs(basis->outward.length()-1.f)<0.00001f,"placement normal is a unit horizontal vector");
  expect(basis->outward.y==0 && basis->toward.y==0 && basis->tangent.y==0,"placement basis is horizontal");
  expect(std::abs(Vec3::dotProduct(basis->outward,basis->tangent))<0.00001f,
         "placement tangent is perpendicular to the actual normal");
  expect(Vec3::dotProduct(basis->toward,search)>0,"wall ray faces the observed wall");
  const auto reversed=horizontalBasis(-normal,search);
  expect(reversed && (reversed->outward-basis->outward).length()<0.00001f,"unflipped physics normal orientation is normalized");
  const Vec3 anchor(64400,2500,-15000);
  for(float requested:{120.f,130.f,140.f,150.f,160.f,170.f}) {
    const auto first=anchor+basis->outward*requested;
    for(float sign:{-1.f,1.f}) {
      const auto second=first+basis->tangent*(450.f*sign);
      expect(std::abs(normalClearance(first,anchor,basis->outward)-requested)<0.01f,
             "primary on physical side zero retains requested normal distance");
      expect(std::abs(normalClearance(second,anchor,basis->outward)-requested)<0.01f,
             "primary on physical side one retains requested normal distance");
      }
    expect(acceptsClearance(requested,requested,requested),"exact requested clearance satisfies both independent bounds");
    }
  const Vec3 legacyToward(1,0,0), legacyTangent(0,0,1);
  const auto legacyFirst=anchor-legacyToward*120.f;
  const auto legacySecond=legacyFirst+legacyTangent*450.f;
  expect(std::abs(normalClearance(legacySecond,anchor,basis->outward)-
                  normalClearance(legacyFirst,anchor,basis->outward))>200.f,
         "search-ray tangent cannot substitute for a tilted wall tangent on side one");
  expect(!horizontalBasis(Vec3(0,0,0),search),"missing object normal fails closed");
  expect(!horizontalBasis(Vec3(0,1,0),search),"vertical-only normal fails closed");
  expect(!horizontalBasis(Vec3(0,0,1),search),"indeterminate normal orientation fails closed");
  expect(!horizontalBasis(Vec3(std::numeric_limits<float>::quiet_NaN(),0,1),search),"nonfinite normal fails closed");
  expect(!acceptsClearance(120,150,120),"old ray-request error bound remains enforced");
  expect(!acceptsClearance(120,120,150),"normal-request error is independently enforced");
  expect(!acceptsClearance(60,59,60) && !acceptsClearance(180,180,181),"both measured clearances remain inside declared bounds");
  expect(!acceptsClearance(120,120,-120),"wrong side of the wall fails closed");
  expect(!acceptsClearance(120,120,std::numeric_limits<float>::infinity()),"nonfinite measured clearance fails closed");
  }

void optionalWallTangentSelection() {
  ++cases;
  using Tempest::Vec3;
  using namespace BodyArenaGeometry;
  expect(validTangentSelection({},false,false),"omitted sign preserves default open setup");
  expect(validTangentSelection({},true,false),"omitted sign preserves legacy near-wall setup");
  for(int sign:{1,-1}) {
    expect(validTangentSelection(sign,true,true),"each explicit integer sign is supported with normal clearance");
    expect(!validTangentSelection(sign,false,true),"explicit sign requires near-wall geometry");
    expect(!validTangentSelection(sign,true,false),"explicit sign requires supplied wall clearance");
    expect(acceptsTangentSign(sign,sign),"requested candidate is retained");
    expect(!acceptsTangentSign(sign,-sign),"opposite candidate cannot become a geometry fallback");
    }
  for(int sign:{0,2,-2,std::numeric_limits<int>::max(),std::numeric_limits<int>::min()})
    expect(!validTangentSelection(sign,true,true),"other integer signs fail closed");
  expect(acceptsTangentSign({},1) && acceptsTangentSign({},-1),"omission preserves both original candidates");
  // The runner's typed gate refuses JSON booleans, floats, strings and overflow before reset.
  for(const char* value:{"true","false","1.0","-1.0","null","\"1\"","2147483648"}) {
    rapidjson::Document input; input.Parse(value);
    expect(!input.HasParseError() && !input.IsInt(),"tangent request must be a signed JSON integer");
    }
  const auto basis=horizontalBasis(Vec3(-0.822759271f,0.159460396f,0.545563519f),Vec3(1,0,0));
  const Vec3 first(64400,2500,-15000);
  for(int sign:{1,-1}) {
    const auto second=first+basis->tangent*float(sign*450);
    for(unsigned side:{0u,1u}) {
      const auto primary=side==0 ? first : second;
      const auto opponent=side==0 ? second : first;
      const auto measured=Vec3::dotProduct(opponent-primary,basis->tangent);
      expect(std::abs(measured-float(sign*450)*(side==0 ? 1.f : -1.f))<0.01f,
             "actual pair displacement proves tangent sign with physical primary side swap");
      expect(std::abs(normalClearance(primary,first,basis->outward))<0.01f &&
             std::abs(normalClearance(opponent,first,basis->outward))<0.01f,
             "mirroring the tangent keeps both actors in the same horizontal wall plane");
      }
    }
  }

void reactionActivationGates() {
  ++cases;
  expect(BodyArena::acceptsReactionScenario("block"),"reaction block task retains its genuine blocked-hit terminal gate");
  expect(BodyArena::acceptsReactionScenario("reset_stress"),"reaction recording can wait for genuine defensive segment completion");
  for(const char* scenario:{"distance","attack","native_duel","unknown",""})
    expect(!BodyArena::acceptsReactionScenario(scenario),"other scenarios cannot enable reaction preparation");
  BodyArena::ReactionGate gate;
  gate.actual.session=11; gate.actual.world=12; gate.actual.primary=13; gate.actual.opponent=14;
  gate.actual.primaryGeneration=15; gate.actual.opponentGeneration=16; gate.actual.observation=17; gate.actual.game=18;
  gate.enabled=gate.armed=gate.owned=gate.idle=gate.eligible=gate.visible=gate.inRange=true;
  gate.attack=gate.prehit=gate.started=gate.unhurt=gate.periodic=gate.fresh=true;
  const auto allowed=[&](const auto& state,const auto& request) {
    return std::string_view(BodyArena::validateReactionActivation(state,request))=="allowed";
    };
  for(const char* provider:{"native","scripted"}) for(const char* mode:{"lockstep","realtime"}) {
    auto request=gate.actual; request.provider=provider; request.mode=mode;
    expect(allowed(gate,request),"same actual windup boundary permits each controller and scheduler");
    }
  for(auto field:{&BodyArena::ReactionGate::enabled,&BodyArena::ReactionGate::armed,&BodyArena::ReactionGate::owned,
                  &BodyArena::ReactionGate::idle,&BodyArena::ReactionGate::eligible,&BodyArena::ReactionGate::visible,
                  &BodyArena::ReactionGate::inRange,&BodyArena::ReactionGate::attack,&BodyArena::ReactionGate::prehit,
                  &BodyArena::ReactionGate::started,&BodyArena::ReactionGate::unhurt,&BodyArena::ReactionGate::periodic,
                  &BodyArena::ReactionGate::fresh}) {
    auto changed=gate; changed.*field=false;
    expect(!allowed(changed,gate.actual),"every actual reaction prerequisite is required");
    }
  auto terminal=gate; terminal.finished=true;
  expect(!allowed(terminal,gate.actual),"terminal or repeated activation cannot open a decision boundary");
  for(auto field:{&BodyArena::ReactionRequest::session,&BodyArena::ReactionRequest::world,&BodyArena::ReactionRequest::primary,
                  &BodyArena::ReactionRequest::opponent,&BodyArena::ReactionRequest::primaryGeneration,
                  &BodyArena::ReactionRequest::opponentGeneration,&BodyArena::ReactionRequest::observation,
                  &BodyArena::ReactionRequest::game}) {
    auto request=gate.actual; ++(request.*field);
    expect(!allowed(gate,request),"stale epoch/actor/generation/source observation cannot activate");
    }
  auto zero=gate.actual; zero.observation=0;
  expect(!allowed(gate,zero),"unobserved windup cannot become a reaction input");
  auto provider=gate.actual; provider.provider="unknown";
  expect(!allowed(gate,provider),"unsupported reaction provider fails closed");
  auto mode=gate.actual; mode.mode="catchup";
  expect(!allowed(gate,mode),"unsupported scheduler fails closed");
  }
}

int main() {
  try {
    const auto run=[](const char* name, void (*test)()) { test(); completed.push_back(name); };
    run("all_policy_masks_and_primitives",masksAndOrdinals);
    run("global_gates_and_continue",globalGatesAndContinue);
    run("individual_animation_availability",individualAnimationAvailability);
    run("attack_distance_orientation_collision",attackDistanceOrientationAndCollision);
    run("defence_focus_and_parade",defensiveGates);
    run("explicit_wait_promotion",explicitWaitPromotion);
    run("native_normalization_preserves_ordinals",nativeNormalizationPreservesOrdinals);
    run("repeat_value_and_queue_purity",repeatPurity);
    run("actual_arbiter_fake_state_checks",actualArbiterFakeStateChecks);
    run("native_combat_acquisition_gates",nativeCombatAcquisitionGates);
    run("native_combat_queue_provenance",nativeCombatQueueProvenance);
    run("boundary_request_lifecycle",boundaryRequestLifecycle);
    run("optional_wall_normal_placement",optionalWallNormalPlacement);
    run("optional_wall_tangent_selection",optionalWallTangentSelection);
    run("reaction_activation_gates",reactionActivationGates);
    emitResult(true);
    return 0;
    } catch(const std::exception& error) {
    emitResult(false,error.what());
    std::cerr << "body resolver test failed: " << error.what() << '\n';
    return 1;
    }
  }
