#include "testbench_internal.h"

#include <cctype>
#include <cmath>
#include <span>

#include "game/gamescript.h"
#include "gothic.h"
#include "world/aiqueue.h"
#include "world/bullet.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

bool physicsTracesDebug(const PhysicsTracesContext& context,World& world,Npc& player,std::string_view command,
                                  std::string& value,std::string& error) {
  const auto fail=[&](std::string_view message) {error=message;return false;};
  const auto park=[&](Npc& npc,const Vec3& point) {
    npc.clearAiQueue();npc.clearState(false);npc.stopWalking();npc.setInteraction(nullptr,true);
    npc.closeWeapon(true);npc.setPosition(point);npc.setExternalControl(false);
    npc.setDirection(context.traceCenter-point);npc.updateTransform();
    npc.aiPush(AiQueue::aiWait(120000));
    };
  const auto floorAt=[&](Vec3& point) {
    const auto floor=world.physic()->landRay(point+Vec3(0,180,0),400);
    if(!floor.hasCol || floor.n.y<.8f || std::abs(floor.v.y-context.traceCenter.y)>180)return false;
    point.y=floor.v.y;
    for(const auto offset:{Vec3(40,0,0),Vec3(-40,0,0),Vec3(0,0,40),Vec3(0,0,-40)})
      if(world.physic()->ray(point+Vec3(0,85,0),point+offset+Vec3(0,85,0)).hasCol)return false;
    return true;
    };
  if(command=="setup" || command.starts_with("reload ")) {
    world.physic()->setMovableVobsEnabled(true);
    Gothic::settingsSetI("GAME","arrow_sticking",1);
    context.traceHost=nullptr;
    for(uint32_t i=0;i<world.npcCount();++i) {
      auto* npc=world.npcById(i);if(!npc)continue;
      auto name=symName(world,npc);
      for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name.find("CORAGON")!=std::string::npos || name.find("KORAGON")!=std::string::npos) {context.traceHost=npc;break;}
      }
    if(!context.traceHost)return fail("trace fixture requires Coragon in Khorinis");
    const auto all=world.physic()->movableVobSnapshot();
    const DynamicWorld::MovableVobSnapshot* table=nullptr;
    if(command.starts_with("reload ")) {
      const auto zenId=uint32_t(std::stoul(std::string(command.substr(7))));
      for(const auto& body:all)if(body.zenId==zenId) {table=&body;break;}
      }
    else {
      float nearest=1400.f*1400.f;
      for(const auto& body:all) {
        if(body.className!="table" || body.zenId==uint32_t(-1))continue;
        const auto distance=(movablePosition(body.transform)-context.traceHost->position()).quadLength();
        if(distance<nearest) {nearest=distance;table=&body;}
        }
      }
    if(!table)return fail("Coragon trace fixture lacks a native map table");
    context.traceTable=table->zenId;context.traceOrigin=table->origin;
    context.traceCenter=(table->localBounds[0]+table->localBounds[1])*.5f;
    table->origin.project(context.traceCenter);
    if(command=="setup" && !world.physic()->setMovableVobTransform(context.traceTable,context.traceOrigin))
      return fail("trace table baseline restoration failed");
    world.script().skipImportantInfos(*context.traceHost,player);
    player.handle().attribute[ATR_HITPOINTS]=100000;player.handle().attribute[ATR_HITPOINTSMAX]=100000;
    bool wallFound=false;
    const auto from=context.traceCenter+Vec3(0,120,0);
    for(unsigned i=0;i<96;++i) {
      const float angle=float(i)*float(M_PI)/48.f;
      const Vec3 direction(std::cos(angle),0,std::sin(angle));
      const auto hit=world.physic()->ray(from,from+direction*1000.f);
      if(!hit.hasCol || std::abs(hit.n.y)>.25f || (hit.v-from).length()<180)continue;
      context.traceWall=hit.v;context.traceShotDirection=direction;wallFound=true;break;
      }
    if(!wallFound)return fail("trace fixture lacks a real vertical tavern wall");
    bool supported=false;
    for(unsigned i=0;i<48;++i) {
      const float angle=float(i)*float(M_PI)/24.f;
      auto point=context.traceCenter+Vec3(std::cos(angle)*400,0,std::sin(angle)*400);
      if(!floorAt(point))continue;
      park(*context.traceHost,point);context.traceVisible=point;supported=true;break;
      }
    if(!supported)return fail("trace fixture lacks supported observer floor");
    auto hero=context.traceVisible+Vec3(100,0,0);
    if(floorAt(hero)) {player.setPosition(hero);player.updateTransform();}
    context.traceReady=true;
    }
  if(!context.traceReady || !context.traceHost)return fail("physics_traces setup required");
  auto& npc=*context.traceHost;
  if(command=="small" || command=="moved" || command=="tilted") {
    auto matrix=context.traceOrigin;
    if(command=="tilted")matrix.rotate(35,0,0,1);
    else matrix.set(3,0,matrix.at(3,0)+(command=="small"?10.f:80.f));
    if(!world.physic()->setMovableVobTransform(context.traceTable,matrix,&player))
      return fail("native trace table transform failed");
    }
  else if(command=="arrow" || command=="arrows") {
    const auto symbol=world.script().findSymbolIndex("ITRW_ARROW");
    if(symbol==size_t(-1))return fail("native trace arrow instance missing");
    Item arrow(world,symbol,Item::T_Inventory);
    const unsigned count=command=="arrow"?1:7;
    for(unsigned i=0;i<count;++i) {
      const auto to=context.traceWall+Vec3(0,float(i)*8,0);
      const auto from=to-context.traceShotDirection*80.f;
      auto& bullet=world.shootArrow(arrow,from,context.traceShotDirection,DynamicWorld::bulletSpeed);
      bullet.setOrigin(&player);bullet.setPrecise(1.f);
      }
    }
  else if(command=="visible" || command=="blocked" || command=="far") {
    const auto traces=world.physicsTraces();
    std::vector<const World::PhysicsTrace*> scene;
    for(const auto& trace:traces)
      if((trace.position-context.traceCenter).length()<1300.f)scene.push_back(&trace);
    if(scene.size()<2)return fail("trace observer placement requires table and wall arrow");
    bool found=false;
    // named arrays: a ?: over two initializer_lists copies the lists, and their arrays die with the expression
    static constexpr float farDistances[]={1700,1900,2100,2400};
    static constexpr float nearDistances[]={300,450,600,800,1000};
    const std::span<const float> distances=command=="far"?std::span<const float>(farDistances):
                                                          std::span<const float>(nearDistances);
    for(float distance:distances) {
      const unsigned steps=command=="far"?360:96;
      for(unsigned i=0;i<steps;++i) {
        const float angle=float(i)*float(M_PI)*2.f/float(steps);
        auto point=context.traceCenter+Vec3(std::cos(angle)*distance,0,std::sin(angle)*distance);
        if(!floorAt(point))continue;
        park(npc,point);
        bool all=true,anyVisible=false;
        for(const auto* trace:scene) {
          bool visible=false;
          for(const auto& sight:trace->sightPoints)
            visible|=command=="visible"?npc.canRayHitPoint(sight,false):npc.canRayHitPoint(sight,true,2000.f);
          const float cm=(trace->position-npc.position()).length();
          anyVisible|=visible;
          if(command=="blocked")all&=!visible && cm<1500;
          else if(command=="far")all&=cm>1500;
          else all&=visible && cm<1500;
          }
        if(command=="far")all&=anyVisible;
        if(all) {found=true;break;}
        }
      if(found)break;
      }
    if(!found)return fail(command=="blocked"?"no supported observer behind a real wall":
                         command=="far"?"no supported visible observer beyond 15m":
                                        "no supported observer seeing both native traces");
    if(command=="visible")context.traceVisible=npc.position();
    if(command=="blocked")context.traceBlocked=npc.position();
    if(command=="far")context.traceFar=npc.position();
    }
  else if(command!="setup" && !command.starts_with("reload ") && command!="sample")
    return fail("unknown physics_traces command");
  std::string rows="[";
  for(const auto& trace:world.physicsTraces()) {
    if((trace.position-context.traceCenter).length()>2500)continue;
    bool visible=false;
    for(const auto& point:trace.sightPoints)visible|=npc.canRayHitPoint(point,true,2000.f);
    if(rows.size()>1)rows+=',';
    const auto* wp=world.findWayPoint(trace.position);
    rows+="{\"id\":"+jsonStr(trace.id)+",\"kind\":"+jsonStr(trace.kind)+",\"name\":"+jsonStr(trace.name)+
      ",\"waypoint\":"+jsonStr(wp?wp->name:"")+",\"position_cm\":"+ragdollVector(trace.position)+
      ",\"distance_cm\":"+ragdollNumber((trace.position-npc.position()).length())+
      ",\"appeared_at\":"+std::to_string(trace.appearedAt)+",\"actor_truth\":"+jsonStr(trace.culprit)+
      ",\"can_ray_hit_point\":"+(visible?"true":"false")+"}";
    }
  value="{\"npc\":"+jsonStr(symName(world,&npc))+",\"hero\":"+jsonStr(symName(world,&player))+
    ",\"table_zen_id\":"+std::to_string(context.traceTable)+
    ",\"table_trace_id\":"+jsonStr(std::string(world.name())+":vob:"+std::to_string(context.traceTable))+
    ",\"npc_position_cm\":"+ragdollVector(npc.position())+",\"table_center_cm\":"+ragdollVector(context.traceCenter)+
    ",\"wall_cm\":"+ragdollVector(context.traceWall)+",\"game_minutes\":"+std::to_string(world.time().toInt()/60000)+
    ",\"truth\":"+rows+"]}";
  return true;
  }

}
