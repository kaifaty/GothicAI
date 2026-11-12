#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>

#include "camera.h"
#include "commandline.h"
#include "game/bodyobservations.h"
#include "game/inventory.h"
#include "jsonread.h"
#include "jsonwriter.h"
#include "npcchests.h"
#include "npcfind.h"
#include "physics/dynamicworld.h"
#include "world/benchhooks.h"
#include "textcodec.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/waypoint.h"

#include "aibridge_impl.h"

using namespace Tempest;

void AiBridge::Impl::onDebug(const rapidjson::Value& msg) {
  const auto req = JsonRead::str(msg,"req");
  if(!CommandLine::inst().isDevMode()) {
    sendResult(req,false,"debug commands need -devmode");
    return;
    }
  const auto cmd = JsonRead::str(msg,"cmd");
  if(cmd=="spawn" || cmd=="player_weapon" || cmd=="player_equip" || cmd=="waypoints" || cmd=="player_goto" || cmd=="ptt" || cmd=="type" || cmd=="kill" ||
     cmd=="time" || cmd=="teleport" || cmd=="skip_important" || cmd=="deal" || cmd=="deal_accept" ||
     cmd=="deal_decline" || cmd=="npc_goto" || cmd=="npc_los" || cmd=="player_hp" || cmd=="player_drop" || cmd=="chest_take" || cmd=="npc_drop" || cmd=="player_hit" || cmd=="npc_hit" || cmd=="spawn_item" ||
     cmd=="player_take" || cmd=="own_npc" || cmd=="physics_noise" || cmd=="physics_traces" || cmd=="destruction_traces" ||
     cmd=="blood") {
    // world changes that are safe within the tick
    runWorldDebug(msg);
    return;
    }
  if(!debugReq.empty()) {
    sendResult(req,false,"another debug command is pending");
    return;
    }
  debugReq  = std::string(req);
  debugCmd  = std::string(JsonRead::str(msg,"cmd"));
  debugSlot = "save_slot_" + std::to_string(JsonRead::integer(msg,"slot",0)) + ".sav";
  debugTimer.start(1);
  }

void AiBridge::Impl::runDebug() {
  debugTimer.stop();
  const auto req  = std::move(debugReq);
  const auto cmd  = std::move(debugCmd);
  const auto slot = std::move(debugSlot);
  debugReq.clear();
  auto& gothic = Gothic::inst();
  if(gothic.checkLoading()!=Gothic::LoadState::Idle) {
    sendResult(req,false,"busy loading or saving");
    return;
    }
  if(cmd=="save") {
    // the game refuses to save during dialogs and cutscenes
    if(gothic.isInDialog() || (gothic.world()!=nullptr && gothic.world()->currentCs()!=nullptr)) {
      sendResult(req,false,"cannot save now (dialog or cutscene)");
      return;
      }
    gothic.save(slot,"AI test");
    sendResult(req,gothic.checkLoading()==Gothic::LoadState::Saving,"save did not start");
    }
  else if(cmd=="load") {
    gothic.load(slot);
    sendResult(req,true,"");
    }
  else {
    sendResult(req,false,"unknown debug command: " + cmd);
    }
  }

void AiBridge::Impl::runWorldDebug(const rapidjson::Value& msg) {
  const auto req   = JsonRead::str(msg,"req");
  const auto cmd   = JsonRead::str(msg,"cmd");
  World*     world = Gothic::inst().world();
  Npc*       pl    = world!=nullptr ? world->player() : nullptr;
  if(world==nullptr || pl==nullptr) {
    sendResult(req,false,"no world loaded");
    return;
    }
  if(cmd=="npc_los") {
    // Read-only scene preparation: the observer's real eye/terrain rays, independent of its ordinary
    // senses range and field of view. No target, pose or AI state is changed.
    Npc* observer = npcBySymbol(*world,JsonRead::str(msg,"npc"));
    Npc* target = observer!=nullptr ? AiNpc::findNpc(*world,JsonRead::str(msg,"to"),observer) : nullptr;
    if(observer==nullptr || target==nullptr || observer==target) {
      sendResult(req,false,"npc_los needs distinct npc and to targets");
      return;
      }
    const float distance = std::sqrt(observer->qDistTo(*target));
    const float extra = std::max(0.f,5000.f-float(observer->handle().senses_range));
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> value(buf);
    value.StartObject();
    value.Key("distance_cm"); value.Double(distance);
    value.Key("center_clear"); value.Bool(distance<=5000 && observer->canRayHitPoint(target->centerPosition(),true,extra));
    value.Key("head_clear"); value.Bool(distance<=5000 && observer->canRayHitPoint(target->mapHeadBone(),true,extra));
    value.EndObject();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    return;
    }
  if(cmd=="blood") {
    // v60: a blood stain on the ground (physics stage 8, step 4c), as Marvin `blood` lays it.
    // `waypoint`: at that point; `near`: `distance` cm (default 300) in front of that npc or the
    // hero; `npc`: under that dead npc, a pool of his body trace (bodyId).
    // `kind` pool|splash (default: pool under an npc, splash elsewhere), `radius` cm.
    const auto waypoint = JsonRead::str(msg,"waypoint");
    const auto name     = JsonRead::str(msg,"npc");
    const auto nearName = JsonRead::str(msg,"near");
    const auto kindName = JsonRead::str(msg,"kind");
    Tempest::Vec3 at;
    uint32_t      body = 0;
    if(!nearName.empty()) {
      Npc* near = nearName=="PLAYER" || nearName=="PC_HERO" ? pl : npcBySymbol(*world,nearName);
      if(near==nullptr) {
        sendResult(req,false,"no such npc");
        return;
        }
      const float dist = float(JsonRead::integer(msg,"distance",300));
      at    = near->position();
      at.x += dist*std::cos(near->rotationRad());
      at.z += dist*std::sin(near->rotationRad());
      }
    else if(!name.empty()) {
      Npc* npc = npcBySymbol(*world,name);
      if(npc==nullptr || !npc->isDead() || npc->bodyTrace().id==0) {
        sendResult(req,false,npc==nullptr ? "no such npc" : "no body trace of that npc yet");
        return;
        }
      at   = npc->position();
      body = npc->bodyTrace().id;
      }
    else if(const auto* point = world->findPoint(waypoint,false); point!=nullptr) {
      at = point->position();
      }
    else {
      sendResult(req,false,"blood needs a waypoint or a dead npc");
      return;
      }
    if(!kindName.empty() && kindName!="pool" && kindName!="splash") {
      sendResult(req,false,"kind must be pool or splash");
      return;
      }
    const bool  pool   = kindName.empty() ? body!=0 : kindName=="pool";
    const float radius = float(JsonRead::integer(msg,"radius",pool ? 40 : 15));
    const auto  id     = world->placeBloodStain(at,pool ? BloodStains::Kind::Pool : BloodStains::Kind::Splash,radius,
                                                pool ? body : 0);
    if(id==0) {
      sendResult(req,false,Gothic::physicsFlags().blood ? "no ground there" : "blood is off ([GAME] blood=0)");
      return;
      }
    sendResult(req,true,"","{\"id\":"+std::to_string(id)+"}");
    return;
    }
  if(cmd=="physics_noise") {
    std::string value, error;
    const bool ok = Bench::physicsNoiseDebug(*world,*pl,JsonRead::str(msg,"text"),value,error);
    sendResult(req,ok,error,value);
    return;
    }
  if(cmd=="physics_traces") {
    std::string value, error;
    const bool ok = Bench::physicsTracesDebug(*world,*pl,JsonRead::str(msg,"text"),value,error);
    sendResult(req,ok,error,value);
    return;
    }
  if(cmd=="destruction_traces") {
    std::string value, error;
    const bool ok = Bench::destructionTracesDebug(*world,*pl,JsonRead::str(msg,"text"),value,error);
    sendResult(req,ok,error,value);
    return;
    }
  if(cmd=="spawn") {
    // a creature next to an npc (or the player): monsters for tests of threats
    std::string inst(JsonRead::str(msg,"instance"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id   = world->script().findSymbolIndex(inst);
    const auto   near = JsonRead::str(msg,"near");
    Npc*         at   = near.empty() ? pl : npcBySymbol(*world,near);
    if(id==size_t(-1) || at==nullptr) {
      sendResult(req,false,id==size_t(-1) ? "unknown instance" : "unknown npc to spawn near");
      return;
      }
    // Prefer the old direction towards the player, else where the npc faces.
    const float dist = float(JsonRead::integer(msg,"distance",600));
    auto        pos  = at->position();
    float       dx   = std::cos(at->rotationRad()), dz = std::sin(at->rotationRad());
    if(at!=pl) {
      const auto  d   = pl->position()-pos;
      const float len = std::sqrt(d.x*d.x+d.z*d.z);
      if(len>1) {
        dx = d.x/len;
        dz = d.z/len;
        }
      }
    pos.x += dist*dx;
    pos.z += dist*dz;
    Npc* spawned = nullptr;
    if(at!=pl && dist>=0) {
      const auto hero = pl->position();
      const auto* physics = world->physic();
      int level = 0, supported = 0;
      const auto grounded = [&](Vec3& point) {
        const float px = point.x-hero.x, pz = point.z-hero.z;
        if(px*px+pz*pz<400.f*400.f)
          return false;
        const auto floor = physics->landRay({point.x,hero.y+100.f,point.z},200.f);
        if(!floor.hasCol || std::abs(floor.v.y-hero.y)>100.f)
          return false;
        // landRay deliberately leaves object normals zero. The footprint below
        // checks their support; a zero normal does not describe a steep slope.
        if(floor.n!=Vec3() && floor.n.y<.8f)
          return false;
        ++level;
        point = floor.v;
        // Support the body, not just its centre on the edge of a floor.
        const std::array<Vec3,4> footprint = {{{30,0,0},{-30,0,0},{0,0,30},{0,0,-30}}};
        for(const auto& offset:footprint) {
          const auto support = physics->landRay(point+offset+Vec3(0,50,0),100.f);
          if(!support.hasCol || std::abs(support.v.y-point.y)>40.f)
            return false;
          }
        if(physics->ray(point+Vec3(0,5,0),point+Vec3(0,100,0)).hasCol)
          return false;
        ++supported;
        // Keep a single creature while testing its actual animated centre: that
        // is the endpoint used by enemy perception, including for attached NPCs.
        if(spawned==nullptr) {
          spawned = world->addNpc(id,point);
          } else {
          spawned->setPosition(point);
          spawned->updateTransform();
          }
        return spawned!=nullptr && at->canRayHitPoint(spawned->centerPosition(),true);
        };
      bool found = grounded(pos);
      // A point on the NPC-to-hero ray can be beyond a wall or on another floor.
      // Search bounded rings around the hero without relaxing the height/LOS gates.
      for(int ring=0; ring<8 && !found; ++ring) {
        const float radius = std::max(dist,401.f)+float(ring)*100.f;
        for(int step=0; step<32 && !found; ++step) {
          const float angle = float(step)*(2.f*float(M_PI)/32.f);
          const float c = std::cos(angle), s = std::sin(angle);
          auto candidate = hero+Vec3(radius*(dx*c-dz*s),0,radius*(dx*s+dz*c));
          if(grounded(candidate)) {
            pos = candidate;
            found = true;
            }
          }
        }
      if(!found) {
        if(spawned!=nullptr)
          world->removeNpc(*spawned);
        sendResult(req,false,"no visible supported ground within 1 m of the hero's height and at least 4 m away "
                             "(level: "+std::to_string(level)+", supported: "+std::to_string(supported)+")");
        return;
        }
      }
    sendResult(req,spawned!=nullptr || world->addNpc(id,pos)!=nullptr,"could not spawn");
    return;
    }
  if(cmd=="own_npc") {
    // an npc of the director's (v33): a symbol of ours made like the script's npc `like`, put at a waypoint
    // or `distance` cm in front of the player. The save keeps both
    std::string why;
    auto*       sym = world->script().ownSymbols().addNpc(JsonRead::str(msg,"npc"),JsonRead::str(msg,"like"),why);
    if(sym==nullptr) {
      sendResult(req,false,why);
      return;
      }
    const auto wp  = JsonRead::str(msg,"waypoint");
    Npc*       npc = nullptr;
    if(!wp.empty()) {
      npc = world->addNpc(sym->index(),wp);
      } else {
      const float dist = float(JsonRead::integer(msg,"distance",300));
      auto        pos  = pl->position();
      pos.x += dist*std::cos(pl->rotationRad());
      pos.z += dist*std::sin(pl->rotationRad());
      npc = world->addNpc(sym->index(),pos);
      }
    if(npc==nullptr) {
      sendResult(req,false,"could not put " + sym->name() + " in the world");
      return;
      }
    if(npc->visualSkeleton()==nullptr) {
      // the code it is made like gave it no body: the world cannot have it (perception needs a head)
      world->removeNpc(*npc);
      sendResult(req,false,"the code of " + std::string(JsonRead::str(msg,"like")) + " gives " + sym->name() + " no body");
      return;
      }
    // {npc, index: its symbol, name: as the game shows it}
    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    const auto                                 shown = TextCodec::toUtf8(npc->displayName());
    w.StartObject();
    w.Key("npc");   w.String(sym->name().data(),rapidjson::SizeType(sym->name().size()));
    w.Key("index"); w.Uint(sym->index());
    w.Key("name");  w.String(shown.data(),rapidjson::SizeType(shown.size()));
    w.EndObject();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    return;
    }
  if(cmd=="teleport") {
    // the player `distance` cm in front of an npc, facing it: the npc is in view
    Npc* at = npcBySymbol(*world,JsonRead::str(msg,"near"));
    if(at==nullptr || at==pl || !pl->setInteraction(nullptr)) {
      sendResult(req,false,at==nullptr || at==pl ? "unknown npc to teleport to" : "the player is busy");
      return;
      }
    const float dist = float(JsonRead::integer(msg,"distance",600));
    auto        pos  = at->position();
    pos.x += dist*std::cos(at->rotationRad());
    pos.z += dist*std::sin(at->rotationRad());
    // Search from the NPC's body height, not above an overhead floor or rock shelf.
    const auto land = world->physic()->landRay({pos.x,at->centerPosition().y,pos.z},1500);
    if(land.hasCol)
      pos.y = land.v.y;
    pl->setPosition(pos);
    pl->setDirection(at->position()-pos);
    pl->updateTransform();
    if(auto c = Gothic::inst().camera())
      c->reset(pl);
    sendResult(req,true,"");
    return;
    }
  if(cmd=="npc_goto") {
    // an npc steps to a waypoint (or `distance` cm in front of npc `to`), on the ground, facing `face`
    // (v15). It stays free and goes on with what it does: its routine may walk it back (start_activity
    // keeps it there). Unlike the bench's `put` it is not frozen
    const auto symbol = JsonRead::str(msg,"npc");
    Npc*       npc    = npcBySymbol(*world,symbol);
    if(npc==nullptr || npc==pl || npc->isDead() || npc->isDown() || npc->isTalk() ||
       ctrl.find(*world,symbol)!=nullptr) {
      sendResult(req,false,"npc not found or busy");
      return;
      }
    Tempest::Vec3 pos;
    if(auto name = JsonRead::str(msg,"waypoint"); !name.empty()) {
      auto wp = world->findPoint(name,false);
      if(wp==nullptr) {
        sendResult(req,false,"unknown waypoint");
        return;
        }
      pos = wp->groundPos;
      }
    else if(Npc* to = npcBySymbol(*world,JsonRead::str(msg,"to"))) {
      const float d = float(JsonRead::integer(msg,"distance",300));
      pos = to->position();
      pos.x += d*std::cos(to->rotationRad());
      pos.z += d*std::sin(to->rotationRad());
      const auto land = world->physic()->landRay({pos.x,to->centerPosition().y,pos.z},1500);
      if(land.hasCol)
        pos.y = land.v.y;
      }
    else {
      sendResult(req,false,"npc_goto needs waypoint or to");
      return;
      }
    npc->setInteraction(nullptr,true);  // off a bench or a stove first
    npc->clearAiQueue();                // a walk under way would take it back at once
    npc->setPosition(pos);
    if(Npc* face = npcBySymbol(*world,JsonRead::str(msg,"face")); face!=nullptr && face!=npc)
      npc->setDirection(face->position()-pos);
    npc->updateTransform();
    sendResult(req,true,"");
    return;
    }
  if(cmd=="skip_important") {
    // an npc's important dialogs count as told: it does not stop the hero to talk (the start of the game:
    // most npc's have one, and a test scene with them near the hero would end in it)
    Npc* npc = npcBySymbol(*world,JsonRead::str(msg,"near"));
    if(npc==nullptr || npc==pl) {
      sendResult(req,false,"unknown npc");
      return;
      }
    world->script().skipImportantInfos(*npc,*pl);
    sendResult(req,true,"");
    return;
    }
  if(cmd=="deal") {
    // what the deal window shows
    sendResult(req,true,"",dealJson());
    return;
    }
  if(cmd=="deal_accept") {
    // Y as the player presses it
    ensureChat();
    const std::string r = chat->canDeal() ? chat->pressDeal() : "blocked";
    sendResult(req,true,"","\"" + r + "\"");
    return;
    }
  if(cmd=="deal_decline") {
    // N as the player presses it
    ensureChat();
    const std::string r = chat->canDeal() ? chat->declineDeal() : "blocked";
    sendResult(req,true,"","\"" + r + "\"");
    return;
    }
  if(cmd=="time") {
    // the clock goes to `hour`:`minute`, the next day if that is earlier than now (like sleeping)
    world->setDayTime(int32_t(JsonRead::integer(msg,"hour",8)),int32_t(JsonRead::integer(msg,"minute",0)));
    sendResult(req,true,"");
    return;
    }
  if(cmd=="kill") {
    // `npc`: that npc dies, killed by no one (a quest giver for the director's repairs); `npc` PLAYER: the hero
    // dies, by the hand of `by` if given (the final card's death, docs/achievements.md).
    // `instance`: the living creature of that instance (WOLF) closest to an npc (`near`, default the player)
    // dies by the hand of `by` (default the player), the way a deadly hit ends it (Npc::takeDamage): in melee,
    // right in front of the killer; npc's around, the killer too, perceive it (ASSESSMURDER)
    if(auto name = JsonRead::str(msg,"npc"); name=="PLAYER") {
      if(pl==nullptr || pl->isDead()) {
        sendResult(req,false,"no living hero");
        return;
        }
      if(const auto byName = JsonRead::str(msg,"by"); !byName.empty())
        pl->setLastHitBy(npcBySymbol(*world,byName));
      pl->changeAttribute(ATR_HITPOINTS,-pl->attribute(ATR_HITPOINTSMAX)-1,false);
      sendResult(req,pl->isDead(),"the hero did not die (immortal?)");
      return;
      }
    if(auto name = JsonRead::str(msg,"npc"); !name.empty()) {
      Npc* npc = npcBySymbol(*world,name);
      if(npc==nullptr || npc->isDead() || npc->isPlayer()) {
        sendResult(req,false,npc==nullptr ? "no such npc" : "it can't die now");
        return;
        }
      npc->changeAttribute(ATR_HITPOINTS,-npc->attribute(ATR_HITPOINTSMAX)-1,false);
      sendResult(req,npc->isDead(),"it did not die (immortal?)");
      return;
      }
    const auto near   = JsonRead::str(msg,"near");
    const auto byName = JsonRead::str(msg,"by");
    Npc*       at     = near.empty() ? pl : npcBySymbol(*world,near);
    Npc*       by     = byName.empty() || byName=="PLAYER" ? pl : npcBySymbol(*world,byName);
    Npc*       victim = at!=nullptr ? AiNpc::findNpc(*world,JsonRead::str(msg,"instance"),at) : nullptr;
    if(victim==nullptr || victim==pl || victim->isDead() || by==nullptr || by==victim) {
      sendResult(req,false,by==nullptr ? "no such killer" : "no such creature alive");
      return;
      }
    auto at1 = by->position();
    at1.x += 150*std::cos(by->rotationRad());
    at1.z += 150*std::sin(by->rotationRad());
    victim->setPosition(at1);
    victim->setLastHitBy(by);  // as a deadly hit leaves it: whose kill it is (hero_kill, v17)
    victim->changeAttribute(ATR_HITPOINTS,-victim->attribute(ATR_HITPOINTS),false);
    world->sendPassivePerc(*victim,*by,*victim,PERC_ASSESSMURDER);
    sendResult(req,victim->isDead(),"it did not die");
    return;
    }
  if(cmd=="player_goto") {
    // the hero steps to a waypoint (walking away from a conversation): a named one, the nearest one at
    // least `distance` cm away from an npc (`near`), or one next to an npc (`to`, `distance` cm off)
    const WayPoint* wp = nullptr;
    if(auto pos = msg.FindMember("pos"); pos!=msg.MemberEnd() && pos->value.IsArray() && pos->value.Size()==3) {
      // a point of the world (v21), on the ground there
      Tempest::Vec3 at(float(pos->value[0].GetDouble()),float(pos->value[1].GetDouble()),
                       float(pos->value[2].GetDouble()));
      const auto land = world->physic()->landRay(at+Tempest::Vec3(0,100,0),1500);
      if(land.hasCol)
        at.y = land.v.y;
      pl->setPosition(at);
      pl->updateTransform();
      sendResult(req,true,"");
      return;
      }
    if(auto name = JsonRead::str(msg,"waypoint"); !name.empty()) {
      wp = world->findPoint(name,false);
      }
    else if(Npc* to = npcBySymbol(*world,JsonRead::str(msg,"to"))) {
      const float off = float(JsonRead::integer(msg,"distance",300));
      const auto  at  = to->position();
      wp = world->findWayPoint(at,[&](const WayPoint& w) {
        return (w.position()-at).quadLength() >= off*off;
        });
      }
    else if(Npc* from = npcBySymbol(*world,JsonRead::str(msg,"near"))) {
      const float min  = float(JsonRead::integer(msg,"distance",1500));
      const auto  at   = from->position();
      wp = world->findWayPoint(pl->position(),[&](const WayPoint& w) {
        return (w.position()-at).quadLength() >= min*min;
        });
      }
    if(wp==nullptr) {
      sendResult(req,false,"unknown waypoint");
      return;
      }
    pl->setPosition(wp->groundPos);
    pl->updateTransform();
    sendResult(req,true,"");
    return;
    }
  if(cmd=="ptt") {
    // the T key as the player presses it (start), holds and lets go (stop) or taps it (cancel): the
    // focus is the hero's own
    const auto state = JsonRead::str(msg,"state");
    if(state!="start" && state!="stop" && state!="cancel") {
      sendResult(req,false,"ptt state: start, stop or cancel");
      return;
      }
    sendResult(req,true,"");
    sendPtt(state=="start" ? "ptt_start" : (state=="stop" ? "ptt_stop" : "ptt_cancel"));
    return;
    }
  if(cmd=="type") {
    // a line typed into the input line and sent
    sendResult(req,true,"");
    submitChat(TextCodec::fromUtf8(JsonRead::str(msg,"text")));
    return;
    }
  if(cmd=="waypoints") {
    // every waypoint and freepoint of the world: for the server's book of places (places.py). v32: the
    // waynet's links of each, how much of the sky above it is covered (`roof`: of five rays up, straight
    // and tilted, how many hit rock or a roof within RoofCm) and the room it is in (`room`, indoors)
    constexpr float RoofCm   = 2000;
    constexpr float RoofTilt = 0.57f;  // sin of 35°
    const auto*     phys     = world->physic();
    std::string     out      = "[";
    char            num[128];
    world->forEachWayPoint([&](const WayPoint& w) {
      const auto pos = w.position();
      int        roof = 0;
      const auto eye  = pos+Tempest::Vec3(0,50,0);
      const Tempest::Vec3 dirs[] = {{0,1,0},
                                    {RoofTilt,0.82f,0},{-RoofTilt,0.82f,0},{0,0.82f,RoofTilt},{0,0.82f,-RoofTilt}};
      for(auto& d:dirs)
        if(phys->ray(eye,eye+d*RoofCm).hasCol)
          ++roof;
      std::snprintf(num,sizeof(num),"\",\"x\":%.0f,\"y\":%.0f,\"z\":%.0f,\"fp\":%s,\"roof\":%d",
                    double(pos.x),double(pos.y),double(pos.z),w.isFreePoint() ? "true" : "false",roof);
      out += (out.size()>1 ? ",{\"name\":\"" : "{\"name\":\"") + w.name + num;
      const auto land = phys->landRay(eye,500);
      if(land.hasCol && land.sector!=nullptr && land.sector[0]!='\0')
        out += std::string(",\"room\":\"") + land.sector + "\"";
      if(!w.connections().empty()) {
        out += ",\"conn\":[";
        for(size_t i=0; i<w.connections().size(); ++i)
          out += (i ? ",\"" : "\"") + w.connections()[i].point->name + "\"";
        out += "]";
        }
      out += "}";
      });
    sendResult(req,true,"",out + "]");
    return;
    }
  if(cmd=="player_equip") {
    // the player gets an item and puts it on (armor of another guild, for the npc's eyes)
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(id==size_t(-1)) {
      sendResult(req,false,"unknown item " + inst);
      return;
      }
    if(!pl->inventory().itemCount(id))
      pl->addItem(id,1);
    pl->useItem(id);
    sendResult(req,true,"");
    return;
    }
  if(cmd=="player_drop") {
    // the player throws a thing down, as with the game's own key (v22): he is given it first if he has none
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(id==size_t(-1)) {
      sendResult(req,false,"unknown item " + inst);
      return;
      }
    if(!pl->inventory().itemCount(id))
      pl->addItem(id,1);
    const size_t before = pl->inventory().itemCount(id);
    pl->dropItem(id,1);
    const bool dropped = pl->inventory().itemCount(id)<before;
    sendResult(req,dropped,dropped ? "" : "the hero could not drop it now");
    return;
    }
  if(cmd=="spawn_item") {
    // A normal world item, with the script instance's own ownership. In particular, a public item
    // stays unowned; no test-only owner is manufactured here.
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(id==size_t(-1)) {
      sendResult(req,false,"unknown item " + inst);
      return;
      }
    const float d = float(JsonRead::integer(msg,"distance",100));
    auto pos = pl->position();
    pos.x += d*std::cos(pl->rotationRad());
    pos.z += d*std::sin(pl->rotationRad());
    const auto land = world->physic()->landRay(pos+Tempest::Vec3(0,100,0),1500);
    if(land.hasCol)
      pos.y = land.v.y;
    Item* item = world->addItem(id,pos);
    if(item!=nullptr)
      item->setCount(size_t(std::max<int64_t>(1,JsonRead::integer(msg,"count",1))));
    sendResult(req,item!=nullptr,item==nullptr ? "the item could not be created" : "");
    return;
    }
  if(cmd=="npc_drop") {
    // The native drop stamps the item's owner with this NPC's script instance. A fixture must use
    // that path too: changing an item's owner in the bridge would conceal ownership bugs.
    Npc* npc = npcBySymbol(*world,JsonRead::str(msg,"npc"));
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(npc==nullptr || npc==pl || npc->isDead() || id==size_t(-1)) {
      sendResult(req,false,npc==nullptr ? "unknown npc" : "unknown item or unavailable npc");
      return;
      }
    const size_t n = size_t(std::max<int64_t>(1,JsonRead::integer(msg,"count",1)));
    if(npc->inventory().itemCount(id)<n)
      npc->addItem(id,n-npc->inventory().itemCount(id));
    const size_t before = npc->inventory().itemCount(id);
    npc->dropItem(id,n);
    const bool dropped = npc->inventory().itemCount(id)<before;
    sendResult(req,dropped,dropped ? "" : "the npc could not drop it now");
    return;
    }
  if(cmd=="player_hit") {
    // One real melee hit, including damage calculation and both native damage perceptions.
    Npc* victim = npcBySymbol(*world,JsonRead::str(msg,"npc"));
    if(victim==nullptr || victim==pl || victim->isDown()) {
      sendResult(req,false,"unknown or unavailable victim");
      return;
      }
    if(pl->qDistTo(*victim)>500.f*500.f) {
      sendResult(req,false,"victim is more than five metres away");
      return;
      }
    const auto before = victim->attribute(ATR_HITPOINTS);
    victim->takeDamage(*pl,nullptr);
    const bool damaged = victim->attribute(ATR_HITPOINTS)<before;
    sendResult(req,damaged,damaged ? "" : "the hit caused no damage");
    return;
    }
  if(cmd=="npc_hit") {
    // A confirmed melee contact for a bench: only parry/backstep are bypassed. Native damage,
    // armour, hit reactions and perceptions still identify the real attacker.
    Npc* victim = npcBySymbol(*world,JsonRead::str(msg,"npc"));
    Npc* by     = npcBySymbol(*world,JsonRead::str(msg,"by"));
    if(victim==nullptr || by==nullptr || victim==by || victim==pl || by==pl ||
       victim->isDown() || by->isDown() || victim->isImmortal()) {
      sendResult(req,false,"npc_hit needs distinct living npcs and a mortal victim");
      return;
      }
    if(by->qDistTo(*victim)>500.f*500.f) {
      sendResult(req,false,"victim is more than five metres from the attacker");
      return;
      }
    const auto mode = by->weaponState();
    const auto* weapon = by->inventory().activeWeapon();
    if((mode!=WeaponState::W1H && mode!=WeaponState::W2H) || weapon==nullptr) {
      sendResult(req,false,"npc_hit needs a drawn melee weapon");
      return;
      }
    bool vulnerable = false;
    for(size_t i=0; i<daedalus::DamageType::NUM; ++i)
      if((weapon->handle().damage_type&(1<<i))!=0 && victim->handle().protection[i]>=0)
        vulnerable = true;
    if(!vulnerable) {
      sendResult(req,false,"victim is immune to the attacker's damage");
      return;
      }
    const auto before = victim->attribute(ATR_HITPOINTS);
    const auto gameMs = world->tickCount();
    BodyObservations::noteEvent(*by,victim,"applied",gameMs);
    victim->takeDamage(*by,nullptr,true);
    const auto after = victim->attribute(ATR_HITPOINTS);
    rapidjson::StringBuffer buf;
    rapidjson::Writer<rapidjson::StringBuffer> value(buf);
    value.StartObject();
    value.Key("attacker"); value.String(npcSymbol(*world,*by).c_str());
    value.Key("victim"); value.String(npcSymbol(*world,*victim).c_str());
    value.Key("game_ms"); value.Uint64(gameMs);
    value.Key("hp_before"); value.Int(before);
    value.Key("hp_after"); value.Int(after);
    value.EndObject();
    sendResult(req,after<before,after<before ? "" : "the hit caused no damage",std::string_view(buf.GetString(),buf.GetSize()));
    return;
    }
  if(cmd=="player_take") {
    // the hero picks up the nearest such thing lying within `distance` cm (v29), as with the game's own key
    std::string inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(id==size_t(-1)) {
      sendResult(req,false,"unknown item " + inst);
      return;
      }
    const float range = float(JsonRead::integer(msg,"distance",500));
    Item*       it    = nullptr;
    float       best  = range*range;
    world->detectItem(pl->position(),range,[&](Item& i) {
      const float d = (i.position()-pl->position()).quadLength();
      if(i.clsId()==id && d<best) {
        it   = &i;
        best = d;
        }
      });
    if(it==nullptr) {
      sendResult(req,false,"no " + inst + " lying nearby");
      return;
      }
    const bool taken = pl->takeItem(*it)!=nullptr;
    sendResult(req,taken,taken ? "" : "the hero could not pick it up now");
    return;
    }
  if(cmd=="chest_take") {
    // the hero takes things out of a chest unseen (v28): what its owner remembers is no longer there
    Interactive* chest = NpcChests::find(*world,JsonRead::str(msg,"chest"));
    std::string  inst(JsonRead::str(msg,"item"));
    for(auto& c:inst)
      c = char(std::toupper(static_cast<unsigned char>(c)));
    const size_t id = world->script().findSymbolIndex(inst);
    if(chest==nullptr || id==size_t(-1)) {
      sendResult(req,false,chest==nullptr ? "no such chest" : "unknown item " + inst);
      return;
      }
    const size_t n = std::min(chest->inventory().itemCount(id),size_t(std::max<int64_t>(1,JsonRead::integer(msg,"count",1))));
    if(n>0)
      pl->addItem(id,*chest,n);
    sendResult(req,n>0,"it is not in the chest",std::to_string(n));
    return;
    }
  if(cmd=="player_hp") {
    // the player's health and its maximum (v21): a test of creatures that bite lets him live through it
    const auto hp = int32_t(std::clamp<int64_t>(JsonRead::integer(msg,"hp",1000),1,100000));
    pl->changeAttribute(ATR_HITPOINTSMAX,hp-pl->attribute(ATR_HITPOINTSMAX),false);
    pl->changeAttribute(ATR_HITPOINTS,hp-pl->attribute(ATR_HITPOINTS),false);
    sendResult(req,true,"");
    return;
    }
  // player_weapon: the player draws ("melee") or puts away ("none") a weapon
  const auto state = JsonRead::str(msg,"state");
  const bool ok    = state=="none" ? pl->closeWeapon(false) : pl->drawWeaponMelee();
  sendResult(req,ok,"the player cannot switch weapons now");
  }

void AiBridge::Impl::onDialogDebug(World* world, const rapidjson::Value& msg) {
  const auto req = JsonRead::str(msg,"req");
  const auto cmd = JsonRead::str(msg,"cmd");
  if(!CommandLine::inst().isDevMode()) {
    sendResult(req,false,"debug commands need -devmode");
    return;
    }
  if(world==nullptr) {
    sendResult(req,false,"no world loaded");
    return;
    }
  if(cmd=="check") {
    sendResult(req,true,"",graph.checkWorld(*world));
    return;
    }
  if(cmd=="triggers") {
    sendResult(req,true,"",triggers.list(*world));
    return;
    }
  if(cmd=="menu") {
    // the dialog window as the player sees it: what it offers now, none while a line plays
    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    if(Gothic::inst().dialogEntriesFn)
      for(auto& e : Gothic::inst().dialogEntriesFn()) {
        auto text = TextCodec::toUtf8(e);
        w.String(text.data(),rapidjson::SizeType(text.size()));
        }
    w.EndArray();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    return;
    }
  if(cmd=="menu_pick") {
    // picks the entry with this text, as the player does with the keys
    const auto text    = TextCodec::fromUtf8(JsonRead::str(msg,"text"));
    auto       entries = Gothic::inst().dialogEntriesFn ? Gothic::inst().dialogEntriesFn() : std::vector<std::string>();
    auto       at      = std::find(entries.begin(),entries.end(),text);
    const bool ok      = at!=entries.end() && Gothic::inst().dialogPickFn &&
                         Gothic::inst().dialogPickFn(size_t(at-entries.begin()));
    sendResult(req,ok,"the window does not offer it now");
    return;
    }
  const auto symbol = JsonRead::str(msg,"npc");
  Npc*       npc    = symbol.empty() ? nullptr : npcBySymbol(*world,symbol);
  if(npc==nullptr && (cmd!="eval" || !symbol.empty())) {
    sendResult(req,false,"no npc " + std::string(symbol));
    return;
    }
  std::string err;
  if(cmd=="dialogs") {
    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for(auto& id : graph.offered(*world,*npc))
      w.String(id.data(),rapidjson::SizeType(id.size()));
    w.EndArray();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    }
  else if(cmd=="talk") {
    // the hero talks to it, as with the action key: the dialog window opens
    Npc* player = world->player();
    if(player!=nullptr)
      npc->startDialog(*player);
    sendResult(req,player!=nullptr,"no player");
    }
  else if(cmd=="choices") {
    // what the dialog menu offers after this dialog's lines: [{id, text}]
    rapidjson::StringBuffer                    buf;
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    w.StartArray();
    for(auto& [id,text] : graph.choices(*world,*npc,JsonRead::str(msg,"node"))) {
      w.StartObject();
      w.Key("id");   w.String(id.data(),rapidjson::SizeType(id.size()));
      w.Key("text"); w.String(text.data(),rapidjson::SizeType(text.size()));
      w.EndObject();
      }
    w.EndArray();
    sendResult(req,true,"",std::string_view(buf.GetString(),buf.GetSize()));
    }
  else if(cmd=="choose") {
    const bool ok = graph.choose(*world,*npc,JsonRead::str(msg,"node"),err);
    sendResult(req,ok,err);
    }
  else if(cmd=="eval") {
    auto it = msg.FindMember("expr");
    std::string value;
    if(it==msg.MemberEnd() || !graph.evaluate(*world,npc,it->value,value,err))
      sendResult(req,false,err.empty() ? "no expr" : err); else
      sendResult(req,true,"",value);
    }
  }
