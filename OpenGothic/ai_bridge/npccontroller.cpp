#include "npccontroller.h"
#include "game/bodycontroller.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "game/gamescript.h"
#include "game/inventory.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "physics/dynamicworld.h"
#include "world/objects/npc.h"
#include "world/waypath.h"
#include "world/waypoint.h"
#include "world/world.h"
#include "commandline.h"
#include "gothic.h"

#include "jsonread.h"
#include "jsonwriter.h"
#include "npcchests.h"
#include "npcfind.h"
#include "npcview.h"
#include "textcodec.h"

namespace {

constexpr uint64_t GraceMs          = 100;   // let the npc pick the action from its queue
constexpr uint64_t SnapshotPeriodMs = 500;
constexpr uint64_t PercThrottleMs   = 1000;
constexpr float    NearbyRadius     = 2000;  // cm
constexpr size_t   NearbyCount      = 6;
constexpr float    NearbyNpcRadius  = 1500;
constexpr size_t   NearbyNpcCount   = 8;
constexpr size_t   InventoryLimit   = 40;
constexpr float    FollowNear       = 400;   // stop closing in
constexpr float    FollowCatchUp    = 800;   // farther than this it runs, whatever the leader does
constexpr float    FollowLost       = 6000;  // give up
constexpr uint64_t FollowStepMs     = 500;
constexpr uint64_t FollowStuckMs    = 2000;  // walking, but it has not moved FollowMovedCm in this long: stuck
constexpr uint64_t WalkStartMs      = 3000;  // arm stuck detection even if it cannot take its first step
constexpr float    FollowMovedCm    = 75;
constexpr float    WalkSameSpotCm   = 100;   // a replanned waynet route got stuck here again
constexpr uint64_t WalkProgressMs   = 5000;
constexpr float    TakeRange        = 2000;  // cm: take_item picks up the nearest such thing within this
constexpr float    ChestNear        = 300;   // cm: use_chest walks to the chest's waypoint from farther
constexpr uint64_t StateStartMs     = 3000;  // fight/flee: the script state must begin within this

std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

using JsonRead::str;
using JsonRead::integer;

Npc* findNpc(World& world, std::string_view name, const Npc* near = nullptr) {
  return AiNpc::findNpc(world,name,near);
  }

std::string npcId(World& world, Npc& npc) {
  if(npc.isPlayer())
    return "PLAYER";
  if(auto sym = world.script().findSymbol(npc.instanceSymbol()))
    return sym->name();
  return "?";
  }

std::string_view percName(PercType p) {
  static const char* names[PERC_Count] = {
    "NONE",
    "ASSESSPLAYER", "ASSESSENEMY", "ASSESSFIGHTER", "ASSESSBODY", "ASSESSITEM",
    "ASSESSMURDER", "ASSESSDEFEAT", "ASSESSDAMAGE", "ASSESSOTHERSDAMAGE", "ASSESSTHREAT",
    "ASSESSREMOVEWEAPON", "OBSERVEINTRUDER", "ASSESSFIGHTSOUND", "ASSESSQUIETSOUND", "ASSESSWARN",
    "CATCHTHIEF", "ASSESSTHEFT", "ASSESSCALL", "ASSESSTALK", "ASSESSGIVENITEM",
    "ASSESSFAKEGUILD", "MOVEMOB", "MOVENPC", "DRAWWEAPON", "OBSERVESUSPECT",
    "NPCCOMMAND", "ASSESSMAGIC", "ASSESSSTOPMAGIC", "ASSESSCASTER", "ASSESSSURPRISE",
    "ASSESSENTERROOM", "ASSESSUSEMOB",
    };
  if(p<PERC_Count)
    return names[p];
  return "UNKNOWN";
  }

std::string itemSymbol(World& world, size_t cls) {
  if(auto sym = world.script().findSymbol(cls))
    return sym->name();
  return "?";
  }

std::string itemName(const Inventory& inv, size_t cls) {
  for(auto it = inv.iterator(Inventory::T_Inventory); it.isValid(); ++it)
    if(it->clsId()==cls)
      return std::string(it->displayName());
  return {};
  }

std::string scriptString(World& world, std::string_view name, std::string_view fallback) {
  auto sym = world.script().findSymbol(name);
  if(sym!=nullptr && sym->type()==zenkit::DaedalusDataType::STRING)
    return sym->get_string();
  return std::string(fallback);
  }

// "pos": [x,y,z], "points": [names]: the waypoint nearest to pos, of these points if given (a place: its
// points and their middle); nullptr without pos
const WayPoint* nearestPoint(World& world, const rapidjson::Value& args) {
  auto pos = args.FindMember("pos");
  if(pos==args.MemberEnd() || !pos->value.IsArray() || pos->value.Size()!=3)
    return nullptr;
  for(auto& v:pos->value.GetArray())
    if(!v.IsNumber())
      return nullptr;
  const Tempest::Vec3 at(float(pos->value[0].GetDouble()),float(pos->value[1].GetDouble()),float(pos->value[2].GetDouble()));
  std::vector<std::string> names;
  auto points = args.FindMember("points");
  if(points!=args.MemberEnd() && points->value.IsArray())
    for(auto& v:points->value.GetArray())
      if(v.IsString())
        names.push_back(upper(std::string_view(v.GetString(),v.GetStringLength())));
  if(names.empty())
    return world.findWayPoint(at);
  const WayPoint* best = nullptr;
  float           dist = 0;
  for(auto& n:names) {
    auto wp = world.findPoint(n,false);
    if(wp==nullptr)
      continue;
    const float d = (wp->position()-at).quadLength();
    if(best==nullptr || d<dist) {
      best = wp;
      dist = d;
      }
    }
  return best;
  }

void writePos(JsonWriter& j, std::string_view key, const Tempest::Vec3& p) {
  j.array(key).item(double(p.x)).item(double(p.y)).item(double(p.z)).endArray();
  }

}

NpcController::NpcController(Emit emit):emit(std::move(emit)) {
  }

NpcController::Controlled* NpcController::find(std::string_view symbol) {
  const auto s = upper(symbol);
  for(auto& c:npcs)
    if(c.symbol==s)
      return &c;
  return nullptr;
  }

const NpcController::Controlled* NpcController::find(std::string_view symbol) const {
  const auto s = upper(symbol);
  for(auto& c:npcs)
    if(c.symbol==s)
      return &c;
  return nullptr;
  }

bool NpcController::isAttached(std::string_view symbol) const {
  return find(symbol)!=nullptr;
  }

Npc* NpcController::find(World& world, std::string_view symbol) {
  auto c = find(symbol);
  if(c==nullptr || world.findNpcByInstance(c->instance)!=c->npc)
    return nullptr;
  return c->npc;
  }

bool NpcController::attach(World& world, std::string_view symbolName, std::string& err) {
  const auto symbol = upper(symbolName);
  if(find(symbol)!=nullptr)
    return true;

  const size_t id  = world.script().findSymbolIndex(symbol);
  Npc*         npc = (id==size_t(-1)) ? nullptr : world.findNpcByInstance(id);
  if(npc==nullptr) {
    err = "npc not found in current world: " + symbol;
    return false;
    }
  if(npc->isPlayer()) {
    err = "cannot attach the player";
    return false;
    }
  if(BodyController::owns(*npc)) {
    BodyController::legacyRejected(*npc,"legacy_attach_rejected");
    err = "npc body is owned by a combat provider";
    return false;
    }
  if(npc->isDown()) {
    err = "npc is down";
    return false;
    }
  if(Gothic::inst().isNpcInDialog(*npc)) {
    err = "npc is in dialog";
    return false;
    }

  npc->clearAiQueue();
  npc->clearState(false);
  npc->setExternalControl(true);

  Controlled c;
  c.instance = id;
  c.symbol   = symbol;
  c.npc      = npc;
  npcs.push_back(std::move(c));

  emit(JsonWriter("attached")
         .str("npc",symbol)
         .str("name",TextCodec::toUtf8(npc->displayName()))
         .finish());
  emitSnapshot(world,npcs.back());
  return true;
  }

bool NpcController::detach(World& world, std::string_view symbol, std::string_view reason, std::string& err) {
  auto* c = find(symbol);
  if(c==nullptr) {
    err = "npc is not attached: " + upper(symbol);
    return false;
    }
  if(c->act)
    finishAction(*c,"cancelled","npc detached");
  // pointer may be stale if the npc was removed in between
  if(world.findNpcByInstance(c->instance)==c->npc)
    release(*c,true);
  emitDetached(c->symbol,reason);
  npcs.erase(npcs.begin()+(c-npcs.data()));
  return true;
  }

void NpcController::detachAll(World& world, std::string_view reason) {
  std::string err;
  while(!npcs.empty()) {
    auto symbol = npcs.back().symbol;
    detach(world,symbol,reason,err);
    }
  percThrottle.clear();
  }

void NpcController::dropAll(std::string_view reason) {
  for(auto& c:npcs) {
    c.npc = nullptr;  // the world is gone already: the pointer dangles
    if(c.act)
      finishAction(c,"cancelled","world unloaded");
    emitDetached(c.symbol,reason);
    }
  npcs.clear();
  percThrottle.clear();
  }

void NpcController::release(Controlled& c, bool resumeRoutine) {
  if(c.npc==nullptr)
    return;
  c.scripted = false;
  c.npc->setExternalControl(false);
  if(resumeRoutine) {
    c.npc->clearAiQueue();
    c.npc->resumeAiRoutine();
    }
  }

void NpcController::action(World& world, const rapidjson::Value& msg) {
  const auto id     = std::string(str(msg,"id"));
  const auto symbol = upper(str(msg,"npc"));
  const auto type   = std::string(str(msg,"action"));

  auto* c = find(symbol);
  if(c==nullptr) {
    emitActionResult(id,symbol,"rejected","npc is not attached",-1);
    return;
    }
  if(c->npc!=nullptr && BodyController::owns(*c->npc)) {
    BodyController::legacyRejected(*c->npc,"legacy_action_rejected");
    emitActionResult(id,symbol,"rejected","npc body is owned by a combat provider",-1);
    return;
    }

  static const rapidjson::Value emptyArgs(rapidjson::kObjectType);
  auto  argsIt = msg.FindMember("args");
  auto& args   = (argsIt!=msg.MemberEnd() && argsIt->value.IsObject()) ? argsIt->value : emptyArgs;

  if(c->act) {
    // queued speech survives: actions sent with a reply run after the npc has said it
    if(!c->npc->haveOutput())
      c->npc->clearAiQueue();
    finishAction(*c,"cancelled","preempted by " + id);
    }

  if(type=="follow") {
    Npc* target = findNpc(world,str(args,"npc").empty() ? "PLAYER" : str(args,"npc"));
    if(target==nullptr || target==c->npc) {
      emitActionResult(id,symbol,"rejected","unknown target npc",-1);
      return;
      }
    Follow f;
    f.toPlayer = target->isPlayer();
    f.target   = target->instanceSymbol();
    // a companion goes with him for as long as it means to (v22): the server stops it; timeout_ms is
    // for whoever wants a limit (Cavalorn gave up after 10 minutes, 2026-10-14)
    const int64_t limit = integer(args,"timeout_ms",0);
    f.until    = limit>0 ? world.tickCount() + uint64_t(limit) : 0;
    c->follow  = f;
    emitActionResult(id,symbol,"done","",-1);
    return;
    }

  if(type=="debug_add_item") {
    // test helper: put an item into the npc's inventory (only with -devmode)
    const auto   name = upper(str(args,"item"));
    const size_t cls  = world.script().findSymbolIndex(name);
    if(!CommandLine::inst().isDevMode() || cls==size_t(-1)) {
      emitActionResult(id,symbol,"rejected",cls==size_t(-1) ? "unknown item" : "needs -devmode",-1);
      return;
      }
    c->npc->addItem(cls,size_t(std::max<int64_t>(1,integer(args,"count",1))));
    c->nextSnapshot = 0;
    emitActionResult(id,symbol,"done","",-1);
    return;
    }

  if(type=="stop") {
    stopFollow(*c,"stopped");
    if(!c->npc->haveOutput())
      c->npc->clearAiQueue();
    emitActionResult(id,symbol,"done","",-1);
    return;
    }

  if(c->follow && type!="give_item" && type!="play_ani" && type!="turn_to" && type!="say_svm" &&
     type!="draw_weapon" && type!="remove_weapon")
    stopFollow(*c,"replaced");
  if(c->follow && (type=="draw_weapon" || type=="remove_weapon") && !c->npc->haveOutput()) {
    // an order in a fight ("К оружию!"): at once, not after the step towards the leader (which may be
    // stuck on the way); following pushes the next step once the weapon is out
    c->npc->clearAiQueue();
    c->npc->clearGoTo();
    }

  if(type=="return_to_routine") {
    emitActionResult(id,symbol,"done","",-1);
    std::string err;
    detach(world,symbol,"routine",err);
    return;
    }

  Action a;
  a.id    = id;
  a.type  = type;
  a.start = world.tickCount();

  std::string err;
  if(!startAction(world,*c,a,args,err)) {
    emitActionResult(id,symbol,"rejected",err,-1);
    return;
    }
  a.timeout  = uint64_t(integer(args,"timeout_ms",int64_t(a.deadline)));
  a.deadline = a.start + a.timeout;
  c->act = std::move(a);
  }

bool NpcController::startAction(World& world, Controlled& c, Action& a, const rapidjson::Value& args, std::string& err) {
  Npc& npc = *c.npc;

  // a.deadline temporarily holds the default timeout, ms
  if(a.type=="go_to_waypoint") {
    auto name = str(args,"waypoint");
    auto wp   = nearestPoint(world,args);
    if(wp==nullptr)
      wp = world.findPoint(name);
    if(wp==nullptr) {
      err = "unknown waypoint: " + std::string(name);
      return false;
      }
    a.point    = wp;
    a.radius   = float(integer(args,"radius",300));
    a.deadline = 60000;
    a.pace     = str(args,"pace");
    if(a.pace=="run")
      npc.setWalkMode(WalkBit::WM_Run); else
    if(a.pace=="walk")
      npc.setWalkMode(WalkBit::WM_Walk); else
    if(a.pace=="hero")
      keepHeroPace(world,npc);
    npc.aiPush(AiQueue::aiGoToPoint(*wp));
    return true;
    }

  if(a.type=="go_to_npc" || a.type=="turn_to") {
    auto name   = str(args,"npc");
    Npc* target = findNpc(world,name,&npc);
    if(target==nullptr || target==&npc) {
      err = "unknown target npc: " + std::string(name);
      return false;
      }
    a.toPlayer = target->isPlayer();
    a.target   = target->instanceSymbol();
    if(a.type=="go_to_npc") {
      a.radius    = float(integer(args,"radius",400));
      a.closeApproach = JsonRead::boolean(args,"close",false);
      a.deadline  = 60000;
      a.updatedAt = a.start;
      planNpcWalk(world,c,a,*target,!npc.canSeeNpc(*target,true));
      } else {
      a.deadline = 5000;
      npc.aiPush(AiQueue::aiTurnToNpc(target));
      }
    return true;
    }

  if(a.type=="play_ani") {
    auto name = upper(str(args,"name"));
    if(name.empty() || !npc.hasAnim(name)) {
      err = "unknown animation: " + name;
      return false;
      }
    const auto hold = integer(args,"hold_ms",0);
    if(hold<0 || hold>60000) {
      err = "play_ani hold_ms must be in [0, 60000]";
      return false;
      }
    a.animation = name;
    a.holdMs = uint64_t(hold);
    a.deadline = 15000+a.holdMs;
    npc.aiPush(AiQueue::aiPlayAnim(name));
    return true;
    }

  if(a.type=="wait") {
    auto ms = integer(args,"ms",1000);
    if(ms<=0 || ms>60000) {
      err = "wait ms must be in (0, 60000]";
      return false;
      }
    a.deadline = uint64_t(ms)+2000;
    npc.aiPush(AiQueue::aiWait(uint64_t(ms)));
    return true;
    }

  if(a.type=="give_item") {
    const auto   name = upper(str(args,"item"));
    const size_t cls  = world.script().findSymbolIndex(name);
    const size_t have = cls==size_t(-1) ? 0 : npc.inventory().itemCount(cls);
    if(have==0) {
      err = "npc has no item " + name;
      return false;
      }
    Npc* target = findNpc(world,str(args,"npc").empty() ? "PLAYER" : str(args,"npc"));
    if(target==nullptr || target==&npc) {
      err = "unknown target npc";
      return false;
      }
    a.item     = cls;
    a.count    = std::min(have,size_t(std::max<int64_t>(1,integer(args,"count",1))));
    a.toPlayer = target->isPlayer();
    a.target   = target->instanceSymbol();
    a.radius   = AiNpc::GiveRange;
    a.deadline = 30000;
    // walk up first if needed, the transfer happens once the queue is done
    if(npc.qDistTo(*target)>AiNpc::GiveRange*AiNpc::GiveRange)
      npc.aiPush(AiQueue::aiGoToNpc(target));
    npc.aiPush(AiQueue::aiTurnToNpc(target));
    return true;
    }

  if(a.type=="take_item") {
    // a thing lying about (v22): it walks up to the nearest of that kind and picks it up, as the scripts'
    // AI_GotoItem and AI_TakeItem do
    const auto   name = upper(str(args,"item"));
    const size_t cls  = world.script().findSymbolIndex(name);
    Item*        it   = nullptr;
    float        best = TakeRange*TakeRange;
    if(cls!=size_t(-1)) {
      world.detectItem(npc.position(),TakeRange,[&](Item& i) {
        const float d = (i.position()-npc.position()).quadLength();
        if(i.clsId()==cls && d<best) {
          it   = &i;
          best = d;
          }
        });
      }
    if(it==nullptr) {
      err = "no " + name + " lying nearby";
      return false;
      }
    a.item     = cls;
    a.count    = npc.inventory().itemCount(cls);  // what it had: it took the thing once it has more
    a.deadline = 30000;
    npc.aiPush(AiQueue::aiGotoItem(*it));
    npc.aiPush(AiQueue::aiTakeItem(*it));
    return true;
    }

  if(a.type=="use_chest") {
    // v28: it walks up to its chest, opens it as the scripts' AI_UseMob does, takes out and puts in, closes it
    Interactive* chest = NpcChests::find(world,str(args,"chest"));
    if(chest==nullptr) {
      err = "no chest " + std::string(str(args,"chest"));
      return false;
      }
    if(NpcChests::whose(world,npc,*chest).empty()) {
      err = "not its chest";
      return false;
      }
    auto moves = [&](std::string_view key, std::vector<std::pair<size_t,size_t>>& out) {
      if(!args.HasMember(key.data()) || !args[key.data()].IsArray())
        return true;
      for(auto& m:args[key.data()].GetArray()) {
        const auto   name = upper(str(m,"item"));
        const size_t cls  = world.script().findSymbolIndex(name);
        if(cls==size_t(-1)) {
          err = "unknown item " + name;
          return false;
          }
        out.emplace_back(cls,size_t(std::max<int64_t>(1,integer(m,"count",1))));
        }
      return true;
      };
    if(!moves("take",a.take) || !moves("put",a.put))
      return false;
    for(auto& [cls,count]:a.put)
      if(npc.inventory().itemCount(cls)==0) {
        err = "npc has no item " + itemSymbol(world,cls);
        return false;
        }
    a.chest    = chest;
    a.deadline = 60000;
    // its owner opens it as if it had the key (the lock stays for the hero)
    if(chest->needToLockpick(npc)) {
      chest->setAsCracked(true);
      a.relock = true;
      }
    // AI_UseMob takes the nearest chest: it goes up to this one first
    if(npc.qDistTo(chest->position())>ChestNear*ChestNear)
      if(auto wp = world.findWayPoint(chest->position()))
        npc.aiPush(AiQueue::aiGoToPoint(*wp));
    npc.aiPush(AiQueue::aiUseMob(chest->schemeName(),1));
    return true;
    }

  if(a.type=="fight")
    return startScriptState(world,c,a,"ZS_ATTACK",args,err);
  if(a.type=="flee")
    return startScriptState(world,c,a,"ZS_FLEE",args,err);

  if(a.type=="draw_weapon") {
    const auto kind = str(args,"kind");
    a.deadline = 5000;
    npc.aiPush(kind=="ranged" ? AiQueue::aiReadyRangedWeapon()
                              : (kind=="melee" ? AiQueue::aiReadyMeleeWeapon() : AiQueue::aiDrawWeapon()));
    return true;
    }

  if(a.type=="remove_weapon") {
    a.deadline = 5000;
    npc.aiPush(AiQueue::aiRemoveWeapon());
    return true;
    }

  if(a.type=="say_svm") {
    // the npc's own recorded line ("$NOTNOW"), in its voice; overlay: said on the move, not waited for
    std::string svm = upper(str(args,"svm"));
    Npc*        to  = findNpc(world,str(args,"npc").empty() ? "PLAYER" : str(args,"npc"),&npc);
    if(svm.empty() || to==nullptr) {
      err = svm.empty() ? "say_svm needs svm" : "unknown target npc";
      return false;
      }
    if(svm[0]!='$')
      svm.insert(svm.begin(),'$');
    a.deadline = 10000;
    if(JsonRead::boolean(args,"overlay",false)) {
      npc.aiPush(AiQueue::aiOutputSvmOverlay(*to,svm,0));
      } else {
      npc.aiPush(AiQueue::aiTurnToNpc(to));
      npc.aiPush(AiQueue::aiOutputSvm(*to,svm,0));
      }
    return true;
    }

  err = "unknown action: " + a.type;
  return false;
  }

bool NpcController::startScriptState(World& world, Controlled& c, Action& a, std::string_view state,
                                     const rapidjson::Value& args, std::string& err) {
  // the game's own fight/flee behaviour: the npc is handed to the script state for as long as it runs
  // (states do not run under external control), then taken back
  Npc&       npc    = *c.npc;
  const auto fn     = AiNpc::scriptState(world,state);
  Npc*       target = findNpc(world,str(args,"npc"),&npc);
  if(!fn.isValid()) {
    err = "the scripts have no " + std::string(state);
    return false;
    }
  if(target==nullptr || target==&npc || target->isDead()) {
    err = "unknown target npc: " + std::string(str(args,"npc"));
    return false;
    }
  a.state     = fn;
  a.stateStart = a.start;
  a.toPlayer  = target->isPlayer();
  a.target    = target->instanceSymbol();
  a.targetNpc = target;
  a.deadline = a.type=="fight" ? 60000 : 20000;
  // Keep perceptions external while queued speech and weapon removal finish.
  // Release script control only once the queue has installed the requested state.
  npc.setTarget(target);
  npc.aiPush(AiQueue::aiStartState(fn,0,target,nullptr,""));
  c.scripted = true;
  return true;
  }

void NpcController::updateStateAction(World& world, Controlled& c, uint64_t now) {
  auto& a  = *c.act;
  Npc&  npc = *c.npc;
  const bool in = npc.isInState(a.state);
  if(in && npc.isExternalControl())
    npc.setExternalControl(false);
  if(!a.entered) {
    if(in && npc.isStateStarted()) {
      a.entered = true;
      } else {
      if(npc.haveOutput()) {
        // Queued speech goes first, just as for the other body actions.
        a.stateStart = now;
        a.deadline = now + a.timeout;
        }
      if(now>a.stateStart+StateStartMs)
        finishAction(c,"failed","the npc did not start to " + a.type);
      return;
      }
    }
  if(in) {
    if(now>=a.deadline)
      finishAction(c,"timeout","");
    return;
    }
  // the very npc it fought (the pointer is only compared, never followed, unless still in the world)
  Npc* target = nullptr;
  world.detectNpc(npc.position(),6000,[&](Npc& n) {
    if(&n==a.targetNpc)
      target = &n;
    });
  std::string_view detail;
  if(target==nullptr)
    detail = "target is gone"; else
  if(target->isDead() || target->isDown())
    detail = "target is down";
  finishAction(c,"done",detail);
  }

void NpcController::keepHeroPace(World& world, Npc& npc) {
  // a guide keeps the hero's pace (v38): runs while he runs, walks while he walks, sneaks while he sneaks
  const Npc* hero = world.player();
  if(hero==nullptr)
    return;
  const auto pace = hero->walkMode() & (WalkBit::WM_Walk | WalkBit::WM_Sneak);
  if((npc.walkMode() & (WalkBit::WM_Walk | WalkBit::WM_Sneak))!=pace)
    npc.setWalkMode(pace);
  }

void NpcController::regainControl(Controlled& c) {
  if(!c.scripted || c.npc==nullptr)
    return;
  c.scripted = false;
  Npc& npc = *c.npc;
  npc.clearAiQueue();     // the state's fight moves too
  npc.clearState(true);
  npc.setTarget(nullptr);
  npc.setExternalControl(true);
  if(npc.weaponState()!=WeaponState::NoWeapon)
    npc.aiPush(AiQueue::aiRemoveWeapon());
  }

void NpcController::planNpcWalk(World& world, Controlled& c, Action& a, Npc& target, bool byWay) {
  const WayPoint* wp = nullptr;
  if(byWay) {
    wp = world.findWayPoint(target.position(),[&](const WayPoint& w) {
      return target.canRayHitPoint(w.position(),true);
      });
    if(wp==nullptr)
      wp = world.findWayPoint(target.position());
    }
  Npc& npc = *c.npc;
  a.byWay     = wp!=nullptr;
  a.movedFrom = npc.position();
  a.plannedFrom = a.movedFrom;
  a.movedAt   = world.tickCount();
  a.distance  = std::sqrt(double(npc.qDistTo(target)));
  if(a.replans>0)
    Tempest::Log::i("[ai] ",c.symbol," go_to_npc ",a.id," replan ",a.replans," at [",
                    a.movedFrom.x,",",a.movedFrom.y,",",a.movedFrom.z,"], ",a.distance/100.," m to ",npcId(world,target));
  const auto seen = npc.canSeeNpc(target,true) ? "visible" : "not visible";
  const auto step = a.replans==0 ? " start to " : " route to ";
  if(wp!=nullptr) {
    auto path = world.wayTo(npc,*wp);
    auto from = npc.position();
    double length = 0;
    size_t count  = 0;
    while(const auto p = path.pop()) {
      length += double((p->position()-from).length());
      from = p->position();
      ++count;
      }
    Tempest::Log::i("[ai] ",c.symbol," go_to_npc ",a.id,step,npcId(world,target),": waynet to ",wp->name,
                    ": ",count," points, ",length/100.," m path; direct ",a.distance/100.," m, ",seen);
    npc.aiPush(AiQueue::aiGoToPoint(*wp));
    } else {
    Tempest::Log::i("[ai] ",c.symbol," go_to_npc ",a.id,step,npcId(world,target),": direct, ",a.distance/100.," m, ",seen);
    }
  if(a.closeApproach)
    npc.aiPush(AiQueue::aiGoToNpc(&target,int(a.radius))); else
    npc.aiPush(AiQueue::aiGoToNpc(&target));
  }

void NpcController::updateAction(World& world, Controlled& c, uint64_t now) {
  auto& a = *c.act;
  a.updatedAt = now;
  if(a.holdMs==0 && now<a.start+GraceMs)
    return;
  if(a.state.isValid()) {
    updateStateAction(world,c,now);
    return;
    }

  if(c.npc->haveOutput())
    a.deadline = now + a.timeout; // queued speech goes first, the clock starts after it
  if(a.holdMs>0) {
    if(c.npc->animProgress(a.animation)>=0.f) {
      if(a.heldAt==0)
        a.heldAt = now;
      if(now>=a.heldAt+a.holdMs) {
        c.npc->clearAiQueue();
        finishAction(c,"done","");
        }
      }
    else if(a.heldAt!=0) {
      finishAction(c,"failed","held animation was interrupted");
      }
    else if(now>=a.deadline) {
      c.npc->clearAiQueue();
      finishAction(c,"timeout","animation did not start");
      }
    return;
    }
  if(a.pace=="hero")
    keepHeroPace(world,*c.npc);
  if(a.type=="go_to_npc") {
    Npc* target = a.toPlayer ? world.player() : world.findNpcByInstance(a.target);
    if(target!=nullptr) {
      a.distance = std::sqrt(double(c.npc->qDistTo(*target)));
      const auto pos = c.npc->position();
      const bool seen = c.npc->canSeeNpc(*target,true);
      if(now>=a.nextProgress) {
        const auto wp = c.npc->currentGoToPoint();
        char progress[128];
        std::snprintf(progress,sizeof(progress)," progress t=%.1fs pos=[%.0f,%.0f,%.0f] d=%.1fm moved=%.0fcm idle=%.1fs",
                      double(now-a.start)/1000.,double(pos.x),double(pos.y),double(pos.z),a.distance/100.,
                      double((pos-a.movedFrom).length()),double(now-a.movedAt)/1000.);
        Tempest::Log::i("[ai] ",c.symbol," go_to_npc ",a.id,progress," point=",wp!=nullptr ? wp->name : "-",
                        " walk=",uint32_t(c.npc->walkMode())," body=",uint32_t(c.npc->bodyStateMasked()),
                        " busy=",int(c.npc->isAiBusy())," queue=",c.npc->aiQueueSize(),
                        " los=",int(seen));
        a.nextProgress = now + WalkProgressMs;
        }
      // Allow initial item animations and turning a short grace; ignore vertical settling too.
      const auto moved = pos-a.movedFrom;
      const bool startClock = !a.walkStarted &&
                              (moved.x*moved.x+moved.z*moved.z>1.f || now>=a.start+WalkStartMs);
      if(startClock)
        a.walkStarted = true;
      if(c.npc->haveOutput() || startClock || a.movedAt==0 || moved.quadLength()>=FollowMovedCm*FollowMovedCm) {
        a.movedFrom = pos;
        a.movedAt   = now;
        }
      if(c.npc->isAiBusy() && !c.npc->haveOutput()) {
        // Stop close enough to see the target, never on the other side of a wall.
        if(a.distance<=a.radius && seen) {
          c.npc->clearAiQueue();
          c.npc->clearGoTo();
          } else if(a.walkStarted && now>=a.movedAt+FollowStuckMs) {
          const bool repeated = a.byWay && a.replans>0 &&
                                (pos-a.plannedFrom).quadLength()<=WalkSameSpotCm*WalkSameSpotCm;
          c.npc->clearAiQueue();
          c.npc->clearGoTo();
          ++a.replans;
          planNpcWalk(world,c,a,*target,!seen && !repeated);
          }
        }
      }
    }
  if(c.npc->isAiBusy()) {
    if(now>=a.deadline) {
      c.npc->clearAiQueue();
      finishAction(c,"timeout","");
      }
    return;
    }

  if(a.point!=nullptr) {
    const double d = std::sqrt(double(c.npc->qDistTo(a.point)));
    if(d<=a.radius)
      finishAction(c,"done","",d); else
      finishAction(c,"failed","stopped away from waypoint",d);
    return;
    }

  if(a.type=="use_chest") {
    useChest(world,c,a);
    return;
    }

  if(a.type=="take_item") {
    if(c.npc->inventory().itemCount(a.item)>a.count)
      finishAction(c,"done",""); else
      finishAction(c,"failed","it is not in its hands: gone, or out of reach");
    return;
    }

  if(a.type=="give_item") {
    std::string err;
    if(giveItem(world,c,a,err))
      finishAction(c,"done",""); else
      finishAction(c,"failed",err);
    return;
    }

  if(a.type=="go_to_npc") {
    Npc* target = a.toPlayer ? world.player() : world.findNpcByInstance(a.target);
    if(target==nullptr) {
      finishAction(c,"failed","target npc is gone");
      return;
      }
    const double d = std::sqrt(double(c.npc->qDistTo(*target)));
    if(d<=a.radius && (!a.closeApproach || c.npc->canSeeNpc(*target,true)))
      finishAction(c,"done","",d); else
      finishAction(c,"failed","stopped away from target",d);
    return;
    }

  finishAction(c,"done","");
  }

bool NpcController::giveItem(World& world, Controlled& c, const Action& a, std::string& err) {
  Npc* target = a.toPlayer ? world.player() : world.findNpcByInstance(a.target);
  if(target==nullptr) {
    err = "target npc is gone";
    return false;
    }
  // what it gives, read before it is gone from the giver's inventory
  const size_t count = std::min(c.npc->inventory().itemCount(a.item),a.count);
  const auto   name  = itemName(c.npc->inventory(),a.item);
  if(!AiNpc::giveItem(*c.npc,*target,a.item,a.count,err))
    return false;

  if(target->isPlayer()) {
    std::string msg = name + ": ";
    msg += count==1 ? scriptString(world,"PRINT_ITEMERHALTEN","\xef\xf0\xe5\xe4\xec\xe5\xf2 \xef\xee\xeb\xf3\xf7\xe5\xed")
                    : std::to_string(count) + " " + scriptString(world,"PRINT_ITEMSERHALTEN","items");
    Gothic::inst().onPrint(msg);
    }
  return true;
  }

void NpcController::updateFollow(World& world, Controlled& c, uint64_t now) {
  auto& f      = *c.follow;
  Npc*  target = f.toPlayer ? world.player() : world.findNpcByInstance(f.target);
  if(target==nullptr || target->isDead()) {
    stopFollow(c,"target_gone");
    return;
    }
  if(target->isDown())
    return;  // knocked out, he gets up again: it stays by him (a companion fights whoever beat him, server)
  const float d = std::sqrt(c.npc->qDistTo(*target));
  if(d>FollowLost) {
    stopFollow(c,"target_too_far");
    return;
    }
  if(f.until!=0 && now>=f.until) {
    stopFollow(c,"timeout");
    return;
    }
  // keeps the leader's pace on the way, not only when it sets off: walks while he walks, runs while he
  // runs, sneaks after him; left behind, it runs to catch up
  const auto pace = d>FollowCatchUp ? WalkBit::WM_Run
                                    : target->walkMode() & (WalkBit::WM_Walk | WalkBit::WM_Sneak);
  if((c.npc->walkMode() & (WalkBit::WM_Walk | WalkBit::WM_Sneak))!=pace)
    c.npc->setWalkMode(pace);
  // a walk to an npc goes straight at him: past a cave's bend or a tower's wall it runs into the rock and
  // stays there. Got nowhere for a while: by the waynet, to the point nearest him that he can see from where
  // he is (as the engine's own fights do), then straight at him again
  const auto pos = c.npc->position();
  if(f.movedAt==0 || (pos-f.movedFrom).quadLength()>=FollowMovedCm*FollowMovedCm) {
    f.movedFrom = pos;
    f.movedAt   = now;
    }
  if(c.npc->isAiBusy() && !c.npc->haveOutput() && d>FollowNear && now>=f.movedAt+FollowStuckMs) {
    c.npc->clearAiQueue();
    c.npc->clearGoTo();
    f.movedAt = now;
    if(!f.byWay) {
      auto wp = world.findWayPoint(target->position(),[&](const WayPoint& w) {
        return target->canRayHitPoint(w.position(),true);
        });
      if(wp!=nullptr) {
        f.byWay = true;
        c.npc->aiPush(AiQueue::aiGoToPoint(*wp));
        return;
        }
      }
    f.byWay = false;  // stuck on the waynet too: straight again
    }
  if(d<=FollowNear || (now<f.nextStep && f.nextStep<=now+FollowStepMs) || c.npc->isAiBusy())
    return;
  f.nextStep = now + FollowStepMs;
  f.byWay    = false;
  c.npc->aiPush(AiQueue::aiGoToNpc(target));
  }

void NpcController::stopFollow(Controlled& c, std::string_view reason) {
  if(!c.follow)
    return;
  c.follow.reset();
  if(!c.npc->haveOutput())
    c.npc->clearAiQueue();
  c.npc->setWalkMode(WalkBit::WM_Run);
  emit(JsonWriter("follow_ended").str("npc",c.symbol).str("reason",reason).finish());
  if(reason!="replaced" && reason!="stopped" && onFollowEnded)
    onFollowEnded(c.symbol,reason);
  }

void NpcController::useChest(World& world, Controlled& c, Action& a) {
  Npc& npc = *c.npc;
  if(a.opened) {  // closed again
    finishAction(c,"done",a.moved);
    return;
    }
  if(npc.interactive()!=a.chest) {
    finishAction(c,"failed",npc.interactive()!=nullptr ? "opened another chest" : "could not open the chest");
    return;
    }
  auto list = [&](const std::vector<std::pair<size_t,size_t>>& moved) {
    std::string out;
    for(auto& [cls,count]:moved)
      out += (out.empty() ? "" : ",") + itemSymbol(world,cls) + ":" + std::to_string(count);
    return out;
    };
  std::vector<std::pair<size_t,size_t>> took, put;
  auto& inv = const_cast<Inventory&>(npc.inventory());  // as the scripts' own transfers do
  for(auto& [cls,want]:a.take) {
    const size_t n = std::min(want,a.chest->inventory().itemCount(cls));
    if(n>0) {
      Inventory::transfer(inv,a.chest->inventory(),nullptr,cls,n,world);
      took.emplace_back(cls,n);
      }
    }
  for(auto& [cls,want]:a.put) {
    const size_t n = std::min(want,npc.inventory().itemCount(cls));
    if(n>0) {
      Inventory::transfer(a.chest->inventory(),inv,&npc,cls,n,world);
      put.emplace_back(cls,n);
      }
    }
  a.moved  = "took=" + list(took) + ";put=" + list(put);
  a.opened = true;
  Tempest::Log::i("[ai] ",c.symbol," chest ",NpcChests::id(*a.chest),": ",a.moved);
  npc.aiPush(AiQueue::aiUseMob(a.chest->schemeName(),-1));
  }

void NpcController::finishAction(Controlled& c, std::string_view status, std::string_view detail, double distance) {
  if(!c.act)
    return;
  if(c.act->type=="go_to_npc") {
    const auto& a = *c.act;
    const double d = distance>=0 ? distance : a.distance;
    Tempest::Log::i("[ai] ",c.symbol," go_to_npc ",a.id," finish status=",status," detail=",detail,
                    " distance=",d/100.," m elapsed=",double(a.updatedAt-a.start)/1000.," s replans=",a.replans);
    }
  if(c.act->type=="use_chest" && c.act->chest!=nullptr) {
    if(c.npc!=nullptr && c.npc->interactive()==c.act->chest && status!="done")
      c.npc->setInteraction(nullptr,true);  // cut off at the open chest: it lets go of it
    if(c.act->relock)
      c.act->chest->setAsCracked(false);
    }
  regainControl(c);
  emitActionResult(c.act->id,c.symbol,status,detail,distance);
  c.act.reset();
  c.nextSnapshot = 0;
  }

bool NpcController::requestSnapshot(World& world, std::string_view symbol) {
  if(auto c = find(symbol)) {
    c->nextSnapshot = 0;
    return true;
    }
  Npc* npc = AiNpc::findNpc(world,symbol);
  if(npc==nullptr || npc->isPlayer())
    return false;
  emitSnapshot(world,*npc,AiNpc::upper(symbol),"",false);
  return true;
  }

void NpcController::tick(World& world) {
  const uint64_t now = world.tickCount();
  for(size_t i=0; i<npcs.size();) {
    auto& c   = npcs[i];
    Npc*  npc = world.findNpcByInstance(c.instance);
    if(npc==nullptr || npc!=c.npc) {
      if(c.act)
        finishAction(c,"cancelled","npc is gone");
      emitDetached(c.symbol,"lost");
      npcs.erase(npcs.begin()+ptrdiff_t(i));
      continue;
      }
    if(npc->isDown()) {
      // death/unconscious states are script-driven: hand the npc back without resuming routine
      if(c.act)
        finishAction(c,"failed","npc is down");
      release(c,false);
      emitDetached(c.symbol,"down");
      npcs.erase(npcs.begin()+ptrdiff_t(i));
      continue;
      }

    if(c.act)
      updateAction(world,c,now);
    else if(c.follow)
      updateFollow(world,c,now);
    if(now>=c.nextSnapshot || c.nextSnapshot>now+SnapshotPeriodMs) {
      emitSnapshot(world,c);
      c.nextSnapshot = now + SnapshotPeriodMs;
      }
    ++i;
    }
  }

void NpcController::perception(World& world, Npc& self, Npc& other, Npc* victim, PercType perc) {
  if(&other==&self)
    return; // e.g. MOVEMOB caused by the npc's own interaction
  auto it = std::find_if(npcs.begin(),npcs.end(),[&](const Controlled& c){ return c.npc==&self; });
  if(it==npcs.end())
    return;

  const uint64_t now = world.tickCount();
  const uint64_t key = (uint64_t(it->instance)<<40) ^ (uint64_t(other.instanceSymbol())<<8) ^ uint64_t(perc);
  auto& last = percThrottle[key];
  if(last!=0 && last<=now && now<last+PercThrottleMs)
    return;
  last = now;

  JsonWriter j("perception");
  j.str("npc",it->symbol)
   .str("perc",percName(perc))
   .str("other",npcId(world,other))
   .num("distance",std::sqrt(double(self.qDistTo(other))));
  if(victim!=nullptr)
    j.str("victim",npcId(world,*victim));
  emit(j.finish());
  }

std::string NpcController::status(World* world) const {
  if(npcs.empty())
    return "no attached npc";
  std::string s;
  for(auto& c:npcs) {
    if(!s.empty())
      s += "; ";
    s += c.symbol;
    if(c.act)
      s += " [" + c.act->type + " " + c.act->id + "]"; else
      s += " [idle]";
    if(world!=nullptr && c.npc!=nullptr && world->findNpcByInstance(c.instance)==c.npc) {
      if(auto wp = c.npc->currentWayPoint())
        s += " at " + wp->name;
      }
    }
  return s;
  }

void NpcController::emitDetached(std::string_view symbol, std::string_view reason) {
  emit(JsonWriter("detached").str("npc",symbol).str("reason",reason).finish());
  if(onDetached)
    onDetached(std::string(symbol),reason);
  }

void NpcController::emitActionResult(std::string_view id, std::string_view npc, std::string_view status,
                                     std::string_view detail, double distance) {
  JsonWriter j("action_result");
  j.str("id",id).str("npc",npc).str("status",status);
  if(!detail.empty())
    j.str("detail",detail);
  if(distance>=0)
    j.num("distance",distance);
  emit(j.finish());
  }

void NpcController::emitSnapshot(World& world, Controlled& c) {
  emitSnapshot(world,*c.npc,c.symbol,c.act ? std::string_view(c.act->type) : std::string_view(),c.follow.has_value());
  }

void NpcController::emitSnapshot(World& world, Npc& npc, std::string_view symbol, std::string_view action,
                                 bool following) {
  const auto pos  = npc.position();
  const auto time = world.time();

  JsonWriter j("snapshot");
  j.integer("t",int64_t(world.tickCount()));

  j.object("world")
     .str("name",world.name())
     .integer("day",time.day())
     .integer("hour",time.hour())
     .integer("minute",time.minute())
   .end();

  j.object("npc")
     .str("id",symbol)
     .str("name",TextCodec::toUtf8(npc.displayName()));
  writePos(j,"pos",pos);
  j.num("rot",double(npc.rotation()))
   .integer("hp",npc.attribute(ATR_HITPOINTS))
   .integer("hp_max",npc.attribute(ATR_HITPOINTSMAX))
   .boolean("busy",npc.isAiBusy())
   .str("animations",npc.animNames());
  if(!action.empty())
    j.str("action",action); else
    j.null("action");
  j.boolean("following",following);
  // v27: whom its eyes are on: the one it talks to, a script's look, its attention (a point: nobody)
  Tempest::Vec3 at;
  const auto    gaze = npc.gazeTarget(at);
  Npc*          who  = gaze==Gaze::Source::Talk      ? npc.talkPartner()     :
                       gaze==Gaze::Source::Script    ? npc.lookAtTarget()    :
                       gaze==Gaze::Source::Attention ? npc.attentionTarget() : nullptr;
  if(who!=nullptr)
    j.str("gaze",npcId(world,*who)); else
    j.null("gaze");
  if(auto wp = npc.currentWayPoint())
    j.str("waypoint",wp->name); else
    j.null("waypoint");
  if(auto wp = world.findWayPoint(pos))
    j.str("nearest_waypoint",wp->name); else
    j.null("nearest_waypoint");
  j.array("inventory");
  size_t n = 0;
  for(auto it = npc.inventory().iterator(Inventory::T_Inventory); it.isValid() && n<InventoryLimit; ++it, ++n) {
    j.object()
       .str("item",itemSymbol(world,it->clsId()))
       .str("name",TextCodec::toUtf8(it->displayName()))
       .integer("count",int64_t(it.count()))
       .boolean("equipped",it.isEquipped());
    if(it->isMission())
      j.boolean("mission",true);  // v39: a mission item (ITEM_MISSION) never changes hands between npc's
    j.end();
    }
  j.endArray();
  j.end();

  if(auto pl = world.player()) {
    auto& inv = const_cast<Inventory&>(pl->inventory());
    auto  equipment = [&](std::string_view key, Item* it) {
      if(it!=nullptr)
        j.str(key,TextCodec::toUtf8(it->displayName())); else
        j.null(key);
      };
    j.object("player")
       .str("name",TextCodec::toUtf8(pl->displayName()))
       .str("animations",pl->animNames())
       .boolean("kneeling",pl->isPlayerKneeling());
    writePos(j,"pos",pl->position());
    // v32: where he faces, for where places are from him (places.py)
    const auto ax = heroAxes(*pl);
    j.array("ahead").item(double(ax.aheadX)).item(double(ax.aheadZ)).endArray();
    j.array("right").item(double(ax.rightX)).item(double(ax.rightZ)).endArray();
    j.num("distance",std::sqrt(double(npc.qDistTo(*pl))))
     .integer("guild",int64_t(pl->guild()))
     .integer("hp",pl->attribute(ATR_HITPOINTS))
     .integer("hp_max",pl->attribute(ATR_HITPOINTSMAX));
    equipment("armor", inv.currentArmor());
    equipment("melee", inv.currentMeleeWeapon());
    equipment("ranged",inv.currentRangedWeapon());
    j.end();
    }

  struct Near { Npc* npc; float dist; };
  std::vector<Near> near;
  world.detectNpc(pos,NearbyNpcRadius,[&](Npc& other) {
    if(&other!=&npc && !other.isPlayer())
      near.push_back({&other,std::sqrt(npc.qDistTo(other))});
    });
  std::sort(near.begin(),near.end(),[](const Near& a, const Near& b){ return a.dist<b.dist; });
  if(near.size()>NearbyNpcCount)
    near.resize(NearbyNpcCount);
  j.array("nearby_npcs");
  for(auto& o:near) {
    j.object()
       .str("id",npcId(world,*o.npc))
       .str("name",TextCodec::toUtf8(o.npc->displayName()))
       .num("distance",double(o.dist))
       .integer("guild",int64_t(o.npc->guild()))
       .boolean("dead",o.npc->isDead())
     .end();
    }
  j.endArray();

  std::vector<const WayPoint*> nearby;
  j.array("nearby_waypoints");
  for(size_t i=0; i<NearbyCount; ++i) {
    auto wp = world.findWayPoint(pos,[&](const WayPoint& w) {
      if(std::find(nearby.begin(),nearby.end(),&w)!=nearby.end())
        return false;
      return (w.position()-pos).quadLength() < NearbyRadius*NearbyRadius;
      });
    if(wp==nullptr)
      break;
    nearby.push_back(wp);
    j.object()
       .str("name",wp->name)
       .num("distance",std::sqrt(double((wp->position()-pos).quadLength())))
     .end();
    }
  j.endArray();

  emit(j.finish());
  }
