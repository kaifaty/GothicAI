#include "testbench_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

#include "game/gamescript.h"
#include "game/playercontrol.h"
#include "gothic.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "resources.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

bool waterOwns(Water& state,const DynamicWorld::WaterBodySnapshot& body) {
  if(body.ragdoll)
    return std::any_of(state.actors.begin(),state.actors.end(),[&](const auto& actor) {
      return actor.npc->ragdollBody()==body.ragdoll;
      });
  return state.ids.contains(body.id) || state.ids.contains(body.parentId);
  }

void waterFields(Water& state,const WaterContext& context,World& world) {
  if(!state.trace || context.movableMeasuring || state.sample==world.tickCount())return;
  state.sample=world.tickCount();
  if(state.killOnThrow && !state.actors.empty()) {
    auto* npc=state.actors.front().npc;
    const auto velocity=npc->spellVelocity();
    const auto displacement=(npc->position()-context.questWater.dry).length();
    if(!npc->isDown() && velocity.length()>100.f && displacement>50.f) {
      // Let the real projectile hit a living target first. Death during its
      // flight inherits ordinary live motion through the production birth path.
      state.killOnThrow=false;
      const auto at=npc->position();npc->setLastHitBy(world.player());
      if(!npc->setRagdollDown(true)) {arenaAbort("water corpse death during real wind flight refused");return;}
      npc->prepareRagdoll();
      const auto* rig=npc->ragdollBody();
      if(!rig) {arenaAbort("water corpse wind death created no native ragdoll");return;}
      const auto state=rig->state();
      event("water_bodies_death","\"actor_id\":"+std::to_string(world.npcId(npc))+
        ",\"position_cm\":"+ragdollVector(at)+",\"observed_wind_velocity_cm_s\":"+ragdollVector(velocity)+
        ",\"displacement_from_lip_cm\":"+ragdollNumber(displacement)+
        ",\"birth_velocity_cm_s\":"+(state.parts.empty()?"null":ragdollVector(state.parts.front().linearVelocity))+
        ",\"path\":\"production onNoHealth death during genuine live WindFist flight\"");
      }
    }
  for(auto& actor:state.actors)
    if(const auto* rig=actor.npc->ragdollBody();rig && rig->isSleeping() && actor.sleepAt==uint64_t(-1)) {
      const auto pelvis=movablePosition(rig->pelvisTransform());
      const auto water=world.physic()->waterRay(pelvis,300.f);
      if(water.hasCol && pelvis.y<water.wdepth+50.f)actor.sleepAt=world.tickCount()-state.start;
      }
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    if(!waterOwns(state,body))continue;
    const auto point=movablePosition(body.transform);
    const auto [origin,inserted]=state.reference.emplace(body.id,point);
    state.travel[body.id]=std::max(state.travel[body.id],(point-origin->second).length());
    // A newly inserted body may initially be inactive above the water. Count
    // only the first sleep after it has reached the real water or its bottom.
    if(body.sleeping && body.inWater && !state.sleepAt.contains(body.id))
      state.sleepAt.emplace(body.id,world.tickCount()-state.start);
    }
  if(state.observer && world.player() && world.tickCount()>=state.observerSample) {
    const auto& hero=*world.player();
    const auto floor=world.physic()->landRay(hero.position()+Vec3(0,180,0),1000.f);
    event("water_observer_sample","\"hero_cm\":"+ragdollVector(hero.position())+
      ",\"displacement_cm\":"+ragdollNumber((hero.position()-state.observerStart).length())+
      ",\"floor_found\":"+(floor.hasCol?"true":"false")+",\"floor_normal\":"+ragdollVector(floor.n)+
      ",\"slide\":"+(hero.isSlide()?"true":"false")+",\"falling\":"+(hero.isFalling()?"true":"false")+
      ",\"root_move_cm_per_33ms\":"+ragdollVector(hero.animMoveSpeed(33)));
    state.observerSample=world.tickCount()+1000;
    }
  if(state.swim && world.player()) {
    const auto point=world.player()->position();
    state.heroMaxStep=std::max(state.heroMaxStep,(point-state.heroLast).length());
    state.heroLast=point;
    state.sawSwim|=world.player()->isSwim();
    }
  }

std::string waterRows(Water& state,World& world) {
  std::string rows="[";
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    if(!waterOwns(state,body))continue;
    if(rows.size()>1)rows+=',';
    auto label=state.labels[body.id];
    if(label.empty() && body.category!=DynamicWorld::C_Fragment)label=state.labels[body.parentId];
    if(label.empty())label=body.category==DynamicWorld::C_Fragment?"crate_fragment":body.visual;
    std::string actorFields;
    if(body.ragdoll) {
      for(const auto& actor:state.actors) {
        if(actor.npc->ragdollBody()!=body.ragdoll)continue;
        const auto* skeleton=actor.npc->visualSkeleton();
        const auto bone=skeleton && body.bone<skeleton->nodes.size()?skeleton->nodes[body.bone].name:std::string();
        label=actor.role+":"+bone;
        actorFields=",\"actor_id\":"+std::to_string(world.npcId(actor.npc))+
          ",\"role\":"+jsonStr(actor.role)+",\"bone\":"+std::to_string(body.bone)+
          ",\"bone_name\":"+jsonStr(bone)+",\"target_fraction\":"+ragdollNumber(body.targetFraction);
        break;
        }
      }
    const auto point=movablePosition(body.transform);
    const bool hasWater=body.inWater && std::isfinite(body.waterY);
    const auto floor=world.physic()->landRay(Vec3(point.x,hasWater?body.waterY+3.f:point.y,point.z),6000.f);
    const auto asleep=state.sleepAt.find(body.id);
    std::string rendered="null";
    std::string itemFields;
    if(body.category==DynamicWorld::C_Item)
      world.detectItem(point,.1f,[&](Item& item) {
        if(std::abs(item.physicsMass()-body.mass)>.001f)return;
        itemFields=",\"item_instance\":"+std::to_string(item.clsId())+
          ",\"torch_burning\":"+(item.isTorchBurn()?"true":"false");
        });
    if(body.category==DynamicWorld::C_Item)
      if(const auto* mesh=Resources::loadMesh(body.visual)) {
        const auto* bounds=mesh->bbox();
        const float infinity=std::numeric_limits<float>::infinity();
        Vec3 low(infinity,infinity,infinity),high(-infinity,-infinity,-infinity);
        for(unsigned corner=0;corner<8;++corner) {
          Vec3 at(bounds[corner&1].x,bounds[(corner>>1)&1].y,bounds[(corner>>2)&1].z);
          body.transform.project(at);
          low.x=std::min(low.x,at.x);low.y=std::min(low.y,at.y);low.z=std::min(low.z,at.z);
          high.x=std::max(high.x,at.x);high.y=std::max(high.y,at.y);high.z=std::max(high.z,at.z);
          }
        rendered="{\"center_cm\":"+ragdollVector((low+high)*.5f)+
          ",\"min_cm\":"+ragdollVector(low)+",\"max_cm\":"+ragdollVector(high)+"}";
        }
    rows+="{\"label\":"+jsonStr(label)+",\"id\":"+std::to_string(body.id)+
      ",\"parent_id\":"+std::to_string(body.parentId)+",\"zen_id\":"+std::to_string(body.zenId)+
      ",\"piece\":"+std::to_string(body.piece)+",\"category\":"+std::to_string(body.category)+
      ",\"visual\":"+jsonStr(body.visual)+",\"mass_kg\":"+ragdollNumber(body.mass)+
      ",\"density_kg_m3\":"+ragdollNumber(body.density)+",\"position_cm\":"+ragdollVector(point)+
      ",\"world_bottom_cm\":"+ragdollNumber(body.worldBounds[0].y)+
      ",\"world_top_cm\":"+ragdollNumber(body.worldBounds[1].y)+
      ",\"rendered_bbox\":"+rendered+itemFields+actorFields+
      ",\"water_y_cm\":"+(hasWater?ragdollNumber(body.waterY):"null")+
      ",\"water_depth_cm\":"+(hasWater && floor.hasCol?ragdollNumber(body.waterY-floor.v.y):"null")+
      ",\"submerged_fraction\":"+ragdollNumber(body.submergedFraction)+
      ",\"sleeping\":"+(body.sleeping?"true":"false")+
      ",\"first_sleep_ms\":"+(asleep==state.sleepAt.end()?"null":std::to_string(asleep->second))+
      ",\"velocity_cm_s\":"+ragdollVector(body.velocity)+
      ",\"max_displacement_cm\":"+ragdollNumber(state.travel[body.id])+
      ",\"transform\":"+ragdollMatrix(body.transform)+"}";
    }
  return rows+"]";
  }

void waterCheckpoint(Water& state,const WaterContext& context,World& world,std::string_view label) {
  waterFields(state,context,world);
  const auto* hero=world.player();
  event("water_checkpoint","\"label\":"+jsonStr(label)+",\"case\":"+jsonStr(state.caseName)+
    ",\"buoyancy_enabled\":"+(world.physic()->buoyancyEnabled()?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string(world.tickCount()-state.start)+
    ",\"bodies\":"+waterRows(state,world)+",\"hero_cm\":"+(hero?ragdollVector(hero->position()):"null")+
    ",\"hero_swimming\":"+(hero && hero->isSwim()?"true":"false")+
    ",\"hero_saw_swim\":"+(state.sawSwim?"true":"false")+
    ",\"hero_max_step_cm\":"+ragdollNumber(state.heroMaxStep)+
    ",\"listeners\":"+noiseListenerRows(world));
  }

bool waterProbe(World& world,const Vec3& at,float minDepth,float maxDepth,Vec3& surface) {
  const auto floor=world.physic()->landRay(at+Vec3(0,180,0),6000.f);
  if(!floor.hasCol)return false;
  const auto water=world.physic()->waterRay(floor.v+Vec3(0,.1f,0),0);
  if(!water.hasCol || water.wdepth-floor.v.y<minDepth || water.wdepth-floor.v.y>maxDepth ||
     water.wdepth>at.y+180.f)return false;
  surface={at.x,water.wdepth,at.z};return true;
  }

bool waterFireballCaster(World& world,const Vec3& target,float targetRadius,Vec3& caster) {
  for(float radius:{700.f,900.f,1100.f,1300.f,1500.f,1700.f,1900.f,2100.f,2300.f,2500.f})
    for(unsigned angle=0;angle<64;++angle) {
      const float a=float(angle)*float(M_PI)/32.f;
      const auto probe=target+Vec3(std::cos(a)*radius,2000.f,std::sin(a)*radius);
      const auto floor=world.physic()->landRay(probe,4000.f);
      if(!floor.hasCol || floor.n.y<.99f || world.physic()->waterRay(floor.v,1.f).hasCol)continue;
      bool supported=true;
      for(float dx:{-35.f,35.f})for(float dz:{-35.f,35.f}) {
        const auto foot=world.physic()->landRay(floor.v+Vec3(dx,120,dz),160.f);
        if(!foot.hasCol || foot.n.y<.99f || std::abs(foot.v.y-floor.v.y)>10.f)supported=false;
        }
      if(!supported)continue;
      const auto obstruction=world.physic()->ray(floor.v+Vec3(0,140,0),target);
      if(obstruction.hasCol && (obstruction.v-target).length()>targetRadius+5.f)continue;
      caster=floor.v;return true;
      }
  return false;
  }

bool waterFindWindSite(Water& state,World& world,std::vector<Vec3>& places) {
  std::vector<const WayPoint*> points;
  const auto harbour=[](const std::string& name) {
    return name.find("HABOUR")!=std::string::npos || name.find("HARB")!=std::string::npos;
    };
  world.forEachWayPoint([&](const WayPoint& point) {
    const auto& name=point.name;
    if(harbour(name) || name.find("WATER")!=std::string::npos || name.find("LAKE")!=std::string::npos ||
       name.find("BEACH")!=std::string::npos || name.find("COAST")!=std::string::npos)
      points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[&](const auto* a,const auto* b) {
    if(harbour(a->name)!=harbour(b->name))return harbour(a->name);
    return a->name<b->name;
    });
  unsigned dryPoints=0,waterlines=0,diagnostics=0;
  for(const auto* point:points) {
    const auto support=world.physic()->landRay(point->pos+Vec3(0,400,0),3000.f);
    if(!support.hasCol || support.n.y<.75f || world.physic()->waterRay(support.v,1.f).hasCol)continue;
    ++dryPoints;
    for(unsigned angle=0;angle<16;++angle) {
      const Vec3 direction(std::cos(float(angle)*float(M_PI)/8.f),0,std::sin(float(angle)*float(M_PI)/8.f));
      for(float distance=25.f;distance<=1800.f;distance+=25.f) {
        Vec3 surface;
        if(!waterProbe(world,support.v+direction*distance,10.f,6000.f,surface))continue;
        ++waterlines;
        bool found=false;Vec3 lowDry(0,0,0);float bestDrop=std::numeric_limits<float>::infinity();
        // Trace this actual shoreline, rather than requiring the seven-body
        // deep-water grid. A low pier or bank avoids a destructive cliff fall.
        for(float d=std::max(0.f,distance-400.f);d<distance;d+=5.f) {
          const auto probe=support.v+direction*d;
          const auto floor=world.physic()->landRay(probe+Vec3(0,400,0),3000.f);
          if(!floor.hasCol || floor.n.y<.99f)continue;
          const float drop=floor.v.y-surface.y;
          bestDrop=std::min(bestDrop,drop);
          if(drop<0.f || drop>100.f || world.physic()->waterRay(floor.v,1.f).hasCol)continue;
          lowDry=floor.v;found=true;
          }
        if(diagnostics<24 || found) {
          ++diagnostics;
          event("water_wind_candidate","\"waypoint\":"+jsonStr(point->name)+
            ",\"authored_cm\":"+ragdollVector(point->pos)+",\"floor_cm\":"+ragdollVector(support.v)+
            ",\"floor_normal\":"+ragdollVector(support.n)+",\"water_cm\":"+ragdollVector(surface)+
            ",\"distance_cm\":"+ragdollNumber(distance)+",\"lowest_flat_drop_cm\":"+
            (std::isfinite(bestDrop)?ragdollNumber(bestDrop):"null")+",\"found_low_support\":"+(found?"true":"false")+
            ",\"low_dry_cm\":"+ragdollVector(lowDry));
          }
        if(found) {
          state.dry=lowDry;state.center=surface;state.direction=direction;state.waypoint=point->name;
          state.rayTop=std::max(point->pos.y,support.v.y)+400.f;
          places={surface};return true;
          }
        break;
        }
      }
    }
  event("water_wind_site_search","\"points\":"+std::to_string(points.size())+
    ",\"dry_points\":"+std::to_string(dryPoints)+",\"waterlines\":"+std::to_string(waterlines)+
    ",\"max_drop_cm\":100,\"min_depth_cm\":10");
  return false;
  }

bool waterFindSite(Water& state,World& world,bool shallow,unsigned count,std::vector<Vec3>& places,bool harbourOnly=false) {
  if(harbourOnly)return waterFindWindSite(state,world,places);
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    const auto& name=point.name;
    if(harbourOnly && name.find("HABOUR")==std::string::npos && name.find("HARB")==std::string::npos)return;
    if(name.find("HABOUR")!=std::string::npos || name.find("HARB")!=std::string::npos ||
       name.find("WATER")!=std::string::npos || name.find("LAKE")!=std::string::npos ||
       name.find("BEACH")!=std::string::npos || name.find("COAST")!=std::string::npos)
      points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[&](const auto* a,const auto* b) {
    // The deep-water film needs open water rather than the lake's reed beds.
    // Keep the shallow and thirty-body performance searches unchanged.
    if(!shallow && count==7) {
      const auto harbour=[](const std::string& name) {
        return name.find("HABOUR")!=std::string::npos || name.find("HARB")!=std::string::npos;
        };
      if(harbour(a->name)!=harbour(b->name))return harbour(a->name);
      }
    return a->name<b->name;
    });
  for(const auto* point:points) {
    const auto floor=world.physic()->landRay(point->pos+Vec3(0,180,0),700.f);
    if(!floor.hasCol || floor.n.y<.75f || world.physic()->waterRay(floor.v,1.f).hasCol)continue;
    const auto dry=floor.v;
    for(unsigned angle=0;angle<16;++angle) {
      const Vec3 direction(std::cos(float(angle)*float(M_PI)/8.f),0,std::sin(float(angle)*float(M_PI)/8.f));
      for(float distance=shallow?25.f:400.f;distance<=1800.f;distance+=shallow?25.f:100.f) {
        const auto candidate=dry+direction*distance;
        Vec3 center;
        if(!waterProbe(world,candidate,shallow?.5f:220.f,shallow?35.f:6000.f,center))continue;
        if(world.physic()->ray(dry+Vec3(0,160,0),Vec3(center.x,dry.y+160,center.z)).hasCol)continue;
        places.clear();
        if(shallow) {
          // Follow the real shoreline contour. Each measured object has its
          // own water/bottom sample; a quay with no shallows is rejected.
          for(int radius=0;radius<=1200 && places.size()<count;radius+=40)
            for(unsigned a=0;a<32 && places.size()<count;++a) {
              Vec3 at;
              const auto probe=center+Vec3(std::cos(float(a)*float(M_PI)/16.f)*float(radius),0,
                                          std::sin(float(a)*float(M_PI)/16.f)*float(radius));
              if(!waterProbe(world,probe,.5f,35.f,at))continue;
              if(std::any_of(places.begin(),places.end(),[&](const Vec3& used){return (used-at).length()<190.f;}))continue;
              places.push_back(at);
              }
          }
        else {
          const auto side=Vec3(-direction.z,0,direction.x);
          for(unsigned i=0;i<count;++i) {
            const auto offset=count==30?side*((float(i%6)-2.5f)*155.f)+direction*((float(i/6)-2.f)*155.f):
              side*((float(i%4)-1.5f)*230.f)+direction*((float(i/4)-.5f)*230.f);
            Vec3 at;
            if(!waterProbe(world,center+offset,220.f,6000.f,at))break;
            places.push_back(at);
            }
          }
        if(places.size()!=count)continue;
        if(!shallow && count==7 && !harbourOnly) {
          // Dense items need a level real seabed for the rest measurement.
          // A sloping harbour floor can correctly keep ore sliding forever.
          const auto levelProbe=[&](const Vec3& probe,Vec3& surface,float& floorY) {
            const auto support=world.physic()->landRay(probe+Vec3(0,180,0),6000.f);
            if(!support.hasCol || support.n.y<.99f)return false;
            const auto water=world.physic()->waterRay(support.v+Vec3(0,.1f,0),0);
            if(!water.hasCol || water.wdepth-support.v.y<220.f || water.wdepth>probe.y+180.f)return false;
            surface={probe.x,water.wdepth,probe.z};floorY=support.v.y;return true;
            };
          bool densePlaced=true;
          for(const unsigned index:{4u,5u}) {
            const auto original=places[index];
            bool found=false;
            for(int radius=0;radius<=1800 && !found;radius+=50)
              for(unsigned a=0;a<32 && !found;++a) {
                const auto probe=original+Vec3(std::cos(float(a)*float(M_PI)/16.f)*float(radius),0,
                                               std::sin(float(a)*float(M_PI)/16.f)*float(radius));
                Vec3 at;float floorY=0;
                if(!levelProbe(probe,at,floorY) || (at-dry).quadLength()>2500.f*2500.f)continue;
                bool separate=true;
                for(size_t other=0;other<places.size();++other)
                  if(other!=index && (places[other]-at).length()<190.f) {separate=false;break;}
                if(!separate)continue;
                float lowest=floorY,highest=floorY;
                bool level=true;
                for(float dx:{-20.f,20.f}) for(float dz:{-20.f,20.f}) {
                  Vec3 corner;float cornerY=0;
                  if(!levelProbe(at+Vec3(dx,0,dz),corner,cornerY) || std::abs(corner.y-at.y)>.1f) {level=false;break;}
                  lowest=std::min(lowest,cornerY);highest=std::max(highest,cornerY);
                  }
                if(!level || highest-lowest>2.f)continue;
                places[index]=at;found=true;
                }
            if(!found) {densePlaced=false;break;}
            }
          if(!densePlaced)continue;
          // Reject slots under moored boats or behind a quay before moving
          // anything. The same supported sight line is checked at the cast.
          Vec3 caster;
          if(!waterFireballCaster(world,places[0],0.f,caster))continue;
          }
        state.dry=dry;state.center=center;state.direction=direction;state.waypoint=point->name;
        return true;
        }
      }
    }
  return false;
  }

bool waterPlaceMap(Water& state,const WaterContext& context,World& world,const DynamicWorld::MovableVobSnapshot& body,const Vec3& surface,std::string label) {
  auto matrix=Matrix4x4::mkIdentity();
  const auto center=(body.localBounds[0]+body.localBounds[1])*.5f;
  matrix.translate(surface.x-center.x,surface.y-body.localBounds[0].y+55.f,surface.z-center.z);
  if(!world.physic()->setMovableVobTransform(body.zenId,matrix))return false;
  // Restoring an authored map pose intentionally leaves it asleep. This
  // fixture places it in mid-air, so provide an ordinary tiny native impulse
  // before observing gravity and water. The hull COM can differ from bbox COM.
  auto worldCenter=center;matrix.project(worldCenter);
  const float radius=(body.localBounds[1]-body.localBounds[0]).length()*.5f+1.f;
  const auto awakened=world.physic()->applyMovableVobImpulse(worldCenter,radius,
    [mass=body.mass](const Vec3&,float targetMass) {
      return std::abs(targetMass-mass)<.001f ? Vec3(0,-.1f*mass,0) : Vec3();
      });
  if(awakened!=1)return false;
  state.ids.insert(body.id);state.labels[body.id]=std::move(label);
  context.movableIds.push_back(body.id);context.movableMapIds.insert(body.id);
  return true;
  }

void waterTorch(Water& state,World& world,Npc& player) {
  if(!state.ready || state.caseName!="deep" || state.items.size()!=4) {
    arenaAbort("water torch requires the seven-object deep-water fixture");return;
    }
  const auto symbol=world.script().findSymbolIndex("ItLsTorchburning");
  if(symbol==size_t(-1)) {arenaAbort("water torch script instance is unavailable");return;}
  const auto before=world.physic()->waterBodySnapshot();
  Vec3 surface;bool found=false;
  for(int radius=0;radius<=900 && !found;radius+=50)
    for(unsigned angle=0;angle<32 && !found;++angle) {
      const auto probe=state.center+Vec3(std::cos(float(angle)*float(M_PI)/16.f)*float(radius),0,
                                             std::sin(float(angle)*float(M_PI)/16.f)*float(radius));
      if(!waterProbe(world,probe,220.f,6000.f,surface) ||
         (surface-state.dry).quadLength()>2500.f*2500.f)continue;
      found=std::none_of(before.begin(),before.end(),[&](const auto& body) {
        if(!waterOwns(state,body))return false;
        auto delta=movablePosition(body.transform)-surface;delta.y=0;
        return delta.quadLength()<190.f*190.f;
        });
      }
  if(!found) {arenaAbort("water torch has no separate real deep-water position");return;}
  auto matrix=Matrix4x4::mkIdentity();matrix.translate(surface+Vec3(0,100,0));
  auto* item=world.addItemDyn(symbol,matrix,world.npcId(&player));
  if(!item || !item->isTorchBurn() || item->physicsMass()<=0) {
    arenaAbort("water torch lacks the burning subtype or native dynamic body");return;
    }
  uintptr_t id=0;
  for(const auto& body:world.physic()->waterBodySnapshot())
    if(body.category==DynamicWorld::C_Item &&
       std::none_of(before.begin(),before.end(),[&](const auto& previous){return previous.id==body.id;})) {
      if(id) {arenaAbort("water torch inserted more than one native body");return;}
      id=body.id;
      }
  if(!id) {arenaAbort("water torch inserted no native body");return;}
  state.items.push_back(item);state.ids.insert(id);state.labels[id]="torch";
  event("water_torch_inserted","\"id\":"+std::to_string(id)+",\"item_instance\":"+std::to_string(symbol)+
    ",\"torch_burning\":true,\"surface_cm\":"+ragdollVector(surface)+",\"bodies\":"+waterRows(state,world));
  }

void prepareWaterFixture(Water& state,const WaterContext& context,World& world,Npc& player,std::string_view mode) {
  if(state.ready || (mode!="deep" && mode!="shallow" && mode!="perf" && mode!="wind")) {
    arenaAbort("water setup expects a fresh fixture and deep, shallow, perf or wind");return;
    }
  const bool shallow=mode=="shallow",perf=mode=="perf",harbourOnly=mode=="wind";
  std::vector<Vec3> places;
  if(!waterFindSite(state,world,shallow,perf?30u:harbourOnly?1u:7u,places,harbourOnly)) {
    arenaAbort(shallow?"water fixture cannot find seven real shallow-water positions":
                       "water fixture cannot find a sufficiently deep unobstructed stock-world water area");return;
    }
  const auto all=world.physic()->movableVobSnapshot();
  const auto native=world.physic()->waterBodySnapshot();
  std::vector<DynamicWorld::MovableVobSnapshot> selected;
  if(perf) {
    // Performance does not save. Thirty copies keep ON/OFF meshes and masses
    // identical without depending on how many authored barrels pass contact
    // validation in a particular compatible old save.
    std::string visual;
    float density=std::numeric_limits<float>::infinity();
    for(const auto& body:all) {
      if(!body.enabled || body.broken || body.className!="barrel")continue;
      for(const auto& sample:native)
        if(sample.parentId==body.id && sample.density<density) {visual=body.visual;density=sample.density;}
      }
    if(visual.empty()) {arenaAbort("water performance fixture requires an available native barrel mesh");return;}
    for(unsigned i=0;i<30;++i) {
      const auto id=destructionAdd(world,visual,places[i]+Vec3(0,55,0));
      if(!id) {arenaAbort("water performance barrel insertion failed");return;}
      auto& handle=context.destructionFixtures.back()->body;
      if(!handle.addImpulse(Vec3(0,-.1f*handle.mass(),0))) {
        arenaAbort("water performance barrel could not be activated");return;
        }
      state.ids.insert(id);state.labels[id]="barrel_"+std::to_string(i);
      }
    }
  else for(const std::string name:{"crate","barrel","table"}) {
    if(harbourOnly && name!="barrel")continue;
    const DynamicWorld::MovableVobSnapshot* found=nullptr;
    float area=std::numeric_limits<float>::infinity(),density=std::numeric_limits<float>::infinity();
    for(const auto& body:all) {
      if(!body.enabled || body.broken || body.zenId==uint32_t(-1) || body.className!=name)continue;
      const auto extent=body.localBounds[1]-body.localBounds[0];
      const float footprint=extent.x*extent.z;
      float rho=std::numeric_limits<float>::infinity();
      for(const auto& sample:native)if(sample.parentId==body.id) {rho=sample.density;break;}
      if((name=="barrel" && (rho<density || (rho==density && footprint<area))) ||
         (name!="barrel" && footprint<area)) {found=&body;area=footprint;density=rho;}
      }
    if(!found) {arenaAbort("water fixture lacks an enabled saved map "+name);return;}
    selected.push_back(*found);
    }
  for(size_t i=0;i<selected.size();++i) {
    if(!waterPlaceMap(state,context,world,selected[i],places[i],perf?"barrel_"+std::to_string(i):selected[i].className)) {
      arenaAbort("water fixture cannot move its saved map body");return;
      }
    if(selected[i].className=="barrel")state.barrelZen=selected[i].zenId;
    if(selected[i].className=="crate")state.crateZen=selected[i].zenId;
    }
  if(!perf && !harbourOnly) {
    const char* symbols[]{"ITRW_ARROW","ITMW_1H_SLD_SWORD","ITMI_STONE","ITMI_LUTE"};
    const char* labels[]{"arrow","sword","stone","wood_item"};
    for(unsigned i=0;i<4;++i) {
      auto symbol=world.script().findSymbolIndex(symbols[i]);
      if(symbol==size_t(-1) && i==1)
        for(size_t candidate=0;candidate<world.script().symbolsCount();++candidate) {
          const auto* entry=world.script().findSymbol(candidate);
          if(!entry || entry->type()!=daedalus::DaedalusDataType::INSTANCE)continue;
          auto name=entry->name();for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
          if(name.starts_with("ITMW_") && name.find("SWORD")!=std::string::npos) {symbol=candidate;break;}
          }
      if(symbol==size_t(-1) && i==2)symbol=world.script().findSymbolIndex("ITMI_NUGGET");
      if(symbol==size_t(-1)) {arenaAbort("water fixture item script instance missing: "+std::string(symbols[i]));return;}
      const auto before=world.physic()->waterBodySnapshot();
      auto matrix=Matrix4x4::mkIdentity();matrix.translate(places[i+3]+Vec3(0,100,0));
      auto* item=world.addItemDyn(symbol,matrix,world.npcId(&player));
      if(!item) {arenaAbort("water fixture world Item insertion failed");return;}
      state.items.push_back(item);
      bool found=false;
      for(const auto& body:world.physic()->waterBodySnapshot())
        if(body.category==DynamicWorld::C_Item &&
           std::none_of(before.begin(),before.end(),[&](const auto& previous){return previous.id==body.id;})) {
          state.ids.insert(body.id);state.labels[body.id]=labels[i];found=true;
          }
      if(!found) {arenaAbort("water fixture world Item has no native dynamic body");return;}
      }
    }
  player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
  player.setPosition(state.dry);player.setDirection(state.center-state.dry);player.updateTransform();
  // Preserve native hearing for the splash; unrelated routines cannot walk
  // into the caster or move the authored fixtures during observation.
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && (npc->position()-state.center).length()<2500.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  state.caseName=mode;state.start=world.tickCount();state.ready=true;state.trace=true;
  noiseBegin(world,"water-entry",state.center);
  context.movableReady=true;context.movableCenter=state.center;
  context.cam=C_Hold;
  if(mode=="deep") {
    // Keep the optical path over open water, above the shore's reeds.
    context.cam=C_Water;state.cameraTarget=state.center;
    lookFrom(state.center+state.direction*200.f+Vec3(0,1000,0),state.center);
    }
  else
    lookFrom(state.center-state.direction*600.f+Vec3(300,360,0),state.center+Vec3(0,25,0));
  event("water_setup","\"case\":"+jsonStr(mode)+",\"waypoint\":"+jsonStr(state.waypoint)+
    ",\"dry_cm\":"+ragdollVector(state.dry)+",\"center_cm\":"+ragdollVector(state.center)+
    ",\"bodies\":"+waterRows(state,world));
  }

void waterInitial(Water& state,const WaterContext& context,World& world,Npc& player,PlayerControl& ctrl,std::string_view label) {
  const auto savedGoal=player.navigationGoal();
  const auto savedPosition=player.position();
  const auto savedFloor=world.physic()->landRay(savedPosition+Vec3(0,400,0),1000.f);
  const bool savedSlide=player.isSlide();
  std::vector<Vec3> observed;
  state.caseName="reload";state.start=world.tickCount();state.trace=true;
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    auto delta=movablePosition(body.transform)-player.position();delta.y=0;
    // Capture sleepers inside the ordinary thirty-metre debris lifetime area
    // before selecting a dry observer within 27 m of every observed body.
    if(delta.length()>2900.f || !body.inWater)continue;
    state.ids.insert(body.id);
    state.labels[body.id]=body.visual;
    observed.push_back((body.worldBounds[0]+body.worldBounds[1])*.5f);
    }
  if(state.ids.empty()) {arenaAbort("water reload has no nearby saved native objects");return;}
  waterCheckpoint(state,context,world,label);
  // The saved hero may stand on a real sliding slope. After the untouched
  // snapshot, select a flat native dry floor while keeping every observed
  // object comfortably inside the ordinary thirty-metre debris lifetime area.
  const auto flatDry=[&](const Vec3& probe,Vec3& stand) {
    const auto floor=world.physic()->landRay(probe+Vec3(0,400,0),1000.f);
    if(!floor.hasCol || floor.n.y<.99f)return false;
    stand=floor.v;
    if(std::any_of(observed.begin(),observed.end(),[&](const Vec3& body){return (body-stand).length()>2700.f;}))return false;
    float lowest=stand.y,highest=stand.y;
    for(float dx:{-35.f,0.f,35.f})for(float dz:{-35.f,0.f,35.f}) {
      const auto support=world.physic()->landRay(stand+Vec3(dx,180,dz),400.f);
      if(!support.hasCol || support.n.y<.99f)return false;
      const auto water=world.physic()->waterRay(support.v+Vec3(0,.1f,0),0);
      if(water.hasCol && water.wdepth>support.v.y)return false;
      if(world.physic()->ray(support.v+Vec3(0,5,0),support.v+Vec3(0,190,0)).hasCol)return false;
      lowest=std::min(lowest,support.v.y);highest=std::max(highest,support.v.y);
      }
    return highest-lowest<=10.f;
    };
  Vec3 observer;bool supported=false;
  for(int radius=0;radius<=1800 && !supported;radius+=50)
    for(unsigned a=0;a<32 && !supported;++a) {
      const auto probe=savedPosition+Vec3(std::cos(float(a)*float(M_PI)/16.f)*float(radius),0,
                                          std::sin(float(a)*float(M_PI)/16.f)*float(radius));
      supported=flatDry(probe,observer);
      }
  if(!supported) {arenaAbort("water reload observer has no nearby flat dry native floor");return;}
  ctrl.clearInput();context.holds.clear();context.taps.clear();context.ctrl=&ctrl;
  player.clearState(false);player.clearAiQueue();player.closeWeapon(true);
  player.stopWalking();player.clearSpeed();player.setExternalControl(true);
  player.setPosition(observer);player.updateTransform();
  unsigned frozen=0;
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && (npc->position()-observer).length()<4000.f) {
      npc->clearAiQueue();npc->clearState(false);npc->setTarget(nullptr);npc->stopWalking();npc->clearSpeed();
      npc->setExternalControl(true);++frozen;
      }
  state.observer=true;state.observerStart=player.position();state.observerSample=world.tickCount()+1000;
  const auto observerFloor=world.physic()->landRay(observer+Vec3(0,180,0),400.f);
  event("water_observer_idle","\"hero_cm\":"+ragdollVector(player.position())+
    ",\"saved_hero_cm\":"+ragdollVector(savedPosition)+",\"saved_floor_normal\":"+ragdollVector(savedFloor.n)+
    ",\"saved_slide\":"+(savedSlide?"true":"false")+",\"observer_floor_normal\":"+ragdollVector(observerFloor.n)+
    ",\"nearby_npcs_paused\":"+std::to_string(frozen)+
    ",\"saved_goal_active\":"+(savedGoal.active?"true":"false")+
    ",\"saved_goal_kind\":"+jsonStr(savedGoal.kind)+",\"saved_goal_cm\":"+ragdollVector(savedGoal.position));

  }

std::string waterActorRows(Water& state,World& world) {
  std::string rows="[";
  const auto samples=world.physic()->waterBodySnapshot();
  for(const auto& actor:state.actors) {
    if(rows.size()>1)rows+=',';
    const auto& npc=*actor.npc;const auto* body=npc.ragdollBody();
    float torso=0,torsoMass=0;
    const auto* skeleton=npc.visualSkeleton();
    for(const auto& sample:samples) {
      if(sample.ragdoll!=body || !skeleton || sample.bone>=skeleton->nodes.size())continue;
      auto name=skeleton->nodes[sample.bone].name;
      for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
      if(name.find("SPINE")==std::string::npos && name.find("PELVIS")==std::string::npos)continue;
      torso+=sample.submergedFraction*sample.mass;torsoMass+=sample.mass;
      }
    rows+="{\"actor_id\":"+std::to_string(world.npcId(&npc))+",\"role\":"+jsonStr(actor.role)+
      ",\"symbol\":"+jsonStr(symName(world,&npc))+",\"dead\":"+(npc.isDead()?"true":"false")+
      ",\"unconscious\":"+(npc.isUnconscious()?"true":"false")+
      ",\"part_count\":"+std::to_string(body?body->partCount():0)+
      ",\"position_cm\":"+ragdollVector(npc.position())+
      ",\"sleeping\":"+(body && body->isSleeping()?"true":"false")+
      ",\"first_sleep_ms\":"+(actor.sleepAt==uint64_t(-1)?"null":std::to_string(actor.sleepAt))+
      ",\"torso_submerged_fraction\":"+(torsoMass>0?ragdollNumber(torso/torsoMass):"null")+
      ",\"parts\":[";
    if(body) {
      const auto state=body->state();
      for(size_t i=0;i<state.parts.size();++i) {
        const auto& part=state.parts[i];if(i)rows+=',';
        rows+="{\"bone\":"+std::to_string(part.bone)+",\"transform\":"+ragdollMatrix(part.transform)+
          ragdollNativePrecision(part)+",\"angular_velocity_rad_s\":"+ragdollVector(part.angularVelocity)+
          ",\"sleeping\":"+(part.sleeping?"true":"false")+"}";
        }
      }
    rows+="]}";
    }
  return rows+"]";
  }

void waterBodiesCheckpoint(Water& state,const WaterContext& context,World& world,std::string_view label) {
  waterFields(state,context,world);
  event("water_bodies_checkpoint","\"label\":"+jsonStr(label)+",\"case\":"+jsonStr(state.caseName)+
    ",\"elapsed_ms\":"+std::to_string(world.tickCount()-state.start)+
    ",\"buoyancy_enabled\":"+(world.physic()->buoyancyEnabled()?"true":"false")+
    ",\"actors\":"+waterActorRows(state,world)+",\"bodies\":"+waterRows(state,world));
  }

bool waterBodiesAdd(Water& state,const WaterContext& context,World& world,Npc& player,std::string_view symbol,std::string role,const Vec3& at,bool dead) {
  const auto instance=world.script().findSymbolIndex(symbol);
  if(instance==size_t(-1)) {arenaAbort("water corpse instance is unavailable: "+std::string(symbol));return false;}
  auto* npc=world.addNpc(instance,at);
  if(!npc) {arenaAbort("water corpse insertion failed");return false;}
  npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
  npc->handle().flags=daedalus::NpcFlag(uint32_t(npc->handle().flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
  npc->setDirection(state.direction);npc->updateTransform();
  if(npc->isHuman()) {
    const auto apple=world.script().findSymbolIndex("ITFO_APPLE");
    if(apple!=size_t(-1))npc->addItem(apple,2);
    }
  state.actors.push_back({npc,std::move(role)});
  context.crew.push_back(npc);
  if(dead) {
    npc->setLastHitBy(&player);
    if(!npc->setRagdollDown(true)) {arenaAbort("water corpse production death path refused");return false;}
    npc->prepareRagdoll();
    if(!npc->ragdollBody()) {arenaAbort("water corpse has no supported native ragdoll");return false;}
    }
  return true;
  }

void prepareWaterBodiesFixture(Water& state,const WaterContext& context,World& world,Npc& player,std::string_view mode) {
  if(state.ready || state.bodiesReady || (mode!="deep" && mode!="shallow" && mode!="perf")) {
    arenaAbort("water-bodies setup expects a fresh deep, shallow or perf fixture");return;
    }
  std::vector<Vec3> places;
  if(mode=="deep") {
    // Use the existing low harbour shore search: the inland quest-return lake
    // has rock and lilies between the camera and the floating body.
    if(context.spellActors.empty() || !waterFindWindSite(state,world,places)) {
      arenaAbort("water corpse film requires a real low harbour water lip");return;
      }
    if(state.waypoint.find("HABOUR")==std::string::npos && state.waypoint.find("HARB")==std::string::npos) {
      arenaAbort("water corpse film requires the harbour rather than the inland lake");return;
      }
    const auto lip=state.dry;
    const auto direction=state.direction;
    const auto side=Vec3(-direction.z,0,direction.x);
    Vec3 caster;bool supported=false;
    for(float distance:{150.f,200.f,250.f,300.f}) {
      const auto floor=world.physic()->landRay(lip-direction*distance+Vec3(0,400,0),600.f);
      if(!floor.hasCol || floor.n.y<.99f || world.physic()->waterRay(floor.v,1.f).hasCol ||
         world.physic()->ray(floor.v+Vec3(0,140,0),lip+Vec3(0,100,0)).hasCol)continue;
      bool flat=true;
      for(float dx:{-30.f,30.f})for(float dz:{-30.f,30.f}) {
        const auto foot=world.physic()->landRay(floor.v+Vec3(dx,100,dz),150.f);
        if(!foot.hasCol || foot.n.y<.99f || std::abs(foot.v.y-floor.v.y)>5.f ||
           world.physic()->waterRay(foot.v,1.f).hasCol)flat=false;
        }
      if(flat) {caster=floor.v;supported=true;break;}
      }
    if(!supported) {arenaAbort("water corpse harbour lip has no supported dry WindFist caster");return;}
    bool found=false;
    for(float distance=100.f;distance<=1800.f && !found;distance+=25.f) {
      Vec3 center,wolf,skeleton;
      if(!waterProbe(world,lip+direction*distance,220.f,6000.f,center) ||
         !waterProbe(world,center+side*220.f,220.f,6000.f,wolf) ||
         !waterProbe(world,center-side*220.f,220.f,6000.f,skeleton))continue;
      state.center=center;places={wolf,skeleton};found=true;
      }
    if(!found) {arenaAbort("water corpse lip has no three separated deep-water slots");return;}
    auto* human=context.spellActors[0].npc;
    for(size_t i=1;i<context.spellActors.size();++i)world.removeNpc(*context.spellActors[i].npc);
    context.spellActors.resize(1);context.crew.clear();context.crew.push_back(human);
    human->handle().flags=daedalus::NpcFlag(uint32_t(human->handle().flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
    human->setPosition(lip);human->setDirection(direction);human->updateTransform();
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
    player.setPosition(caster);player.setDirection(direction);player.updateTransform();
    context.questWater={true,lip,direction,state.center.y,state.waypoint};
    state.dry=caster;
    const auto apple=world.script().findSymbolIndex("ITFO_APPLE");
    if(apple!=size_t(-1))human->addItem(apple,2);
    state.actors.push_back({human,"human"});
    context.spellGuardTrace=false;
    if(!waterBodiesAdd(state,context,world,player,"Wolf","wolf",places[0]+Vec3(0,55,0)) ||
       !waterBodiesAdd(state,context,world,player,"Skeleton","skeleton",places[1]+Vec3(0,55,0)))return;
    }
  else {
    const bool shallow=mode=="shallow";
    if(!waterFindSite(state,world,shallow,shallow?1u:10u,places)) {
      arenaAbort("water corpse fixture cannot find real separated water positions");return;
      }
    player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
    player.setPosition(state.dry);player.setDirection(state.center-state.dry);player.updateTransform();
    context.crew.clear();
    for(size_t i=0;i<places.size();++i)
      if(!waterBodiesAdd(state,context,world,player,"BAU_904_BAUER",shallow?"human":"human_"+std::to_string(i),
                         places[i]+Vec3(0,shallow?5.f:55.f,0)))return;
    }
  state.caseName="bodies-"+std::string(mode);state.start=world.tickCount();
  state.ready=true;state.bodiesReady=true;state.trace=true;
  context.movableReady=true;context.movableCenter=state.center;
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && !npc->isDown() &&
       (npc->position()-state.center).length()<2500.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  noiseBegin(world,"water-body-entry",state.center);
  context.cam=C_Water;state.cameraTarget=state.center;
  lookFrom(state.center+state.direction*150.f+Vec3(0,850,0),state.center);
  event("water_bodies_setup","\"case\":"+jsonStr(mode)+",\"waypoint\":"+jsonStr(state.waypoint)+
    ",\"dry_cm\":"+ragdollVector(state.dry)+",\"center_cm\":"+ragdollVector(state.center)+
    ",\"camera_cm\":"+ragdollVector(context.camPos)+",\"actors\":"+waterActorRows(state,world));
  }

void waterBodiesInitial(Water& state,const WaterContext& context,World& world,Npc& player,std::string_view label) {
  state.start=world.tickCount();state.caseName="bodies-reload";
  state.trace=true;state.ready=true;state.bodiesReady=true;
  Vec3 center;size_t count=0;
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* npc=world.npcById(id);
    if(!npc || !npc->isDead() || !npc->ragdollBody() || (npc->position()-player.position()).length()>3000.f)continue;
    const auto* rig=npc->ragdollBody();bool wet=false;
    for(const auto& part:world.physic()->waterBodySnapshot())if(part.ragdoll==rig && part.inWater) {wet=true;break;}
    if(!wet)continue;
    auto symbol=symName(world,npc);for(auto& c:symbol)c=char(std::toupper(static_cast<unsigned char>(c)));
    const std::string role=npc->isHuman()?"human":symbol.find("WOLF")!=std::string::npos?"wolf":
      symbol.find("SKELETON")!=std::string::npos?"skeleton":symbol;
    state.actors.push_back({npc,role});center+=movablePosition(rig->pelvisTransform());++count;
    }
  if(!count) {arenaAbort("water body reload has no nearby saved wet corpses");return;}
  // Capture the untouched native pose before restoring transient bench control.
  waterBodiesCheckpoint(state,context,world,label);
  for(auto& actor:state.actors)actor.npc->setExternalControl(true);
  player.clearAiQueue();player.stopWalking();player.clearSpeed();player.setExternalControl(true);
  state.center=center/float(count);context.cam=C_Water;state.cameraTarget=state.center;
  lookFrom(state.center+Vec3(0,850,150),state.center);
  }

Npc* waterLootCorpse(Water& state) {
  for(auto& actor:state.actors)
    if(actor.npc->isHuman() && actor.npc->isDead() && actor.npc->ragdollBody())return actor.npc;
  return nullptr;
  }

void waterBodiesLootReady(Water& state,World& world,Npc& player,std::string_view mode) {
  state.lootMode=mode;state.lootReady=false;
  auto* corpse=waterLootCorpse(state);
  if(!corpse || (mode!="shore" && mode!="swim")) {arenaAbort("water corpse loot expects a human and shore or swim");return;}
  const auto center=movablePosition(corpse->ragdollBody()->pelvisTransform());
  Vec3 at;bool found=false;
  if(mode=="swim") {
    Vec3 surface;
    if(waterProbe(world,center,160.f,6000.f,surface)) {
      at=surface-Vec3(0,100,0)+Vec3(100,0,0);found=true;
      }
    }
  else for(float radius:{80.f,120.f,160.f,200.f,240.f}) {
    for(unsigned angle=0;angle<32;++angle) {
      const auto probe=center+Vec3(std::cos(float(angle)*float(M_PI)/16.f)*radius,400.f,
                                   std::sin(float(angle)*float(M_PI)/16.f)*radius);
      const auto floor=world.physic()->landRay(probe,1000.f);
      if(!floor.hasCol || floor.n.y<.98f || world.physic()->waterRay(floor.v,1.f).hasCol ||
         (floor.v-center).length()>300.f)continue;
      at=floor.v;found=true;break;
      }
    if(found)break;
    }
  event("water_bodies_loot_position","\"mode\":"+jsonStr(mode)+",\"available\":"+(found?"true":"false")+
    ",\"actor_id\":"+std::to_string(world.npcId(corpse))+",\"corpse_cm\":"+ragdollVector(center)+
    ",\"hero_cm\":"+(found?ragdollVector(at):"null"));
  if(!found)return;
  player.setExternalControl(false);player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
  player.setPosition(at);player.setDirection(center-at);player.updateTransform();
  state.lootReady=true;
  }

void waterBodiesLoot(Water& state,const WaterContext& context,World& world,Npc& player,PlayerControl& ctrl) {
  auto* corpse=waterLootCorpse(state);
  if(!state.lootReady || !corpse) {
    event("water_bodies_loot","\"mode\":"+jsonStr(state.lootMode)+",\"available\":false");return;
    }
  ctrl.clearFocus();ctrl.tickFocus();const auto focus=ctrl.focus();
  const auto inventory=ragdollInventory(*corpse,world);
  context.expectedRagdollLoot=corpse;context.ragdollLootDidOpen=false;
  const bool interacted=focus.npc==corpse && ctrl.interact(*corpse);
  context.expectedRagdollLoot=nullptr;
  event("water_bodies_loot","\"mode\":"+jsonStr(state.lootMode)+",\"available\":true"+
    ",\"actor_id\":"+std::to_string(world.npcId(corpse))+",\"focus_actor_id\":"+std::to_string(world.npcId(focus.npc))+
    ",\"hero_swimming\":"+(player.isSwim()?"true":"false")+
    ",\"interacted\":"+(interacted?"true":"false")+",\"opened\":"+(context.ragdollLootDidOpen?"true":"false")+
    ",\"inventory_unchanged\":"+(inventory==ragdollInventory(*corpse,world)?"true":"false")+
    ",\"lootable_inventory\":"+ragdollInventory(*corpse,world,Inventory::T_Ransack));
  }

void prepareWaterSpell(Water& state,const WaterContext& context,World& world,Npc& player,bool wind) {
  if(!state.ready) {arenaAbort("water real spell needs its fixture");return;}
  const auto all=world.physic()->movableVobSnapshot();
  const auto zen=wind?state.barrelZen:state.crateZen;
  const auto found=std::find_if(all.begin(),all.end(),[&](const auto& body){return body.zenId==zen;});
  if(found==all.end() || found->broken) {arenaAbort("water spell map target is unavailable");return;}
  auto target=(found->localBounds[0]+found->localBounds[1])*.5f;found->transform.project(target);
  Vec3 caster=state.dry,windDummy;
  if(!wind) {
    // A quay or a moored ship may block the route from the original dry WP.
    // Find a real supported firing position with a clear path to this crate.
    const float targetRadius=(found->localBounds[1]-found->localBounds[0]).length()*.5f;
    if(!waterFireballCaster(world,target,targetRadius,caster)) {
      arenaAbort("water fireball requires supported dry ground and a clear native shot path");return;
      }
    }
  if(wind) {
    // Follow the real terrain to the waterline, including any dry terraces
    // above it. A height change alone is not the edge of the water.
    const auto direction=Vec3::normalize(Vec3(target.x,state.dry.y,target.z)-state.dry);
    bool supported=false,reachedWater=false;Vec3 lip=state.dry;
    float shoreWaterY=state.center.y;
    for(float d=0;d<=1800.f;d+=5.f) {
      const auto at=state.dry+direction*d;
      const auto floor=world.physic()->landRay(at+Vec3(0,400,0),3000.f);
      if(!floor.hasCol)continue;
      const auto water=world.physic()->waterRay(floor.v+Vec3(0,.1f,0),0);
      if(water.hasCol && water.wdepth>floor.v.y) {reachedWater=true;shoreWaterY=water.wdepth;break;}
      lip=floor.v;supported=true;
      }
    if(!supported || !reachedWater) {arenaAbort("water wind barrel has no supported real shoreline lip");return;}
    const auto extent=(found->localBounds[1]-found->localBounds[0])*.5f;
    // The public impulse callback exposes native COM. Returning zero reads
    // it without waking the body, applying a force or damaging the barrel.
    Vec3 center;float nearest=extent.quadLength()+1.f;bool hasCenter=false;
    world.physic()->applyMovableVobImpulse(target,extent.length()+1.f,[&](const Vec3& nativeCenter,float mass) {
      const auto distance=(nativeCenter-target).quadLength();
      if(std::abs(mass-found->mass)<.001f && distance<nearest) {
        center=nativeCenter;nearest=distance;hasCenter=true;
        }
      return Vec3();
      });
    if(!hasCenter) {arenaAbort("water wind barrel has no native centre of mass");return;}
    auto inverse=found->transform;inverse.inverse();inverse.project(center);
    // Find the nearest small dry support footprint behind the real edge.
    // The rest of the hull may overhang, so normal wind still reaches water.
    const auto side=Vec3(-direction.z,0,direction.x);
    // The shoreline can be several metres below the dry dock. Start every
    // placement ray above both levels so it can still find that upper support.
    const auto shoreFloor=[&](const Vec3& probe) {
      return world.physic()->landRay(Vec3(probe.x,std::max(probe.y+400.f,state.rayTop),probe.z),4000.f);
      };
    unsigned supportTrials=0,supportNoFloor=0,supportSteep=0,supportWet=0,supportUneven=0,supportRejected=0;
    std::string supportRejections="[";
    const auto rejectSupport=[&](std::string_view why,const Vec3& probe,const DynamicWorld::RayLandResult& support,bool hasWater,float waterY) {
      if(supportRejected>=3)return;
      if(supportRejected++)supportRejections+=',';
      supportRejections+="{\"reason\":"+jsonStr(why)+",\"probe_cm\":"+ragdollVector(probe)+
        ",\"floor_found\":"+(support.hasCol?"true":"false")+",\"floor_cm\":"+ragdollVector(support.v)+
        ",\"floor_normal\":"+ragdollVector(support.n)+",\"water_found\":"+(hasWater?"true":"false")+
        ",\"water_y_cm\":"+(hasWater?ragdollNumber(waterY):"null")+"}";
      };
    const auto drySupport=[&](const Vec3& probe,DynamicWorld::RayLandResult& support) {
      support=shoreFloor(probe);
      if(!support.hasCol) {++supportNoFloor;rejectSupport("no_floor",probe,support,false,0);return false;}
      const auto water=world.physic()->waterRay(support.v+Vec3(0,.1f,0),0);
      if(support.n.y<.99f) {++supportSteep;rejectSupport("steep_floor",probe,support,water.hasCol,water.wdepth);return false;}
      if(water.hasCol && water.wdepth>support.v.y) {++supportWet;rejectSupport("wet_floor",probe,support,true,water.wdepth);return false;}
      if(support.v.y-shoreWaterY>100.f) {++supportUneven;rejectSupport("high_above_water",probe,support,true,shoreWaterY);return false;}
      return true;
      };
    Vec3 at,floorPoint,floorNormal;float chosenRetreat=0,chosenLateral=0;bool footprint=false;
    for(float retreat=50.f;retreat<=250.f && !footprint;retreat+=10.f)
      for(float lateral:{0.f,-50.f,50.f,-100.f,100.f,-150.f,150.f,-200.f,200.f,-250.f,250.f,-300.f,300.f}) {
      if(footprint)break;
      ++supportTrials;
      const auto candidate=lip-direction*retreat+side*lateral;
      DynamicWorld::RayLandResult floor;
      if(!drySupport(candidate,floor))continue;
      bool supported=true;float lowest=floor.v.y,highest=floor.v.y;
      for(float across:{-extent.x,0.f,extent.x})for(float along:{-extent.z,0.f,extent.z}) {
        const auto probe=candidate+side*across+direction*along;
        DynamicWorld::RayLandResult support;
        if(!drySupport(probe,support)) {supported=false;continue;}
        lowest=std::min(lowest,support.v.y);highest=std::max(highest,support.v.y);
        }
      if(highest-lowest>10.f) {
        ++supportUneven;rejectSupport("height_span",candidate,floor,false,0);supported=false;
        }
      if(!supported)continue;
      at=candidate;floorPoint=floor.v;floorNormal=floor.n;chosenRetreat=retreat;chosenLateral=lateral;footprint=true;
      }
    event("water_wind_support","\"lip_cm\":"+ragdollVector(lip)+",\"shore_water_y_cm\":"+ragdollNumber(shoreWaterY)+
      ",\"found\":"+(footprint?"true":"false")+",\"retreat_cm\":"+(footprint?ragdollNumber(chosenRetreat):"null")+
      ",\"lateral_cm\":"+(footprint?ragdollNumber(chosenLateral):"null")+",\"floor_normal\":"+ragdollVector(floorNormal)+
      ",\"trials\":"+std::to_string(supportTrials)+",\"no_floor\":"+std::to_string(supportNoFloor)+
      ",\"steep_floor\":"+std::to_string(supportSteep)+",\"wet_floor\":"+std::to_string(supportWet)+
      ",\"height_span_failures\":"+std::to_string(supportUneven)+",\"first_rejections\":"+supportRejections+"]");
    if(!footprint) {arenaAbort("water barrel centre is unsupported at the real shoreline lip");return;}
    const auto up=Vec3::normalize(floorNormal);
    const auto axisX=Vec3::normalize(Vec3(1,0,0)-up*up.x);
    const auto axisZ=Vec3::crossProduct(axisX,up);
    Matrix4x4 matrix(axisX.x,up.x,axisZ.x,0,
                     axisX.y,up.y,axisZ.y,0,
                     axisX.z,up.z,axisZ.z,0,
                     0,0,0,1);
    auto rotatedCenter=center;matrix.project(rotatedCenter);
    float bottom=std::numeric_limits<float>::infinity();
    for(unsigned corner=0;corner<8;++corner) {
      Vec3 vertex(found->localBounds[corner&1].x,found->localBounds[(corner>>1)&1].y,found->localBounds[(corner>>2)&1].z);
      matrix.project(vertex);bottom=std::min(bottom,Vec3::dotProduct(vertex,up));
      }
    const float tx=at.x-rotatedCenter.x,tz=at.z-rotatedCenter.z;
    const float ty=(Vec3::dotProduct(floorPoint,up)+.5f-bottom-up.x*tx-up.z*tz)/up.y;
    matrix.set(3,0,tx);matrix.set(3,1,ty);matrix.set(3,2,tz);
    if(!world.physic()->setMovableVobTransform(zen,matrix,&player)) {arenaAbort("water shore barrel placement failed");return;}
    target=center;matrix.project(target);
    // The ordinary cone's impulse is 450*(1-distance/1000) Ns. Stay below
    // this native barrel's break threshold while still pushing it off the lip.
    const float safeDistance=std::clamp(1000.f*(1.f-.8f*found->impactThreshold/450.f),400.f,960.f);
    bool casterSupported=false;
    unsigned noFloor=0,steepFloor=0,wetFloor=0,occluded=0,rejectionCount=0;
    std::string rejections="[";
    const auto reject=[&](std::string_view why,const Vec3& candidate,const DynamicWorld::RayLandResult& floor,const Vec3* obstruction=nullptr) {
      if(rejectionCount>=3)return;
      if(rejectionCount++)rejections+=',';
      rejections+="{\"reason\":"+jsonStr(why)+",\"candidate_cm\":"+ragdollVector(candidate)+
        ",\"floor_found\":"+(floor.hasCol?"true":"false")+",\"floor_cm\":"+ragdollVector(floor.v)+
        ",\"floor_normal\":"+ragdollVector(floor.n)+",\"obstruction_cm\":"+(obstruction?ragdollVector(*obstruction):"null")+"}";
      };
    const auto actorCenterOffset=player.collosionCenter()-player.position();
    float chosenNativeDistance=0,chosenConeCosine=0;
    for(float distance=safeDistance;distance<=980.f && !casterSupported;distance+=10.f)
      for(unsigned a=0;a<64 && !casterSupported;++a) {
        const int turn=a==0?0:int((a+1)/2)*(a%2==1?1:-1);
        const float angle=float(turn)*float(M_PI)/32.f;
        const auto radial=direction*(-std::cos(angle))+side*std::sin(angle);
        // The real cone uses three-dimensional distance from the caster's
        // collision centre. A high quay needs a smaller horizontal radius.
        if(Vec3::dotProduct(radial,direction)>-.25f)continue;
        float horizontal=distance;
        DynamicWorld::RayLandResult stand;
        Vec3 candidate;bool converged=false;
        for(unsigned iteration=0;iteration<6;++iteration) {
          candidate=floorPoint+radial*horizontal;
          stand=shoreFloor(candidate);
          if(!stand.hasCol)break;
          const float height=stand.v.y+actorCenterOffset.y-target.y;
          const float along=Vec3::dotProduct(radial,actorCenterOffset);
          const float perpendicular=actorCenterOffset.x*actorCenterOffset.x+actorCenterOffset.z*actorCenterOffset.z;
          const float discriminant=along*along+distance*distance-height*height-perpendicular;
          if(discriminant<=0.f)break;
          const float next=std::sqrt(discriminant)-along;
          if(next<=0.f)break;
          if(std::abs(next-horizontal)<.25f) {converged=true;break;}
          horizontal=next;
          }
        if(!stand.hasCol) {++noFloor;reject("no_floor",candidate,stand);continue;}
        if(!converged || stand.n.y<.99f) {++steepFloor;reject("unsupported_cast_floor",candidate,stand);continue;}
        const auto standWater=world.physic()->waterRay(stand.v+Vec3(0,.1f,0),0);
        if(standWater.hasCol && standWater.wdepth>stand.v.y) {++wetFloor;reject("wet_floor",candidate,stand);continue;}
        const auto casterCenter=stand.v+actorCenterOffset;
        const auto obstruction=world.physic()->ray(casterCenter,target);
        if(obstruction.hasCol && (obstruction.v-target).length()>extent.length()+5.f) {
          ++occluded;reject("occluded",candidate,stand,&obstruction.v);continue;
          }
        auto firing=target-stand.v;firing.y=0;firing=Vec3::normalize(firing);
        const auto targetFloor=shoreFloor(target-firing*250.f);
        if(!targetFloor.hasCol || targetFloor.n.y<.99f) {++steepFloor;reject("unsupported_spell_target",candidate,stand);continue;}
        const auto targetWater=world.physic()->waterRay(targetFloor.v+Vec3(0,.1f,0),0);
        if(targetWater.hasCol && targetWater.wdepth>targetFloor.v.y) {++wetFloor;reject("wet_spell_target",candidate,stand);continue;}
        const auto axis=Vec3::normalize(targetFloor.v+actorCenterOffset-casterCenter);
        const float cosine=Vec3::dotProduct(Vec3::normalize(target-casterCenter),axis);
        if(cosine<.819152f) {++occluded;reject("outside_native_cone",candidate,stand);continue;}
        caster=stand.v;windDummy=targetFloor.v;casterSupported=true;
        chosenNativeDistance=(target-casterCenter).length();chosenConeCosine=cosine;
        }
    event("water_wind_site","\"lip_cm\":"+ragdollVector(lip)+",\"shore_water_y_cm\":"+ragdollNumber(shoreWaterY)+
      ",\"floor_cm\":"+ragdollVector(floorPoint)+",\"target_cm\":"+ragdollVector(target)+
      ",\"local_native_com_cm\":"+ragdollVector(center)+",\"extent_cm\":"+ragdollVector(extent)+
      ",\"safe_distance_cm\":"+ragdollNumber(safeDistance)+",\"caster_found\":"+(casterSupported?"true":"false")+
      ",\"native_caster_distance_cm\":"+ragdollNumber(chosenNativeDistance)+",\"native_cone_cosine\":"+ragdollNumber(chosenConeCosine)+
      ",\"ray_top_cm\":"+ragdollNumber(state.rayTop)+
      ",\"no_floor\":"+std::to_string(noFloor)+",\"steep_floor\":"+std::to_string(steepFloor)+
      ",\"wet_floor\":"+std::to_string(wetFloor)+",\"occluded\":"+std::to_string(occluded)+",\"first_rejections\":"+rejections+"]");
    if(!casterSupported) {arenaAbort("water safe wind caster requires dry support and a clear native sight line");return;}
    const auto splashSource=Vec3(floorPoint.x,shoreWaterY,floorPoint.z)+direction*(chosenRetreat+80.f);
    noiseRetireListeners();noiseBegin(world,"water-splash",splashSource);
    // Choose a supported listener outside the quiet dry-contact radius, but
    // within the real acoustic range of a splash from this low shoreline.
    Npc* listener=nullptr;unsigned listenerNoFloor=0,listenerWet=0,listenerBlocked=0;
    float listenerOcclusion=0,listenerDistance=0;
    for(float radius:{350.f,400.f,450.f,500.f,550.f})
      for(unsigned a=0;a<32 && !listener;++a) {
        const float angle=float(a)*float(M_PI)/16.f;
        const auto offset=direction*std::cos(angle)*radius+side*std::sin(angle)*radius;
        // Keep the observer out of the projectile's line through its target.
        if(std::abs(Vec3::dotProduct(offset,side))<150.f)continue;
        const auto listenerAt=shoreFloor(floorPoint+offset);
        if(!listenerAt.hasCol || listenerAt.n.y<.65f) {++listenerNoFloor;continue;}
        if((listenerAt.v-floorPoint).length()<=300.f)continue;
        const auto water=world.physic()->waterRay(listenerAt.v+Vec3(0,.1f,0),0);
        if(water.hasCol && water.wdepth>listenerAt.v.y) {++listenerWet;continue;}
        const auto head=listenerAt.v+Vec3(0,160,0);
        const float occlusion=world.physic()->soundOclusion(splashSource,head);
        const float distance=(splashSource-head).length();
        const float setupOcclusion=world.physic()->soundOclusion(state.center,head);
        if(distance>=650.f*(1.f-occlusion) ||
           (state.center-head).length()>=650.f*(1.f-setupOcclusion)) {++listenerBlocked;continue;}
        listener=noiseAddListener(world,"water-near",listenerAt.v);
        listenerOcclusion=occlusion;listenerDistance=distance;
        }
    event("water_wind_listener",std::string("\"found\":")+(listener?"true":"false")+
      ",\"source_cm\":"+ragdollVector(splashSource)+",\"distance_cm\":"+ragdollNumber(listenerDistance)+
      ",\"occlusion\":"+ragdollNumber(listenerOcclusion)+",\"no_floor\":"+std::to_string(listenerNoFloor)+
      ",\"wet_floor\":"+std::to_string(listenerWet)+",\"acoustically_blocked\":"+std::to_string(listenerBlocked));
    if(!listener) {
      arenaAbort("water splash requires a real nearby supported listener");return;
      }
    }
  auto direction=target-caster;direction.y=0;
  if(direction.length()<1.f) {arenaAbort("water spell target has no horizontal firing direction");return;}
  direction=Vec3::normalize(direction);
  auto dummy=target+direction*100.f-Vec3(0,80,0);
  if(wind)dummy=windDummy; // Already checked with the actual caster and native cone.
  auto* npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),dummy);
  if(!npc) {arenaAbort("water real spell target insertion failed");return;}
  npc->clearAiQueue();npc->clearState(false);npc->setExternalControl(true);
  npc->handle().attribute[ATR_HITPOINTS]=npc->handle().attribute[ATR_HITPOINTSMAX]=50000;
  npc->updateTransform();context.spellActors.insert(context.spellActors.begin(),{npc,"water_target"});
  int32_t spell=-1;
  for(size_t i=0;i<world.script().symbolsCount();++i) {
    const auto* symbol=world.script().findSymbol(i);
    if(!symbol || symbol->type()!=daedalus::DaedalusDataType::INSTANCE)continue;
    auto name=symbol->name();for(auto& c:name)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(!name.starts_with("ITRU_") && !name.starts_with("ITSC_"))continue;
    auto item=std::make_unique<Item>(world,i,Item::T_Inventory);
    const auto kind=world.script().physicalSpell(item->spellId());
    if(kind!=(wind?GameScript::PhysicalSpell::WindFist:GameScript::PhysicalSpell::Fireball))continue;
    spell=item->spellId();context.spellRunes[spell]=i;player.addItem(i,item->isRune()?1:5);break;
    }
  if(spell<0) {arenaAbort("water fixture real spell rune is missing");return;}
  player.closeWeapon(true);player.setPosition(caster);player.setDirection(direction);player.updateTransform();
  player.handle().attribute[ATR_MANA]=player.handle().attribute[ATR_MANAMAX]=50000;player.setTalentSkill(TALENT_MAGE,6);
  Gothic::settingsSetI("GAME","spell_physics",1);
  player.useItem(context.spellRunes[spell],Item::NSLOT,true);
  if(player.drawSpell(spell))context.spellCastingId=spell;
  else {context.spellEquipPending=spell;context.spellEquipUntil=world.tickCount()+3000;}
  auto cameraTarget=target;
  if(wind)cameraTarget.y=(target.y+state.center.y)*.5f;
  context.cam=C_Hold;
  if(!wind && state.caseName=="deep") {
    context.cam=C_Water;state.cameraTarget=cameraTarget;
    lookFrom(target+state.direction*380.f+Vec3(0,650,0),cameraTarget);
    }
  else
    lookFrom(caster+Vec3(-direction.z*300.f,210,direction.x*300.f),cameraTarget);
  event("water_spell_ready","\"spell_kind\":"+jsonStr(wind?"wind":"fireball")+",\"spell_id\":"+
    std::to_string(spell)+",\"zen_id\":"+std::to_string(zen)+",\"target_cm\":"+ragdollVector(target)+
    ",\"caster_cm\":"+ragdollVector(caster)+",\"dummy_cm\":"+ragdollVector(dummy));
  }

bool waterCommand(Water& waterState,const WaterContext& context,const std::vector<std::string>& a,World& w,Npc& pl,PlayerControl& ctrl) {
  const std::string& c = a[0];
  if(c=="water" && a.size()==2 && (a[1]=="on" || a[1]=="off")) {
    Gothic::settingsSetI("GAME","buoyancy",a[1]=="on");
    w.physic()->setBuoyancyEnabled(a[1]=="on");
    }
  else if(c=="water" && a.size()==3 && a[1]=="setup")prepareWaterFixture(waterState,context,w,pl,a[2]);
  else if(c=="water-bodies" && a.size()==3 && a[1]=="setup") {
    if(a[2]=="deep")prepareSpellFixture(w,pl,"wind");
    prepareWaterBodiesFixture(waterState,context,w,pl,a[2]);
    }
  else if(c=="water-bodies" && a.size()==2 && a[1]=="kill-human") {
    if(waterState.actors.empty() || waterState.caseName!="bodies-deep" || waterState.actors.front().npc->isDown()) {
      arenaAbort("water corpse kill requires the living human at the prepared water lip");return true;
      }
    waterState.killOnThrow=true;
    event("water_bodies_death_armed","\"actor_id\":"+std::to_string(w.npcId(waterState.actors.front().npc))+
      ",\"minimum_wind_speed_cm_s\":100,\"minimum_displacement_cm\":50");
    }
  else if(c=="water-bodies" && a.size()==3 && a[1]=="checkpoint")waterBodiesCheckpoint(waterState,context,w,a[2]);
  else if(c=="water-bodies" && a.size()==3 && a[1]=="initial")waterBodiesInitial(waterState,context,w,pl,a[2]);
  else if(c=="water-bodies" && a.size()==2 && a[1]=="reset-motion") {
    waterState.reference.clear();waterState.travel.clear();waterState.sample=uint64_t(-1);waterFields(waterState,context,w);
    waterBodiesCheckpoint(waterState,context,w,"motion-reference");
    }
  else if(c=="water-bodies" && a.size()==3 && a[1]=="save") {
    try {
      if(waterState.actors.empty() || std::any_of(waterState.actors.begin(),waterState.actors.end(),[](const auto& actor) {
        return !actor.npc->ragdollBody() || !actor.npc->ragdollBody()->isSleeping();
        })) {arenaAbort("water body save requires every observed corpse to have slept");return true;}
      waterBodiesCheckpoint(waterState,context,w,"saved");saveMovableSession(w,a[2]);
      }
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="water-bodies" && a.size()==3 && a[1]=="loot-ready")waterBodiesLootReady(waterState,w,pl,a[2]);
  else if(c=="water-bodies" && a.size()==2 && a[1]=="loot")waterBodiesLoot(waterState,context,w,pl,ctrl);
  else if(c=="water" && a.size()==2 && a[1]=="torch")waterTorch(waterState,w,pl);
  else if(c=="water" && a.size()==3 && a[1]=="checkpoint")waterCheckpoint(waterState,context,w,a[2]);
  else if(c=="water" && a.size()==3 && a[1]=="initial")waterInitial(waterState,context,w,pl,ctrl,a[2]);
  else if(c=="water" && a.size()==2 && a[1]=="push-ready")prepareWaterSpell(waterState,context,w,pl,true);
  else if(c=="water" && a.size()==2 && a[1]=="fireball-ready")prepareWaterSpell(waterState,context,w,pl,false);
  else if(c=="water" && a.size()==3 && a[1]=="save") {
    try {
      for(const auto& body:w.physic()->waterBodySnapshot())
        if(waterOwns(waterState,body) && !body.sleeping) {
          arenaAbort("water save requires every observed body to have settled and slept");return true;
          }
      waterCheckpoint(waterState,context,w,"saved");saveMovableSession(w,a[2]);
      }
    catch(const std::exception& error) {arenaAbort(error.what());}
    }
  else if(c=="water" && a.size()==2 && a[1]=="swim-ready") {
    const auto all=w.physic()->movableVobSnapshot();
    const auto barrel=std::find_if(all.begin(),all.end(),[&](const auto& body){return body.zenId==waterState.barrelZen;});
    if(barrel==all.end() || barrel->broken) {arenaAbort("water swim requires the intact native barrel");return true;}
    auto center=(barrel->localBounds[0]+barrel->localBounds[1])*.5f;barrel->transform.project(center);
    Vec3 surface;
    if(!waterProbe(w,Vec3(center.x,waterState.center.y,center.z),160.f,6000.f,surface)) {
      arenaAbort("water swim barrel has not reached deep enough real water");return true;
      }
    pl.closeWeapon(true);pl.clearAiQueue();pl.stopWalking();pl.setDirection(waterState.direction);
    pl.setPosition(surface-waterState.direction*280.f-Vec3(0,100,0));pl.updateTransform();
    waterState.heroStart=pl.position();waterState.heroLast=pl.position();waterState.heroMaxStep=0;
    waterState.swim=true;waterState.sawSwim=false;
    context.cam=C_Hold;lookFrom(surface-waterState.direction*600.f+Vec3(250,250,0),surface);
    event("water_swim_begin","\"barrel_cm\":"+ragdollVector(center)+",\"hero_cm\":"+ragdollVector(pl.position())+
      ",\"direction\":"+ragdollVector(waterState.direction));
    }
  else if(c=="water" && a.size()==2 && a[1]=="pickup-ready") {
    if(waterState.items.empty()) {arenaAbort("water pickup requires the fixture arrow");return true;}
    waterState.swim=false;
    const auto point=waterState.items.front()->midPosition();
    const auto floor=w.physic()->landRay(point+Vec3(70,100,0),6000.f);
    if(!floor.hasCol) {arenaAbort("water pickup requires real ground beneath the item");return true;}
    const auto water=w.physic()->waterRay(floor.v+Vec3(0,.1f,0),0);
    pl.closeWeapon(true);pl.clearAiQueue();pl.stopWalking();
    auto at=point+Vec3(70,0,0);at.y=water.hasCol && water.wdepth-floor.v.y>150.f?water.wdepth-100.f:floor.v.y;
    pl.setPosition(at);pl.setDirection(point-at);pl.updateTransform();
    event("water_pickup_ready","\"item_cm\":"+ragdollVector(point)+",\"hero_cm\":"+ragdollVector(at));
    }
  else if(c=="water" && a.size()==2 && a[1]=="pickup") {
    if(waterState.items.empty()) {arenaAbort("water pickup fixture arrow is absent");return true;}
    auto* item=waterState.items.front();
    const auto symbol=item->clsId();
    const auto before=pl.itemCount(symbol);
    const auto point=item->midPosition();const auto state=pl.bodyStateMasked();
    const bool swimming=pl.isSwim();
    const bool accepted=ctrl.interact(*item);
    if(accepted)waterState.items.erase(waterState.items.begin());
    event("water_pickup","\"accepted\":"+std::string(accepted?"true":"false")+
      ",\"body_state\":"+std::to_string(unsigned(state))+",\"swimming\":"+(swimming?"true":"false")+
      ",\"inventory_before\":"+std::to_string(before)+",\"inventory_after\":"+std::to_string(pl.itemCount(symbol))+
      ",\"distance_cm\":"+ragdollNumber((pl.position()-point).length())+
      ",\"item_cm\":"+ragdollVector(point));
    }
  else return false;
  return true;
  }

}
