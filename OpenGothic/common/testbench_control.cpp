#include "testbench_internal.h"

#include <algorithm>
#include <cmath>

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "marvin.h"
#include "world/aiqueue.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;
using namespace testbench_detail;

namespace {

bool keyAction(std::string_view k, KeyCodec::Action& a) {
  static const struct { std::string_view name; KeyCodec::Action a; } keys[] = {
    {"forward",KeyCodec::Forward}, {"back",KeyCodec::Back},       {"left",KeyCodec::Left},  {"right",KeyCodec::Right},
    {"turnl",  KeyCodec::RotateL}, {"turnr",KeyCodec::RotateR},   {"jump",KeyCodec::Jump},  {"walk", KeyCodec::Walk},
    {"sneak",  KeyCodec::Sneak},   {"weapon",KeyCodec::Weapon},   {"action",KeyCodec::ActionGeneric},
    {"attack", KeyCodec::ActionGeneric},
    {"melee",  KeyCodec::WeaponMele},{"bow",KeyCodec::WeaponBow}, {"spell",KeyCodec::WeaponMage3},
    {"parade", KeyCodec::Parade},  {"breath",KeyCodec::Walk},     {"dodge",KeyCodec::Dodge},
    {"heal", KeyCodec::Heal},      {"potion",KeyCodec::Potion},
    };
  for(auto& i:keys)
    if(i.name==k) {
      a = i.a;
      return true;
      }
  return false;
  }

// The bench owns only the hero pose. Path selection and waypoint heights come from WayMatrix.
void beginRoute(const ControlContext& ctx,const std::vector<std::string>& args, World& world, Npc& player) {
  const float speed=num(args,1,0);
  if(args.size()<4 || !isNumber(args[1]) || !std::isfinite(speed) || speed<=0) {
    arenaAbort("route expects a positive speed in cm/s and at least two waypoints");return;
    }
  std::vector<const WayPoint*> stops;
  for(size_t i=2;i<args.size();++i) {
    auto* point=world.findWayPoint(args[i]);
    if(point==nullptr) {arenaAbort("route waypoint is missing: "+args[i]);return;}
    stops.push_back(point);
    }
  std::vector<Vec3> points{stops.front()->position()};
  for(size_t i=1;i<stops.size();++i) {
    auto path=world.wayTo(*stops[i-1],*stops[i]);
    if(path.last()!=stops[i]) {arenaAbort("route waypoints are not connected: "+stops[i]->name);return;}
    while(auto* point=path.pop())
      if((points.back()-point->position()).quadLength()>0)
        points.push_back(point->position());
    }
  if(points.size()<2) {arenaAbort("route has no distance to travel");return;}
  ctx.routeLength=0;
  for(size_t i=1;i<points.size();++i)
    ctx.routeLength+=(points[i]-points[i-1]).length();
  ctx.routePoints=std::move(points);ctx.routeNext=1;ctx.routeSpeed=speed;
  ctx.routePosition=ctx.routePoints.front();ctx.routeStarted=ctx.time;
  ctx.routeWasExternal=player.isExternalControl();ctx.routeActive=true;
  player.stopWalking();player.clearAiQueue();player.setExternalControl(true);
  player.setPosition(ctx.routePosition);player.updateTransform();
  event("route_start","\"speed_cm_s\":"+std::to_string(speed)+",\"length_cm\":"+std::to_string(ctx.routeLength)+
    ",\"points\":"+std::to_string(ctx.routePoints.size()));
  }

}

namespace testbench_detail {

std::string rest(const std::string& line) {
  const size_t sp = line.find(' ');
  return sp==std::string::npos ? "" : line.substr(sp+1);
  }

Npc* npcBySymbol(World& w, std::string_view symbol) {
  const size_t id = w.script().findSymbolIndex(symbol);
  return id==size_t(-1) ? nullptr : w.findNpcByInstance(id);
  }

Marvin& marvin() {
  static Marvin m;
  return m;
  }

bool controlCommand(const ControlContext& ctx,const std::string& line,const std::vector<std::string>& a,
                    World& w,Npc& pl,PlayerControl& ctrl) {
  const auto& c=a[0];
  if(c=="wait") {
    if(a.size()>1 && a[1]=="real")
      ctx.realWaitUntil=std::chrono::steady_clock::now()+std::chrono::milliseconds(uint64_t(num(a,2,1000)));
    else
      ctx.waitUntil = ctx.time + uint64_t(num(a,1,1000));
    }
  else if(c=="waitarrive") {
    ctx.waitArrive = true;
    ctx.arriveEnd  = ctx.time + uint64_t(num(a,1,10000));
    }
  else if(c=="say" || c=="log") {
    say(rest(line));
    }
  else if(c=="label") {
    ctx.label = rest(line);
    }
  else if(c=="goto") {
    // goto <waypoint> | goto pos <x> <y> <z> (a roof, a rock: where no waypoint is)
    const bool ok = marvin().exec(a.size()>1 && a[1]=="pos" ? line : "goto waypoint " + rest(line));
    ctx.hasPrev = false;
    say(std::string(ok ? "goto " : "goto failed: ") + rest(line));
    }
  else if(c=="route")beginRoute(ctx,a,w,pl);
  else if(c=="walkto") {
    auto wp = w.findPoint(rest(line));
    if(wp!=nullptr)
      pl.aiPush(AiQueue::aiGoToPoint(*wp));
    say(std::string(wp ? "walkto " : "walkto: no waypoint ") + rest(line));
    }
  else if(c=="near") {
    // near <npc> [dist cm 170] [degrees off the npc's facing 0]: the hero stands in front of the npc, facing it
    Npc* npc = a.size()>1 ? npcBySymbol(w,a[1]) : nullptr;
    if(npc==nullptr) {
      say("near: no npc " + (a.size()>1 ? a[1] : std::string()));
      return true;
      }
    Vec3 at = npc->position() + rotY(facing(*npc),num(a,3,0))*num(a,2,170);
    const float g = groundY(w,at.x,at.z,at.y+100);
    if(!std::isnan(g))
      at.y = g;
    pl.setPosition(at);
    face(pl,npc->position()-at);
    ctx.hasPrev = false;
    say("near " + a[1]);
    }
  else if(c=="approach") {
    // approach <npc>: walk up to the npc (AI path), then waitarrive
    if(Npc* npc = a.size()>1 ? npcBySymbol(w,a[1]) : nullptr)
      pl.aiPush(AiQueue::aiGoToNpc(npc)); else
      say("approach: no npc " + (a.size()>1 ? a[1] : std::string()));
    }
  else if(c=="walkmode") {
    const std::string m = a.size()>1 ? a[1] : "run";
    pl.setWalkMode(m=="walk" ? WalkBit::WM_Walk : m=="sneak" ? WalkBit::WM_Sneak : WalkBit::WM_Run);
    }
  else if(c=="face") {
    const std::string m = a.size()>1 ? a[1] : "";
    float an = 0, r = 0;
    Vec3  d;
    if(m=="uphill" || m=="downhill" || m=="across") {
      if(slopeAt(w,pl.position(),an,d,r))
        face(pl, m=="uphill" ? d*-1.f : m=="downhill" ? d : Vec3(d.z,0,-d.x));
      }
    else if(m=="wp" && a.size()>2) {
      if(auto wp = w.findPoint(a[2]))
        face(pl,wp->pos-pl.position());
      }
    else if(m=="npc" && a.size()>2) {
      if(auto npc = npcBySymbol(w,a[2]))
        face(pl,npc->position()-pl.position()); else
        say("face: no npc " + a[2]);
      }
    else if(m=="rel") {
      // face rel <deg>: along the facing of the watched npc turned by that much (the angles of place)
      const float y = (ctx.subjectOf(pl).rotation()+num(a,2,0))*float(M_PI)/180.f;
      face(pl,Vec3(std::cos(y),0,std::sin(y)));
      }
    else {
      const float y = num(a,1,0)*float(M_PI)/180.f;
      face(pl,Vec3(std::cos(y),0,std::sin(y)));
      }
    }
  else if(c=="turn") {
    const float y = (pl.rotation()+num(a,1,90))*float(M_PI)/180.f;
    face(pl,Vec3(std::cos(y),0,std::sin(y)));
    }
  else if(c=="wheel") {
    // the mouse wheel, as the window gets it: -1 down, 1 up
    ctrl.onMouseWheel(int(num(a,1,-1)));
    }
  else if(c=="press" || c=="release" || c=="tap" || c=="hold") {
    if(c=="release" && a.size()>1 && a[1]=="all") {
      for(auto& h:ctx.holds)
        release(ctrl,h.a);
      ctx.holds.clear();
      return true;
      }
    KeyCodec::Action k = KeyCodec::Idle;
    if(a.size()<2 || !keyAction(a[1],k)) {
      say("unknown key: " + line);
      return true;
      }
    if(c=="press") {
      press(ctrl,k);
      ctx.holds.push_back({k,uint64_t(-1)});
      }
    else if(c=="release") {
      if((k==KeyCodec::Heal || k==KeyCodec::Potion) && !ctx.healing.test.label.empty() && !ctx.healing.test.hotkeyAt)
        ctx.healing.test.hotkeyAt=w.tickCount();
      release(ctrl,k);
      ctx.holds.erase(std::remove_if(ctx.holds.begin(),ctx.holds.end(),[k](const Hold& h){ return h.a==k; }),ctx.holds.end());
      }
    else if(c=="tap") {
      press(ctrl,k);
      ctx.taps.push_back(k);
      }
    else {
      press(ctrl,k);
      const auto until = ctx.time+uint64_t(num(a,2,1000));
      ctx.holds.push_back({k,until});
      if(k==KeyCodec::ActionGeneric || k==KeyCodec::Forward)
        ctx.inputTraceUntil = until+2000;
      }
    healingObserve(ctx.healing,w,pl);
    }
  else {
    return false;
    }
  return true;
  }

}
