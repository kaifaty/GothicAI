#include "testbench_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <map>

#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>

#include "game/gamescript.h"
#include "gothic.h"
#include "graphics/mesh/protomesh.h"
#include "graphics/mesh/submesh/packedmesh.h"
#include "graphics/worldview.h"
#include "physics/physicmeshshape.h"
#include "physics/physicvbo.h"
#include "resources.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

using namespace Tempest;

namespace testbench_detail {

namespace {

// Numeric-only Item hull fixtures. Source vertices and native transforms are in centimetres.
float itemHullSupportY(ItemHulls& state,float x,float z=0) {
  if(state.itemHullSurface=="terrain")
    return -(state.itemHullNormal.x*x+state.itemHullNormal.z*z)/state.itemHullNormal.y;
  if(state.itemHullSurface=="slope")return x*std::tan(10.f*float(M_PI)/180.f);
  if(state.itemHullSurface=="stairs")return std::floor((x+600.f)/35.f)*16.f;
  return state.itemHullSurface=="table"?80.f:0.f;
  }

bool itemHullAboveSupport(ItemHulls& state,const Vec3& local) {
  if(state.itemHullSurface=="terrain")return true;
  const float left=state.itemHullSurface=="stairs"?-600.f:-1200.f;
  const float right=state.itemHullSurface=="stairs"?625.f:1200.f;
  return local.x>=left && local.x<=right && local.z>=-900.f && local.z<=900.f;
  }

asset::MaterialGroup itemHullMaterial(int32_t material) {
  switch(material) {
    case MAT_WOOD:case MAT_LEATHER:return asset::MaterialGroup::WOOD;
    case MAT_STONE:case MAT_GLAS:return asset::MaterialGroup::STONE;
    case MAT_METAL:return asset::MaterialGroup::METAL;
    case MAT_CLAY:return asset::MaterialGroup::EARTH;
    default:return asset::MaterialGroup::UNDEFINED;
    }
  }

}

void itemHullCensus(World& world) {
  size_t instances=0,hulls=0,fallbacks=0,extraVisuals=0;
  std::map<std::string,std::string> visuals;
  std::map<std::string,size_t> reasons;
  std::vector<float> ratios,pointsBefore,pointsAfter;
  std::string outliers="[",extraPadding="[";
  world.script().getVm().enumerate_instances_by_class_name("C_ITEM",[&](daedalus::DaedalusSymbol& symbol) {
    Item item(world,symbol.index(),Item::T_Inventory);++instances;
    std::string visual=item.handle().visual;
    for(auto& c:visual)c=char(std::toupper(static_cast<unsigned char>(c)));
    if(visual.empty())return;
    const auto* mesh=Resources::loadMesh(visual);
    if(!mesh) {
      if(visuals.emplace(visual,"missing mesh").second) {++fallbacks;++reasons["missing mesh"];
        event("item_hulls_census_visual","\"visual\":"+jsonStr(visual)+",\"hull\":false,\"reason\":\"missing mesh\"");}
      return;
      }
    const auto& shape=world.physic()->itemHullShape(*mesh,visual);
    if(visuals.emplace(visual,shape.fallbackReason).second) {
      const bool hull=shape.shape!=nullptr;
      const auto before=hull?static_cast<const JPH::ConvexHullShape&>(*shape.originalShape).GetNumPoints():0;
      const auto after=hull?static_cast<const JPH::ConvexHullShape&>(*shape.shape).GetNumPoints():0;
      if(hull) {
        ++hulls;if(shape.boundsVolume>0)ratios.push_back(shape.volume/shape.boundsVolume);
        pointsBefore.push_back(float(before));pointsAfter.push_back(float(after));
        if(shape.extraPaddingCm>0.f) {
          ++extraVisuals;
          if(extraPadding.size()>1)extraPadding+=',';
          extraPadding+="{\"visual\":"+jsonStr(visual)+",\"extra_padding_cm\":"+ragdollNumber(shape.extraPaddingCm)+"}";
          }
        }
      else {++fallbacks;++reasons[shape.fallbackReason];}
      event("item_hulls_census_visual","\"visual\":"+jsonStr(visual)+",\"hull\":"+(hull?"true":"false")+
        ",\"reason\":"+jsonStr(shape.fallbackReason)+",\"hull_volume_m3\":"+ragdollNumber(shape.volume)+
        ",\"bounds_volume_m3\":"+ragdollNumber(shape.boundsVolume)+",\"ratio\":"+
        (shape.boundsVolume>0?ragdollNumber(shape.volume/shape.boundsVolume):"null")+
        ",\"original_points\":"+std::to_string(before)+",\"inflated_points\":"+std::to_string(after)+
        ",\"extra_padding_cm\":"+ragdollNumber(shape.extraPaddingCm));
      }
    Bounds bounds;bounds.assign(mesh->bboxMesh);
    const auto material=itemHullMaterial(item.handle().material);
    const auto identity=Matrix4x4::mkIdentity();
    auto oldBody=world.physic()->addDynamicObj(identity,bounds,material);
    auto newBody=world.physic()->addDynamicObj(identity,bounds,material,mesh,visual,true);
    const auto before=oldBody.mass(),after=newBody.mass();
    if(before>0 && after>0 && (after/before>10.f || before/after>10.f)) {
      if(outliers.size()>1)outliers+=',';
      outliers+="{\"symbol\":"+jsonStr(symbol.name())+",\"visual\":"+jsonStr(visual)+
        ",\"off_mass_kg\":"+ragdollNumber(before)+",\"on_mass_kg\":"+ragdollNumber(after)+
        ",\"on_over_off\":"+ragdollNumber(after/before)+"}";
      }
    });
  std::sort(ratios.begin(),ratios.end());
  std::sort(pointsBefore.begin(),pointsBefore.end());std::sort(pointsAfter.begin(),pointsAfter.end());
  const auto percentile=[](const std::vector<float>& values,float fraction) {
    return values.empty()?0.f:values[std::min(values.size()-1,size_t(std::round(float(values.size()-1)*fraction)))];
    };
  std::string failures="{";
  for(const auto& [reason,count]:reasons) {
    if(failures.size()>1)failures+=',';
    failures+=jsonStr(reason)+":"+std::to_string(count);
    }
  event("item_hulls_census","\"instances\":"+std::to_string(instances)+",\"visuals\":"+std::to_string(visuals.size())+
    ",\"hulls\":"+std::to_string(hulls)+",\"fallbacks\":"+std::to_string(fallbacks)+
    ",\"fallback_reasons\":"+failures+"},\"volume_ratio_p10\":"+ragdollNumber(percentile(ratios,.1f))+
    ",\"volume_ratio_median\":"+ragdollNumber(percentile(ratios,.5f))+",\"volume_ratio_p90\":"+ragdollNumber(percentile(ratios,.9f))+
    ",\"points_before_median\":"+ragdollNumber(percentile(pointsBefore,.5f))+
    ",\"points_before_p90\":"+ragdollNumber(percentile(pointsBefore,.9f))+
    ",\"points_before_max\":"+ragdollNumber(percentile(pointsBefore,1.f))+
    ",\"points_after_median\":"+ragdollNumber(percentile(pointsAfter,.5f))+
    ",\"points_after_p90\":"+ragdollNumber(percentile(pointsAfter,.9f))+
    ",\"points_after_max\":"+ragdollNumber(percentile(pointsAfter,1.f))+
    ",\"extra_visuals\":"+std::to_string(extraVisuals)+",\"extra_padding_visuals\":"+extraPadding+"]"+
    ",\"mass_outliers\":"+outliers+"],\"pass\":"+(hulls*100>=visuals.size()*99?"true":"false"));
  }

void itemHullClear(ItemHulls& state,World& world) {
  state.itemHullTracing=false;
  for(auto& drop:state.itemHullDrops)world.removeItem(*drop.item);
  state.itemHullDrops.clear();
  state.itemHullSupportBody=DynamicWorld::Item();state.itemHullSupportShape.reset();state.itemHullSupportQuery=nullptr;
  }

namespace {

bool itemHullTerrainPatch(ItemHulls& state,World& world,const Vec3& origin) {
  std::vector<const WayPoint*> points;
  world.forEachWayPoint([&](const WayPoint& point) {
    if(!point.underWater && (point.position()-origin).quadLength()<5000.f*5000.f)
      points.push_back(&point);
    });
  std::sort(points.begin(),points.end(),[&](const auto* a,const auto* b) {
    const auto da=(a->position()-origin).quadLength(),db=(b->position()-origin).quadLength();
    return da==db?a->name<b->name:da<db;
    });
  std::vector<Vec3> seeds{origin};
  for(size_t i=0;i<std::min(points.size(),size_t(128));++i)seeds.push_back(points[i]->position());
  for(const auto& seed:seeds)for(float x:{0.f,-150.f,150.f,-300.f,300.f})for(float z:{0.f,-150.f,150.f,-300.f,300.f}) {
    const auto center=world.physic()->landRay(seed+Vec3(x,300,z),1000.f);
    if(!center.hasCol || center.n.length()<.99f)continue;
    const auto normal=Vec3::normalize(center.n);
    if(normal.y<std::cos(2.f*float(M_PI)/180.f))continue;
    float maxError=0,maxNormal=0;bool flat=true;
    for(int ix=-2;ix<=2 && flat;++ix)for(int iz=-2;iz<=2;++iz) {
      const Vec3 offset(float(ix)*150.f,0,float(iz)*150.f);
      const auto y=center.v.y-Vec3::dotProduct(normal,offset)/normal.y;
      const auto hit=world.physic()->landRay(Vec3(center.v.x+offset.x,y+100.f,center.v.z+offset.z),200.f);
      if(!hit.hasCol || hit.n.length()<.99f) {flat=false;break;}
      const float error=std::abs(Vec3::dotProduct(hit.v-center.v,normal));
      const float angle=std::acos(std::clamp(Vec3::dotProduct(normal,Vec3::normalize(hit.n)),-1.f,1.f))*180.f/float(M_PI);
      maxError=std::max(maxError,error);maxNormal=std::max(maxNormal,angle);
      if(error>1.f || angle>1.f) {flat=false;break;}
      }
    if(!flat)continue;
    state.itemHullAnchor=center.v;state.itemHullNormal=normal;
    event("item_hulls_terrain","\"origin_cm\":"+ragdollVector(center.v)+",\"normal\":"+ragdollVector(normal)+
      ",\"distance_from_hero_cm\":"+ragdollNumber((center.v-origin).length())+
      ",\"sample_extent_cm\":300,\"samples\":25,\"max_plane_error_cm\":"+ragdollNumber(maxError)+
      ",\"max_normal_error_degrees\":"+ragdollNumber(maxNormal)+",\"synthetic_support\":false,\"pass\":true");
    return true;
    }
  return false;
  }

DynamicWorld::RayLandResult itemHullTerrainRay(ItemHulls& state,World& world,const Vec3& point) {
  return world.physic()->landRay(Vec3(point.x,state.itemHullAnchor.y+500.f,point.z),1500.f);
  }

}

void itemHullSetup(ItemHulls& state,World& world,Npc& player,std::string_view surface) {
  if(surface!="flat" && surface!="slope" && surface!="stairs" && surface!="table" && surface!="load" && surface!="terrain" && surface!="giant_quad") {
    arenaAbort("item hull setup expects flat, slope, stairs, table, load, terrain or giant_quad");return;
    }
  itemHullClear(state,world);
  const auto collisionStats=world.physic()->staticCollisionStats();
  state.itemHullLoad=surface=="load" || surface=="terrain" || surface=="giant_quad";state.itemHullSurface=std::string(surface);
  if(state.itemHullAnchor.y==0)state.itemHullHero=player.position();
  if(surface=="terrain") {
    if(!itemHullTerrainPatch(state,world,state.itemHullHero)) {arenaAbort("item hull terrain fixture has no nearby flat native landscape patch");return;}
    }
  else {state.itemHullAnchor=state.itemHullHero+Vec3(0,10000,0);state.itemHullNormal=Vec3(0,1,0);}
  const auto anchor=state.itemHullAnchor;
  asset::WorldMesh support;support.materials.resize(1);support.materials[0].group=asset::MaterialGroup::STONE;
  const auto quad=[&](Vec3 a,Vec3 b,Vec3 c,Vec3 d) {
    const auto index=uint32_t(support.vertices.size());
    for(const auto point:{a,b,c,d})support.vertices.push_back({point.x,point.y,point.z});
    support.polygons.vertex_indices.insert(support.polygons.vertex_indices.end(),{index,index+2,index+1,index,index+3,index+2});
    support.polygons.material_indices.insert(support.polygons.material_indices.end(),{0,0});
    };
  if(surface=="terrain") {
    // The original world landscape is the only support in this diagnostic.
    }
  else if(surface=="stairs") {
    for(int i=0;i<35;++i) {
      const float x=-600.f+35.f*float(i),y=16.f*float(i);
      quad({x,y,-900},{x,y,900},{x+35,y,900},{x+35,y,-900});
      quad({x+35,y,-900},{x+35,y,900},{x+35,y+16,900},{x+35,y+16,-900});
      }
    }
  else if(surface=="giant_quad") {
    // Keep the two-triangle support to catch small hulls crossing large faces.
    quad({-1200,0,-900},{-1200,0,900},{1200,0,900},{1200,0,-900});
    }
  else {
    for(int x=-1200;x<1200;x+=100)for(int z=-900;z<900;z+=100) {
      const float left=itemHullSupportY(state,float(x)),right=itemHullSupportY(state,float(x+100));
      quad({float(x),left,float(z)},{float(x),left,float(z+100)},
           {float(x+100),right,float(z+100)},{float(x+100),right,float(z)});
      }
    }
  if(surface!="terrain") {
    state.itemHullSupportQuery=PhysicVbo(PackedMesh(support,PackedMesh::PK_Physic)).shape();
    state.itemHullSupportShape.reset(PhysicMeshShape::load(PackedMesh(support,PackedMesh::PK_Physic)));
    auto matrix=Matrix4x4::mkIdentity();matrix.translate(anchor);
    state.itemHullSupportBody=world.physic()->addStaticObj(state.itemHullSupportShape.get(),matrix,"BENCH_ITEM_HULL_SUPPORT");
    if(state.itemHullSupportBody.isEmpty()) {arenaAbort("item hull fixture lacks native support");return;}
    }
  const Vec3 heroLocal=surface=="terrain"?Vec3():
                       surface=="stairs"?Vec3(572.5f,itemHullSupportY(state,572.5f),-700):Vec3(-850,itemHullSupportY(state,-850),0);
  player.closeWeapon(true);player.clearAiQueue();player.stopWalking();
  player.setPosition(anchor+heroLocal);player.updateTransform();
  struct Spec {const char* label;std::initializer_list<const char*> symbols;bool longAxis=false;};
  const Spec specs[]{
    {"coin",{"ITMI_GOLD"}},
    {"potion",{"ITPO_HEALTH_01","ITPO_HEALTH_02"}},
    {"beer",{"ITFO_BEER"}},
    {"apple",{"ITFO_APPLE"}},
    {"sword",{"ITMW_1H_COMMON_01","ITMW_1H_SLD_SWORD"},true},
    {"axe",{"ITMW_2H_AXE_L_01","ITMW_2H_AXE_L_02","ITMW_2H_AXE_H_01"},true},
    {"bow",{"ITRW_BOW_L_01","ITRW_BOW_L_02"},true},
    {"scroll",{"ITSC_LIGHT","ITSC_FIREBOLT"}},
    {"torch",{"ITLSTORCH","ITLSTORCHBURNING"}},
    {"cup",{"ITMI_CUP","ITMI_SILVERCUP","ITMI_GOLDCUP","ITMI_PLATE"}}
    };
  for(size_t i=0;i<(state.itemHullLoad?50:std::size(specs));++i) {
    const auto& spec=specs[i%std::size(specs)];size_t symbol=size_t(-1);
    for(const auto name:spec.symbols)if((symbol=world.script().findSymbolIndex(name))!=size_t(-1))break;
    if(symbol==size_t(-1)) {arenaAbort("item hull fixture script instance missing: "+std::string(spec.label));return;}
    Item definition(world,symbol,Item::T_Inventory);
    const auto* mesh=Resources::loadMesh(definition.handle().visual);
    if(!mesh) {arenaAbort("item hull fixture has no rendered mesh: "+std::string(spec.label));return;}
    const auto& hull=world.physic()->itemHullShape(*mesh,definition.handle().visual);
    std::vector<Vec3> visibleVertices;mesh->appendItemVertices(visibleVertices);
    if(visibleVertices.empty()) {arenaAbort("item hull fixture has no visible mesh vertices: "+std::string(spec.label));return;}
    Vec3 place(float(i%5)*190.f-380.f,0,float(i/5)*260.f-130.f);
    // Stairs: one mid-tread line, a 170 cm lane per item, the hero above them. On a riser edge the
    // support ray below depends on float rounding, and an item rolling down a shared lane wakes those below.
    if(surface=="stairs")place=Vec3(12.5f,0,float(i)*170.f-765.f);
    if(state.itemHullLoad) {
      const float angle=float(i%10)*2.f*float(M_PI)/10.f,radius=110.f+35.f*float(i/10);
      place=heroLocal+Vec3(std::cos(angle)*radius,0,std::sin(angle)*radius);
      }
    // All drop from the lowest visible mesh point, exactly 100 cm above support.
    // A 20 degree tilt forces a contact response without an exactly vertical unstable rest pose.
    auto pose=Matrix4x4::mkIdentity();pose.rotateOX(20);pose.rotateOY(37);
    Vec3 axis;
    const auto extent=mesh->bboxMesh[1]-mesh->bboxMesh[0];
    if(extent.x>=extent.y && extent.x>=extent.z)axis=Vec3(1,0,0);
    else if(extent.y>=extent.z) {axis=Vec3(0,1,0);pose.rotateOZ(70);}
    else axis=Vec3(0,0,1);
    if(surface=="stairs" && spec.longAxis) {
      // The tread direction is Z. Start 20 degrees above horizontal, freely
      // falling; the solver alone controls every pose after insertion.
      pose=Matrix4x4::mkIdentity();
      pose.rotateOX(axis.y>0?70.f:20.f);
      if(axis.x>0)pose.rotateOY(90);
      }
    float lowest=std::numeric_limits<float>::infinity();
    for(auto point:visibleVertices) {pose.project(point);lowest=std::min(lowest,point.y-itemHullSupportY(state,place.x+point.x,place.z+point.z));}
    place.y=100.f-lowest;
    const auto at=anchor+place;pose.set(3,0,at.x);pose.set(3,1,at.y);pose.set(3,2,at.z);
    const float surfaceY=itemHullSupportY(state,place.x,place.z);
    const auto floor=world.physic()->landRay(anchor+Vec3(place.x,surfaceY+10,place.z),20);
    if(!floor.hasCol || std::abs(floor.v.y-anchor.y-surfaceY)>(surface=="terrain"?1.f:.1f)) {
      arenaAbort("item hull native support ray disagrees with authored surface");return;
      }
    auto* item=world.addItemDyn(symbol,pose,world.npcId(&player));
    if(!item || !item->isDynamic()) {arenaAbort("item hull fixture world Item has no native body");return;}
    ItemHulls::ItemHullDrop drop;drop.item=item;drop.label=spec.label;
    drop.symbol=world.script().findSymbol(symbol)->name();drop.vertices=std::move(visibleVertices);drop.shape=hull.originalShape;
    drop.extraPaddingCm=Gothic::settingsGetI("GAME","item_hulls")?hull.extraPaddingCm:0.f;
    if(spec.longAxis)drop.axis=axis;
    drop.mass=item->physicsMass();state.itemHullDrops.push_back(std::move(drop));
    event("item_hulls_drop","\"surface\":"+jsonStr(surface)+",\"index\":"+std::to_string(i)+
      ",\"symbol\":"+jsonStr(state.itemHullDrops.back().symbol)+",\"label\":"+jsonStr(spec.label)+
      ",\"mass_kg\":"+ragdollNumber(item->physicsMass())+",\"extra_padding_cm\":"+ragdollNumber(state.itemHullDrops.back().extraPaddingCm)+
      ",\"gap_max_cm\":"+ragdollNumber(1.05f+state.itemHullDrops.back().extraPaddingCm)+
      ",\"drop_height_cm\":100,\"hull\":"+(hull.shape!=nullptr?"true":"false")+
      ",\"position_cm\":"+ragdollVector(at)+",\"native_floor_cm\":"+ragdollVector(floor.v)+",\"native_floor_normal\":"+ragdollVector(floor.n));
    }
  state.itemHullStart=world.tickCount();state.itemHullSample=uint64_t(-1);state.itemHullCosts.clear();state.itemHullTracing=true;
  event("item_hulls_setup","\"surface\":"+jsonStr(surface)+",\"items\":"+std::to_string(state.itemHullDrops.size())+
    ",\"enabled\":"+(Gothic::settingsGetI("GAME","item_hulls")?"true":"false")+",\"hero_cm\":"+ragdollVector(player.position())+
    ",\"world_load_ms\":"+ragdollNumber(float(world.loadMilliseconds()))+
    ",\"static_shape_bytes\":"+std::to_string(collisionStats.shapeBytes)+",\"static_triangles\":"+std::to_string(collisionStats.triangles)+
    ",\"support_origin_cm\":"+ragdollVector(anchor)+",\"native_support_verified\":true");
  }

void itemHullSample(ItemHulls& state,World& world) {
  if(!state.itemHullTracing || state.itemHullSample==world.tickCount())return;
  state.itemHullSample=world.tickCount();
  state.itemHullCosts.push_back(world.physic()->physicsStats().stepMilliseconds);
  for(auto& drop:state.itemHullDrops) {
    const auto pose=drop.item->transform();
    drop.belowWorld|=drop.item->position().y<world.view()->bbox().first.y;
    if(drop.item->physicsSleeping() && drop.sleepAt==uint64_t(-1)) {
      drop.sleepAt=world.tickCount()-state.itemHullStart;drop.sleepTransform=pose;
      drop.sleepPosition=drop.item->position();
      }
    if(drop.sleepAt!=uint64_t(-1))
      for(auto point:drop.vertices) {
        auto previous=point;pose.project(point);drop.sleepTransform.project(previous);
        drop.maxJitter=std::max(drop.maxJitter,(point-previous).length());
        }
    }
  }

namespace {

float itemHullGap(ItemHulls& state,World& world,const ItemHulls::ItemHullDrop& drop) {
  float gap=std::numeric_limits<float>::infinity();
  for(auto point:drop.vertices) {
    drop.item->transform().project(point);
    if(state.itemHullSurface=="terrain") {
      const auto floor=itemHullTerrainRay(state,world,point);
      if(floor.hasCol)gap=std::min(gap,point.y-floor.v.y);
      }
    else {
      point-=state.itemHullAnchor;
      gap=std::min(gap,point.y-itemHullSupportY(state,point.x,point.z));
      }
    }
  if(state.itemHullSurface!="stairs" || drop.shape==nullptr)return gap;
  // A hull face can touch a tread edge with no hull vertex at that height.
  // Query the identical native support mesh, including those clipped contacts.
  // Side-only riser contacts do not measure the gap above an upward support.
  auto pose=drop.item->transform();
  const JPH::Mat44 local(
    JPH::Vec4(pose.at(0,0),pose.at(0,1),pose.at(0,2),0),
    JPH::Vec4(pose.at(1,0),pose.at(1,1),pose.at(1,2),0),
    JPH::Vec4(pose.at(2,0),pose.at(2,1),pose.at(2,2),0),
    JPH::Vec4((pose.at(3,0)-state.itemHullAnchor.x)*.01f,
              (pose.at(3,1)-state.itemHullAnchor.y)*.01f,
              (pose.at(3,2)-state.itemHullAnchor.z)*.01f,1));
  JPH::CollideShapeSettings settings;settings.mMaxSeparationDistance=.5f;
  settings.mActiveEdgeMode=JPH::EActiveEdgeMode::CollideOnlyWithActive;
  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> contacts;
  JPH::CollisionDispatch::sCollideShapeVsShape(drop.shape,state.itemHullSupportQuery,
    JPH::Vec3::sOne(),JPH::Vec3::sOne(),local.PreTranslated(drop.shape->GetCenterOfMass()),
    JPH::Mat44::sTranslation(state.itemHullSupportQuery->GetCenterOfMass()),{},{},settings,contacts);
  float depth=-std::numeric_limits<float>::infinity();
  for(const auto& contact:contacts.mHits)
    if(std::abs(contact.mPenetrationAxis.NormalizedOr(JPH::Vec3::sZero()).GetY())>.7f)
      depth=std::max(depth,contact.mPenetrationDepth);
  return std::isfinite(depth)?-100.f*depth:gap;
  }

}

void itemHullReport(ItemHulls& state,World& world) {
  itemHullSample(state,world);state.itemHullTracing=false;
  const bool enabled=Gothic::settingsGetI("GAME","item_hulls")!=0;
  const bool reportOnly=false;
  bool pass=true;size_t through=0,escaped=0,below=0,jitter=0,unslept=0,unsleptSupported=0;
  for(size_t i=0;i<state.itemHullDrops.size();++i) {
    const auto& drop=state.itemHullDrops[i];const float gap=itemHullGap(state,world,drop);
    const auto position=drop.item->position();
    const bool leftSupport=!itemHullAboveSupport(state,position-state.itemHullAnchor);
    float highest=-std::numeric_limits<float>::infinity();
    for(auto point:drop.vertices) {
      drop.item->transform().project(point);
      if(state.itemHullSurface=="terrain") {
        const auto floor=itemHullTerrainRay(state,world,point);
        if(floor.hasCol)highest=std::max(highest,point.y-floor.v.y);
        }
      else {
        point-=state.itemHullAnchor;
        highest=std::max(highest,point.y-itemHullSupportY(state,point.x,point.z));
        }
      }
    // Falling off a finite support is distinct from crossing its surface.
    const bool passedThrough=!leftSupport && highest<-1.f;
    Vec3 origin,axis=drop.axis;drop.item->transform().project(origin);drop.item->transform().project(axis);axis-=origin;
    const float angle=axis.length()>0?std::asin(std::clamp(std::abs(axis.y)/axis.length(),0.f,1.f))*180.f/float(M_PI):0.f;
    const bool slept=drop.sleepAt!=uint64_t(-1),moved=drop.maxJitter>.0001f;
    through+=passedThrough;escaped+=leftSupport;below+=drop.belowWorld;jitter+=moved;unslept+=!slept;
    unsleptSupported+=!slept && !leftSupport;
    // Compare ON/OFF sleep times only on flat/table. Other finite supports
    // allow a rounded item to roll off, but a supported item must settle.
    const bool ok=!passedThrough && !drop.belowWorld && (leftSupport || state.itemHullLoad ||
      (slept && !moved && gap>=-1.05f && gap<=1.05f+drop.extraPaddingCm && (drop.axis.quadLength()==0 || angle<=15.f)));
    pass&=ok;
    event("item_hulls_result","\"surface\":"+jsonStr(state.itemHullSurface)+",\"index\":"+std::to_string(i)+
      ",\"symbol\":"+jsonStr(drop.symbol)+",\"label\":"+jsonStr(drop.label)+",\"mass_kg\":"+ragdollNumber(drop.mass)+
      ",\"sleep_ms\":"+(slept?std::to_string(drop.sleepAt):"null")+",\"support_gap_cm\":"+ragdollNumber(gap)+
      ",\"extra_padding_cm\":"+ragdollNumber(drop.extraPaddingCm)+
      ",\"gap_max_cm\":"+ragdollNumber(1.05f+drop.extraPaddingCm)+
      ",\"long_axis_degrees\":"+(drop.axis.quadLength()>0?ragdollNumber(angle):"null")+
      ",\"post_sleep_motion_cm\":"+ragdollNumber(drop.maxJitter)+",\"position_cm\":"+ragdollVector(position)+
      ",\"above_support\":"+(!leftSupport?"true":"false")+",\"escaped_edge\":"+(leftSupport?"true":"false")+
      ",\"passed_through\":"+(passedThrough?"true":"false")+
      ",\"below_world\":"+(drop.belowWorld?"true":"false")+",\"hull_criteria_pass\":"+(ok?"true":"false")+
      ",\"report_only\":"+(reportOnly?"true":"false")+",\"pass\":"+(!enabled || reportOnly || ok?"true":"false"));
    }
  auto costs=state.itemHullCosts;std::sort(costs.begin(),costs.end());
  const float p95=costs.empty()?0.f:costs[std::min(costs.size()-1,size_t(std::ceil(float(costs.size())*.95f)-1))];
  if(state.itemHullLoad && state.itemHullSurface!="giant_quad")pass&=p95<=2.f;
  event("item_hulls_summary","\"surface\":"+jsonStr(state.itemHullSurface)+",\"items\":"+std::to_string(state.itemHullDrops.size())+
    ",\"samples\":"+std::to_string(costs.size())+",\"physics_p95_ms\":"+ragdollNumber(p95)+
    ",\"passed_through\":"+std::to_string(through)+",\"escaped_edge\":"+std::to_string(escaped)+
    ",\"below_world\":"+std::to_string(below)+",\"jittering\":"+std::to_string(jitter)+
    ",\"unslept\":"+std::to_string(unslept)+",\"unslept_supported\":"+std::to_string(unsleptSupported)+
    ",\"hull_criteria_pass\":"+(pass?"true":"false")+",\"report_only\":"+(reportOnly?"true":"false")+
    ",\"pass\":"+(!enabled || reportOnly || pass?"true":"false"));
  }

bool itemHullsCommand(ItemHulls& state,const std::vector<std::string>& a,World& w,Npc& pl) {
  const std::string& c = a[0];
  if(c=="item-hulls" && a.size()==2 && a[1]=="census")itemHullCensus(w);
  else if(c=="item-hulls" && a.size()==3 && a[1]=="setup")itemHullSetup(state,w,pl,a[2]);
  else if(c=="item-hulls" && a.size()==2 && a[1]=="report")itemHullReport(state,w);
  else if(c=="item-hulls" && a.size()==2 && a[1]=="clear") {
    itemHullClear(state,w);pl.setPosition(state.itemHullHero);pl.updateTransform();
    }
  else return false;
  return true;
  }

}
