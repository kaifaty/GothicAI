#include "testbench_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>

#include "game/gamescript.h"
#include "gothic.h"
#include "graphics/mesh/protomesh.h"
#include "resources.h"
#include "world/aiqueue.h"
#include "world/movablevobs.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

uint64_t furniturePeakMemory() {
#if defined(__unix__) || defined(__APPLE__)
  rusage usage{};
  if(getrusage(RUSAGE_SELF,&usage)!=0)return 0;
#if defined(__APPLE__)
  return uint64_t(usage.ru_maxrss);
#else
  return uint64_t(usage.ru_maxrss)*1024;
#endif
#else
  return 0;
#endif
  }

void furnitureClearDrops(FurnitureParts& state,World& world) {
  for(auto& drop:state.drops)world.removeItem(*drop.item);
  state.drops.clear();
  }

namespace {

// Intersections of a vertical line with the source mesh's horizontal surfaces,
// in visual centimetres. This selects real shelves without a visual judgement.
std::vector<float> furnitureSurfaces(const asset::StaticMesh& mesh,float x,float z) {
  std::vector<float> result;
  for(const auto& sub:mesh.sub_meshes)for(const auto& face:sub.triangles) {
    const auto& a=mesh.positions[sub.wedges[face.wedges[0]].index];
    const auto& b=mesh.positions[sub.wedges[face.wedges[1]].index];
    const auto& c=mesh.positions[sub.wedges[face.wedges[2]].index];
    const float det=(b.x-a.x)*(c.z-a.z)-(c.x-a.x)*(b.z-a.z);
    if(std::abs(det)<.001f)continue;
    const Vec3 u(b.x-a.x,b.y-a.y,b.z-a.z),v(c.x-a.x,c.y-a.y,c.z-a.z);
    const auto normal=Vec3::crossProduct(u,v);
    if(std::abs(normal.y)<normal.length()*.98f)continue;
    const float ab=((x-a.x)*(c.z-a.z)-(c.x-a.x)*(z-a.z))/det;
    const float ac=((b.x-a.x)*(z-a.z)-(x-a.x)*(b.z-a.z))/det;
    if(ab<-.001f || ac<-.001f || ab+ac>1.001f)continue;
    result.push_back(a.y+ab*(b.y-a.y)+ac*(c.y-a.y));
    }
  std::sort(result.begin(),result.end());
  result.erase(std::unique(result.begin(),result.end(),[](float a,float b){return std::abs(a-b)<1.f;}),result.end());
  return result;
  }

bool furnitureDrop(FurnitureParts& state,World& world,Npc& player,std::string role,const char* instance,
                   const Vec3& bottom,float supportY,const Vec3& speed={}) {
  const auto symbol=world.script().findSymbolIndex(instance);
  if(symbol==size_t(-1)) {arenaAbort("furniture preparation: Item instance unavailable");return false;}
  Item definition(world,symbol,Item::T_Inventory);
  const auto* mesh=Resources::loadMesh(definition.handle().visual);
  if(!mesh) {arenaAbort("furniture preparation: Item mesh unavailable");return false;}
  FurnitureDrop drop;drop.role=std::move(role);drop.supportY=supportY;
  mesh->appendItemVertices(drop.vertices);
  if(drop.vertices.empty()) {arenaAbort("furniture preparation: Item source vertices unavailable");return false;}
  const auto& hull=world.physic()->itemHullShape(*mesh,definition.handle().visual);
  if(Gothic::physicsFlags().itemHulls && hull.shape!=nullptr) {
    const auto& shape=static_cast<const JPH::ConvexHullShape&>(*hull.shape);
    for(JPH::uint i=0;i<shape.GetNumPoints();++i) {
      const auto point=(shape.GetPoint(i)+shape.GetCenterOfMass())*100.f;
      drop.collisionVertices.emplace_back(point.GetX(),point.GetY(),point.GetZ());
      }
    } else {
    const auto center=(mesh->bboxMesh[0]+mesh->bboxMesh[1])*.5f,extent=(mesh->bboxMesh[1]-mesh->bboxMesh[0])*.5f;
    const Vec3 half(std::max(15.f,extent.x),std::max(15.f,extent.y),std::max(15.f,extent.z));
    for(unsigned i=0;i<8;++i)drop.collisionVertices.push_back(center+Vec3(i&1?half.x:-half.x,i&2?half.y:-half.y,i&4?half.z:-half.z));
    }
  float low=std::numeric_limits<float>::infinity();
  for(const auto& point:drop.vertices)low=std::min(low,point.y);
  auto pose=Matrix4x4::mkIdentity();pose.translate(bottom-Vec3(0,low,0));
  drop.item=world.addItemDyn(symbol,pose,world.npcId(&player));
  if(!drop.item || !drop.item->isDynamic()) {arenaAbort("furniture preparation: Item has no native body");return false;}
  if(speed.quadLength()>0 && !drop.item->addSpellImpulse(speed*(.01f*drop.item->physicsMass()),&player)) {
    arenaAbort("furniture preparation: native Item throw refused");return false;
    }
  if(drop.role=="under_apple" && state.traceApple && !world.physic()->startItemTrace(*drop.item)) {
    arenaAbort("furniture diagnosis: cannot trace the native apple");return false;
    }
  event("furniture_parts_drop","\"role\":"+jsonStr(drop.role)+",\"symbol\":"+jsonStr(instance)+
    ",\"bottom_cm\":"+ragdollVector(bottom)+",\"velocity_cm_s\":"+ragdollVector(speed)+
    ",\"support_y_cm\":"+ragdollNumber(supportY)+",\"item_hulls\":"+(Gothic::physicsFlags().itemHulls?"true":"false")+
    ",\"extra_padding_cm\":"+ragdollNumber(hull.extraPaddingCm));
  state.drops.push_back(std::move(drop));return true;
  }

}

void furnitureTraceBegin(FurnitureParts& state,World& world) {
  state.traceApple=true;
  const auto geometry=world.physic()->navigationGeometry();
  const auto center=state.tableCenter;
  auto* file=std::fopen((outDir+"/furniture-parts-floor.csv").c_str(),"w");
  if(!file) {arenaAbort("furniture diagnosis: cannot write native floor geometry");return;}
  std::fprintf(file,"triangle,ax_cm,ay_cm,az_cm,bx_cm,by_cm,bz_cm,cx_cm,cy_cm,cz_cm,max_edge_cm\n");
  size_t count=0;
  for(size_t i=0;i<geometry.triangles.size();i+=3) {
    Vec3 points[3];
    for(size_t j=0;j<3;++j) {
      const auto at=size_t(geometry.triangles[i+j])*3;
      points[j]=Vec3(geometry.vertices[at],geometry.vertices[at+1],geometry.vertices[at+2])*100.f;
      }
    Vec3 low=points[0],high=points[0];
    for(const auto& p:points) {
      low=Vec3(std::min(low.x,p.x),std::min(low.y,p.y),std::min(low.z,p.z));
      high=Vec3(std::max(high.x,p.x),std::max(high.y,p.y),std::max(high.z,p.z));
      }
    if(low.x>center.x+500.f || high.x<center.x-500.f || low.z>center.z+500.f || high.z<center.z-500.f ||
       low.y>center.y+200.f || high.y<center.y-300.f)continue;
    const float edge=std::max({(points[0]-points[1]).length(),(points[1]-points[2]).length(),(points[2]-points[0]).length()});
    std::fprintf(file,"%zu,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",i/3,
      double(points[0].x),double(points[0].y),double(points[0].z),double(points[1].x),double(points[1].y),double(points[1].z),
      double(points[2].x),double(points[2].y),double(points[2].z),double(edge));
    ++count;
    }
  std::fclose(file);
  event("furniture_parts_trace_begin","\"native_floor_triangles\":"+std::to_string(count));
  }

void furnitureTraceReport(FurnitureParts& state,World& world) {
  world.physic()->stopItemTrace();
  const auto samples=world.physic()->takeItemTrace();
  auto* file=std::fopen((outDir+"/furniture-parts-apple-trace.jsonl").c_str(),"w");
  if(!file) {arenaAbort("furniture diagnosis: cannot write native apple trace");return;}
  uint64_t previous=0,maximumGap=0;
  for(const auto& sample:samples) {
    if(previous!=0)maximumGap=std::max(maximumGap,sample.nativeStep-previous);
    previous=sample.nativeStep;
    std::string contacts="[";
    for(const auto& contact:sample.contacts) {
      if(contacts.size()>1)contacts+=',';
      contacts+="{\"body_id\":"+std::to_string(contact.bodyId)+",\"zen_id\":"+std::to_string(contact.zenId)+
        ",\"category\":"+std::to_string(contact.category)+",\"visual\":"+jsonStr(contact.visual)+
        ",\"depth_cm\":"+ragdollNumber(contact.depth)+",\"normal\":"+ragdollVector(contact.normal)+
        ",\"point_cm\":"+ragdollVector(contact.point)+",\"other_point_cm\":"+ragdollVector(contact.otherPoint)+"}";
      }
    const auto row="{\"step\":"+std::to_string(sample.nativeStep)+",\"index\":"+std::to_string(sample.index)+
      ",\"native_steps\":"+std::to_string(sample.nativeSteps)+",\"before_step\":"+(sample.beforeStep?"true":"false")+
      ",\"seconds\":"+ragdollNumber(sample.seconds)+",\"step_seconds\":"+ragdollNumber(sample.stepSeconds)+
      ",\"position_cm\":"+ragdollVector(sample.position)+",\"com_cm\":"+ragdollVector(sample.centerOfMass)+
      ",\"velocity_cm_s\":"+ragdollVector(sample.velocity)+",\"bounds_min_cm\":"+ragdollVector(sample.bounds[0])+
      ",\"bounds_max_cm\":"+ragdollVector(sample.bounds[1])+",\"active\":"+(sample.active?"true":"false")+
      ",\"sleeping\":"+(sample.sleeping?"true":"false")+",\"contacts\":"+contacts+"]}\n";
    std::fputs(row.c_str(),file);
    }
  std::fclose(file);
  state.traceApple=false;
  event("furniture_parts_trace_report","\"samples\":"+std::to_string(samples.size())+
    ",\"maximum_native_step_gap\":"+std::to_string(maximumGap)+",\"pass\":"+(!samples.empty() && maximumGap<=1?"true":"false"));
  }

void furnitureRemoveTable(FurnitureParts& state,World& world) {
  auto matrix=state.tableOrigin;
  matrix.translate(0,-100000.f,0);
  const bool moved=world.physic()->setMovableVobTransform(state.table,matrix);
  event("furniture_parts_table_removed","\"zen_id\":"+std::to_string(state.table)+
    ",\"transform\":"+ragdollMatrix(matrix)+",\"pass\":"+(moved?"true":"false"));
  if(!moved)arenaAbort("furniture diagnosis: cannot remove table collision from the throw");
  }

void furnitureSetup(FurnitureParts& state,World& world,Npc& player) {
  auto& fixture=state;
  const auto* anchor=world.findWayPoint("NW_CITY_HABOUR_TAVERN01_IN");
  if(!anchor) {arenaAbort("furniture preparation: authored harbour tavern waypoint is absent");return;}
  // This authored NORMAL_01 table keeps a passing 10 cm centre void. Coragon's
  // PEASANT_01 tables correctly retain the hull when decomposition fails it.
  const auto table=furnitureBody(world,21150);
  if(!table || !table->enabled || !table->visual.starts_with("NW_CITY_TABLE_NORMAL_01") ||
     (world.physic()->furniturePartsEnabled() && table->collisionParts<=1)) {
    arenaAbort("furniture preparation: harbour tavern table 21150 needs native collision and a valid compound cache in ON mode");return;
    }
  fixture.table=table->zenId;fixture.tableVisual=table->visual;fixture.tableOrigin=table->transform;
  fixture.tableCenter=(table->localBounds[0]+table->localBounds[1])*.5f;table->transform.project(fixture.tableCenter);
  // The observer is outside the Item/CharacterVirtual contact range.
  player.clearAiQueue();player.stopWalking();player.clearSpeed();player.closeWeapon(true);
  auto observer=fixture.tableCenter+Vec3(450,0,450);
  const auto ground=world.physic()->landRay(observer+Vec3(0,200,0),700.f);
  if(!ground.hasCol) {arenaAbort("furniture preparation: tavern observer has no floor");return;}
  observer.y=ground.v.y;player.setPosition(observer);player.updateTransform();
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* npc=world.npcById(id);
    if(npc && npc!=&player && !npc->isDown() && (npc->position()-fixture.tableCenter).length()<1200.f) {
      npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->clearSpeed();npc->setExternalControl(true);
      }
    }
  event("furniture_parts_setup","\"world\":"+jsonStr(world.name())+",\"enabled\":"+
    (world.physic()->furniturePartsEnabled()?"true":"false")+",\"table_zen_id\":"+std::to_string(table->zenId)+
    ",\"table_visual\":"+jsonStr(table->visual)+",\"tavern_waypoint\":"+jsonStr(anchor->name)+
    ",\"native_collision_parts\":"+std::to_string(table->collisionParts)+
    ",\"table_transform\":"+ragdollMatrix(table->transform)+",\"table_min_cm\":"+ragdollVector(table->localBounds[0])+
    ",\"table_max_cm\":"+ragdollVector(table->localBounds[1])+",\"mass_kg\":"+ragdollNumber(table->mass)+
    ",\"world_load_ms\":"+ragdollNumber(float(world.loadMilliseconds()))+
    ",\"peak_rss_bytes\":"+std::to_string(furniturePeakMemory()));
  }

void furnitureTableItems(FurnitureParts& state,World& world,Npc& player) {
  furnitureClearDrops(state,world);
  const auto table=furnitureBody(world,state.table);
  if(!table) {arenaAbort("furniture preparation: table setup required");return;}
  const auto mesh=Resources::loadVisualMesh(table->visual);
  const auto center=(table->localBounds[0]+table->localBounds[1])*.5f;
  const auto surfaces=furnitureSurfaces(mesh,center.x,center.z);
  if(surfaces.empty()) {arenaAbort("furniture preparation: table source has no central tabletop");return;}
  auto top=Vec3(center.x,surfaces.back(),center.z);table->transform.project(top);
  auto below=Vec3(center.x,table->localBounds[0].y+15.f,center.z);table->transform.project(below);
  const auto floor=world.physic()->landRay(below,500.f);
  if(!floor.hasCol || top.y-floor.v.y<50.f) {arenaAbort("furniture preparation: table has no usable native floor below it");return;}
  for(unsigned i=0;i<2;++i) {
    // Release outside the single hull and throw through its front, rather than
    // creating an overlapping body whose penetration solver can eject below it.
    auto under=Vec3(center.x+(i==0?-20.f:20.f),table->localBounds[0].y+30.f,table->localBounds[0].z-35.f);
    table->transform.project(under);
    Vec3 speed(0,0,200.f);table->transform.project(speed);speed-=movablePosition(table->transform);
    if(!furnitureDrop(state,world,player,i==0?"under_apple":"under_bottle",i==0?"ITFO_APPLE":"ITFO_BEER",under,floor.v.y,speed))return;
    }
  if(furnitureDrop(state,world,player,"tabletop","ITFO_APPLE",top+Vec3(0,20,0),top.y))
    state.drops.back().supportLocalY=surfaces.back();
  }

void furnitureCupboardItem(FurnitureParts& state,World& world,Npc& player) {
  furnitureClearDrops(state,world);
  // The full scene census permits this authored cupboard. Candidate 25787
  // has a floor intersection and correctly retains its original static mesh.
  const auto selected=furnitureBody(world,24003);
  if(!selected || !selected->enabled || !selected->visual.starts_with("NW_CITY_CUPBOARD_POOR_01") ||
     (world.physic()->furniturePartsEnabled() && selected->collisionParts<=1)) {
    arenaAbort("furniture preparation: authored cupboard 24003 needs its permitted native body and required collision cache");return;
    }
  const auto mesh=Resources::loadVisualMesh(selected->visual);
  const auto surfaces=furnitureSurfaces(mesh,0.f,-27.5f);
  float shelf=-std::numeric_limits<float>::infinity(),ceiling=selected->localBounds[1].y;
  for(const float height:surfaces)
    if(height<57.497288f)shelf=std::max(shelf,height);else ceiling=std::min(ceiling,height);
  if(!std::isfinite(shelf) || shelf<selected->localBounds[0].y+25.f || ceiling-shelf<30.f) {
    arenaAbort("furniture preparation: authored cupboard front shelf lost its source support or clearance");return;
    }
  const Vec3 local(0,shelf,-27.5f);
  state.cupboard=selected->zenId;
  auto support=local;selected->transform.project(support);
  player.setPosition(support+Vec3(450,0,450));player.updateTransform();
  event("furniture_parts_shelf_setup","\"zen_id\":"+std::to_string(selected->zenId)+",\"visual\":"+jsonStr(selected->visual)+
    ",\"source_support_local_cm\":"+ragdollVector(local)+",\"source_support_world_cm\":"+ragdollVector(support)+
    ",\"native_collision_parts\":"+std::to_string(selected->collisionParts)+",\"transform\":"+ragdollMatrix(selected->transform));
  if(furnitureDrop(state,world,player,"shelf","ITFO_BEER",support+Vec3(0,20,0),support.y))
    state.drops.back().supportLocalY=shelf;
  }

void furnitureItemReport(FurnitureParts& state,World& world) {
  const auto table=furnitureBody(world,state.table);
  if(!table || state.drops.empty()) {arenaAbort("furniture report requires released Items");return;}
  const bool enabled=world.physic()->furniturePartsEnabled();bool all=true;
  for(const auto& drop:state.drops) {
    const auto support=drop.role=="shelf"?furnitureBody(world,state.cupboard):table;
    if(!support) {arenaAbort("furniture report lost its native support body");return;}
    auto inverse=support->transform;inverse.inverse();
    float low=std::numeric_limits<float>::infinity(),high=-low;Vec3 center;
    for(auto point:drop.vertices) {
      drop.item->transform().project(point);low=std::min(low,point.y);high=std::max(high,point.y);center+=point;
      }
    center/=float(drop.vertices.size());auto local=center;inverse.project(local);
    const bool under=drop.role.starts_with("under_");
    const bool footprint=local.x>support->localBounds[0].x+5.f && local.x<support->localBounds[1].x-5.f &&
                         local.z>support->localBounds[0].z+5.f && local.z<support->localBounds[1].z-5.f;
    float collisionLow=std::numeric_limits<float>::infinity(),collisionHigh=-collisionLow;
    for(auto point:drop.collisionVertices) {
      drop.item->transform().project(point);collisionLow=std::min(collisionLow,point.y);collisionHigh=std::max(collisionHigh,point.y);
      }
    float supportY=drop.supportY;
    if(under) {
      auto middle=(support->localBounds[0]+support->localBounds[1])*.5f;support->transform.project(middle);
      const auto floor=world.physic()->landRay(Vec3(center.x,middle.y,center.z),500.f);
      if(floor.hasCol)supportY=floor.v.y;
      } else {
      auto point=Vec3(local.x,drop.supportLocalY,local.z);support->transform.project(point);supportY=point.y;
      }
    const float gap=low-supportY,collisionGap=collisionLow-supportY;
    const bool supported=std::abs(collisionGap)<=3.f;
    auto currentTop=Vec3(local.x,support->localBounds[1].y,local.z);support->transform.project(currentTop);
    const bool floorUnder=footprint && supported && collisionHigh<currentTop.y-5.f;
    const bool pass=under?(enabled?floorUnder:!floorUnder):((drop.role=="shelf" && !enabled) || (supported && footprint));
    all&=pass;
    event("furniture_parts_item_result","\"enabled\":"+std::string(enabled?"true":"false")+",\"role\":"+jsonStr(drop.role)+
      ",\"position_cm\":"+ragdollVector(drop.item->position())+",\"center_local_cm\":"+ragdollVector(local)+
      ",\"bottom_cm\":"+ragdollNumber(low)+",\"top_cm\":"+ragdollNumber(high)+
      ",\"support_y_cm\":"+ragdollNumber(supportY)+",\"support_gap_cm\":"+ragdollNumber(gap)+
      ",\"collision_bottom_cm\":"+ragdollNumber(collisionLow)+",\"collision_top_cm\":"+ragdollNumber(collisionHigh)+
      ",\"collision_support_gap_cm\":"+ragdollNumber(collisionGap)+",\"support_transform\":"+ragdollMatrix(support->transform)+
      ",\"inside_support_footprint\":"+(footprint?"true":"false")+",\"on_floor_under_table\":"+(floorUnder?"true":"false")+
      ",\"native_dynamic\":"+(drop.item->isDynamic()?"true":"false")+",\"sleeping\":"+(drop.item->physicsSleeping()?"true":"false")+
      ",\"pass\":"+(pass?"true":"false"));
    }
  if(!all)arenaAbort("furniture Item support mechanic failed after native settling");
  }

void furnitureRagdoll(FurnitureParts& state,World& world,Npc& player) {
  furnitureClearDrops(state,world);auto& fixture=state;
  if(!world.physic()->setMovableVobTransform(fixture.table,fixture.tableOrigin)) {
    arenaAbort("furniture preparation: cannot restore the authored table pose before NPC fall");return;
    }
  const auto table=furnitureBody(world,fixture.table);
  if(!table) {arenaAbort("furniture preparation: table setup required");return;}
  auto local=(table->localBounds[0]+table->localBounds[1])*.5f;
  local.x=table->localBounds[0].x-20.f;local.y=table->localBounds[0].y+10.f;
  auto at=local;table->transform.project(at);
  Vec3 direction(1,0,0);table->transform.project(direction);direction-=movablePosition(table->transform);direction.y=0;direction=Vec3::normalize(direction);
  auto* npc=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),at);
  if(!npc) {arenaAbort("furniture preparation: falling NPC insertion failed");return;}
  npc->clearAiQueue();npc->clearState(false);npc->stopWalking();npc->clearSpeed();npc->setExternalControl(true);
  npc->setDirection(direction);npc->updateTransform();
  npc->handle().flags=daedalus::NpcFlag(uint32_t(npc->handle().flags)&~uint32_t(daedalus::NpcFlag::IMMORTAL));
  npc->setLastHitBy(&player);
  if(!npc->setRagdollDown(true)) {arenaAbort("furniture preparation: NPC death refused");return;}
  npc->prepareRagdoll();
  if(!npc->ragdollBody()) {arenaAbort("furniture preparation: NPC has no native ragdoll");return;}
  npc->addRagdollImpulse(direction*35.f);fixture.corpse=npc;
  event("furniture_parts_ragdoll_setup","\"npc_id\":"+std::to_string(world.npcId(npc))+
    ",\"root_cm\":"+ragdollVector(at)+",\"impulse_ns\":"+ragdollVector(direction*35.f));
  }

void furnitureRagdollReport(FurnitureParts& state,World& world) {
  auto& fixture=state;const auto table=furnitureBody(world,fixture.table);
  if(!table || !fixture.corpse || !fixture.corpse->ragdollBody()) {arenaAbort("furniture ragdoll setup required");return;}
  auto inverse=table->transform;inverse.inverse();size_t inside=0,total=0;std::string parts="[";
  float maximumInset=0,maximumTablePenetration=0;size_t tableContacts=0;
  for(const auto& body:world.physic()->waterBodySnapshot()) {
    if(body.ragdoll!=fixture.corpse->ragdollBody())continue;
    auto center=(body.collisionBounds[0]+body.collisionBounds[1])*.5f;inverse.project(center);++total;
    const float inset=std::min({center.x-table->localBounds[0].x,table->localBounds[1].x-center.x,
                               center.z-table->localBounds[0].z,table->localBounds[1].z-center.z});
    const bool under=inset>0 && center.y>table->localBounds[0].y-5.f && center.y<table->localBounds[1].y-15.f;
    inside+=under;if(under)maximumInset=std::max(maximumInset,inset);
    if(parts.size()>1)parts+=',';
    parts+="{\"bone\":"+std::to_string(body.bone)+",\"center_local_cm\":"+ragdollVector(center)+
      ",\"footprint_inset_cm\":"+ragdollNumber(inset)+",\"under_tabletop\":"+(under?"true":"false")+"}";
    }
  for(const auto& contact:world.physic()->ragdollSupports(*fixture.corpse->ragdollBody(),2.f))
    if(contact.zenId==fixture.table) {++tableContacts;maximumTablePenetration=std::max(maximumTablePenetration,contact.depth);}
  const bool enabled=world.physic()->furniturePartsEnabled(),pass=total>0 && (!enabled || inside>0);
  event("furniture_parts_ragdoll_result","\"enabled\":"+std::string(enabled?"true":"false")+
    ",\"native_parts\":"+std::to_string(total)+",\"parts_under_tabletop\":"+std::to_string(inside)+
    ",\"maximum_under_tabletop_center_inset_cm\":"+ragdollNumber(maximumInset)+
    ",\"table_contacts\":"+std::to_string(tableContacts)+",\"maximum_table_penetration_cm\":"+ragdollNumber(maximumTablePenetration)+
    ",\"parts\":"+parts+"],\"pass\":"+(pass?"true":"false"));
  world.removeNpc(*fixture.corpse);fixture.corpse=nullptr;
  if(!pass)arenaAbort("furniture ragdoll mechanic failed after native settling");
  }

void furnitureWalk(FurnitureParts& state,World& world,Npc& player,bool npc) {
  auto& fixture=state;
  if(!world.physic()->setMovableVobTransform(fixture.table,fixture.tableOrigin)) {
    arenaAbort("furniture preparation: cannot restore the authored table pose before passage");return;
    }
  const auto table=furnitureBody(world,fixture.table);
  if(!table) {arenaAbort("furniture preparation: table setup required");return;}
  bool found=false;Vec3 start,goal;
  // Follow an ordinary supported lane alongside the complete authored table.
  // The same source bounds select the route in compound and hull modes.
  for(unsigned axis=0;axis<2 && !found;++axis)for(float side:{-1.f,1.f}) {
    const auto low=table->localBounds[0],high=table->localBounds[1];
    if(axis==0) {
      const float z=side<0?low.z-70.f:high.z+70.f;
      start=Vec3(low.x-100.f,low.y,z);goal=Vec3(high.x+100.f,low.y,z);
      } else {
      const float x=side<0?low.x-70.f:high.x+70.f;
      start=Vec3(x,low.y,low.z-100.f);goal=Vec3(x,low.y,high.z+100.f);
      }
    table->transform.project(start);table->transform.project(goal);
    const auto direction=Vec3::normalize(Vec3(goal.x-start.x,0,goal.z-start.z));
    const Vec3 across(-direction.z,0,direction.x);
    found=true;float height=0;
    for(unsigned i=0;i<=10 && found;++i)for(float lateral:{-30.f,0.f,30.f}) {
      auto point=start+(goal-start)*(float(i)/10.f)+across*lateral;
      const auto floor=world.physic()->landRay(point+Vec3(0,100,0),300.f);
      if(!floor.hasCol || floor.n.y<.95f) {found=false;break;}
      if(i==0 && lateral==0)height=floor.v.y;
      if(i>0 && std::abs(floor.v.y-height)>10.f) {found=false;break;}
      if(i==0 && lateral==0)start.y=floor.v.y;
      if(i==10 && lateral==0)goal.y=floor.v.y;
      }
    for(float lateral:{-30.f,0.f,30.f})for(float y:{20.f,100.f,170.f})
      if(found && world.physic()->ray(start+across*lateral+Vec3(0,y,0),goal+across*lateral+Vec3(0,y,0)).hasCol)found=false;
    if(found)break;
    }
  if(!found) {arenaAbort("furniture preparation: passage needs a supported native lane alongside the table");return;}
  player.clearAiQueue();player.stopWalking();player.clearSpeed();player.closeWeapon(true);
  if(npc) {
    player.setPosition(start+Vec3(0,0,450));player.updateTransform();
    fixture.walker=world.addNpc(world.script().findSymbolIndex("BAU_904_BAUER"),start);
    if(!fixture.walker) {arenaAbort("furniture preparation: passing NPC insertion failed");return;}
    fixture.walker->clearAiQueue();fixture.walker->clearState(false);fixture.walker->setExternalControl(true);
    fixture.walker->setDirection(goal-start);fixture.walker->updateTransform();fixture.walker->setWalkMode(WalkBit::WM_Walk);
    fixture.walkDestination=WayPoint(goal,"FP_FURNITURE_PASSAGE");fixture.walkDestination.groundPos=goal;
    fixture.walker->aiPush(AiQueue::aiGoToPoint(fixture.walkDestination));
    } else {
    player.setPosition(start);player.setDirection(goal-start);player.updateTransform();
    }
  fixture.walkStart=start;fixture.walkGoal=goal;fixture.walkDirection=Vec3::normalize(Vec3(goal.x-start.x,0,goal.z-start.z));
  fixture.walkPrevious=start;fixture.walkMaxStep=0;fixture.walkMaxCrossTrack=0;fixture.walkArrived=0;
  fixture.walkStartTick=world.tickCount();fixture.walking=true;
  event("furniture_parts_walk_setup","\"actor\":"+jsonStr(npc?"npc":"hero")+",\"start_cm\":"+ragdollVector(start)+
    ",\"goal_cm\":"+ragdollVector(goal)+",\"route_length_cm\":"+ragdollNumber((goal-start).length()));
  }

void furnitureWalkReport(FurnitureParts& state,World& world,Npc& player) {
  auto& fixture=state;fixture.walking=false;
  const auto end=fixture.walker?fixture.walker->position():player.position();
  const auto delta=end-fixture.walkStart;
  const bool pass=fixture.walkArrived>0 && fixture.walkMaxCrossTrack<35.f && fixture.walkMaxStep<20.f;
  event("furniture_parts_walk_result","\"enabled\":"+std::string(world.physic()->furniturePartsEnabled()?"true":"false")+
    ",\"actor\":"+jsonStr(fixture.walker?"npc":"hero")+",\"arrived\":"+(fixture.walkArrived?"true":"false")+
    ",\"elapsed_ms\":"+std::to_string((fixture.walkArrived?fixture.walkArrived:world.tickCount())-fixture.walkStartTick)+
    ",\"window_ms\":"+std::to_string(world.tickCount()-fixture.walkStartTick)+",\"start_cm\":"+ragdollVector(fixture.walkStart)+
    ",\"goal_cm\":"+ragdollVector(fixture.walkGoal)+",\"end_cm\":"+ragdollVector(end)+",\"travel_cm\":"+ragdollNumber(std::hypot(delta.x,delta.z))+
    ",\"max_cross_track_cm\":"+ragdollNumber(fixture.walkMaxCrossTrack)+",\"max_vertical_step_cm\":"+ragdollNumber(fixture.walkMaxStep)+
    ",\"character_virtual\":"+(Gothic::physicsFlags().characterVirtual?"true":"false")+",\"pass\":"+(pass?"true":"false"));
  if(fixture.walker) {world.removeNpc(*fixture.walker);fixture.walker=nullptr;}
  if(!pass)arenaAbort("furniture passage failed to arrive without a stall or vertical jump");
  }

void furniturePerformance(FurnitureParts& state,World& world,Npc& player) {
  auto& fixture=state;
  const auto* mesh=Resources::loadMesh(fixture.tableVisual);
  const auto* rule=MovableVobs::classifyVisual(fixture.tableVisual);
  if(!mesh || !rule || !fixture.loadBodies.empty()) {arenaAbort("furniture performance requires its tavern mesh and an empty load fixture");return;}
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    if(!point.underWater && (point.position()-fixture.tableCenter).quadLength()<5000.f*5000.f)points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[&](const auto* a,const auto* b) {
    const auto da=(a->position()-fixture.tableCenter).quadLength(),db=(b->position()-fixture.tableCenter).quadLength();
    return da==db?a->name<b->name:da<db;
    });
  const auto size=mesh->bbox()[1]-mesh->bbox()[0];
  std::vector<Vec3> supports;std::string waypoint;Vec3 observer;
  for(const auto* point:points) {
    const auto floor=world.physic()->landRay(point->position()+Vec3(0,100,0),300.f);
    if(!floor.hasCol || floor.n.y<.98f || std::abs(floor.v.y-point->position().y)>100.f)continue;
    for(unsigned columns:{5u,2u}) {
      supports.clear();
      for(unsigned i=0;i<10;++i) {
        const auto at=floor.v+Vec3((float(i%columns)-float(columns-1)*.5f)*(size.x+100.f),0,
                                   (float(i/columns)-float(10/columns-1)*.5f)*(size.z+100.f));
        Vec3 support,failed;
        if(!destructionSupported(world,fixture.tableVisual,at,support,failed))break;
        supports.push_back(support);
        }
      if(supports.size()==10) {waypoint=point->name;observer=floor.v;break;}
      }
    if(!waypoint.empty())break;
    }
  if(waypoint.empty()) {arenaAbort("furniture preparation: no clear native floor grid supports ten full table footprints");return;}
  player.clearAiQueue();player.stopWalking();player.clearSpeed();player.setPosition(observer);player.updateTransform();
  fixture.loadBodies.reserve(10);std::string placements="[";
  for(const auto& support:supports) {
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(support-Vec3(0,mesh->bbox()[0].y-.2f,0));
    fixture.loadBodies.push_back(world.physic()->addMovableVob(*mesh,fixture.tableVisual,matrix,
      rule->massMultiplier,rule->blocksWalking,rule->className,true));
    if(fixture.loadBodies.back().isEmpty() || fixture.loadBodies.back().mass()<=0) {
      arenaAbort("furniture preparation: load fixture lost a native dynamic table");return;
      }
    if(placements.size()>1)placements+=',';
    placements+=ragdollVector(support);
    }
  fixture.loadDecomposed=0;
  for(const auto& body:world.physic()->movableVobSnapshot())
    if(body.zenId==uint32_t(-1) && body.visual==fixture.tableVisual)fixture.loadDecomposed+=body.collisionParts>1;
  if(world.physic()->furniturePartsEnabled() && fixture.loadDecomposed!=10) {
    arenaAbort("furniture preparation: loaded-world performance needs ten decomposed native tables");return;
    }
  fixture.loadSamples.clear();fixture.loadSamples.reserve(720);fixture.loadMinimumActive=10;
  fixture.loadWarmUntil=world.tickCount()+2000;fixture.loadEnd=fixture.loadWarmUntil+10000;
  fixture.loadLastNative=world.physic()->simulationStep();fixture.loadZeroFrames=0;fixture.loadRunning=true;
  fixture.loadNextWake=world.tickCount()+1000;fixture.loadWakeResets=0;
  fixture.loadBounds[0]=mesh->bbox()[0];fixture.loadBounds[1]=mesh->bbox()[1];
  fixture.loadResetPoseError=fixture.loadResetVelocityError=fixture.loadResetAngularError=0;
  for(auto& body:fixture.loadBodies)body.addImpulse(Vec3(body.mass()*.3f,0,0));
  event("furniture_parts_performance_setup","\"world\":"+jsonStr(world.name())+",\"waypoint\":"+jsonStr(waypoint)+
    ",\"furniture\":10,\"decomposed_furniture\":"+std::to_string(fixture.loadDecomposed)+
    ",\"warmup_ms\":2000,\"measurement_ms\":10000,\"source_floor_verified\":true,\"support_cm\":"+placements+"]"+
    ",\"observer_cm\":"+ragdollVector(observer)+",\"world_bodies\":"+std::to_string(world.physic()->physicsStats().bodies)+
    ",\"world_npc_slots\":"+std::to_string(world.npcCount()));
  }

void furniturePerformanceSample(FurnitureParts& state,World& world,uint64_t dt) {
  auto& fixture=state;
  if(!fixture.loadRunning)return;
  const auto step=world.physic()->simulationStep(),advanced=step-fixture.loadLastNative;
  fixture.loadLastNative=step;
  if(world.tickCount()>=fixture.loadWarmUntil && world.tickCount()<=fixture.loadEnd) {
    size_t active=0;for(const auto& body:fixture.loadBodies)active+=!body.isSleeping();
    fixture.loadMinimumActive=std::min(fixture.loadMinimumActive,active);
    if(advanced>0) {
      const auto stats=world.physic()->physicsStats();
      fixture.loadSamples.push_back({world.tickCount(),dt,advanced,stats.stepMilliseconds,stats.bodies,active});
      }
    else ++fixture.loadZeroFrames;
    }
  if(world.tickCount()>=fixture.loadEnd)return;
  // The game's two-second sleep test permits bounded motion within 1.6 m.
  // Reset only these ten bodies' sleep timers every second, preserving their
  // poses and velocities through the existing native restore API.
  if(world.tickCount()>=fixture.loadNextWake) {
    fixture.loadNextWake=world.tickCount()+1000;
    for(auto& body:fixture.loadBodies) {
      const auto pose=body.matrix();const auto velocity=body.velocity(),angular=body.angularVelocity();
      body.restorePhysics(pose,false,velocity,angular);++fixture.loadWakeResets;
      const auto restored=body.matrix();
      for(unsigned i=0;i<8;++i) {
        Vec3 point(fixture.loadBounds[i&1].x,fixture.loadBounds[(i>>1)&1].y,fixture.loadBounds[(i>>2)&1].z);
        auto before=point,after=point;pose.project(before);restored.project(after);
        fixture.loadResetPoseError=std::max(fixture.loadResetPoseError,(after-before).length());
        }
      fixture.loadResetVelocityError=std::max(fixture.loadResetVelocityError,(body.velocity()-velocity).length());
      fixture.loadResetAngularError=std::max(fixture.loadResetAngularError,(body.angularVelocity()-angular).length());
      }
    }
  // A bounded horizontal drive exercises ordinary floor contacts as well.

  const float speed=(world.tickCount()/500)%2==0?.2f:-.2f;
  for(auto& body:fixture.loadBodies)
    body.addImpulse(Vec3(body.mass()*(speed-body.velocity().x*.01f),0,0));
  }

void furniturePerformanceReport(FurnitureParts& state,World& world) {
  auto& fixture=state;fixture.loadRunning=false;
  if(fixture.loadSamples.empty()) {arenaAbort("furniture performance has no loaded-world native samples");return;}
  auto* file=std::fopen((outDir+"/furniture-parts-physics.csv").c_str(),"w");
  if(!file) {arenaAbort("cannot write furniture native timing samples");return;}
  std::fprintf(file,"t_game_ms,dt_ms,native_steps,active_furniture,world_bodies,step_ms\n");
  std::vector<float> costs;costs.reserve(fixture.loadSamples.size());uint64_t steps=0,maxSteps=0;
  for(const auto& sample:fixture.loadSamples) {
    std::fprintf(file,"%llu,%llu,%llu,%zu,%zu,%.9g\n",(unsigned long long)sample.time,(unsigned long long)sample.dt,
      (unsigned long long)sample.nativeSteps,sample.active,sample.bodies,double(sample.milliseconds));
    costs.push_back(sample.milliseconds);steps+=sample.nativeSteps;maxSteps=std::max(maxSteps,sample.nativeSteps);
    }
  std::fclose(file);std::sort(costs.begin(),costs.end());const float p95=costs[size_t(std::ceil(.95*double(costs.size())))-1];
  const bool pass=fixture.loadMinimumActive==10 && steps>=590 && p95<=2.f &&
    fixture.loadResetPoseError<=.001f && fixture.loadResetVelocityError<=.001f && fixture.loadResetAngularError<=.000001f;
  event("furniture_parts_performance","\"world\":"+jsonStr(world.name())+",\"enabled\":"+
    (world.physic()->furniturePartsEnabled()?"true":"false")+",\"furniture\":10,\"decomposed_furniture\":"+
    std::to_string(fixture.loadDecomposed)+",\"minimum_active_furniture\":"+std::to_string(fixture.loadMinimumActive)+
    ",\"samples\":"+std::to_string(costs.size())+",\"native_steps\":"+std::to_string(steps)+
    ",\"max_native_steps_per_sample\":"+std::to_string(maxSteps)+",\"zero_native_frames\":"+std::to_string(fixture.loadZeroFrames)+
    ",\"warmup_ms\":2000,\"measurement_ms\":10000,\"p95_step_ms\":"+ragdollNumber(p95)+
    ",\"keep_awake_interval_ms\":1000,\"keep_awake_resets\":"+std::to_string(fixture.loadWakeResets)+
    ",\"maximum_reset_pose_error_cm\":"+ragdollNumber(fixture.loadResetPoseError)+
    ",\"maximum_reset_velocity_error_cm_s\":"+ragdollNumber(fixture.loadResetVelocityError)+
    ",\"maximum_reset_angular_error_rad_s\":"+ragdollNumber(fixture.loadResetAngularError)+
    ",\"peak_rss_bytes\":"+std::to_string(furniturePeakMemory())+",\"world_load_ms\":"+ragdollNumber(float(world.loadMilliseconds()))+
    ",\"timing_source\":\"loaded NEWWORLD DynamicWorld::physicsStats after ordinary world update\",\"pass\":"+(pass?"true":"false"));
  fixture.loadBodies.clear();
  if(!pass)arenaAbort("furniture loaded-world performance exceeded its active-scene budget or lost coverage");
  }

bool furniturePartsCommand(FurnitureParts& state,const std::vector<std::string>& a,World& w,Npc& pl) {
  const std::string& c = a[0];
  if(c=="furniture-parts" && a.size()==2 && a[1]=="setup") furnitureSetup(state,w,pl);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="trace") furnitureTraceBegin(state,w);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="trace-report") furnitureTraceReport(state,w);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="remove-table") furnitureRemoveTable(state,w);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="table-items") furnitureTableItems(state,w,pl);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="shelf-item") furnitureCupboardItem(state,w,pl);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="items-report") furnitureItemReport(state,w);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="clear-items") furnitureClearDrops(state,w);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="ragdoll") furnitureRagdoll(state,w,pl);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="ragdoll-report") furnitureRagdollReport(state,w);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="walk") furnitureWalk(state,w,pl);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="npc-walk") furnitureWalk(state,w,pl,true);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="walk-report") furnitureWalkReport(state,w,pl);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="performance") furniturePerformance(state,w,pl);
  else if(c=="furniture-parts" && a.size()==2 && a[1]=="performance-report") furniturePerformanceReport(state,w);
  else return false;
  return true;
  }

}
