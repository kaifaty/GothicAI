#include "testbench_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "camera.h"
#include "gothic.h"
#include "game/gamescript.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/skeleton.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

// Film fixtures are deliberately separate from the water regression fixtures:
// each shot owns its actors/props and accepts the stock, non-ragdoll outcome.
std::string waterFilmPosition(World& world,const Vec3& point) {
  const auto floor=world.physic()->landRay(point+Vec3(0,300,0),6000.f);
  const auto water=floor.hasCol?world.physic()->waterRay(floor.v+Vec3(0,.1f,0),0):DynamicWorld::RayWaterResult{};
  return "\"position_cm\":"+ragdollVector(point)+
    ",\"water_y_cm\":"+(water.hasCol?ragdollNumber(water.wdepth):"null")+
    ",\"bottom_y_cm\":"+(floor.hasCol?ragdollNumber(floor.v.y):"null");
  }

void waterFilmCheckpoint(WaterFilmContext ctx,World& world) {
  auto& state=ctx.film;
  std::string actors="[",items="[",fragments="[";
  for(const auto& actor:ctx.water.actors) {
    if(actors.size()>1)actors+=',';
    actors+="{\"role\":"+jsonStr(actor.role)+",\"dead\":"+(actor.npc->isDead()?"true":"false")+
      ",\"swimming\":"+(actor.npc->isSwim()?"true":"false")+
      ",\"last_hit_spell\":"+std::to_string(actor.npc->lastHitSpellId())+
      ",\"ragdoll\":"+(actor.npc->ragdollBody()?"true":"false")+","+waterFilmPosition(world,actor.npc->position())+"}";
    }
  for(size_t i=0;i<ctx.water.items.size();++i) {
    if(i)items+=',';
    items+="{\"label\":"+jsonStr(std::array{"sword","ore","lute","arrow"}[i])+","+
      waterFilmPosition(world,ctx.water.items[i]->midPosition())+"}";
    }
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    if(body.category!=DynamicWorld::C_Fragment || !ctx.water.ids.contains(body.parentId))continue;
    if(fragments.size()>1)fragments+=',';
    fragments+="{\"parent\":"+jsonStr(ctx.water.labels[body.parentId])+",\"piece\":"+std::to_string(body.piece)+
      ",\"submerged_fraction\":"+ragdollNumber(body.submergedFraction)+","+
      waterFilmPosition(world,(body.worldBounds[0]+body.worldBounds[1])*.5f)+"}";
    }
  event("water_film_checkpoint","\"scene\":"+jsonStr(state.waterFilm)+",\"elapsed_ms\":"+
    std::to_string(world.tickCount()-state.waterFilmStart)+",\"actors\":"+actors+"],\"items\":"+items+
    "],\"props\":"+destructionRows(ctx.destruction,world)+",\"fragments\":"+fragments+"],\"bodies\":"+waterRows(ctx.water,world));
  }

void waterFilmTick(WaterFilmContext ctx,World& world) {
  auto& state=ctx.film;
  if(!state.waterFilmKill || ctx.water.actors.empty())return;
  auto& npc=*ctx.water.actors.front().npc;
  const auto velocity=npc.spellVelocity();
  const auto displacement=(npc.position()-state.waterFilmLip).length();
  if(npc.lastHitSpellId()!=26 || npc.lastHitBy()!=world.player() ||
     velocity.length()<=100.f || displacement<=50.f)return;
  state.waterFilmKill=false;
  const auto at=npc.position();
  if(!npc.setRagdollDown(true)) {arenaAbort("water film real wind death refused");return;}
  npc.prepareRagdoll();
  event("water_film_death","\"scene\":"+jsonStr(state.waterFilm)+","+waterFilmPosition(world,at)+
    ",\"observed_wind_velocity_cm_s\":"+ragdollVector(velocity)+
    ",\"displacement_cm\":"+ragdollNumber(displacement)+
    ",\"ragdoll\":"+(npc.ragdollBody()?"true":"false")+",\"path\":\"onNoHealth after real WindFist hit\"");
  }

// Measure the rendered pose/model in the actual capture viewport. A stock
// corpse may leave the close shot; that is evidence, not a fixture failure.
struct WaterFilmPixels {
  Matrix4x4 vp;
  float width,height;
  std::vector<Vec2> points;
  bool inFrame=true;

  WaterFilmPixels(const Matrix4x4& viewProjection,uint32_t w,uint32_t h)
    :vp(viewProjection),width(float(w)),height(float(h)) {}

  void point(const Vec3& p) {
    float x=p.x,y=p.y,z=p.z,w=1;vp.project(x,y,z,w);
    if(!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) || !std::isfinite(w) || w<=0) {
      inFrame=false;return;
      }
    inFrame&=std::abs(x)<=w && std::abs(y)<=w && z>=0 && z<=w;
    // The Vulkan projection already points Y down, like the captured pixmap.
    points.emplace_back((x/w+1.f)*width*.5f,(y/w+1.f)*height*.5f);
    }

  void box(const Vec3* bounds,const Matrix4x4& object) {
    if(!bounds)return;
    for(unsigned i=0;i<8;++i) {
      Vec3 p(bounds[i&1].x,bounds[(i>>1)&1].y,bounds[(i>>2)&1].z);
      object.project(p);point(p);
      }
    }

  std::string json() const {
    if(points.empty())return "null";
    auto lo=points.front(),hi=lo;float span=0;
    for(const auto& p:points) {
      lo.x=std::min(lo.x,p.x);lo.y=std::min(lo.y,p.y);
      hi.x=std::max(hi.x,p.x);hi.y=std::max(hi.y,p.y);
      for(const auto& q:points)span=std::max(span,(p-q).length());
      }
    return "{\"width_px\":"+ragdollNumber(hi.x-lo.x)+",\"height_px\":"+ragdollNumber(hi.y-lo.y)+
      ",\"span_px\":"+ragdollNumber(span)+",\"rect_px\":["+ragdollNumber(lo.x)+","+ragdollNumber(lo.y)+","+
      ragdollNumber(hi.x)+","+ragdollNumber(hi.y)+"],\"in_frame\":"+(inFrame?"true":"false")+"}";
    }
  };

void waterFilmFraming(WaterFilmContext ctx,World& world,uint32_t width,uint32_t height) {
  auto& state=ctx.film;
  if(!ctx.rec || state.waterFilm.empty())return;
  const auto* camera=Gothic::inst().camera();
  if(!camera)return;
  std::string objects="[";
  const auto row=[&](std::string_view label,std::string_view method,const WaterFilmPixels& pixels) {
    if(objects.size()>1)objects+=',';
    objects+="{\"label\":"+jsonStr(label)+",\"method\":"+jsonStr(method)+",\"pixels\":"+pixels.json()+"}";
    };
  if(state.waterFilm=="body" || state.waterFilm=="skeleton")
    for(const auto& actor:ctx.water.actors) {
      WaterFilmPixels pixels(camera->viewProj(),width,height);
      const auto& npc=*actor.npc;
      if(const auto* skeleton=npc.visualSkeleton())
        for(size_t i=0;i<skeleton->nodes.size();++i) {
          if(!skeleton->nodes[i].name.starts_with("BIP01"))continue;
          Vec3 point{};npc.renderedBone(i).project(point);pixels.point(point);
          }
      row(actor.role,"rendered_bones",pixels);
      }
  for(size_t i=0;i<ctx.water.items.size();++i) {
    WaterFilmPixels pixels(camera->viewProj(),width,height);
    pixels.box(ctx.water.items[i]->bBox(),ctx.water.items[i]->transform());
    row(std::array{"sword","ore","lute","arrow"}[i],"native_mesh_bbox",pixels);
    }
  if(state.waterFilm=="fireball")
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(!ctx.water.ids.contains(body.id) || body.broken)continue;
      WaterFilmPixels pixels(camera->viewProj(),width,height);
      if(const auto* mesh=Resources::loadMesh(body.visual))pixels.box(mesh->bbox(),body.transform);
      row(ctx.water.labels[body.id],"native_mesh_bbox",pixels);
      }
  event("water_film_framing","\"scene\":"+jsonStr(state.waterFilm)+",\"elapsed_ms\":"+
    std::to_string(ctx.time-ctx.recStart)+",\"viewport_px\":["+std::to_string(width)+","+
    std::to_string(height)+"],\"objects\":"+objects+"]");
  }

void waterFilmSetup(WaterFilmContext ctx,World& world,Npc& player,std::string_view scene) {
  auto& state=ctx.film;
  if(scene!="body" && scene!="skeleton" && scene!="fireball" && scene!="things") {
    arenaAbort("water-film setup expects body, skeleton, fireball or things");return;
    }
  if(ctx.movables.spellRunes.empty())prepareSpellFixture(world,player,"wind");
  if(ctx.movables.spellRunes.empty())return;
  player.setTarget(nullptr);player.closeWeapon(true);player.clearAiQueue();player.clearGoTo();player.stopWalking();player.clearSpeed();
  ctx.subject=nullptr;ctx.crew.clear();ctx.water.actors.clear();state.waterFilmKill=false;
  ctx.movables.spellTrace=false;ctx.movables.spellGuardTrace=false;ctx.movables.spellReleaseAt=0;ctx.movables.spellEquipPending=0;
  for(const auto& actor:ctx.movables.spellActors)world.removeNpc(*actor.npc);
  for(auto* item:ctx.movables.spellItems)world.removeItem(*item);
  for(auto* item:ctx.water.items)world.removeItem(*item);
  ctx.movables.spellActors.clear();ctx.movables.spellItems.clear();ctx.water.items.clear();
  ctx.destruction.destructionFixtures.clear();ctx.destruction.destructionIds.clear();ctx.movables.movableIds.clear();
  ctx.water.ids.clear();ctx.water.labels.clear();ctx.water.reference.clear();ctx.water.travel.clear();ctx.water.sleepAt.clear();
  ctx.destruction.destructionTrace=false;
  std::vector<Vec3> places;
  if(!waterFindWindSite(ctx.water,world,places) || ctx.water.waypoint!="NW_CITY_HABOUR_05") {
    arenaAbort("water film needs the low NW_CITY_HABOUR_05 pier");return;
    }
  const auto dryFloor=[&](const Vec3& at,Vec3& result) {
    const auto floor=world.physic()->landRay(at+Vec3(0,2000,0),4000.f);
    if(!floor.hasCol || floor.n.y<.99f || world.physic()->waterRay(floor.v,1.f).hasCol)return false;
    for(float dx:{-30.f,30.f})for(float dz:{-30.f,30.f}) {
      const auto foot=world.physic()->landRay(floor.v+Vec3(dx,100,dz),150.f);
      if(!foot.hasCol || foot.n.y<.99f || std::abs(foot.v.y-floor.v.y)>5.f ||
         world.physic()->waterRay(foot.v,1.f).hasCol)return false;
      }
    result=floor.v;return true;
    };
  auto lip=ctx.water.dry;
  const auto direction=ctx.water.direction;
  // The regression lip is the last perfectly flat point. Follow the remaining
  // dry slope to the actual waterline before measuring the 1-2 m drop distance.
  const auto flatLip=lip;
  for(float distance=5.f;distance<=200.f;distance+=5.f) {
    const auto floor=world.physic()->landRay(flatLip+direction*distance+Vec3(0,400,0),600.f);
    if(!floor.hasCol || floor.n.y<.75f || world.physic()->waterRay(floor.v,1.f).hasCol)break;
    lip=floor.v;
    }
  bool deep=false;
  for(float distance=100.f;distance<=200.f;distance+=5.f) {
    Vec3 surface;
    if(!waterProbe(world,lip+direction*distance,220.f,6000.f,surface))continue;
    bool slots=true;
    if(scene=="things")for(unsigned i=0;i<4;++i) {
      Vec3 drop;
      if(!waterProbe(world,surface+Vec3(-direction.z,0,direction.x)*(float(i)-1.5f)*65.f,220.f,6000.f,drop))slots=false;
      }
    if(slots) {ctx.water.center=surface;deep=true;break;}
    }
  if(!deep) {arenaAbort("water film needs deep water 1-2m from the actual waterline");return;}
  ctx.water.dry=lip;ctx.water.direction=direction;
  const auto side=Vec3(-direction.z,0,direction.x);
  state.waterFilmLip=lip;state.waterFilm=scene;ctx.water.caseName="film-"+std::string(scene);
  std::array<std::string,2> visuals={"NW_HARBOUR_CRATE_01.3DS",""};
  std::array<Vec3,2> supports;
  if(scene=="fireball") {
    // Some stock harbour barrels are authored as stone. Copy a real wooden,
    // cached barrel visual into a NEW owned vob; never move a map object.
    float footprint=std::numeric_limits<float>::infinity();
    const auto native=world.physic()->waterBodySnapshot();
    for(const auto& body:world.physic()->movableVobSnapshot()) {
      if(!body.enabled || !body.destructible || body.broken || body.className!="barrel")continue;
      const bool wood=std::any_of(native.begin(),native.end(),[&](const auto& sample) {
        return sample.parentId==body.id && sample.targetFraction<1.f;
        });
      const auto size=body.localBounds[1]-body.localBounds[0];
      const float area=size.x*size.z;
      if(wood && (area<footprint || (area==footprint && body.visual<visuals[1]))) {
        visuals[1]=body.visual;footprint=area;
        }
      }
    if(visuals[1].empty()) {arenaAbort("water film needs a stock wooden destructible barrel visual");return;}
    for(unsigned i=0;i<2;++i) {
      Vec3 failed;bool found=false;
      for(float retreat:{140.f,165.f,190.f}) {
        const auto floor=world.physic()->landRay(lip-direction*retreat+side*(i?85.f:-85.f)+Vec3(0,400,0),600.f);
        if(floor.hasCol && !world.physic()->waterRay(floor.v,1.f).hasCol &&
           destructionSupported(world,visuals[i],floor.v,supports[i],failed)) {found=true;break;}
        }
      if(!found) {arenaAbort("water film crate/barrel needs dry full-footprint support");return;}
      }
    }
  Vec3 caster,spellTarget=lip;bool supported=false;
  if(scene=="fireball" && !dryFloor(lip-direction*220.f,spellTarget)) {
    arenaAbort("water film fireball target needs dry native support");return;
    }
  std::vector<float> distances={200,250,300,150};
  if(scene=="fireball") {
    distances.clear();for(float distance=600.f;distance<=900.f;distance+=10.f)distances.push_back(distance);
    }
  for(float distance:distances) {
    for(unsigned angle=0;angle<(scene=="fireball"?181u:1u);++angle) {
      const int step=angle==0?0:int((angle+1)/2)*(angle%2?1:-1);
      const float turn=float(step);
      if(!dryFloor(lip-rotY(direction,turn)*distance,caster) ||
         world.physic()->ray(caster+Vec3(0,140,0),spellTarget+Vec3(0,100,0)).hasCol)continue;
      if(scene=="fireball") {
        bool range=true;
        for(unsigned i=0;i<2;++i) {
          const auto* mesh=Resources::loadMesh(visuals[i]);
          const auto center=supports[i]+Vec3(0,(mesh->bbox()[1].y-mesh->bbox()[0].y)*.5f,0);
          for(const auto& at:{supports[i],center})
            if((caster-at).length()<600.f || (caster-at).length()>800.f)range=false;
          }
        if(!range)continue;
        }
      supported=true;break;
      }
    if(supported)break;
    }
  if(!supported) {
    Vec3 standard;
    const bool found=scene=="fireball" && waterFireballCaster(world,spellTarget+Vec3(0,100,0),0.f,standard);
    event("water_film_caster_search","\"scene\":"+jsonStr(scene)+",\"standard_found\":"+(found?"true":"false")+
      ",\"standard_caster_cm\":"+(found?ragdollVector(standard):"null")+
      ",\"standard_distance_cm\":"+(found?ragdollNumber((standard-lip).length()):"null"));
    arenaAbort("water film caster needs a clear supported dry lane");return;
    }
  player.setPosition(caster);player.setDirection(direction);player.updateTransform();
  player.handle().attribute[ATR_MANA]=player.handle().attribute[ATR_MANAMAX]=50000;player.setTalentSkill(TALENT_MAGE,6);
  for(uint32_t id=0;id<world.npcCount();++id)
    if(auto* npc=world.npcById(id);npc && npc!=&player && (npc->position()-lip).length()<2500.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->setExternalControl(true);
      }
  if(scene!="things") {
    if(!waterBodiesAdd(ctx.water,ctx.waterContext,world,player,scene=="skeleton"?"SKELETON":"BAU_904_BAUER",std::string(scene),lip,false))return;
    auto* target=ctx.water.actors.back().npc;
    if(scene=="fireball") {target->setPosition(spellTarget);target->updateTransform();}
    target->handle().attribute[ATR_HITPOINTS]=target->handle().attribute[ATR_HITPOINTSMAX]=50000;
    ctx.movables.spellActors.push_back({target,"water_target"});
    }
  if(scene=="fireball") {
    // Fresh owned vobs keep this dry authored pose when movable is disabled.
    // The real projectile hits the actor in the gap; only its ordinary blast
    // can break the two props or send their fragments over the edge.
    for(unsigned i=0;i<2;++i) {
      const auto id=destructionAdd(ctx.destruction,ctx.destructionContext,world,visuals[i],supports[i]);
      if(!id) {arenaAbort("water film fresh crate/barrel insertion failed");return;}
      ctx.water.ids.insert(id);ctx.water.labels[id]=i?"barrel":"crate";
      }
    }
  ctx.water.ready=true;ctx.water.trace=true;ctx.water.start=world.tickCount();ctx.water.sample=uint64_t(-1);
  ctx.movables.noiseTrace=true;ctx.movables.noisePhase="water-film-"+std::string(scene);
  const bool bodies=scene=="body" || scene=="skeleton";
  // Broadside close shots keep the surface legible. Do not widen the body
  // shot to chase the stock corpse ten metres away from its buoyant partner.
  Vec3 focus,eye;
  bool camera=false;
  if(bodies) {
    focus=lip+direction*450.f;focus.y=ctx.water.center.y;
    for(float lateral:{420.f,-420.f}) {
      eye=focus+side*lateral+Vec3(0,160,0);
      if(world.physic()->ray(eye,ctx.water.center+Vec3(0,20,0)).hasCol ||
         world.physic()->ray(eye,lip+Vec3(0,110,0)).hasCol)continue;
      camera=true;break;
      }
    }
  else if(scene=="fireball") {
    const float pierY=(supports[0].y+supports[1].y)*.5f;
    // Behind the target and above its head: it stays low in the frame and the
    // crate, the barrel and the water where their fragments land stay in view.
    focus=lip+direction*150.f;focus.y=ctx.water.center.y;
    for(float lateral:{0.f,-40.f,40.f}) {
      eye=lip-direction*480.f+side*lateral;eye.y=pierY+330.f;
      bool clear=!world.physic()->ray(eye,ctx.water.center+Vec3(0,20,0)).hasCol;
      for(unsigned i=0;i<2;++i) {
        const auto* mesh=Resources::loadMesh(visuals[i]);
        const auto center=supports[i]+Vec3(0,(mesh->bbox()[1].y-mesh->bbox()[0].y)*.5f,0);
        const auto hit=world.physic()->ray(eye,center);
        if(hit.hasCol && (hit.v-center).length()>85.f)clear=false;
        }
      if(clear) {camera=true;break;}
      }
    }
  else {
    focus=ctx.water.center+Vec3(0,3,0);
    eye=ctx.water.center-direction*210.f+Vec3(0,100,0);
    camera=true;
    for(unsigned i=0;i<4;++i)
      if(world.physic()->ray(eye,ctx.water.center+side*(float(i)-1.5f)*65.f+Vec3(0,5,0)).hasCol)camera=false;
    }
  if(!camera) {arenaAbort("water film camera has no clear close side view");return;}
  ctx.cam=C_Water;ctx.water.cameraTarget=focus;lookFrom(eye,focus);
  event("water_film_setup","\"scene\":"+jsonStr(scene)+",\"waypoint\":"+jsonStr(ctx.water.waypoint)+
    ",\"lip_cm\":"+ragdollVector(lip)+",\"water_cm\":"+ragdollVector(ctx.water.center)+
    ",\"caster_cm\":"+ragdollVector(caster)+",\"camera_cm\":"+ragdollVector(ctx.camPos)+
    ",\"camera_target_cm\":"+ragdollVector(focus));
  }

void waterFilmBegin(WaterFilmContext ctx,World& world,Npc& player) {
  auto& state=ctx.film;
  state.waterFilmStart=world.tickCount();
  state.waterFilmKill=state.waterFilm=="body" || state.waterFilm=="skeleton";
  if(state.waterFilm=="things") {
    const auto side=Vec3(-ctx.water.direction.z,0,ctx.water.direction.x);
    const char* symbols[]{"ITMW_1H_SLD_SWORD","ITMI_NUGGET","ITMI_LUTE","ITRW_ARROW"};
    for(unsigned i=0;i<4;++i) {
      auto symbol=world.script().findSymbolIndex(symbols[i]);
      if(symbol==size_t(-1) && i==0)
        for(size_t candidate=0;candidate<world.script().symbolsCount();++candidate) {
          const auto* entry=world.script().findSymbol(candidate);
          if(entry && entry->type()==daedalus::DaedalusDataType::INSTANCE && entry->name().starts_with("ITMW_") &&
             entry->name().find("SWORD")!=std::string::npos) {symbol=candidate;break;}
          }
      if(symbol==size_t(-1)) {arenaAbort("water film item instance missing");return;}
      Vec3 surface;
      if(!waterProbe(world,ctx.water.center+side*(float(i)-1.5f)*65.f,220.f,6000.f,surface)) {
        arenaAbort("water film item drop needs four adjacent deep-water slots");return;
        }
      const auto before=world.physic()->waterBodySnapshot();
      auto matrix=Matrix4x4::mkIdentity();matrix.translate(surface+Vec3(0,100,0));
      auto* item=world.addItemDyn(symbol,matrix,world.npcId(&player));
      if(!item) {arenaAbort("water film native item drop failed");return;}
      ctx.water.items.push_back(item);
      for(const auto& body:world.physic()->waterBodySnapshot())
        if(body.category==DynamicWorld::C_Item &&
           std::none_of(before.begin(),before.end(),[&](const auto& old){return old.id==body.id;})) {
          ctx.water.ids.insert(body.id);ctx.water.labels[body.id]=std::array{"sword","ore","lute","arrow"}[i];
          }
      }
    }
  event("water_film_begin","\"scene\":"+jsonStr(state.waterFilm));
  }

bool waterFilmCommand(WaterFilmContext ctx,const std::vector<std::string>& a,World& w,Npc& pl) {
  const auto& c=a[0];
  if(c=="water-film" && a.size()==3 && a[1]=="setup")waterFilmSetup(ctx,w,pl,a[2]);
  else if(c=="water-film" && a.size()==2 && a[1]=="begin")waterFilmBegin(ctx,w,pl);
  else if(c=="water-film" && a.size()==2 && a[1]=="checkpoint")waterFilmCheckpoint(ctx,w);
  else return false;
  return true;
  }

}
