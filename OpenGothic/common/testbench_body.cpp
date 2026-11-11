#include "testbench_internal.h"
#include "testbench.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include "camera.h"
#include "game/bodyarena.h"
#include "game/bodycontroller.h"
#include "game/bodyobservations.h"
#include "game/bodyreplay.h"
#include "game/gamescript.h"
#include "gothic.h"
#include "graphics/mesh/animationsolver.h"
#include "world/aiqueue.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/objects/sound.h"
#include "world/world.h"
#include "world/worldsound.h"

using namespace Tempest;

namespace testbench_detail {

void stopBodyLive() {
  const auto context=bodyContext();
  if(!context.bodyLive)
    return;
  BodyController::setSink({});
  context.bodyLive = false;
  }

const char* animationModeName() {
  const auto context=bodyContext();
  switch(context.bodyAnimation.mode) {
    case BodyAnimationControl::Isolated: return "isolated";
    case BodyAnimationControl::Replay: return "replay";
    default: return "default";
    }
  }

void animationControlError(const std::string& reason) {
  const auto context=bodyContext();
  ++context.bodyAnimation.errors;
  say("body animation control: " + reason);
  event("body_animation_control_error","\"schema_version\":1,\"mode\":"+jsonStr(animationModeName())+
         ",\"seed\":"+std::to_string(context.bodyAnimation.seed)+",\"reason\":"+jsonStr(reason));
  // The existing strict baseline reader must reject a failed replay, even if the duel finishes.
  event("body_duel_error","\"reason\":"+jsonStr("body animation control: "+reason));
  }

bool uniqueJsonKeys(const rapidjson::Value& value) {
  if(value.IsObject()) {
    std::unordered_set<std::string> names;
    for(auto i=value.MemberBegin(); i!=value.MemberEnd(); ++i)
      if(!names.emplace(i->name.GetString(),i->name.GetStringLength()).second || !uniqueJsonKeys(i->value))
        return false;
    }
  else if(value.IsArray()) {
    for(const auto& i:value.GetArray())
      if(!uniqueJsonKeys(i))
        return false;
    }
  return true;
  }

int blockVariant(std::string_view name) {
  for(std::string_view prefix:{"T_1HPARADE_0","T_2HPARADE_0"}) {
    if(name==prefix) return 0;
    if(name==std::string(prefix)+"_A2") return 1;
    if(name==std::string(prefix)+"_A3") return 2;
    }
  return -1;
  }

bool initializeAnimationControl() {
  const auto context=bodyContext();
  auto& control = context.bodyAnimation;
  if(control.initialized)
    return control.valid;
  control.initialized = true;
  const char* mode = envStr("OPENGOTHIC_BODY_ANIMATION_RNG");
  if(mode==nullptr)
    return true;
  if(std::string_view(mode)=="isolated") {
    control.mode = BodyAnimationControl::Isolated;
    return true;
    }
  control.mode = BodyAnimationControl::Replay;
  const auto fail = [&](const std::string& reason) {
    control.valid = false;
    animationControlError(reason);
    return false;
    };
  if(std::string_view(mode)!="replay")
    return fail("unknown OPENGOTHIC_BODY_ANIMATION_RNG mode");
  const char* path = envStr("OPENGOTHIC_BODY_ANIMATION_FIXTURE");
  if(path==nullptr)
    return fail("replay requires OPENGOTHIC_BODY_ANIMATION_FIXTURE");
  std::ifstream input(path,std::ios::binary);
  if(!input)
    return fail("cannot read block-variant fixture");
  input.seekg(0,std::ios::end);
  const auto size = input.tellg();
  if(size<0 || size>4*1024*1024)
    return fail("invalid block-variant fixture size");
  input.seekg(0,std::ios::beg);
  const std::string text((std::istreambuf_iterator<char>(input)),std::istreambuf_iterator<char>());
  rapidjson::Document fixture;
  fixture.Parse<rapidjson::kParseValidateEncodingFlag>(text.c_str(),text.size());
  if(fixture.HasParseError() || !fixture.IsObject() || !uniqueJsonKeys(fixture))
    return fail("invalid/duplicate-key block-variant fixture JSON");
  if(!fixture.HasMember("schema_version") || !fixture["schema_version"].IsInt() || fixture["schema_version"].GetInt()!=1 ||
     !fixture.HasMember("kind") || !fixture["kind"].IsString() || std::string_view(fixture["kind"].GetString())!="body_animation_branch_fixture" ||
     !fixture.HasMember("source_events_sha256") || !fixture["source_events_sha256"].IsString() ||
     !fixture.HasMember("entries") || !fixture["entries"].IsArray())
    return fail("unsupported/incomplete block-variant fixture");
  control.sourceEventsHash = fixture["source_events_sha256"].GetString();
  if(control.sourceEventsHash.size()!=64 || control.sourceEventsHash.find_first_not_of("0123456789abcdef")!=std::string::npos)
    return fail("invalid fixture source_events_sha256");
  std::unordered_set<std::string> keys;
  std::unordered_set<uint64_t> sourceSequences;
  for(const auto& item:fixture["entries"].GetArray()) {
    if(!item.IsObject() || !item.HasMember("seed") || !item["seed"].IsUint() ||
       !item.HasMember("role") || !item["role"].IsString() ||
       !item.HasMember("relative_time_ms") || !item["relative_time_ms"].IsUint64() ||
       !item.HasMember("animation") || !item["animation"].IsString() ||
       !item.HasMember("variant") || !item["variant"].IsInt() ||
       !item.HasMember("source_seq") || !item["source_seq"].IsUint64() || item["source_seq"].GetUint64()==0)
      return fail("incomplete block-variant fixture entry");
    BodyAnimationEntry entry;
    entry.seed = item["seed"].GetUint();
    const std::string_view role = item["role"].GetString();
    if(role!="actor" && role!="target")
      return fail("unknown fixture actor role");
    entry.side = role=="actor" ? 0 : 1;
    entry.time = item["relative_time_ms"].GetUint64();
    entry.animation = item["animation"].GetString();
    entry.variant = item["variant"].GetInt();
    entry.sourceSeq = item["source_seq"].GetUint64();
    if(entry.variant<0 || entry.variant>2 || blockVariant(entry.animation)!=entry.variant)
      return fail("fixture animation/variant mismatch");
    const auto key = std::to_string(entry.seed)+":"+std::to_string(entry.side)+":"+std::to_string(entry.time);
    if(!keys.insert(key).second)
      return fail("duplicate seed/role/time fixture entry");
    if(!sourceSequences.insert(entry.sourceSeq).second)
      return fail("duplicate source_seq fixture entry");
    control.fixture.push_back(std::move(entry));
    }
  return true;
  }

bool beginAnimationControl(uint32_t seed, const std::vector<Npc*>& actors) {
  const auto context=bodyContext();
  auto& control = context.bodyAnimation;
  control.seed = seed;
  if((actors.size()!=2 && actors.size()!=4) || std::any_of(actors.begin(),actors.end(),[](Npc* npc) { return npc==nullptr; }))
    return false;
  if(!initializeAnimationControl())
    return false;
  if(control.mode==BodyAnimationControl::Default)
    return true;
  if(actors.size()==4 && control.mode==BodyAnimationControl::Replay) {
    animationControlError("four-actor animation replay requires a separate fixture contract");
    return false;
    }
  control.active = true;
  control.errorsAtStart = control.errors;
  control.actorCount = unsigned(actors.size());
  for(auto& actor:control.actors) actor = {};
  for(unsigned side=0; side<control.actorCount; ++side) {
    control.actors[side].npc = actors[side];
    // Defined uint32 overflow; slots 0/1 preserve the archived two-actor streams.
    // The four-actor load fixture gets two further independent streams, never std-RNG fallback.
    control.actors[side].rng = seed ^ (0x9e3779b9u*(side+1u));
    if(control.actors[side].rng==0)
      control.actors[side].rng = 0x6d2b79f5u;
    }
  event("body_animation_control_start","\"schema_version\":1,\"mode\":"+jsonStr(animationModeName())+
        ",\"seed\":"+std::to_string(seed)+",\"source_events_sha256\":"+jsonStr(control.sourceEventsHash)+
        (control.actorCount==4 ? ",\"actor_count\":4" : ""));
  return true;
  }

bool beginAnimationControl(uint32_t seed, Npc& first, Npc& second) {
  return beginAnimationControl(seed,std::vector<Npc*>{&first,&second});
  }

unsigned animationActorIndex(const Npc& npc) {
  const auto context=bodyContext();
  const auto& control=context.bodyAnimation;
  for(unsigned index=0;index<control.actorCount;++index)
    if(control.actors[index].npc==&npc) return index;
  return control.actorCount;
  }

std::string animationActorFields(unsigned index) {
  const auto context=bodyContext();
  return context.bodyAnimation.actorCount==4 ? ",\"pair_index\":"+std::to_string(index/2)+
         ",\"animation_actor_index\":"+std::to_string(index) : "";
  }

BodyAnimationEntry* animationFixtureEntry(unsigned side, uint64_t time) {
  const auto context=bodyContext();
  for(auto& entry:context.bodyAnimation.fixture)
    if(entry.seed==context.bodyAnimation.seed && entry.side==side && entry.time==time)
      return &entry;
  return nullptr;
  }

void observeAnimationAttempt(const Npc& npc, const std::string& fields, uint64_t gameTime) {
  const auto context=bodyContext();
  auto& control = context.bodyAnimation;
  if(!control.active || control.mode==BodyAnimationControl::Default)
    return;
  if(npc.weaponState()!=WeaponState::W1H && npc.weaponState()!=WeaponState::W2H)
    return;
  const unsigned side = animationActorIndex(npc);
  if(side>=control.actorCount)
    return;
  rapidjson::Document row;
  const auto json = "{"+fields+"}";
  row.Parse(json.c_str(),json.size());
  if(row.HasParseError() || !row.IsObject() || !row.HasMember("requested_anim") || !row["requested_anim"].IsInt() ||
     row["requested_anim"].GetInt()!=int(AnimationSolver::AttackBlock))
    return;
  if(!row.HasMember("accepted") || !row["accepted"].IsBool() || !row["accepted"].GetBool() ||
     !row.HasMember("pose_changed") || !row["pose_changed"].IsBool() || !row["pose_changed"].GetBool())
    return;
  auto& actor = control.actors[side];
  ++actor.accepted;
  const uint64_t time = gameTime-context.duelFrom;
  std::string animation;
  if(row.HasMember("layers_after") && row["layers_after"].IsArray())
    for(const auto& layer:row["layers_after"].GetArray())
      if(layer.IsObject() && layer.HasMember("name") && layer["name"].IsString() && blockVariant(layer["name"].GetString())>=0) {
        if(!animation.empty()) {
          animationControlError("multiple eligible block layers after accepted attempt");
          return;
          }
        animation = layer["name"].GetString();
        }
  const int variant = blockVariant(animation);
  auto* entry = control.mode==BodyAnimationControl::Replay ? animationFixtureEntry(side,time) : nullptr;
  if(variant<0 || actor.lastTime!=time || actor.lastVariant!=variant)
    animationControlError("accepted block has no matching controlled solver branch");
  if(control.mode==BodyAnimationControl::Replay) {
    if(entry==nullptr)
      animationControlError("unexpected accepted block: no exact seed/role/time fixture entry");
    else if(entry->consumed!=0)
      animationControlError("duplicate accepted block fixture consumption");
    else if(entry->animation!=animation || entry->variant!=variant)
      animationControlError("accepted block differs from fixture animation/variant");
    else if(actor.lastTime==time && actor.lastVariant==variant)
      ++entry->consumed;
    }
  TestBench::bodyEvent(npc,npc.target(),"animation_branch_accept",gameTime,
                      "\"mode\":"+jsonStr(animationModeName())+",\"seed\":"+std::to_string(control.seed)+
                      ",\"role\":"+jsonStr(side%2==0 ? "actor" : "target")+",\"relative_time_ms\":"+std::to_string(time)+
                      ",\"selected_variant\":"+std::to_string(variant)+",\"animation_name\":"+jsonStr(animation)+
                      ",\"source_seq\":"+std::to_string(entry==nullptr ? 0 : entry->sourceSeq)+animationActorFields(side));
  }

void endAnimationControl() {
  const auto context=bodyContext();
  auto& control = context.bodyAnimation;
  if(!control.active)
    return;
  uint64_t expected = 0, consumed = 0, solver = 0, controlled = 0, fallback = 0, accepted = 0;
  for(const auto& entry:control.fixture)
    if(entry.seed==control.seed) {
      ++expected;
      consumed += entry.consumed;
      }
  for(const auto& actor:control.actors) {
    solver += actor.solverCalls;
    controlled += actor.controlledCalls;
    fallback += actor.fallbackCalls;
    accepted += actor.accepted;
    }
  if(control.mode==BodyAnimationControl::Replay && expected!=consumed)
    animationControlError("unconsumed archived block fixture entries at episode terminal");
  const auto errors = control.errors-control.errorsAtStart;
  event("body_animation_control_summary","\"schema_version\":1,\"mode\":"+jsonStr(animationModeName())+
        ",\"seed\":"+std::to_string(control.seed)+",\"expected_entries\":"+std::to_string(expected)+
        ",\"consumed_entries\":"+std::to_string(consumed)+",\"remaining_entries\":"+std::to_string(expected-consumed)+
        ",\"solver_calls\":"+std::to_string(solver)+",\"controlled_calls\":"+std::to_string(controlled)+
        ",\"fallback_calls\":"+std::to_string(fallback)+",\"accepted_blocks\":"+std::to_string(accepted)+
        ",\"errors\":"+std::to_string(errors)+",\"status\":"+jsonStr(errors==0 ? "complete" : "incomplete"));
  control.active = false;
  for(auto& actor:control.actors)
    actor.npc = nullptr;
  }

void animationControlTotals() {
  const auto context=bodyContext();
  auto& control = context.bodyAnimation;
  if(control.mode==BodyAnimationControl::Default || control.totalsWritten)
    return;
  control.totalsWritten = true;
  uint64_t consumed = 0;
  for(const auto& entry:control.fixture)
    consumed += entry.consumed;
  if(control.mode==BodyAnimationControl::Replay && consumed!=control.fixture.size())
    animationControlError("unconsumed archived block fixture entries at scenario end");
  event("body_animation_control_totals","\"schema_version\":1,\"mode\":"+jsonStr(animationModeName())+
        ",\"expected_entries\":"+std::to_string(control.fixture.size())+",\"consumed_entries\":"+std::to_string(consumed)+
        ",\"remaining_entries\":"+std::to_string(control.fixture.size()-consumed)+
        ",\"errors\":"+std::to_string(control.errors)+",\"status\":"+jsonStr(control.errors==0 ? "complete" : "incomplete"));
  }

// `bodyarena`, `bodyreplay`: the configured arena's participants, with isolated animation RNG
void setupArena(World& w, Npc& pl) {
  const auto context=bodyContext();
  try {
    if(context.bodyLive)
      throw std::runtime_error("bodylive must be off before bodyarena/bodyreplay");
    BodyArena::setup(w,pl);
    const auto actors=BodyArena::actors();
    context.duelFrom=w.tickCount();
    if(!beginAnimationControl(BodyArena::seed(),actors))
      throw std::runtime_error("arena animation RNG setup failed");
    event("body_arena_setup","\"arena\":"+BodyArena::statusJson());
    }
  catch(const std::exception& error) {
    event("body_arena_error","\"reason\":"+jsonStr(error.what()));
    context.pc=context.prog.size();
    throw;
    }
  }

bool waitingForBodyLive() {
  const auto context=bodyContext();
  if(context.bodyLiveWait.empty())
    return false;
  std::error_code error;
  if(std::filesystem::is_regular_file(context.bodyLiveWait,error)) {
    event("body_live_ready","\"path\":"+jsonStr(context.bodyLiveWait));
    context.bodyLiveWait.clear();
    return false;
    }
  if(std::chrono::steady_clock::now()<context.bodyLiveDeadline)
    return true;
  event("body_live_error","\"reason\":\"readiness file timeout\",\"path\":"+jsonStr(context.bodyLiveWait));
  context.bodyLiveWait.clear();
  context.pc=context.prog.size();
  context.waitUntil=0;
  return false;
  }

bool bodyCommand(const BodyContext& context,const std::vector<std::string>& a,World& w,Npc& pl) {
  const std::string& c = a[0];
  if(c=="bodyarena") {
    setupArena(w,pl);
    }
  else if(c=="bodylive") {
    // Live engine commands only: no controller state, watch list, policy, or lease is modified.
    if(a.size()==2 && a[1]=="off") {
      stopBodyLive();
      event("body_live","\"enabled\":false");
      }
    else if(a.size()==2 && a[1]=="on" && !BodyArena::enabled() && !BodyReplay::active()) {
      context.bodyLive = true;
      BodyController::setSink([](std::string_view row) {
        if(bodyContext().bodyLive)
          event("body_live_control","\"control\":"+std::string(row));
        });
      event("body_live","\"enabled\":true");
      }
    else if(a.size()>=3 && a.size()<=4 && a[1]=="wait" && std::filesystem::path(a[2]).is_absolute() && num(a,3,120000)>0) {
      context.bodyLiveWait=a[2];
      context.bodyLiveDeadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(int64_t(num(a,3,120000)));
      event("body_live_wait","\"path\":"+jsonStr(context.bodyLiveWait));
      }
    else {
      event("body_live_error","\"reason\":\"expected bodylive on/off or wait /absolute/path [timeout ms]; live capture requires arena/replay off\"");
      context.pc=context.prog.size();
      }
    }
  else if(c=="bodyreplay") {
    // bodyreplay <file> [tail ms 3000]: a recorded lockstep arena episode, replayed for the camera
    // (game/bodyreplay.h); put it in place of `bodyarena`, the following `wait` ends with the replay
    try {
      if(context.bodyLive)
        throw std::runtime_error("bodylive must be off before bodyreplay");
      BodyReplay::load(a.size()>1 ? a[1] : "",outDir,uint64_t(num(a,2,3000)));
      setupArena(w,pl);
      }
    catch(const std::exception& error) {
      event("body_replay_error","\"reason\":"+jsonStr(error.what()));
      say("bodyreplay: "+std::string(error.what()));
      BodyReplay::stop("error");
      context.pc=context.prog.size();
      }
    }
  else if(c=="bodyobserve" || c=="bodyapi") {
    const bool bodyApi = c=="bodyapi";
    const size_t actorCount = bodyApi ? 2 : 4;
    // Four mortal native fighters. High HP keeps real damage/defence alive for the ten-minute
    // observation test; no healing, invulnerability, held input or policy owns their bodies.
    const auto fail = [&](const std::string& reason) {
      event(bodyApi ? "body_api_error" : "body_observe_error","\"reason\":"+jsonStr(reason));
      say(c+": "+reason);
      context.pc = context.prog.size();
      };
    const char* names[] = {"BDT_1003_BANDIT_M","PAL_255_RITTER","ItMw_1h_Common_01","ItAr_Leather_L",
                           "ZS_ATTACK","FAI_HUMAN_MASTER","AIV_ATTACKREASON","AR_KILL"};
    size_t ids[std::size(names)] = {};
    for(size_t i=0;i<std::size(names);++i) {
      ids[i] = w.script().findSymbolIndex(names[i]);
      if(ids[i]==size_t(-1)) { fail(std::string("missing symbol ")+names[i]); return true; }
      }
    const int reason = w.script().findSymbol(names[6])->get_int();
    if(reason<0 || size_t(reason)>=daedalus::INpc::aivar_count) { fail("invalid attack reason"); return true; }
    for(auto* actor:context.observeActors)
      w.removeNpc(*actor);
    context.observeActors.clear();
    const uint32_t seed = uint32_t(num(a,1,17));
    const uint64_t duration = uint64_t(std::clamp(num(a,2,600000),1000.f,600000.f));
    w.script().setSeed(seed);
    const float an = pl.rotation()*float(M_PI)/180.f;
    const Vec3 forward(std::cos(an),0,std::sin(an)), side(-forward.z,0,forward.x);
    Vec3 positions[4];
    for(size_t i=0;i<actorCount;++i) {
      auto p = pl.position()+forward*1200.f+side*((i<2 ? -550.f : 550.f)+(i%2==0 ? -150.f : 150.f));
      const float y = groundY(w,p.x,p.z,p.y);
      if(!std::isfinite(y)) { fail("arena ground not found"); return true; }
      p.y = y;
      positions[i] = p;
      }
    for(size_t i=0;i<actorCount;++i) {
      Npc* actor = w.addNpc(ids[i%2],positions[i]);
      if(actor==nullptr) { fail("NPC creation failed"); return true; }
      context.observeActors.push_back(actor);
      actor->setExternalControl(true);
      actor->setInteraction(nullptr,true);
      actor->clearAiQueue(); actor->clearState(true); actor->clearGoTo();
      std::vector<size_t> equipped;
      for(auto it=actor->inventory().iterator(Inventory::T_Inventory);it.isValid();++it)
        if(it.isEquipped()) equipped.push_back(it->clsId());
      for(auto item:equipped) actor->unequipItem(item);
      actor->clearInventory();
      actor->addItem(ids[2],1); actor->useItem(ids[2],Item::NSLOT,true);
      actor->addItem(ids[3],1); actor->useItem(ids[3],Item::NSLOT,true);
      auto& h = actor->handle();
      h.flags = daedalus::NpcFlag(uint32_t(h.flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
      h.attribute[ATR_HITPOINTS] = h.attribute[ATR_HITPOINTSMAX] = 1000000;
      h.attribute[ATR_STRENGTH] = h.attribute[ATR_DEXTERITY] = 60;
      h.hitchance[TALENT_1H] = 60; actor->setTalentSkill(TALENT_1H,2);
      h.protection[PROT_EDGE] = h.protection[PROT_BLUNT] = 30;
      h.guild = i%2==0 ? GIL_BDT : GIL_PAL;
      h.fight_tactic = w.script().findSymbol(names[5])->get_int();
      h.aivar[size_t(reason)] = w.script().findSymbol(names[7])->get_int();
      actor->setPosition(positions[i]); actor->setDirection(positions[i^1]-positions[i]);
      actor->updateTransform();
      if(actor->currentMeleeWeapon()==nullptr || actor->currentRangedWeapon()!=nullptr || actor->isImmortal()) {
        fail("native melee setup validation failed"); return true;
        }
      }
    w.script().setSeed(seed);
    std::string actors = "[";
    for(size_t i=0;i<actorCount;++i) {
      auto* actor = context.observeActors[i];
      auto* target = context.observeActors[i^1];
      actor->setExternalControl(false); actor->setTarget(target);
      actor->aiPush(AiQueue::aiStartState(ScriptFn(ids[4]),0,target,nullptr,""));
      TestBench::bodyEvent(*actor,target,"observation_actor_mapping",w.tickCount(),
                          "\"observation_entity_id\":"+std::to_string(BodyObservations::entityId(*actor)));
      if(i>0) actors += ",";
      actors += "{\"entity_id\":"+std::to_string(BodyObservations::entityId(*actor))+
                ",\"symbol\":"+jsonStr(names[i%2])+",\"hp\":1000000,\"hp_max\":1000000,\"immortal\":false}";
      }
    context.observeFrom = w.tickCount(); context.observeEnd = context.observeFrom+duration;
    // Stage 3 explicitly forbids external/frozen actors. Stage 4 intentionally tests those
    // transitions and owns its assertions in the API suite, while retaining the same real setup.
    context.observeActive = !bodyApi; context.observeVisibleSaved = false;
    event(bodyApi ? "body_api_setup" : "body_observe_setup","\"seed\":"+std::to_string(seed)+",\"t_game_ms\":"+
          std::to_string(context.observeFrom)+",\"duration_ms\":"+std::to_string(duration)+
          ",\"ownership\":\"native\",\"actors\":"+actors+"]");
    }
  else if(c=="bodyduel") {
    // bodyduel <seed> [timeout ms]: native human melee reference, both participants mortal. The
    // nearby player is only an observer. These constants are versioned in evals/npc-body/melee-v1.json.
    auto fail = [&](const std::string& reason) {
      say("bodyduel: setup_failed " + reason);
      event("body_duel_error","\"reason\":" + jsonStr(reason));
      context.pc = context.prog.size();
      };
    if(!TestBench::bodyTracing()) {
      fail("OPENGOTHIC_BODY_TRACE=1 is required");
      return true;
      }
    const char* names[] = {"BDT_1003_BANDIT_M", "PAL_255_RITTER", "ItMw_1h_Common_01", "ItAr_Leather_L",
                           "ZS_ATTACK", "FAI_HUMAN_MASTER", "AIV_ATTACKREASON", "AR_KILL"};
    size_t ids[std::size(names)] = {};
    for(size_t i=0; i<std::size(names); ++i) {
      ids[i] = w.script().findSymbolIndex(names[i]);
      if(ids[i]==size_t(-1)) {
        fail(std::string("missing symbol ") + names[i]);
        return true;
        }
      }
    const int reason = w.script().findSymbol(names[6])->get_int();
    if(reason<0 || size_t(reason)>=daedalus::INpc::aivar_count) {
      fail("invalid AIV_ATTACKREASON index");
      return true;
      }
    for(Npc* n:{context.duelA,context.duelB}) {
      if(n==nullptr)
        continue;
      if(context.subject==n)
        context.subject = nullptr;
      w.removeNpc(*n);
      }
    context.duelA = context.duelB = nullptr;
    context.duelWaiting = false;
    const uint32_t seed = uint32_t(num(a,1,17));
    const uint64_t timeout = uint64_t(std::clamp(num(a,2,60000),1000.f,60000.f));
    w.script().setSeed(seed); // constructors/equipment callbacks get the same RNG start too
    const float an = pl.rotation()*float(M_PI)/180.f;
    const Vec3 forward(std::cos(an),0,std::sin(an));
    const Vec3 side(-forward.z,0,forward.x);
    const Vec3 center = pl.position()+forward*1200.f;
    Vec3 pos[2] = {center-side*150.f,center+side*150.f};
    for(auto& p:pos) {
      DynamicWorld::RayLandResult ground;
      for(float y=p.y+1500; y>p.y-1500 && !ground.hasCol; y-=400)
        ground = w.physic()->ray({p.x,y,p.z},{p.x,y-450,p.z});
      if(!ground.hasCol) {
        fail("arena ground not found");
        return true;
        }
      p.y = ground.v.y;
      }
    context.duelA = w.addNpc(ids[0],pos[0]);
    context.duelB = w.addNpc(ids[1],pos[1]);
    if(context.duelA==nullptr || context.duelB==nullptr) {
      fail("NPC creation failed");
      return true;
      }
    Npc* fighters[] = {context.duelA,context.duelB};
    for(size_t i=0; i<2; ++i) {
      Npc& n = *fighters[i];
      n.setInteraction(nullptr,true);
      n.clearAiQueue();
      n.clearState(true);
      n.clearGoTo();
      n.setExternalControl(true);
      // Inventory::clear deliberately preserves equipped items. Unequip first, including passive
      // jewellery modifiers, so an archer source cannot keep its bow or inherited armour bonuses.
      std::vector<size_t> equipped;
      for(auto it=n.inventory().iterator(Inventory::T_Inventory); it.isValid(); ++it)
        if(it.isEquipped())
          equipped.push_back(it->clsId());
      for(auto cls:equipped)
        n.unequipItem(cls);
      n.clearInventory();
      n.addItem(ids[2],1);
      n.useItem(ids[2],Item::NSLOT,true);
      n.addItem(ids[3],1);
      n.useItem(ids[3],Item::NSLOT,true);
      auto& h = n.handle();
      h.flags = daedalus::NpcFlag(uint32_t(h.flags) & ~uint32_t(daedalus::NpcFlag::IMMORTAL));
      h.attribute[ATR_STRENGTH] = 60;
      h.attribute[ATR_DEXTERITY] = 60;
      h.attribute[ATR_HITPOINTSMAX] = 300;
      h.attribute[ATR_HITPOINTS] = 300;
      h.hitchance[TALENT_1H] = 60;
      n.setTalentSkill(TALENT_1H,2);
      h.protection[PROT_EDGE] = 30;
      h.protection[PROT_BLUNT] = 30;
      h.guild = i==0 ? GIL_BDT : GIL_PAL;
      h.fight_tactic = w.script().findSymbol(names[5])->get_int();
      h.aivar[size_t(reason)] = w.script().findSymbol(names[7])->get_int();
      n.setPosition(pos[i]);
      n.setDirection(pos[1-i]-pos[i]);
      n.updateTransform();
      auto sword = n.currentMeleeWeapon();
      if(sword==nullptr || n.currentRangedWeapon()!=nullptr || n.currentArmor()==nullptr || n.isImmortal()) {
        fail("melee-only equipment or mortality validation failed");
        return true;
        }
      char fields[1000];
      std::snprintf(fields,sizeof(fields),
                    "\"side\":%zu,\"seed\":%u,\"hp\":%d,\"hpmax\":%d,\"str\":%d,\"dex\":%d,"
                    "\"onehand\":%d,\"onehand_talent\":%d,\"edge_protection\":%d,\"blunt_protection\":%d,"
                    "\"fight_tactic\":%d,\"guild\":%d,\"weapon_damage\":%d,\"weapon_length\":%d,"
                    "\"immortal\":false,\"x\":%.3f,\"y\":%.3f,\"z\":%.3f,\"observer_distance_cm\":%.3f",
                    i,seed,n.attribute(ATR_HITPOINTS),n.attribute(ATR_HITPOINTSMAX),n.attribute(ATR_STRENGTH),
                    n.attribute(ATR_DEXTERITY),n.hitChance(TALENT_1H),n.talentSkill(TALENT_1H),
                    n.protection(PROT_EDGE),n.protection(PROT_BLUNT),h.fight_tactic,h.guild,
                    sword->handle().damage_total,sword->handle().range,double(pos[i].x),double(pos[i].y),double(pos[i].z),
                    double((pos[i]-pl.position()).length()));
      TestBench::bodyEvent(n,fighters[1-i],"actor_setup",w.tickCount(),std::string(fields) +
                           ",\"weapon\":"+jsonStr(names[2])+",\"armor\":"+jsonStr(names[3]));
      }
    // Separate the native fight RNG from constructor/equipment callback consumption. No scripted
    // attacks, strike/hit commands, invulnerability, held inputs or model take part in the duel.
    w.script().setSeed(seed);
    context.lcg = seed*2654435761u+1u;
    context.duelFrom = w.tickCount();
    context.duelEnd = context.duelFrom+timeout;
    if(!beginAnimationControl(seed,*fighters[0],*fighters[1])) {
      fail("cannot initialize requested block-variant control");
      return true;
      }
    context.duelWaiting = true;
    TestBench::bodyEvent(*fighters[0],fighters[1],"episode_start",w.tickCount(),
                         "\"seed\":"+std::to_string(seed)+",\"timeout_ms\":"+std::to_string(timeout)+
                         ",\"scenario\":\"melee-v1\",\"source\":\"native\"");
    for(size_t i=0; i<2; ++i) {
      fighters[i]->setExternalControl(false);
      fighters[i]->setTarget(fighters[1-i]);
      fighters[i]->aiPush(AiQueue::aiStartState(ScriptFn(ids[4]),0,fighters[1-i],nullptr,""));
      }
    say("bodyduel seed "+std::to_string(seed)+" timeout "+std::to_string(timeout));
    }
  else if(c=="bodyobserveprobe") {
    // A separately labelled privacy fixture: native geometry and visibility, deliberately frozen
    // participants. It is not a combat episode or evidence of autonomous policy behavior.
    const auto actors = TestBench::bodyObservationActors();
    const auto fail = [&](const std::string& reason) {
      event("body_observe_error","\"reason\":"+jsonStr(reason));
      context.pc = context.prog.size();
      };
    if(actors.size()<2 || context.duelWaiting) { fail("a completed native setup is required"); return true; }
    Npc& first = *actors[0]; Npc& second = *actors[1];
    const std::string mode = a.size()>1 ? a[1] : "visible";
    if(mode!="visible" && mode!="occluded" && mode!="hidden_move" && mode!="reappear") {
      fail("expected visible, occluded, hidden_move or reappear"); return true;
      }
    context.observeActive = false;
    for(auto* actor:actors) {
      actor->clearAiQueue(); actor->clearState(true); actor->clearGoTo(); actor->clearSpeed();
      actor->setExternalControl(true); actor->setAnim(Npc::Anim::Idle);
      }
    const auto place = [&](const Vec3& p0,const Vec3& p1) {
      first.setPosition(p0); second.setPosition(p1);
      BodyObservations::discontinuity(first); BodyObservations::discontinuity(second);
      first.setDirection(p1-p0); second.setDirection(p0-p1);
      first.updateTransform(); second.updateTransform();
      };
    if(mode=="visible") {
      context.observeVisibleA = first.position(); context.observeVisibleB = second.position();
      context.observeVisibleSaved = true;
      place(context.observeVisibleA,context.observeVisibleB);
      }
    else if(mode=="reappear") {
      if(!context.observeVisibleSaved) { fail("visible fixture was not initialized"); return true; }
      place(context.observeVisibleA,context.observeVisibleB);
      }
    else if(mode=="occluded") {
      std::vector<const WayPoint*> points;
      const auto origin = first.position();
      w.findWayPoint(origin,[&](const WayPoint& p) { points.push_back(&p); return false; });
      std::sort(points.begin(),points.end(),[&](const WayPoint* p0,const WayPoint* p1) {
        const auto d0 = (p0->pos-origin).quadLength(), d1 = (p1->pos-origin).quadLength();
        return d0!=d1 ? d0<d1 : p0->name<p1->name;
        });
      bool found = false; size_t attempts = 0;
      const float range = float(first.handle().senses_range);
      for(const auto* p0:points) {
        if(found || attempts>=8192) break;
        const float y0 = groundY(w,p0->pos.x,p0->pos.z,p0->pos.y);
        if(!std::isfinite(y0) || std::abs(y0-p0->pos.y)>120.f) continue;
        for(const auto* p1:points) {
          const float distance = (p1->pos-p0->pos).length();
          if(distance<200.f || distance>std::min(range-250.f,2500.f)) continue;
          const float y1 = groundY(w,p1->pos.x,p1->pos.z,p1->pos.y);
          if(!std::isfinite(y1) || std::abs(y1-p1->pos.y)>120.f) continue;
          ++attempts; place({p0->pos.x,y0,p0->pos.z},{p1->pos.x,y1,p1->pos.z});
          const auto center = w.physic()->ray(first.mapHeadBone(),second.collosionCenter());
          const auto head = w.physic()->ray(first.mapHeadBone(),second.mapHeadBone());
          if(center.hasCol && head.hasCol && center.hitFraction>0.01f && center.hitFraction<0.99f &&
             head.hitFraction>0.01f && head.hitFraction<0.99f && !first.canSeeNpc(second,true)) {
            found = true; break;
            }
          if(attempts>=8192) break;
          }
        }
      if(!found) { fail("bounded search found no real occlusion"); return true; }
      }
    else {
      const auto p0 = first.position(), p1 = second.position();
      bool found = false;
      const Vec3 offsets[] = {{50,0,0},{-50,0,0},{0,0,50},{0,0,-50}};
      for(const auto& offset:offsets) {
        auto candidate = p1+offset;
        const float y = groundY(w,candidate.x,candidate.z,candidate.y);
        if(!std::isfinite(y)) continue;
        candidate.y = y; place(p0,candidate);
        if(!first.canSeeNpc(second,true)) { found = true; break; }
        }
      if(!found) { fail("hidden movement lost geometric occlusion"); return true; }
      second.changeAttribute(ATR_HITPOINTS,-17,false);
      }
    first.setTarget(&second); second.setTarget(&first);
    const bool visible = first.canSeeNpc(second,false);
    const auto center = w.physic()->ray(first.mapHeadBone(),second.collosionCenter());
    const auto head = w.physic()->ray(first.mapHeadBone(),second.mapHeadBone());
    const bool hidden = mode=="occluded" || mode=="hidden_move";
    if(visible==hidden || (hidden && (!center.hasCol || !head.hasCol))) {
      fail("privacy fixture visibility disagrees with real rays"); return true;
      }
    pl.setPosition(first.position()); pl.updateTransform();
    Gothic::inst().gameSession()->camera().reset(&pl);
    const auto pos = second.position();
    context.observeProbeCase = mode;
    event("body_observe_probe","\"case\":"+jsonStr(mode)+",\"t_game_ms\":"+std::to_string(w.tickCount())+
          ",\"actor_id\":"+std::to_string(BodyObservations::entityId(first))+
          ",\"target_id\":"+std::to_string(BodyObservations::entityId(second))+
          ",\"visible\":"+(visible ? "true" : "false")+
          ",\"center_ray_hit\":"+(center.hasCol ? "true" : "false")+
          ",\"head_ray_hit\":"+(head.hasCol ? "true" : "false")+
          ",\"truth_self_hp\":"+std::to_string(first.attribute(ATR_HITPOINTS))+
          ",\"truth_self_yaw_rad\":"+std::to_string(first.rotationRad())+
          ",\"truth_self_position_cm\":["+std::to_string(first.position().x)+","+
          std::to_string(first.position().y)+","+std::to_string(first.position().z)+"]"+
          ",\"truth_target_hp\":"+std::to_string(second.attribute(ATR_HITPOINTS))+
          ",\"truth_target_position_cm\":["+std::to_string(pos.x)+","+std::to_string(pos.y)+","+std::to_string(pos.z)+"]");
    }
  else if(c=="bodyprobe") {
    // Separate stage 2 proof: real landscape/object collision rays and native sight/sound APIs.
    // A short bodyduel first provides the same real skeletons and 30 native simulation steps.
    auto fail = [&](const std::string& reason) {
      say("bodyprobe: failed " + reason);
      event("body_probe_error","\"reason\":"+jsonStr(reason));
      context.pc = context.prog.size();
      };
    if(!TestBench::bodyTracing() || context.duelA==nullptr || context.duelB==nullptr || context.duelWaiting) {
      fail("a completed traced bodyduel setup is required");
      return true;
      }
    Npc& first = *context.duelA;
    Npc& second = *context.duelB;
    const std::string mode = a.size()>1 ? a[1] : "visible";
    const auto vec = [](const Vec3& v) {
      return "["+std::to_string(v.x)+","+std::to_string(v.y)+","+std::to_string(v.z)+"]";
      };
    if(mode=="sound_end") {
      if(!context.probeSound || context.probeSoundDuration==0) {
        fail("no sound probe is active");
        return true;
        }
      const bool finished = context.probeSound->isFinished();
      const bool sourceVisible = w.sound()->canSeeSource(first.mapHeadBone());
      const uint64_t elapsed = w.tickCount()-context.probeSoundStart;
      TestBench::bodyEvent(first,&second,"senses_sound_end",w.tickCount(),
                           "\"case\":"+jsonStr(context.probeOccluded ? "occluded" : "visible")+
                           ",\"duration_ms\":"+std::to_string(context.probeSoundDuration)+
                           ",\"elapsed_ms\":"+std::to_string(elapsed)+
                           ",\"finished\":"+(finished ? "true" : "false")+
                           ",\"source_visible_after_cleanup\":"+(sourceVisible ? "true" : "false"));
      context.probeSound.reset();
      if(!finished || elapsed<context.probeSoundDuration || sourceVisible)
        fail("sound duration or effect3d cleanup invariant failed");
      return true;
      }
    if(mode!="visible" && mode!="occluded") {
      fail("expected visible, occluded or sound_end");
      return true;
      }
    if(context.probeSound) {
      fail("finish the preceding sound probe first");
      return true;
      }
    const bool occluded = mode=="occluded";
    const auto nativePolicy = first.processPolicy();
    if(!occluded && nativePolicy!=NpcProcessPolicy::AiNormal) {
      fail("the native fighter was not processed with AiNormal");
      return true;
      }
    for(Npc* n:{&first,&second}) {
      n->clearAiQueue();
      n->clearState(true);
      n->clearGoTo();
      n->clearSpeed();
      n->setTarget(nullptr);
      n->setExternalControl(true);
      n->setAnim(Npc::Anim::Idle);
      n->updateTransform();
      }
    const auto place = [&](const Vec3& p0, const Vec3& p1) {
      first.setPosition(p0);
      second.setPosition(p1);
      first.setDirection(p1-p0);
      second.setDirection(p0-p1);
      first.updateTransform();
      second.updateTransform();
      };
    std::string pointA = "native_arena", pointB = "native_arena";
    size_t attempts = 0;
    const float range = float(first.handle().senses_range);
    if(occluded) {
      // Both endpoints must be real ground waypoints, in range. This excludes a false result caused
      // only by FOV, LOD, distance or putting the source inside landscape geometry.
      std::vector<const WayPoint*> points;
      const Vec3 origin = first.position();
      w.findWayPoint(origin,[&](const WayPoint& wp) { points.push_back(&wp); return false; });
      std::sort(points.begin(),points.end(),[&](const WayPoint* x, const WayPoint* y) {
        const auto dx = (x->pos-origin).quadLength(), dy = (y->pos-origin).quadLength();
        return dx!=dy ? dx<dy : x->name<y->name;
        });
      bool found = false;
      for(const auto* p0:points) {
        if(found || attempts>=8192)
          break;
        const float y0 = groundY(w,p0->pos.x,p0->pos.z,p0->pos.y);
        if(!std::isfinite(y0) || std::abs(y0-p0->pos.y)>120.f)
          continue;
        for(const auto* p1:points) {
          const float distance = (p1->pos-p0->pos).length();
          if(distance<200.f || distance>std::min(range-250.f,2500.f))
            continue;
          const float y1 = groundY(w,p1->pos.x,p1->pos.z,p1->pos.y);
          if(!std::isfinite(y1) || std::abs(y1-p1->pos.y)>120.f)
            continue;
          ++attempts;
          place({p0->pos.x,y0,p0->pos.z},{p1->pos.x,y1,p1->pos.z});
          const auto head = first.mapHeadBone();
          const auto centerRay = w.physic()->ray(head,second.collosionCenter());
          const auto headRay = w.physic()->ray(head,second.mapHeadBone());
          if(first.qDistTo(second.collosionCenter())<=range*range && first.qDistTo(second.mapHeadBone())<=range*range &&
             centerRay.hasCol && centerRay.hitFraction>0.01f && centerRay.hitFraction<0.99f &&
             headRay.hasCol && headRay.hitFraction>0.01f && headRay.hitFraction<0.99f && !first.canSeeNpc(second,true)) {
            pointA = p0->name;
            pointB = p1->name;
            found = true;
            break;
            }
          if(attempts>=8192)
            break;
          }
        }
      if(!found) {
        fail("no genuine center/head occlusion found in bounded waypoint search");
        return true;
        }
      } else {
      place(first.position(),second.position());
      }
    const auto head = first.mapHeadBone(), targetHead = second.mapHeadBone(), targetCenter = second.collosionCenter();
    const auto centerRay = w.physic()->ray(head,targetCenter), headRay = w.physic()->ray(head,targetHead);
    const bool visible = first.canSeeNpc(second,true), fovVisible = first.canSeeNpc(second,false);
    const bool centerInRange = first.qDistTo(targetCenter)<=range*range, headInRange = first.qDistTo(targetHead)<=range*range;
    TestBench::bodyEvent(first,&second,"senses_vision",w.tickCount(),
                         "\"case\":"+jsonStr(mode)+",\"process_policy\":"+jsonStr(policyName(nativePolicy))+
                         ",\"can_see_npc_free_los\":"+(visible ? "true" : "false")+
                         ",\"can_see_npc_fov\":"+(fovVisible ? "true" : "false")+
                         ",\"center_ray_hit\":"+(centerRay.hasCol ? "true" : "false")+
                         ",\"head_ray_hit\":"+(headRay.hasCol ? "true" : "false")+
                         ",\"center_hit_fraction\":"+std::to_string(centerRay.hitFraction)+
                         ",\"head_hit_fraction\":"+std::to_string(headRay.hitFraction)+
                         ",\"center_in_range\":"+(centerInRange ? "true" : "false")+
                         ",\"head_in_range\":"+(headInRange ? "true" : "false")+
                         ",\"distance_cm\":"+std::to_string(std::sqrt(first.qDistTo(second)))+
                         ",\"senses_range_cm\":"+std::to_string(range)+",\"actor_head\":"+vec(head)+
                         ",\"target_head\":"+vec(targetHead)+",\"target_center\":"+vec(targetCenter)+
                         ",\"center_hit\":"+(centerRay.hasCol ? vec(centerRay.v) : "null")+
                         ",\"head_hit\":"+(headRay.hasCol ? vec(headRay.v) : "null")+
                         ",\"actor_waypoint\":"+jsonStr(pointA)+",\"target_waypoint\":"+jsonStr(pointB)+
                         ",\"search_attempts\":"+std::to_string(attempts));
    if(!centerInRange || !headInRange || visible==occluded || (!occluded && (centerRay.hasCol || headRay.hasCol))) {
      fail("native visibility predicate disagrees with real in-range collision rays");
      return true;
      }
    // Keep the listener close to these probe actors. WorldSound still executes its own range and
    // occlusion rules, using the loaded CPU camera; no rendering or listener device is involved.
    pl.setPosition(first.position());
    pl.updateTransform();
    Gothic::inst().gameSession()->camera().reset(&pl);
    w.sound()->tick(pl);
    const bool before = w.sound()->canSeeSource(head);
    if(before) {
      fail("an unrelated live 3D sound would mask this probe");
      return true;
      }
    context.probeSound = std::make_unique<::Sound>(w,::Sound::T_3D,"LEVELUP.WAV",targetHead,5000.f,false);
    context.probeSoundDuration = context.probeSound->effectPrefferedTime();
    if(context.probeSound->isEmpty() || context.probeSoundDuration==0 || context.probeSoundDuration>10000) {
      fail("LEVELUP.WAV is missing or has an invalid duration");
      return true;
      }
    context.probeSound->setLooping(false);
    context.probeSound->play();
    context.probeSoundStart = w.tickCount();
    context.probeOccluded = occluded;
    const bool finished = context.probeSound->isFinished(), sourceVisible = w.sound()->canSeeSource(head);
    TestBench::bodyEvent(first,&second,"senses_sound_start",w.tickCount(),
                         "\"case\":"+jsonStr(mode)+",\"asset\":\"LEVELUP.WAV\",\"duration_ms\":"+
                         std::to_string(context.probeSoundDuration)+",\"finished\":"+(finished ? "true" : "false")+
                         ",\"source_visible_before\":false,\"source_visible\":"+(sourceVisible ? "true" : "false")+
                         ",\"source\":"+vec(targetHead)+",\"listener\":"+vec(head)+
                         ",\"source_ray_hit\":"+(headRay.hasCol ? "true" : "false")+
                         ",\"clock\":"+jsonStr("simulation"));
    if(finished || sourceVisible==occluded) {
      fail("3D sound lifecycle or source visibility invariant failed");
      return true;
      }
    context.waitUntil = context.time+context.probeSoundDuration+34;
    say("bodyprobe "+mode+" verified; sound duration "+std::to_string(context.probeSoundDuration));
    }
  else return false;
  return true;
  }

}
