#pragma once

#include "navbench.h"
#include "physics/navmesh.h"
#include "physics/dynamicworld.h"
#include "world/world.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

// Passive scene qualification for the live stand. No queried truth enters the
// teacher, eye, memory or policy, and the existing NPC supplies only guild sizes.
namespace NavLiveProbes {
using Tempest::Vec3;

inline DynamicWorld::RayWaterResult waterAbove(World& world,const Vec3& floor) {
  auto water=world.physic()->waterRay(floor+Vec3(0,2,0),50.f);
  if(water.hasCol)return water;
  // A cave-roof check can reject the floor at fraction zero. The downward
  // physical query proves a water surface without changing collision filters.
  const auto surface=world.physic()->navigationRay(floor+Vec3(0,20000,0),floor+Vec3(0,2,0),nullptr);
  if(surface.hasCol && surface.kind==DynamicWorld::NavigationHit::Water) {
    water.hasCol=true;water.wdepth=surface.point.y;
  }
  return water;
}

namespace Detail {
constexpr float Pi=3.14159265358979323846f;
inline void number(std::ostream& out,double value) {if(std::isfinite(value))out<<value;else out<<"null";}
inline void vector(std::ostream& out,const Vec3& value) {
  out<<'[';number(out,value.x);out<<',';number(out,value.y);out<<',';number(out,value.z);out<<']';
}
inline NavMesh::Point meters(const Vec3& value) {return {value.x*.01f,value.y*.01f,value.z*.01f};}
inline Vec3 centimeters(const NavMesh::Point& value) {return {value[0]*100.f,value[1]*100.f,value[2]*100.f};}
inline Vec3 point(World& world,const std::string& text) {
  if(text.rfind("cm:",0)!=0) {
    const auto* value=world.findPoint(text,false);
    if(!value)throw std::runtime_error("nav live probes: missing waypoint "+text);
    return value->groundPos;
  }
  auto values=text.substr(3);std::replace(values.begin(),values.end(),',',' ');
  std::istringstream input(values);Vec3 value;std::string extra;
  if(!(input>>value.x>>value.y>>value.z) || input>>extra ||
     !std::isfinite(value.x) || !std::isfinite(value.y) || !std::isfinite(value.z))
    throw std::runtime_error("nav live probes: invalid cm:x,y,z point");
  return value;
}
inline const char* materialName(asset::MaterialGroup value) {
  switch(value) {
    case asset::MaterialGroup::UNDEFINED:return "undefined";
    case asset::MaterialGroup::METAL:return "metal";
    case asset::MaterialGroup::STONE:return "stone";
    case asset::MaterialGroup::WOOD:return "wood";
    case asset::MaterialGroup::EARTH:return "earth";
    case asset::MaterialGroup::WATER:return "water";
    case asset::MaterialGroup::SNOW:return "snow";
    case asset::MaterialGroup::NONE:return "none";
  }
  return "unknown";
}
inline const char* hitName(DynamicWorld::NavigationHit value) {
  switch(value) {
    case DynamicWorld::NavigationHit::None:return "none";
    case DynamicWorld::NavigationHit::World:return "world";
    case DynamicWorld::NavigationHit::StaticVob:return "static_vob";
    case DynamicWorld::NavigationHit::MovableVob:return "movable_vob";
    case DynamicWorld::NavigationHit::Npc:return "npc";
    case DynamicWorld::NavigationHit::Water:return "water";
  }
  return "unknown";
}
struct Depth final {
  bool floor=false,water=false;
  float centimeters=0;
  Vec3 surface;
};
inline Depth depth(World& world,const Vec3& at) {
  const auto floor=world.physic()->landRay(at+Vec3(0,200,0));
  if(!floor.hasCol)return {};
  const auto water=waterAbove(world,floor.v);
  Depth result;result.floor=true;result.water=water.hasCol && std::isfinite(water.wdepth);
  if(result.water) {
    result.centimeters=std::max(0.f,water.wdepth-floor.v.y);
    result.surface={at.x,water.wdepth,at.z};
  }
  return result;
}
inline void support(std::ostream& out,World& world,const Npc& npc,const Vec3& at,const NavMesh& mesh) {
  const auto floor=world.physic()->landRay(at+Vec3(0,200,0),500.f);
  Vec3 normal=floor.n;
  if(floor.hasCol && normal.length()<.5f) {
    const auto hit=world.physic()->navigationRay(floor.v+Vec3(0,2,0),floor.v-Vec3(0,2,0),&npc);
    if(hit.hasCol && (hit.point-floor.v).length()<3.f)normal=hit.normal;
  }
  const float normalLength=normal.length();
  const bool normalValid=normalLength>.5f;
  if(normalValid)normal*=1.f/normalLength;
  const bool supported=floor.hasCol && normalValid && normal.y>=.65f && std::abs(floor.v.y-at.y)<=100.f;
  bool clear=false;
  float waterDepth=0;
  if(floor.hasCol) {
    const auto capsule=npc.navigationCapsule();
    clear=!world.physic()->sweep(floor.v+Vec3(0,capsule.x+2.f,0),
                                floor.v+Vec3(0,std::max(capsule.x+2.f,capsule.y-capsule.x),0),capsule.x).hasCol;
    const auto water=waterAbove(world,floor.v);
    if(water.hasCol && std::isfinite(water.wdepth))waterDepth=std::max(0.f,water.wdepth-floor.v.y);
  }
  const auto location=mesh.groundLocation(meters(at));
  out<<"{\"supported\":"<<supported<<",\"floor_found\":"<<floor.hasCol<<",\"floor_cm\":";
  if(floor.hasCol)vector(out,floor.v);else out<<"null";
  out<<",\"normal_valid\":"<<normalValid<<",\"normal_y\":";if(normalValid)number(out,normal.y);else out<<"null";
  out<<",\"slope_deg\":";if(normalValid)number(out,std::acos(std::clamp(normal.y,-1.f,1.f))*180.f/Pi);else out<<"null";
  out<<",\"height_delta_cm\":";if(floor.hasCol)number(out,floor.v.y-at.y);else out<<"null";
  out<<",\"capsule_clear\":"<<clear<<",\"water_depth_cm\":";if(floor.hasCol)number(out,waterDepth);else out<<"null";
  out<<",\"dry\":"<<(floor.hasCol && waterDepth<=10.f)<<",\"ground_known\":"<<location.known
     <<",\"ground_component\":"<<location.component<<",\"ground_snap_cm\":"<<location.snapDistance*100.f
     <<",\"ground_xz_snap_cm\":"<<location.horizontalDistance*100.f<<",\"ground_y_offset_cm\":"<<location.verticalOffset*100.f<<'}';
}
// The native edgeAhead sampling recipe at an explicit pose, without teleporting
// the prototype NPC. Eight directions and 200 cm bound the per-pair work.
inline float edgeAhead(World& world,const Vec3& from,const Vec3& direction,float stepHeight) {
  const auto floor=[&](float distance,float height) {
    const auto at=from+direction*distance;
    return world.physic()->landRay({at.x,height+stepHeight+2.f,at.z});
  };
  auto ground=floor(0,from.y);
  if(!ground.hasCol)return 200.f;
  float previous=0,height=ground.v.y;
  for(float distance=10.f;distance<=200.f;distance+=10.f) {
    ground=floor(distance,height);
    if(!ground.hasCol || height-ground.v.y>100.f) {
      float high=distance;
      for(unsigned iteration=0;iteration<8;++iteration) {
        const float middle=(previous+high)*.5f;
        const auto candidate=floor(middle,height);
        if(candidate.hasCol && height-candidate.v.y<=100.f)previous=middle;else high=middle;
      }
      return previous;
    }
    previous=distance;height=ground.v.y;
  }
  return 200.f;
}
inline Vec3 along(const NavMesh::Path& path,float distance) {
  for(size_t i=1;i<path.corners.size();++i) {
    const auto begin=centimeters(path.corners[i-1]),end=centimeters(path.corners[i]);
    const float length=(end-begin).length();
    if(distance<=length && length>0)return begin+(end-begin)*(distance/length);
    distance-=length;
  }
  return centimeters(path.corners.back());
}
}

inline void command(const std::vector<std::string>& args,World& world,const std::string& output) {
  using namespace Detail;
  if(args.size()!=4)throw std::runtime_error("navdemos live-probes NPC FILE (rows LABEL START GOAL)");
  const auto* npc=world.findNpcByInstance(world.script().findSymbolIndex(args[2]));
  if(!npc)throw std::runtime_error("nav live probes: missing NPC");
  std::ifstream input(args[3]);
  if(!input)throw std::runtime_error("nav live probes: cannot read input");
  std::ofstream out(std::filesystem::path(output)/"live-probes.jsonl");
  if(!out)throw std::runtime_error("nav live probes: cannot write output");
  out<<std::setprecision(9)<<std::boolalpha;
  const auto& mesh=NavBench::navigationMesh(world);
  const auto& guild=world.script().guildVal();
  const float chest=float(guild.water_depth_chest[npc->guild()]);
  const float configuredStep=float(guild.step_height[npc->guild()]);
  const float step=configuredStep>0.f?configuredStep:50.f;
  std::string line;std::set<std::string> labels;size_t count=0;
  while(std::getline(input,line)) {
    std::istringstream row(line);std::string label,startText,goalText,extra;
    if(!(row>>label) || label[0]=='#')continue;
    if(!(row>>startText>>goalText) || row>>extra || !labels.insert(label).second)
      throw std::runtime_error("nav live probes: rows require unique LABEL START GOAL");
    ++count;
    const auto start=point(world,startText),goal=point(world,goalText),delta=goal-start;
    const float direct=delta.length(),horizontal=std::hypot(delta.x,delta.z);
    const auto path=mesh.findPath(meters(start),meters(goal));
    const auto rayStart=start+Vec3(0,90,0),rayEnd=goal+Vec3(0,90,0);
    size_t selfHits=0;std::string obstructionLabel;
    const auto obstruction=world.physic()->rayClothCamera(rayStart,rayEnd,"",nullptr,selfHits,&obstructionLabel);
    const auto sensor=world.physic()->navigationRay(rayStart,rayEnd,npc);
    auto obstructionNormal=obstruction.n;
    if(obstruction.hasCol && sensor.hasCol && (obstruction.v-sensor.point).length()<3.f && obstructionNormal.length()<.5f)
      obstructionNormal=sensor.normal;
    const float normalLength=obstructionNormal.length();
    if(normalLength>.5f)obstructionNormal*=1.f/normalLength;
    const auto visibleFrom=start+Vec3(0,160,0),visibleTo=goal+Vec3(0,160,0);
    std::string visibleLabel;
    const auto visible=world.physic()->rayClothCamera(visibleFrom,visibleTo,"",nullptr,selfHits,&visibleLabel);
    float firstDot=0;bool firstDirection=false;
    for(size_t i=1;i<path.corners.size();++i) {
      const auto direction=centimeters(path.corners[i])-centimeters(path.corners.front());
      const float length=std::hypot(direction.x,direction.z);
      if(length<=1.f || horizontal<=1.f)continue;
      firstDot=(direction.x*delta.x+direction.z*delta.z)/(length*horizontal);firstDirection=true;break;
    }
    const size_t directSegments=std::max(size_t(1),size_t(std::ceil(horizontal/25.f)));
    size_t wet=0,deep=0,noFloor=0;Vec3 firstDeep;
    for(size_t i=0;i<=directSegments;++i) {
      const auto water=depth(world,start+delta*(float(i)/float(directSegments)));
      noFloor+=!water.floor;
      wet+=water.water && water.centimeters>10.f;
      if(water.water && water.centimeters>=chest) {if(!deep)firstDeep=water.surface;++deep;}
    }
    float routeLength=0;
    for(size_t i=1;i<path.corners.size();++i)routeLength+=(centimeters(path.corners[i])-centimeters(path.corners[i-1])).length();
    const size_t routeSamples=path.corners.empty()?0u:std::min(size_t(32),std::max(size_t(2),size_t(std::ceil(routeLength/100.f))+1));
    size_t routeDeep=0,routeWet=0,routeNoFloor=0;
    float edgeClearance=200.f;
    for(size_t i=0;i<routeSamples;++i) {
      const auto at=along(path,routeLength*float(i)/float(routeSamples-1));
      const auto water=depth(world,at);
      routeNoFloor+=!water.floor;routeWet+=water.water && water.centimeters>10.f;
      routeDeep+=water.water && water.centimeters>=chest;
      for(unsigned direction=0;direction<8;++direction) {
        const float angle=float(direction)*Pi*.25f;
        edgeClearance=std::min(edgeClearance,edgeAhead(world,at,{std::cos(angle),0,std::sin(angle)},step));
      }
    }
    out<<"{\"schema\":1,\"label\":"<<std::quoted(label)<<",\"npc\":"<<std::quoted(args[2])
       <<",\"start\":"<<std::quoted(startText)<<",\"goal\":"<<std::quoted(goalText)<<",\"start_cm\":";vector(out,start);
    out<<",\"goal_cm\":";vector(out,goal);out<<",\"start_support\":";support(out,world,*npc,start,mesh);
    out<<",\"goal_support\":";support(out,world,*npc,goal,mesh);
    out<<",\"detour_complete\":"<<path.complete<<",\"detour_reason\":"<<std::quoted(path.reason)
       <<",\"detour_length_cm\":"<<path.length*100.f<<",\"direct_cm\":"<<direct<<",\"direct_horizontal_cm\":"<<horizontal
       <<",\"detour_ratio\":";if(direct>0)number(out,path.length*100.f/direct);else out<<"null";
    out<<",\"detour_corners_cm\":[";
    for(size_t i=0;i<path.corners.size();++i) {if(i)out<<',';vector(out,centimeters(path.corners[i]));}
    out<<"],\"first_detour_goal_dot\":";if(firstDirection)number(out,firstDot);else out<<"null";
    out<<",\"straight_walkable\":"<<mesh.straightWalkable(meters(start),meters(goal))
       <<",\"direct_obstruction\":{\"hit\":"<<obstruction.hasCol<<",\"label\":"<<std::quoted(obstructionLabel)
       <<",\"material\":"<<unsigned(obstruction.mat)<<",\"material_name\":"<<std::quoted(materialName(obstruction.mat))
       <<",\"world_hit\":"<<(obstruction.hasCol && obstruction.n.length()>.5f)<<",\"normal\":";
    if(obstruction.hasCol && normalLength>.5f)vector(out,obstructionNormal);else out<<"null";
    out<<",\"normal_y\":";if(obstruction.hasCol && normalLength>.5f)number(out,obstructionNormal.y);else out<<"null";
    out<<",\"point_cm\":";
    if(obstruction.hasCol)vector(out,obstruction.v);else out<<"null";
    out<<"},\"direct_sensor\":{\"hit\":"<<sensor.hasCol<<",\"kind\":"<<std::quoted(hitName(sensor.kind))<<",\"point_cm\":";
    if(sensor.hasCol)vector(out,sensor.point);else out<<"null";
    out<<"},\"goal_visible\":"<<!visible.hasCol<<",\"goal_visibility\":{\"height_cm\":160,\"from_cm\":";vector(out,visibleFrom);
    out<<",\"to_cm\":";vector(out,visibleTo);out<<",\"hit\":"<<visible.hasCol<<",\"label\":"<<std::quoted(visibleLabel)<<",\"point_cm\":";
    if(visible.hasCol)vector(out,visible.v);else out<<"null";
    out<<"},\"direct_sample_count\":"<<directSegments+1<<",\"direct_water_samples\":"<<wet
       <<",\"direct_deep_water_samples\":"<<deep<<",\"direct_no_floor_samples\":"<<noFloor<<",\"first_deep_water_cm\":";
    if(deep)vector(out,firstDeep);else out<<"null";
    out<<",\"route_sample_count\":"<<routeSamples<<",\"route_water_samples\":"<<routeWet
       <<",\"route_deep_water_samples\":"<<routeDeep<<",\"route_no_floor_samples\":"<<routeNoFloor
       <<",\"route_min_edge_clearance_cm\":";if(routeSamples)number(out,edgeClearance);else out<<"null";
    out<<",\"route_edge_clearance_censored\":"<<(edgeClearance>=200.f)
       <<",\"probe\":{\"direct_step_cm\":25,\"route_max_positions\":32,\"route_water\":\"point_depth_only\",\"edge_directions\":8,\"edge_radius_cm\":200,\"edge_step_cm\":10,\"edge_drop_cm\":100,\"ray_height_cm\":90,\"deep_water_threshold_cm\":"<<chest<<"}}\n";
  }
  if(!input.eof() || !count || !out)throw std::runtime_error("nav live probes: incomplete input/output");
}
}
