#include "navbench.h"
#include "game/navigationeyeworld.h"
#include "navtransitions.h"
#include "physics/navmesh.h"
#include "world/world.h"
#include "world/objects/npc.h"
#include "world/aiqueue.h"
#include "testbench.h"
#include "gothic.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <limits>
#include <random>
#include <set>
#include <stdexcept>

namespace {
using Tempest::Vec3;
std::unique_ptr<NavMesh> mesh;
std::string meshWorld;
float capsuleHeight=0, capsuleCenter=0;
// Bench teacher only. Distances are centimetres; clocks are simulation milliseconds.
constexpr float cornerRadius=20.f, cornerHeight=100.f, progressStep=20.f;
constexpr float linkTakeoffRadius=20.f, linkLandingRadius=60.f;
constexpr uint64_t progressTimeout=3000, fallbackTimeout=65000;
constexpr uint64_t linkLandingTime=300, linkTimeout=6000;
constexpr unsigned maxReplans=2, maxFallbacks=1;
constexpr float walkingSpeed=85.f, meshTimeBudget=.85f, nativeShortcutRatio=.8f;
struct Trial {
  Npc* npc=nullptr;
  Vec3 goal;
  std::string mode,start,target,output;
  NavMesh::Path path;
  uint64_t began=0,limit=0,elapsed=0;
  uint64_t frames=0,cvFrames=0,normalFrames=0;
  size_t corner=1;
  unsigned replans=0,fallbacks=0;
  uint64_t progressAt=0,fallbackAt=0,sampleAt=0,linkAt=0,landedAt=0;
  float bestRemaining=std::numeric_limits<float>::max();
  float initialLength=0,nativeEstimate=0,fallbackBest=0;
  Vec3 fallbackGoal;
  bool haveFallbackGoal=false;
  const WayPoint* fallback=nullptr;
  bool onFallback=false,linkStarted=false;
  bool sawAir=false,sawJump=false,sawClimb=false;
  int linkHp=0;
  Vec3 linkOrigin,linkEnd;
  std::string failure,fallbackReason;
  std::ofstream trace;
  bool active=false,success=false,finished=false;
} trial;

NavMesh::Point meters(const Vec3& p) { return {p.x*.01f,p.y*.01f,p.z*.01f}; }
Vec3 centimeters(const NavMesh::Point& p) { return {p[0]*100,p[1]*100,p[2]*100}; }
void vec(std::ostream& out,const Vec3& p) { out<<'['<<p.x<<','<<p.y<<','<<p.z<<']'; }
std::ofstream file(const std::string& output,const char* name) {
  std::ofstream out(std::filesystem::path(output)/name);
  if(!out) throw std::runtime_error(std::string("navmesh: cannot write ")+name);
  out<<std::setprecision(9)<<std::boolalpha;
  return out;
}
Npc& actor(World& w,std::string_view symbol) {
  auto* n=w.findNpcByInstance(w.script().findSymbolIndex(symbol));
  if(n==nullptr) throw std::runtime_error("navmesh: missing NPC "+std::string(symbol));
  return *n;
}
void requireMesh(World& w) {
  if(!mesh || meshWorld!=w.name()) throw std::runtime_error("navmesh build must precede queries");
}
void stats(const std::string& output,double extractionSeconds,double cacheLoadSeconds) {
  const auto& s=mesh->stats();const auto& p=mesh->params();
  auto out=file(output,"navmesh-stats.json");
  out<<"{\"key\":"<<std::quoted(s.key)<<",\"complete\":"<<s.complete<<",\"whole_world\":"<<p.regions.empty()
     <<",\"movable_vobs\":false"
     <<",\"from_cache\":"<<s.fromCache<<",\"build_seconds\":"<<s.buildSeconds
     <<",\"load_seconds\":"<<cacheLoadSeconds<<",\"extraction_seconds\":"<<extractionSeconds
     <<",\"cache_bytes\":"<<s.cacheBytes<<",\"triangles\":"<<s.triangles
     <<",\"water_triangles\":"<<s.waterTriangles<<",\"tiles\":"<<s.tiles<<",\"polygons\":"<<s.polygons
     <<",\"radius_m\":"<<p.radius<<",\"height_m\":"<<p.height<<",\"climb_m\":"<<p.climb
     <<",\"capsule_height_m\":"<<capsuleHeight<<",\"capsule_center_height_m\":"<<capsuleCenter
     <<",\"slope_deg\":"<<p.slope<<",\"cell_size_m\":"<<p.cellSize<<",\"cell_height_m\":"<<p.cellHeight
     <<",\"water_depth_m\":"<<p.waterDepth<<",\"tile_cells\":"<<p.tileSize<<",\"links\":"<<p.links.size()<<"}\n";
}
bool closeCorner(const Vec3& at,const Vec3& to) {
  const auto d=at-to;
  return d.x*d.x+d.z*d.z<=cornerRadius*cornerRadius && std::abs(d.y)<=cornerHeight;
}
Vec3 destination() {
  if(trial.linkStarted) return trial.linkEnd;
  if(trial.onFallback) {
    if(const auto* point=trial.npc->currentWayPoint()) return point->groundPos;
    if(trial.fallback) return trial.fallback->groundPos;
  }
  return trial.corner<trial.path.corners.size()?centimeters(trial.path.corners[trial.corner]):trial.goal;
}
float remaining(const Vec3& at) {
  float value=(at-destination()).length();
  for(size_t i=trial.corner+(trial.linkStarted?2:1);i<trial.path.corners.size();++i)
    value+=(centimeters(trial.path.corners[i])-centimeters(trial.path.corners[i-1])).length();
  return value;
}
void replan(World& w) {
  auto& n=*trial.npc;
  n.clearAiQueue();n.clearGoTo();n.setWalkMode(WalkBit::WM_Walk);
  trial.path=mesh->findPath(meters(n.position()),meters(trial.goal));
  trial.linkStarted=false;
  trial.corner=0;trial.progressAt=w.tickCount();trial.bestRemaining=remaining(n.position());
}
void fallback(World& w,const char* reason) {
  auto& n=*trial.npc;
  n.clearAiQueue();n.clearGoTo();n.setWalkMode(WalkBit::WM_Walk);
  trial.fallback=w.findWayPoint(trial.goal,[](const WayPoint& p){return p.isConnected();});
  ++trial.fallbacks;trial.fallbackAt=trial.progressAt=w.tickCount();trial.onFallback=true;
  trial.haveFallbackGoal=false;trial.fallbackReason=reason;
  if(trial.fallback) n.aiPush(AiQueue::aiGoToPoint(*trial.fallback));
  else {trial.onFallback=false;trial.failure="no_waypoint";}
}
void checkRouteBudget(World& w) {
  if(!trial.path.complete || trial.initialLength<=walkingSpeed*float(trial.limit)*.001f*meshTimeBudget) return;
  auto& n=*trial.npc;
  const auto* point=w.findWayPoint(trial.goal,[](const WayPoint& p){return p.isConnected();});
  if(!point) return;
  const auto tail=mesh->findPath(meters(point->groundPos),meters(trial.goal));
  if(!tail.complete) return;
  auto route=w.wayTo(n,*point);
  if(!route.first() && (n.position()-point->groundPos).length()>150.f) return;
  auto previous=n.position();
  float length=0;
  while(const auto* next=route.pop()) {length+=(next->groundPos-previous).length();previous=next->groundPos;}
  trial.nativeEstimate=length+tail.length*100;
  if(trial.nativeEstimate<trial.initialLength*nativeShortcutRatio) fallback(w,"mesh_time_budget");
}
void finish(World& w) {
  if(trial.npc==nullptr) throw std::runtime_error("navmesh finish without trial");
  if(trial.active) {trial.elapsed=w.tickCount()-trial.began;trial.active=false;}
  if(!trial.success && trial.failure.empty()) trial.failure="path_timeout";
  auto& n=*trial.npc;
  auto out=file(trial.output,"trial.json");
  out<<"{\"mode\":"<<std::quoted(trial.mode)<<",\"start\":"<<std::quoted(trial.start)
     <<",\"goal\":"<<std::quoted(trial.target)<<",\"success\":"<<trial.success
     <<",\"elapsed_ms\":"<<trial.elapsed<<",\"limit_ms\":"<<trial.limit
     <<",\"distance_cm\":"<<(n.position()-trial.goal).length()<<",\"path_complete\":"<<trial.path.complete
     <<",\"path_reason\":"<<std::quoted(trial.path.reason)<<",\"corners\":"<<trial.path.corners.size()
     <<",\"corners_visited\":"<<trial.corner<<",\"final_position\":";
  vec(out,n.position());out<<",\"target_position\":";vec(out,trial.goal);
  out<<",\"replans\":"<<trial.replans<<",\"fallbacks\":"<<trial.fallbacks
     <<",\"fallback_reason\":"<<std::quoted(trial.fallbackReason)
     <<",\"failure\":"<<std::quoted(trial.failure)<<",\"next_position\":";vec(out,destination());
  out<<",\"goal_direct_move_clear\":"<<n.testMove(trial.goal)
     <<",\"path_length_cm\":"<<trial.path.length*100
     <<",\"initial_path_length_cm\":"<<trial.initialLength<<",\"native_estimate_cm\":"<<trial.nativeEstimate
     <<",\"start_snap_m\":"<<trial.path.startSnapDistance<<",\"end_snap_m\":"<<trial.path.endSnapDistance
     <<",\"frames\":"<<trial.frames<<",\"cv_frames\":"<<trial.cvFrames<<",\"normal_ai_frames\":"<<trial.normalFrames;
  const auto hit=navigationRay(*w.physic(),n.centerPosition(),destination()+Vec3(0,100,0),&n);
  out<<",\"blocker_category\":"<<int(hit.kind)<<",\"blocker_position\":";vec(out,hit.point);
  // Sweep a sphere along the standing capsule's axis at the requested goal,
  // independently of the route from the actor. This is diagnostic only.
  const auto capsule=n.navigationCapsule();
  auto forward=destination()-n.position();forward.y=0;
  const float forwardLength=forward.length();
  if(forwardLength>0) forward*=std::min(75.f,forwardLength)/forwardLength;
  DynamicWorld::NavigationRayResult nearHit;
  float nearest=std::numeric_limits<float>::max();
  const float bodyCenter=n.navigationClearance()-capsule.y*.5f;
  const float span=std::max(0.f,capsule.y*.5f-capsule.x);
  for(float y:{bodyCenter-span,bodyCenter,bodyCenter+span}) {
    const auto from=n.position()+Vec3(0,y,0);
    const auto probe=navigationRay(*w.physic(),from,from+forward,&n);
    const float distance=(probe.point-from).length();
    if(probe.hasCol && distance<nearest) {nearHit=probe;nearest=distance;}
  }
  out<<",\"near_blocker_category\":"<<int(nearHit.kind)<<",\"near_blocker_position\":";
  if(nearHit.hasCol) vec(out,nearHit.point); else out<<"null";
  const auto center=trial.goal+Vec3(0,n.navigationClearance()-capsule.y*.5f,0);
  const auto half=Vec3(0,std::max(0.f,capsule.y*.5f-capsule.x),0);
  const auto volume=w.physic()->sweep(center+half,center-half,capsule.x);
  out<<",\"goal_capsule_hit\":"<<volume.hasCol<<",\"goal_capsule_hit_position\":";
  if(volume.hasCol) vec(out,volume.v); else out<<"null";
  out<<"}\n";
  trial.trace.close();
  n.clearAiQueue();n.clearGoTo();n.stopWalking();
  trial.finished=true;
}
}

void NavBench::reset() { trial=Trial{};NavTransitions::reset(); }
const NavMesh& NavBench::navigationMesh(World& world) { requireMesh(world);return *mesh; }
bool NavBench::running() { return trial.active || NavTransitions::running(); }

void NavBench::command(const std::vector<std::string>& a,World& w,Npc& pl,const std::string& output) {
  if(a.size()<2) throw std::runtime_error("navmesh: expected build|coverage|pairs|start|follow|finish");
  if(a[1]=="build" && (a.size()==3 || a.size()==4)) {
    if(Gothic::physicsFlags().movableVobs) throw std::runtime_error("navmesh measurement requires movable_vobs=0 before world load");
    if(!std::filesystem::path(a[2]).is_absolute()) throw std::runtime_error("navmesh cache must be absolute");
    const auto before=std::chrono::steady_clock::now();
    auto geometry=w.physic()->navigationGeometry();
    const double extraction=std::chrono::duration<double>(std::chrono::steady_clock::now()-before).count();
    auto& human=actor(w,"BAU_954_MALETH");
    const auto capsule=human.navigationCapsule();
    const auto& guild=w.script().guildVal();const auto g=human.guild();
    NavMesh::Params p;p.radius=capsule.x*.01f;p.height=human.navigationClearance()*.01f;
    capsuleHeight=capsule.y*.01f;capsuleCenter=p.height-capsuleHeight*.5f;
    p.climb=float(guild.step_height[g])*.01f;if(p.climb<=0) p.climb=.5f;
    p.slope=float(guild.slide_angle[g]);p.waterDepth=float(guild.water_depth_chest[g])*.01f;
    if(a.size()==4) {
      if(!std::filesystem::path(a[3]).is_absolute()) throw std::runtime_error("navmesh links must be absolute");
      std::ifstream input(a[3]);
      if(!input) throw std::runtime_error("navmesh cannot read links");
      NavMesh::OffMeshLink link;unsigned type=0;
      while(input>>link.start[0]>>link.start[1]>>link.start[2]>>link.end[0]>>link.end[1]>>link.end[2]>>type>>link.id) {
        if(type<1 || type>3) throw std::runtime_error("navmesh invalid link type");
        link.type=NavMesh::LinkType(type);p.links.push_back(link);
      }
      if(!input.eof()) throw std::runtime_error("navmesh invalid links file");
    }
    mesh=std::make_unique<NavMesh>();meshWorld=w.name();
    mesh->buildOrLoad(geometry,p,a[2],meshWorld);
    if(!mesh->stats().complete) {
      // Bound the fallback to all measurement areas, leaving whole-world coverage explicit.
      for(const char* name:{"NW_CITY_MERCHANT_PATH_20","NW_CITY_HABOUR_06_B","NW_BIGFARM_HOUSE_OUT_04","NW_FARM1_ENTRANCE_01","NW_CITY_HABOUR_POOR_AREA_HUT_03","NW_CITY_UPTOWN_PATH_13"}) {
        const auto* wp=w.findPoint(name,false);if(!wp) continue;
        auto center=meters(wp->position());
        p.regions.push_back({{center[0]-150,-200,center[2]-150},{center[0]+150,300,center[2]+150}});
      }
      mesh->buildOrLoad(geometry,p,a[2],meshWorld);
    }
    NavMesh cached;cached.buildOrLoad(geometry,p,a[2],meshWorld);
    stats(output,extraction,cached.stats().loadSeconds);
    mesh->writePolygons(std::filesystem::path(output)/"navmesh-polygons.jsonl");
    return;
  }
  requireMesh(w);
  if(NavTransitions::command(a,w,pl,*mesh,output)) return;
  if(a[1]=="coverage" && a.size()==2) {
    auto edges=file(output,"coverage.jsonl"),points=file(output,"points.jsonl");
    w.forEachWayPoint([&](const WayPoint& wp) {
      points<<"{\"name\":"<<std::quoted(wp.name)<<",\"freepoint\":"<<wp.isFreePoint()<<",\"position\":";
      vec(points,wp.groundPos);points<<"}\n";
      for(const auto& c:wp.connections()) {
        if(wp.name>=c.point->name) continue;
        auto path=mesh->findPath(meters(wp.groundPos),meters(c.point->groundPos));
        const auto reverse=mesh->findPath(meters(c.point->groundPos),meters(wp.groundPos));
        edges<<"{\"from\":"<<std::quoted(wp.name)<<",\"to\":"<<std::quoted(c.point->name)
             <<",\"complete\":"<<path.complete<<",\"reason\":"<<std::quoted(path.reason)
             <<",\"reverse_complete\":"<<reverse.complete
             <<",\"edge_length_cm\":"<<c.len
             <<",\"path_length_cm\":"<<path.length*100<<",\"start_snap_m\":"<<path.startSnapDistance
             <<",\"end_snap_m\":"<<path.endSnapDistance<<"}\n";
      }
    });
    return;
  }
  if(a[1]=="pairs" && a.size()==4) {
    std::vector<const WayPoint*> starts,goals;
    w.forEachWayPoint([&](const WayPoint& wp) {
      if(wp.isConnected()) starts.push_back(&wp);
      if(wp.isFreePoint() && wp.name.rfind("FP_",0)==0) goals.push_back(&wp);
    });
    const auto sort=[](auto* x,auto* y){return x->name<y->name;};
    std::sort(starts.begin(),starts.end(),sort);std::sort(goals.begin(),goals.end(),sort);
    struct Region {const char* name;const char* anchor;float radius;const char* prefix;};
    const Region regions[]={
      {"market","NW_CITY_MERCHANT_PATH_20",5000,"NW_CITY_MERCHANT"},
      {"harbour","NW_CITY_HABOUR_06_B",6000,"NW_CITY_HABOUR"},
      {"onar","NW_BIGFARM_HOUSE_OUT_04",12000,"NW_BIGFARM"},
      {"lobart","NW_FARM1_ENTRANCE_01",10000,"NW_FARM1"},
      {"port_streets","NW_CITY_HABOUR_POOR_AREA_HUT_03",3500,"NW_CITY_HABOUR_POOR"},
      {"upper_streets","NW_CITY_UPTOWN_PATH_13",5000,"NW_CITY_UPTOWN"}};
    std::mt19937 rng(uint32_t(std::stoul(a[2])));
    std::set<std::pair<std::string,std::string>> used;
    auto out=file(output,"pairs.jsonl");
    for(const auto& r:regions) {
      const auto* anchor=w.findPoint(r.anchor,false);if(!anchor) throw std::runtime_error("missing region anchor "+std::string(r.anchor));
      std::vector<std::pair<const WayPoint*,const WayPoint*>> candidates;
      for(auto* from:starts) {
        if(from->name.rfind(r.prefix,0)!=0 || (from->groundPos-anchor->groundPos).quadLength()>r.radius*r.radius) continue;
        for(auto* to:goals) {
          const auto d=(to->groundPos-from->groundPos).length();
          if(d>=1000 && d<=4000 && !used.count({from->name,to->name})) candidates.emplace_back(from,to);
        }
      }
      std::shuffle(candidates.begin(),candidates.end(),rng);
      const size_t count=std::stoul(a[3]);
      if(candidates.size()<count) throw std::runtime_error("too few pairs for "+std::string(r.name));
      for(size_t i=0;i<count;++i) {
        const auto [from,to]=candidates[i];used.emplace(from->name,to->name);
        out<<"{\"region\":"<<std::quoted(r.name)<<",\"start\":"<<std::quoted(from->name)<<",\"goal\":"<<std::quoted(to->name)
           <<",\"distance_cm\":"<<(from->groundPos-to->groundPos).length()<<"}\n";
      }
    }
    return;
  }
  if((a[1]=="start" && a.size()==7) || (a[1]=="follow" && a.size()==6)) {
    const bool place=a[1]=="start";
    if(a[2]!="direct" && a[2]!="mesh" && a[2]!="teacher") throw std::runtime_error("navmesh: mode direct|mesh|teacher");
    auto& n=actor(w,a[3]);
    const auto* goal=w.findPoint(a[place?5:4],false);
    const auto* start=place?w.findPoint(a[4],false):nullptr;
    if(!goal || (place && !start)) throw std::runtime_error("navmesh: missing trial point");
    n.setInteraction(nullptr,true);n.clearAiQueue();n.clearState(false);n.setExternalControl(true);n.clearGoTo();
    if(place) {
      n.setPosition(start->groundPos);n.updateTransform();
      // Keep the hero close enough for native full AI, outside the route's first capsule.
      pl.setPosition(start->groundPos+Vec3(0,0,500));pl.updateTransform();
    }
    n.setDirection(goal->groundPos-n.position());n.setWalkMode(WalkBit::WM_Walk);
    trial=Trial{};trial.npc=&n;trial.goal=goal->groundPos;trial.mode=a[2];
    trial.start=place?a[4]:"intentional";trial.target=goal->name;trial.output=output;
    trial.limit=std::stoull(a[place?6:5]);trial.began=w.tickCount();trial.active=true;
    trial.path=mesh->findPath(meters(n.position()),meters(trial.goal));
    trial.initialLength=trial.path.length*100;
    if(trial.mode=="teacher") trial.corner=0;
    trial.progressAt=w.tickCount();trial.bestRemaining=remaining(n.position());
    if(trial.mode=="teacher") trial.trace=file(output,"teacher-trace.jsonl");
    if(trial.mode=="teacher") checkRouteBudget(w);
    if(trial.mode=="direct") n.aiPush(AiQueue::aiGoToPoint(*goal));
    return;
  }
  if(a[1]=="finish" && a.size()==2) {finish(w);return;}
  throw std::runtime_error("invalid navmesh command");
}

void NavBench::tick(World& w) {
  NavTransitions::tick(w);
  if(!trial.active || !trial.npc) return;
  auto& n=*trial.npc;trial.elapsed=w.tickCount()-trial.began;
  ++trial.frames;if(n.characterVirtualActive()) ++trial.cvFrames;
  if(n.processPolicy()==NpcProcessPolicy::AiNormal) ++trial.normalFrames;
  if((n.position()-trial.goal).length()<100 && !trial.linkStarted) {trial.success=true;trial.failure.clear();finish(w);return;}
  if(trial.elapsed>=trial.limit) {finish(w);return;}
  if(trial.mode=="teacher") {
    if(w.tickCount()>=trial.sampleAt) {
      trial.sampleAt=w.tickCount()+1000;
      trial.trace<<"{\"elapsed_ms\":"<<trial.elapsed<<",\"corner\":"<<trial.corner<<",\"fallback\":"<<trial.onFallback<<",\"position\":";
      vec(trial.trace,n.position());trial.trace<<",\"next\":";vec(trial.trace,destination());trial.trace<<"}\n";
    }
    if(trial.onFallback) {
      const auto* point=n.currentWayPoint();
      if(point && n.moveHint()==Npc::GT_Way) {
        const float left=(n.position()-point->groundPos).length();
        if(!trial.haveFallbackGoal || (point->groundPos-trial.fallbackGoal).length()>1.f) {
          trial.fallbackGoal=point->groundPos;trial.haveFallbackGoal=true;
          trial.fallbackBest=left;trial.progressAt=w.tickCount();
        } else if(left+progressStep<trial.fallbackBest) {
          trial.fallbackBest=left;trial.progressAt=w.tickCount();
        }
      }
      const bool reached=trial.fallback && (n.position()-trial.fallback->groundPos).length()<150.f;
      if(reached || (n.isAiQueueEmpty() && n.moveHint()==Npc::GT_No) ||
         w.tickCount()-trial.fallbackAt>=fallbackTimeout || w.tickCount()-trial.progressAt>=progressTimeout) {
        trial.onFallback=false;replan(w);
      }
      return;
    }
    if(!trial.path.complete) {
      trial.failure="no_path:"+trial.path.reason;
      if(trial.fallbacks<maxFallbacks) fallback(w,"no_mesh_path");
      return;
    }
    // Advance only after the body actually reaches the corner. Preserve link takeoffs.
    while(!trial.linkStarted && trial.corner<trial.path.corners.size() && closeCorner(n.position(),destination())) {
      bool takeoff=false;
      for(const auto& link:trial.path.transitions) if(link.cornerIndex==trial.corner) takeoff=true;
      if(takeoff) break;
      ++trial.corner;trial.linkStarted=false;n.clearGoTo();
    }
    for(const auto& link:trial.path.transitions) {
      if(link.cornerIndex!=trial.corner) continue;
      if(!trial.linkStarted && (n.position()-centimeters(link.start)).length()<=linkTakeoffRadius && NavTransitions::supported(w,n)) {
        n.clearGoTo();n.setDirection(centimeters(link.end)-n.position());
        n.setWalkMode(WalkBit::WM_Run);
        bool started=false;
        if(link.type==NavMesh::LinkType::Drop) started=n.goToPosition(centimeters(link.end));
        else {
          const auto jump=n.tryJump();
          if(link.type==NavMesh::LinkType::Climb && jump.anim!=Npc::Anim::Jump && jump.anim!=Npc::Anim::Idle) started=n.startClimb(jump);
          else if(link.type==NavMesh::LinkType::Jump && jump.anim==Npc::Anim::Jump) started=n.setAnim(jump.anim);
        }
        if(started) {
          trial.linkStarted=true;trial.linkAt=trial.progressAt=w.tickCount();trial.landedAt=0;
          trial.sawAir=trial.sawJump=trial.sawClimb=false;
          trial.linkOrigin=n.position();trial.linkEnd=centimeters(link.end);trial.linkHp=n.attribute(ATR_HITPOINTS);
          trial.bestRemaining=remaining(n.position());
        } else n.setWalkMode(WalkBit::WM_Walk);
      }
      if(trial.linkStarted) {
        trial.sawAir|=n.isInAir() || n.isFalling();
        trial.sawJump|=n.isJump() || n.isJumpAnim();
        trial.sawClimb|=n.bodyStateMasked()==BS_CLIMB;
        if(n.attribute(ATR_HITPOINTS)<trial.linkHp) {trial.failure="link_damage";finish(w);return;}
        const bool witnessed=link.type==NavMesh::LinkType::Drop?trial.sawAir && trial.linkOrigin.y-n.position().y>20.f:
                             link.type==NavMesh::LinkType::Climb?trial.sawClimb:trial.sawJump && trial.sawAir;
        bool landing=false;
        if(witnessed && NavTransitions::supported(w,n) && (n.position()-trial.linkEnd).length()<=linkLandingRadius) {
          const auto local=mesh->findPath(meters(n.position()),link.end);
          landing=local.complete && local.transitions.empty() && local.length<=1.5f && local.startSnapDistance<=.25f;
        }
        if(landing) {
          n.clearGoTo();n.stopWalking();
          if(trial.landedAt==0) trial.landedAt=w.tickCount();
          if(w.tickCount()-trial.landedAt>=linkLandingTime) {
            trial.corner+=2;trial.linkStarted=false;n.setWalkMode(WalkBit::WM_Walk);
          }
        } else {
          trial.landedAt=0;
          if(NavTransitions::supported(w,n) && n.moveHint()==Npc::GT_No) n.goToPosition(trial.linkEnd);
        }
        if(trial.linkStarted && w.tickCount()-trial.linkAt<linkTimeout) return;
      }
    }
    const float left=remaining(n.position());
    if(left+progressStep<trial.bestRemaining) {trial.bestRemaining=left;trial.progressAt=w.tickCount();}
    if(w.tickCount()-trial.progressAt>=progressTimeout) {
      trial.failure="body_blocked";
      if(trial.replans<maxReplans) {++trial.replans;replan(w);}
      else if(trial.fallbacks<maxFallbacks) fallback(w,"no_progress");
      else trial.progressAt=w.tickCount();
      return;
    }
    if(n.moveHint()==Npc::GT_No && !trial.linkStarted) n.goToPosition(destination());
    return;
  }
  if(trial.mode!="mesh" || !trial.path.complete) return;
  if(n.moveHint()!=Npc::GT_No) return;
  if(trial.corner<trial.path.corners.size()) {
    if(n.goToPosition(centimeters(trial.path.corners[trial.corner]))) ++trial.corner;
  } else {
    // The original target may be slightly outside the nearest polygon's erosion margin.
    n.goToPosition(trial.goal);
  }
}
