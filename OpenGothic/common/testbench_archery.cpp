#include "testbench_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>

#include "game/archerai.h"
#include "game/archery.h"
#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "gothic.h"
#include "graphics/mesh/aim.h"
#include "graphics/mesh/bowdraw.h"
#include "graphics/mesh/flinch.h"
#include "graphics/mesh/skeleton.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

const char* leanLineName(Aim::LeanLine line) {
  switch(line) {
    case Aim::LeanLine::Shot: return "shot";
    case Aim::LeanLine::Head: return "head";
    default: return "none";
    }
  }


// the point aimed at: the middle of a zone of the npc; `leg`: the limb capsules below the knees' height (limb
// alone is the middle of arms and legs, at the hips)
bool aimPoint(Npc& n, HitZones::Zone z, bool leg, Vec3& out) {
  if(leg) {
    std::vector<HitZones::Posed> caps;
    n.hitZonesPosed(caps);
    const float knees = n.position().y + 80.f;
    Vec3  sum   = {};
    float count = 0;
    for(auto& c:caps) {
      const Vec3 m = (c.a+c.b)*0.5f;
      if(c.zone==HitZones::Limb && m.y<knees) {
        sum   += m;
        count += 1;
        }
      }
    if(count>0) {
      out = sum*(1.f/count);
      return true;
      }
    }
  return n.hitZoneCenter(z,out);
  }

// cam arrow: beside the newest arrow (from the hero until one flies), looking a little ahead of it
void arrowShot(World& w, Npc& pl, Vec3& org, Vec3& tgt) {
  const auto context=archeryContext();
  Vec3 head = pl.position() + Vec3(0,150,0);
  Vec3 dir  = facing(pl);
  const Trail* tr = nullptr;
  for(auto& t:context.trails)
    if(t.pts.size()>=2 && (tr==nullptr || t.last>=tr->last))
      tr = &t;
  if(tr!=nullptr) {
    head = tr->pts.back();
    Vec3 d = tr->pts[1]-tr->pts[0];
    d.y = 0;
    if(d.length()>1e-3f)
      dir = d/d.length();
    }
  const Vec3 side = Vec3(-dir.z,0,dir.x)*(context.camAngle<0 ? -1.f : 1.f);
  org = head + side*context.camDist + Vec3(0,context.camHeight,0);
  tgt = head + dir*context.camLook;
  // the ground may rise beside the way: keep the camera a man's height over it
  const float g = groundY(w,org.x,org.z,org.y+400);
  if(!std::isnan(g) && org.y<g+120)
    org.y = g+120;
  }

// degrees between where the arrow points and the aim (sway, the turn lagging); -1 not aiming by hand
float manualAimOff(Npc& pl) {
  Vec3 pos, dir;
  if(!pl.hasManualAim() || !pl.arrowRay(pos,dir))
    return -1;
  const Vec3 want = Vec3::normalize(pl.manualAimPoint()-pos);
  return std::acos(std::clamp(Vec3::dotProduct(dir,want),-1.f,1.f))*180.f/float(M_PI);
  }

const char* archerStage(Archer::Stage s) {
  switch(s) {
    case Archer::Lowered:   return "lowered";
    case Archer::Raising:   return "raising";
    case Archer::Nocked:    return "nocked";
    case Archer::Drawing:   return "drawing";
    case Archer::Held:      return "held";
    case Archer::Reloading: return "reloading";
    }
  return "?";
  }

bool arrowAimPoint(Npc& npc, Vec3& point) {
  auto& state=archeryContext().archery;
  const auto* skeleton=npc.visualSkeleton();
  if(skeleton==nullptr || state.arrowAim.empty()) return false;
  if(state.arrowAim=="world") {point=state.stickingWorldPoint;return true;}
  if(state.arrowAim=="corpse") {
    if(const auto* body=npc.ragdollBody()) {
      const auto state=body->state();
      const auto wanted=arrowBone(*skeleton,"SPINE2");
      for(const auto& part:state.parts)
        if(part.bone==wanted) {point=part.centerOfMassMeters*100.f;return true;}
      }
    return false;
    }
  const bool leg=state.arrowAim=="leg";
  const bool chest=state.arrowAim=="chest";
  const auto first=arrowBone(*skeleton,chest ? "SPINE1" : leg ? "L CALF" : "L UPPERARM");
  auto second=arrowBone(*skeleton,chest ? "SPINE2" : leg ? "L FOOT" : "L FOREARM");
  if(chest && second==size_t(-1)) second=arrowBone(*skeleton,"NECK");
  if(first==size_t(-1) || second==size_t(-1)) return false;
  const auto& a=npc.animationBone(first);const auto& b=npc.animationBone(second);
  point=Vec3(a.at(3,0)+b.at(3,0),a.at(3,1)+b.at(3,1),a.at(3,2)+b.at(3,2))*.5f;
  return true;
  }

size_t npcArrowPins(World& world) {
  size_t count=0;
  for(uint32_t i=0;i<world.npcCount();++i)
    if(const auto* npc=world.npcById(i)) count+=npc->stuckArrowCount();
  return count;
  }

void stickingFields(World& world) {
  auto& state=archeryContext().archery;
  if(!state.stickingTrace || world.tickCount()==state.stickingSampleTick) return;
  state.stickingSampleTick=world.tickCount();
  const auto* npc=state.arrowNpc;
  const auto* living=npc ? npc->livingBody() : nullptr;
  const auto* corpse=npc ? npc->ragdollBody() : nullptr;
  const auto* body=living ? living : corpse;
  const auto physics=world.physic()->physicsStats();
  std::string pins="[",worldPins="[";
  if(npc) {
    state.arrowSeenHits=npc->arrowPhysicsProbe().hits;
    for(size_t i=0;i<npc->stuckArrowCount();++i) {
      Npc::ArrowPinProbe pin;
      if(!npc->stuckArrowProbe(i,pin)) {arenaAbort("stuck NPC pin has no actual draw node");return;}
      if(i) pins+=',';
      pins+="{\"index\":"+std::to_string(i)+",\"bone\":"+std::to_string(pin.bone)+
        ",\"local\":"+ragdollMatrix(pin.local)+",\"world\":"+ragdollMatrix(pin.world)+
        ",\"expected\":"+ragdollMatrix(npc->renderedBone(pin.bone)*pin.local)+
        ",\"render_bone\":"+ragdollMatrix(npc->renderedBone(pin.bone))+
        ",\"animation_bone\":"+ragdollMatrix(npc->animationBone(pin.bone))+"}";
      }
    }
  for(size_t i=0;i<world.stuckArrowCount();++i) {
    Matrix4x4 actual;
    if(!world.stuckArrowTransform(i,actual)) {arenaAbort("stuck world pin has no actual draw node");return;}
    if(i) worldPins+=',';
    worldPins+="{\"index\":"+std::to_string(i)+",\"world\":"+ragdollMatrix(actual)+"}";
    }
  event("arrow_sticking_frame","\"case\":"+jsonStr(state.stickingCase)+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"feature\":"+(Gothic::settingsGetI("GAME","arrow_sticking")?"true":"false")+
    ",\"npc_present\":"+(npc?"true":"false")+
    ",\"root_cm\":"+ragdollVector(npc?npc->position():Vec3())+
    ",\"yaw_deg\":"+ragdollNumber(npc?npc->rotation():0.f)+
    ",\"ko\":"+(npc && npc->isUnconscious()?"true":"false")+
    ",\"dead\":"+(npc && npc->isDead()?"true":"false")+
    ",\"recovering\":"+(npc && npc->isRagdollBlending()?"true":"false")+
    ",\"collision_overlap\":"+(npc && npc->hasCollision()?"true":"false")+
    ",\"living_parts\":"+std::to_string(living?living->partCount():0)+
    ",\"corpse_parts\":"+std::to_string(corpse?corpse->partCount():0)+
    ",\"weight\":"+ragdollNumber(body?body->reactionWeight():0.f)+
    ",\"world_bodies\":"+std::to_string(physics.bodies)+",\"physics_ms\":"+ragdollNumber(physics.stepMilliseconds)+
    ",\"npc_pin_count\":"+std::to_string(npc?npc->stuckArrowCount():0)+
    ",\"all_npc_pin_count\":"+std::to_string(npcArrowPins(world))+
    ",\"world_pin_count\":"+std::to_string(world.stuckArrowCount())+
    ",\"npc_pins\":"+pins+"],\"world_pins\":"+worldPins+"]");
  }

void arrowFields(World& world) {
  auto& state=archeryContext().archery;
  auto* npc=state.arrowNpc;
  if(!state.arrowTrace || npc==nullptr || world.tickCount()==state.arrowSampleTick) return;
  state.arrowSampleTick=world.tickCount();
  const auto& hit=npc->arrowPhysicsProbe();
  state.arrowSeenHits=hit.hits;
  const auto* living=npc->livingBody();const auto* corpse=npc->ragdollBody();
  const auto* body=living!=nullptr ? living : corpse;
  const auto* skeleton=npc->visualSkeleton();
  const auto physics=world.physic()->physicsStats();
  std::vector<Matrix4x4> native;
  const bool posed=body!=nullptr && body->getPose(native);
  std::string bones="[",active="[";
  if(body!=nullptr) {
    for(const auto bone:body->reactionBones()) {
      if(active.size()>1) active+=',';
      active+=std::to_string(bone);
      }
    if(posed && skeleton!=nullptr)
      for(const auto bone:body->boneIndices()) {
        if(bone>=native.size() || bone>=skeleton->nodes.size()) continue;
        if(bones.size()>1) bones+=',';
        const auto parent=skeleton->nodes[bone].parent;
        bones+="{\"id\":"+std::to_string(bone)+",\"name\":"+jsonStr(skeleton->nodes[bone].name)+
          ",\"parent\":"+(parent<skeleton->nodes.size() ? std::to_string(parent) : "null")+
          ",\"native\":"+ragdollMatrix(native[bone])+",\"animation\":"+ragdollMatrix(npc->animationBone(bone));
        if(parent<native.size())
          bones+=",\"native_parent\":"+ragdollMatrix(native[parent])+",\"animation_parent\":"+ragdollMatrix(npc->animationBone(parent));
        bones+='}';
        }
    }
  const auto& flinch=Flinch::subjectProbe;
  event("arrow_reaction_frame","\"case\":"+jsonStr(state.arrowCase)+
    ",\"t_game_ms\":"+std::to_string(world.tickCount())+
    ",\"feature\":"+(Gothic::settingsGetI("GAME","arrow_physics") ? "true" : "false")+
    ",\"root_cm\":"+ragdollVector(npc->position())+
    ",\"dead\":"+(npc->isDead()?"true":"false")+",\"ko\":"+(npc->isUnconscious()?"true":"false")+
    ",\"living_parts\":"+std::to_string(living ? living->partCount() : 0)+
    ",\"corpse_parts\":"+std::to_string(corpse ? corpse->partCount() : 0)+
    ",\"world_bodies\":"+std::to_string(physics.bodies)+",\"physics_ms\":"+ragdollNumber(physics.stepMilliseconds)+
    ",\"kinematic\":"+(body!=nullptr && body->isKinematic()?"true":"false")+
    ",\"weight\":"+ragdollNumber(body ? body->reactionWeight() : 0.f)+
    ",\"reaction_bones\":"+active+"]"+
    ",\"hits\":"+std::to_string(hit.hits)+",\"applied\":"+std::to_string(hit.applied)+
    ",\"corpse_impulses\":"+std::to_string(hit.corpseImpulses)+
    ",\"hit_bone\":"+(hit.bone!=size_t(-1) ? std::to_string(hit.bone) : "null")+
    ",\"point_cm\":"+ragdollVector(hit.point)+",\"impulse_ns\":"+ragdollVector(hit.impulseNs)+
    ",\"strength\":"+ragdollNumber(hit.strength)+
    ",\"flinch_active\":"+(flinch.active?"true":"false")+",\"flinch_hits\":"+std::to_string(flinch.hits)+
    ",\"bones\":"+bones+"]");
  }

// Closest points of two closed segments. The first parameter also locates the closest
// point along the actual camera ray; capsules are the same posed shapes as arrow hits.
float aimSegmentDistance(const Vec3& p, const Vec3& q, const Vec3& a, const Vec3& b, float& along) {
  const Vec3 u=q-p, v=b-a, w=p-a;
  const float uu=Vec3::dotProduct(u,u), vv=Vec3::dotProduct(v,v);
  const float uv=Vec3::dotProduct(u,v), uw=Vec3::dotProduct(u,w), vw=Vec3::dotProduct(v,w);
  float s=0, t=0;
  if(uu<=1e-6f) {
    t=vv>1e-6f ? std::clamp(vw/vv,0.f,1.f) : 0.f;
    }
  else if(vv<=1e-6f) {
    s=std::clamp(-uw/uu,0.f,1.f);
    }
  else {
    const float den=uu*vv-uv*uv;
    if(den>1e-6f)
      s=std::clamp((uv*vw-uw*vv)/den,0.f,1.f);
    t=(uv*s+vw)/vv;
    if(t<0) { t=0; s=std::clamp(-uw/uu,0.f,1.f); }
    if(t>1) { t=1; s=std::clamp((uv-uw)/uu,0.f,1.f); }
    }
  along=s;
  return (p+u*s-a-v*t).length();
  }

// Exact segment/AABB distance: between crossings of the six box planes the squared
// distance is one quadratic. The runtime attachment matrix turns this into an OBB test.
float aimBoxDistance(const Vec3& from, const Vec3& to, const Vec3 (&box)[2]) {
  const float p[3]={from.x,from.y,from.z}, d[3]={to.x-from.x,to.y-from.y,to.z-from.z};
  const float low[3]={box[0].x,box[0].y,box[0].z}, high[3]={box[1].x,box[1].y,box[1].z};
  float cuts[8]={0,1};
  size_t count=2;
  for(size_t i=0;i<3;++i)
    if(std::abs(d[i])>1e-6f)
      for(float bound:{low[i],high[i]}) {
        const float t=(bound-p[i])/d[i];
        if(t>0 && t<1) cuts[count++]=t;
        }
  // at most 8: insertion sort (std::sort over the array trips GCC 15's -Warray-bounds on Linux)
  for(size_t a=1;a<count;++a)
    for(size_t b=a;b>0 && cuts[b-1]>cuts[b];--b)
      std::swap(cuts[b-1],cuts[b]);
  auto distance2=[&](float t) {
    float value=0;
    for(size_t i=0;i<3;++i) {
      const float x=p[i]+d[i]*t;
      const float gap=x-std::clamp(x,low[i],high[i]);
      value+=gap*gap;
      }
    return value;
    };
  float closest=distance2(0);
  for(size_t k=1;k<count;++k) {
    const float mid=(cuts[k-1]+cuts[k])*0.5f;
    float dd=0, pd=0;
    for(size_t i=0;i<3;++i) {
      const float x=p[i]+d[i]*mid;
      if(x>=low[i] && x<=high[i]) continue;
      dd+=d[i]*d[i];
      pd+=(p[i]-(x<low[i] ? low[i] : high[i]))*d[i];
      }
    const float t=dd>1e-6f ? std::clamp(-pd/dd,cuts[k-1],cuts[k]) : mid;
    closest=std::min(closest,std::min(distance2(t),distance2(cuts[k])));
    }
  return std::sqrt(closest);
  }

bool archeryCommand(const ArcheryContext& context,const std::string& line,const std::vector<std::string>& a,
                    World& w,Npc& pl,PlayerControl& ctrl) {
  auto& state=context.archery;
  const auto& c=a[0];
  if(c=="sticking" && a.size()==2 && a[1]=="setup") {
    state.stickingTrace=true;state.arrowTrace=false;
    event("arrow_sticking_setup","\"default_enabled\":"+
      std::string(Gothic::settingsGetI("GAME","arrow_sticking")?"true":"false"));
    }
  else if(c=="sticking" && a.size()==3 && a[1]=="case") {
    state.stickingCase=a[2];state.stickingTrace=true;
    event("arrow_sticking_case","\"case\":"+jsonStr(a[2])+
      ",\"npc_pins_before\":"+std::to_string(state.arrowNpc?state.arrowNpc->stuckArrowCount():0)+
      ",\"world_pins_before\":"+std::to_string(w.stuckArrowCount()));
    }
  else if(c=="sticking" && a.size()==3 && a[1]=="feature")
    Gothic::settingsSetI("GAME","arrow_sticking",a[2]=="on");
  else if(c=="sticking" && a.size()==2 && a[1]=="world") {
    const auto ahead=facing(pl),side=Vec3(-ahead.z,0,ahead.x);
    const auto target=pl.position()+ahead*450.f+side*220.f;
    const auto floor=w.physic()->landRay(target+Vec3(0,300,0),900.f);
    if(!floor.hasCol) {arenaAbort("arrow sticking ground target has no world surface");return true;}
    state.stickingWorldPoint=floor.v;state.arrowAim="world";state.arrowDraw=1.f;
    state.aimNpc=nullptr;state.aimFixed=false;
    context.cam=C_Hold;
    lookFrom(floor.v-ahead*230.f+side*180.f+Vec3(0,125,0),floor.v+Vec3(0,10,0));
    event("arrow_sticking_world_target","\"point_cm\":"+ragdollVector(floor.v)+",\"normal\":"+ragdollVector(floor.n));
    }
  else if(c=="sticking" && a.size()==2 && a[1]=="motion") {
    auto* npc=state.arrowNpc;
    if(npc==nullptr || npc->isDown()) {arenaAbort("arrow pin motion needs a living target");return true;}
    context.living.livingWalkers.clear();context.living.livingWalkers.emplace_back();auto& walk=context.living.livingWalkers.back();
    walk.npc=npc;
    walk.ends[0]=WayPoint(npc->position()+Vec3(250,0,0),"FP_ARROW_PIN_A");
    walk.ends[1]=WayPoint(npc->position(),"FP_ARROW_PIN_B");
    for(auto& point:walk.ends) {
      const auto floor=w.physic()->landRay(point.pos+Vec3(0,200,0),500);
      if(!floor.hasCol) {arenaAbort("arrow pin walking path has no ground");return true;}
      point.pos.y=floor.v.y;point.groundPos=point.pos;
      }
    npc->setWalkMode(WalkBit::WM_Walk);context.living.livingMotion=true;
    }
  else if(c=="sticking" && a.size()==2 && (a[1]=="still" || a[1]=="turn")) {
    if(auto* npc=state.arrowNpc) {
      context.living.livingMotion=false;context.living.livingWalkers.clear();npc->clearAiQueue();npc->stopWalking();
      npc->setExternalControl(true);npc->setAnim(Npc::Anim::Idle);
      npc->setDirection(a[1]=="turn" ? rotY(facing(*npc),90.f) : pl.position()-npc->position());npc->updateTransform();
      }
    }
  else if(c=="sticking" && a.size()==2 && a[1]=="remove") {
    const auto pins=npcArrowPins(w),bodies=w.physic()->physicsStats().bodies,worldPins=w.stuckArrowCount();
    exec("arrow cleanup",w,pl,ctrl);
    event("arrow_sticking_remove","\"npc_pins_before\":"+std::to_string(pins)+
      ",\"npc_pins_after\":"+std::to_string(npcArrowPins(w))+
      ",\"world_pins_before\":"+std::to_string(worldPins)+",\"world_pins_after\":"+std::to_string(w.stuckArrowCount())+
      ",\"bodies_before\":"+std::to_string(bodies)+",\"bodies_after\":"+std::to_string(w.physic()->physicsStats().bodies));
    }
  else if(c=="sticking" && a.size()==2 && a[1]=="stop") {
    state.stickingTrace=false;state.arrowAim.clear();
    }
  else if(c=="arrow" && a.size()==2 && (a[1]=="setup" || a[1]=="replace")) {
    if(a[1]=="setup") {
      if(!chooseRagdollArena(w,pl)) {arenaAbort("arrow fixture needs supported empty ground");return true;}
      exec("clear 2500",w,pl,ctrl);
      pl.setDirection({0,0,1});pl.updateTransform();
      } else exec("arrow cleanup",w,pl,ctrl);
    const auto symbol=w.script().findSymbolIndex("BAU_904_BAUER");
    auto at=context.ragdollArenaTarget-Vec3(0,80,400);
    auto* npc=w.addNpc(symbol,at);
    if(npc==nullptr) {arenaAbort("arrow fixture NPC insertion failed");return true;}
    npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
    npc->setDirection(pl.position()-at);npc->updateTransform();
    npc->handle().attribute[ATR_HITPOINTS]=10000;
    npc->handle().attribute[ATR_HITPOINTSMAX]=10000;
    state.arrowNpc=npc;context.subject=npc;context.crew.push_back(npc);
    state.arrowSeenHits=0;state.arrowSampleTick=0;
    exec("watch watched",w,pl,ctrl);
    event("arrow_reaction_setup","\"actors\":1,\"root_pinned\":false,\"position_cm\":"+ragdollVector(npc->position()));
    }
  else if(c=="arrow" && a.size()==3 && a[1]=="case") {
    state.arrowCase=a[2];state.arrowTrace=true;
    event("arrow_reaction_case","\"case\":"+jsonStr(a[2])+",\"hits_before\":"+
      std::to_string(state.arrowNpc ? state.arrowNpc->arrowPhysicsProbe().hits : 0));
    }
  else if(c=="arrow" && a.size()>=3 && a[1]=="aim") {
    state.arrowAim=a[2];state.arrowDraw=std::clamp(num(a,3,1.f),.1f,1.f);
    state.aimNpc=nullptr;state.aimFixed=false;
    }
  else if(c=="arrow" && a.size()==3 && a[1]=="feature")
    Gothic::settingsSetI("GAME","arrow_physics",a[2]=="on");
  else if(c=="arrow" && a.size()==2 && a[1]=="ko-on-hit")
    state.arrowKoOnHit=true;
  else if(c=="arrow" && a.size()==2 && a[1]=="cleanup") {
    if(auto* npc=state.arrowNpc) {
      state.arrowTrace=false;state.arrowAim.clear();state.arrowNpc=nullptr;state.arrowKoOnHit=false;
      const auto before=w.physic()->physicsStats().bodies;
      const auto parts=(npc->livingBody()?npc->livingBody()->partCount():0)+(npc->ragdollBody()?npc->ragdollBody()->partCount():0);
      exec("watch player",w,pl,ctrl);context.crew.clear();w.removeNpc(*npc);
      const auto after=w.physic()->physicsStats().bodies;
      event("arrow_reaction_cleanup","\"parts\":"+std::to_string(parts)+",\"before\":"+
        std::to_string(before)+",\"after\":"+std::to_string(after));
      }
    }
  else if(c=="arrow" && a.size()==2 && a[1]=="stop") {
    state.arrowTrace=false;state.arrowAim.clear();
    }
  else if(c=="bowstrength") {
    // bowstrength on|off: strength sets a bow's draw speed and limit (game/archery)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Archery::setBowStrengthEnabled(m=="on"); else
      say("unknown bowstrength command: " + line);
    }
  else if(c=="bowdraw") {
    // bowdraw on|off: the hand draws the string as far as the bow is drawn, the reload holds the aim
    // (graphics/mesh/bowdraw)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      BowDraw::setEnabled(m=="on"); else
      say("unknown bowdraw command: " + line);
    }
  else if(c=="bowrelease") {
    // bowrelease on|off: the release as an archer's (the hand to the ear, the bow kicks, turns and rings in
    // the hand); off: as before (graphics/mesh/bowdraw)
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      BowDraw::setReleaseEnabled(m=="on"); else
      say("unknown bowrelease command: " + line);
    }
  else if(c=="aimwalk") {
    // aimwalk on|off: the legs step under the aim (graphics/mesh/aimwalk)
    AimWalk::setEnabled(a.size()<2 || a[1]!="off");
    }
  else if(c=="arrows") {
    // arrows <npc|player> <n>: it carries exactly n arrows of its bow; the quiver on its hip shows them by
    // steps (graphics/mesh/quiver)
    Npc*        n   = findNpc(w,pl,a.size()>1 ? a[1] : "player");
    const Item* bow = n!=nullptr ? n->currentRangedWeapon() : nullptr;
    if(bow==nullptr || bow->handle().munition<=0) {
      say("arrows: no bow " + line);
      return true;
      }
    const size_t cls  = size_t(bow->handle().munition);
    const size_t have = n->inventory().itemCount(cls);
    const size_t want = size_t(std::max(0.f,num(a,2,0)));
    if(want>have)
      n->addItem(cls,want-have); else
    if(want<have)
      n->delItem(cls,uint32_t(have-want));
    n->updateQuiver();
    const auto q = n->quiverProbe();
    say("arrows " + symName(w,n) + " " + std::to_string(n->inventory().itemCount(cls)) + " shown " +
        std::to_string(q.arrows) + (q.on ? "" : " (no quiver)"));
    }
  else if(c=="arc") {
    // arc <npc> <count> <dist> <step deg>: that many of it stand that far in front of the hero, a step
    // apart round it, facing it (`all` of fight)
    const size_t id = w.script().findSymbolIndex(a.size()>1 ? a[1] : "");
    if(id==size_t(-1)) {
      say("arc: no npc " + line);
      return true;
      }
    const int   cnt  = int(num(a,2,4));
    const float d    = num(a,3,1500);
    const float step = num(a,4,15);
    std::string out  = "arc";
    context.crew.clear();
    for(int i=0; i<cnt; ++i) {
      const float an  = (pl.rotation() + (float(i)-float(cnt-1)*0.5f)*step)*float(M_PI)/180.f;
      Vec3        pos = pl.position() + Vec3(std::cos(an),0,std::sin(an))*d;
      DynamicWorld::RayLandResult r;
      for(float y=pos.y+1500; y>pos.y-1500 && !r.hasCol; y-=400)
        r = w.physic()->ray({pos.x,y,pos.z},{pos.x,y-450,pos.z});
      if(r.hasCol)
        pos.y = r.v.y;
      Npc* n = w.addNpc(id,pos);
      if(n==nullptr)
        continue;
      const Vec3 dir = pl.position()-pos;
      n->setDirection(Vec3(dir.x,0,dir.z));
      n->updateTransform();
      context.crew.push_back(n);
      char buf[100];
      std::snprintf(buf,sizeof(buf)," %.0f %.0f %.0f",pos.x,pos.y,pos.z);
      out += buf;
      }
    say(out);
    }
  else if(c=="archers") {
    // One primary ranged weapon per NPC: equipped first, otherwise the strongest in its inventory.
    uint32_t total = 0, weak = 0, dice = 0;
    for(uint32_t id=0; id<w.npcCount(); ++id) {
      auto* n = w.npcById(id);
      if(n==nullptr || n->isPlayer())
        continue;
      const Item* weapon = n->currentRangedWeapon();
      if(weapon==nullptr)
        for(auto item=n->inventory().iterator(Inventory::T_Inventory); item.isValid(); ++item) {
          if((item->mainFlag() & ITM_CAT_FF)==0)
            continue;
          if(weapon==nullptr || std::make_pair(item->handle().damage_total,item->handle().value)>
                                std::make_pair(weapon->handle().damage_total,weapon->handle().value))
            weapon = &*item;
          }
      if(weapon==nullptr)
        continue;
      const auto st = Archery::stats(*n,weapon);
      const bool below = st.bowStrength && st.burstDraw<Archery::MinDraw;
      const bool burst = st.bowStrength && st.steadyDraw<Archery::MinDraw && !below;
      const char* mode = below ? "dice (draw below minimum)" : (burst ? "hands burst" : "hands steady");
      const auto name = n->displayName();
      const auto symbol = symName(w,n);
      const auto weaponName = w.script().findSymbol(weapon->clsId())->name();
      char buf[640];
      std::snprintf(buf,sizeof(buf),"archers npc %s name %.*s: str %d dex %d weapon %.*s damage %d need %.2f r %.4f f_s %.4f f_b %.4f mode %s",
                    symbol.c_str(),int(name.size()),name.data(),
                    n->attribute(ATR_STRENGTH),n->attribute(ATR_DEXTERITY),
                    int(weaponName.size()),weaponName.data(),
                    int(weapon->handle().damage_total),double(st.need),double(st.strength),
                    double(st.steadyDraw),double(st.burstDraw),mode);
      say(buf);
      ++total;
      weak += st.strength<1.f ? 1u : 0u;
      dice += below ? 1u : 0u;
      }
    say("archers summary: total " + std::to_string(total) + " r<1 " + std::to_string(weak) + " dice " + std::to_string(dice));
    }
  else if(c=="archer") {
    // archer <npc|all> at <npc|player> [dice] | archer off: it stands where it is and shoots at the target
    // every time it can, by hand (npcaim on) or by the dice: no fight AI, no perception's wait (the aim's
    // calibration); all: the npcs of arc
    auto release = [&]() {
      for(auto& [n,at]:state.archers)
        if(n!=nullptr) {
          n->archerAi().cancel(*n);
          n->setExternalControl(false);
          }
      state.archers.clear();
      };
    if(a.size()>1 && a[1]=="off") {
      release();
      state.archerAt = nullptr;
      say("archer off");
      return true;
      }
    std::vector<Npc*> who;
    if(a.size()>1 && a[1]=="all")
      who = context.crew; else
    if(Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest"))
      who.push_back(n);
    release();
    state.archerAt   = findNpc(w,pl,a.size()>3 ? a[3] : "player");
    state.archerDice = a.size()>4 && a[4]=="dice";
    for(auto n:who) {
      if(n==state.archerAt || n->isDown())
        continue;
      n->setInteraction(nullptr,true);
      n->clearAiQueue();
      n->clearState(false);
      n->setExternalControl(true);
      state.archers.emplace_back(n,n->position());
      }
    say("archer " + std::to_string(state.archers.size()) + " at " + symName(w,state.archerAt) + (state.archerDice ? " by the dice" : ""));
    }
  else if(c=="aimat") {
    // aimat <npc|nearest> [head|body|limb|leg] | aimat <x> <y> <z> | aimat ahead [dist] [height] | aimat off: the
    // manual aim aims there
    // instead of the mouse - the middle of a zone of the npc, every frame. A last word `mouse`: the game's camera
    // turns onto it as the mouse would, the hero with it, the shot goes from the center of the screen
    auto a = split(line);
    state.aimNpc   = nullptr;
    state.aimFixed = false;
    state.aimMouse = a.size()>2 && a.back()=="mouse";
    if(state.aimMouse) {
      a.pop_back();
      ctrl.setAimOverride(false);
      }
    if(a.size()>1 && a[1]=="off") {
      ctrl.setAimOverride(false);
      say("aimat off");
      return true;
      }
    if(a.size()>1 && a[1]=="ahead") {
      // aimat ahead [dist cm 2000] [height cm 140]: a point that far ahead of the hero as it faces now
      const Vec3 f   = facing(pl);
      state.aimFixed = true;
      state.aimAt    = pl.position() + f*num(a,2,2000) + Vec3(0,num(a,3,140),0);
      ctrl.setAimOverride(!state.aimMouse,state.aimAt);
      say(std::string("aimat ahead") + (state.aimMouse ? " mouse" : ""));
      return true;
      }
    if(a.size()>3 && (std::isdigit(uint8_t(a[1][0])) || a[1][0]=='-')) {
      state.aimFixed = true;
      state.aimAt    = Vec3(num(a,1,0),num(a,2,0),num(a,3,0));
      ctrl.setAimOverride(!state.aimMouse,state.aimAt);
      say("aimat " + a[1] + " " + a[2] + " " + a[3] + (state.aimMouse ? " mouse" : ""));
      return true;
      }
    Npc* n = findNpc(w,pl,a.size()>1 ? a[1] : "nearest");
    if(n==nullptr || n==&pl) {
      say("aimat: no npc " + line);
      return true;
      }
    const std::string z = a.size()>2 ? a[2] : "body";
    state.aimNpc  = n;
    state.aimZone = z=="head" ? HitZones::Head : (z=="limb" || z=="leg" ? HitZones::Limb : HitZones::Body);
    state.aimLeg  = z=="leg";
    state.aimRaw  = a.size()>3 && a[3]=="raw";
    say("aimat " + symName(w,n) + " " + std::string(HitZones::name(state.aimZone)) + (state.aimRaw ? " raw" : "") +
        (state.aimMouse ? " mouse" : ""));
    }
  else if(c=="aim") {
    // aim on|off: the upper body turns and bends to the target with a bow, a crossbow, a spell
    const std::string m = a.size()>1 ? a[1] : "";
    if(m=="on" || m=="off")
      Aim::setEnabled(m=="on"); else
      say("unknown aim command: " + line);
    }
  else if(c=="aimlean") {
    // aimlean auto|degrees: force a settled pose for the head prediction check, without changing aim direction.
    if(a.size()>1 && a[1]=="auto") {
      Aim::playerLeanOverride = -1.f;
      say("aimlean auto");
      }
    else if(a.size()>1) {
      const float degrees = std::clamp(num(a,1,0),0.f,Aim::LeanMax);
      Aim::playerLeanOverride = degrees*float(M_PI)/180.f;
      say("aimlean " + std::to_string(degrees));
      }
    else {
      say("aimlean: expected auto or degrees 0..70");
      }
    }
  else return false;
  return true;
  }

}
