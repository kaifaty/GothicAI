#include "navdemos_internal.h"
#include "game/navigationeyeworld.h"
#include "navdemos.h"
#include "navbench.h"
#include "game/knownroadsbench.h"
#include "world/world.h"
#include "world/objects/npc.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace NavDemosInternal {
NavMesh::Point meters(const Vec3& p) {return {p.x*.01f,p.y*.01f,p.z*.01f};}
Vec3 cm(const NavMesh::Point& p) {return {p[0]*100,p[1]*100,p[2]*100};}
void jsonNumber(std::ostream& out,double value) {if(std::isfinite(value))out<<value;else out<<"null";}
void vector(std::ostream& out,const Vec3& p) {
  out<<'[';jsonNumber(out,p.x);out<<',';jsonNumber(out,p.y);out<<',';jsonNumber(out,p.z);out<<']';
}
std::array<float,3> local(const Vec3& p,float yaw) {
  const float c=std::cos(yaw),s=std::sin(yaw);
  return {-p.x*s+p.z*c,p.y,p.x*c+p.z*s};
}
// Stand-only point syntax shared by route qualification and v2 recording. Coordinates
// are centimetres, like the saved world and NAVDEMO poses; mesh queries use metres.
Vec3 demoPoint(World& world,const std::string& name) {
  if(name.rfind("cm:",0)!=0) {
    const auto* point=world.findPoint(name,false);
    if(!point)throw std::runtime_error("navdemos missing waypoint "+name);
    return point->groundPos;
  }
  std::string values=name.substr(3);std::replace(values.begin(),values.end(),',',' ');
  std::istringstream in(values);Vec3 point;std::string extra;
  if(!(in>>point.x>>point.y>>point.z) || (in>>extra) ||
     !std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
    throw std::runtime_error("navdemos invalid cm:x,y,z point "+name);
  return point;
}
const char* transitionName(NavMesh::LinkType type) {
  switch(type) {
    case NavMesh::LinkType::Jump:return "jump";
    case NavMesh::LinkType::Climb:return "climb";
    case NavMesh::LinkType::Drop:return "drop";
  }
  return "unknown";
}
void writeV2Plan(std::ostream& out,const Vec3& position,const Vec3& goal,
                 const KnownRoads::Guidance& guidance,const NavMesh::Path& path,bool direct) {
  out<<",\"position_cm\":";vector(out,position);out<<",\"goal_cm\":";vector(out,goal);
  out<<",\"carrot_cm\":";vector(out,guidance.carrot);
  out<<",\"beyond_known\":"<<guidance.beyondKnown<<",\"remaining_known_cm\":"<<guidance.remainingKnownCm
     <<",\"direct_walkable\":"<<direct<<",\"complete\":"<<(direct || path.complete)
     <<",\"reason\":"<<std::quoted(direct?"direct_walkable":path.reason)
     <<",\"length_m\":"<<(direct?(guidance.carrot-position).length()*.01f:path.length)
     <<",\"start_snap_m\":"<<path.startSnapDistance<<",\"end_snap_m\":"<<path.endSnapDistance
     <<",\"corners_cm\":[";
  if(direct) {vector(out,position);out<<',';vector(out,guidance.carrot);}
  else for(size_t i=0;i<path.corners.size();++i) {if(i)out<<',';vector(out,cm(path.corners[i]));}
  out<<"],\"transitions\":[";
  if(!direct)for(size_t i=0;i<path.transitions.size();++i) {
    if(i)out<<',';
    const auto& link=path.transitions[i];
    out<<"{\"id\":"<<link.id<<",\"type\":"<<std::quoted(transitionName(link.type))
       <<",\"corner_index\":"<<link.cornerIndex<<",\"start_cm\":";
    vector(out,cm(link.start));out<<",\"end_cm\":";vector(out,cm(link.end));out<<'}';
  }
  out<<']';
}
void planV2(World& world,Npc& npc,std::ostream& out,const std::string& label,
            const std::string& from,const std::string& to) {
  const auto position=demoPoint(world,from),goal=demoPoint(world,to);
  KnownRoads::Guidance guidance;
  KnownRoadsBench::readGuidance(world,npc,position,goal,guidance);
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(position),meters(guidance.carrot));
  const bool direct=!guidance.beyondKnown && mesh.straightWalkable(meters(position),meters(guidance.carrot));
  out<<"{\"label\":"<<std::quoted(label)<<",\"start\":"<<std::quoted(from)<<",\"goal\":"<<std::quoted(to);
  writeV2Plan(out,position,goal,guidance,path,direct);
  // A generated coordinate start must have actual support at its own elevation;
  // Detour snapping to a nearby rooftop or a floor beneath it is insufficient.
  const auto floor=world.physic()->landRay(position+Vec3(0,20,0),40.f);
  out<<",\"start_supported\":"<<(floor.hasCol && floor.n.y>=.65f && std::abs(floor.v.y-position.y)<=20.f)<<"}\n";
  if(!out)throw std::runtime_error("navdemos plan-v2 write failed");
}
void detourPlanV3(World& world,std::ostream& out,const std::string& label,
                  const std::string& from,const std::string& to) {
  // Evaluation only: this path goes to the actual final goal and never enters
  // NavMemoryTeacher or its learner-input boundary.
  const auto start=demoPoint(world,from),goal=demoPoint(world,to);
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(start),meters(goal));
  out<<"{\"label\":"<<std::quoted(label)<<",\"start\":"<<std::quoted(from)<<",\"goal\":"<<std::quoted(to)
     <<",\"start_cm\":";vector(out,start);out<<",\"goal_cm\":";vector(out,goal);
  out<<",\"complete\":"<<path.complete<<",\"reason\":"<<std::quoted(path.reason)
     <<",\"length_cm\":";jsonNumber(out,path.length*100.f);
  out<<",\"start_snap_cm\":";jsonNumber(out,path.startSnapDistance*100.f);
  out<<",\"end_snap_cm\":";jsonNumber(out,path.endSnapDistance*100.f);
  out<<",\"straight_walkable\":"<<mesh.straightWalkable(meters(start),meters(goal))
     <<",\"corners_cm\":[";
  for(size_t i=0;i<path.corners.size();++i) {
    if(i!=0) {
      out<<',';
    }
    vector(out,cm(path.corners[i]));
  }
  out<<"],\"transitions\":[";
  for(size_t i=0;i<path.transitions.size();++i) {
    if(i!=0) {
      out<<',';
    }
    const auto& link=path.transitions[i];
    out<<"{\"id\":"<<link.id<<",\"type\":"<<std::quoted(transitionName(link.type))<<",\"start_cm\":";
    vector(out,cm(link.start));out<<",\"end_cm\":";vector(out,cm(link.end));out<<'}';
  }
  out<<"]}\n";
  if(!out) {
    throw std::runtime_error("navdemos detour-plans-v3 write failed");
  }
}
float pointSegmentXZSquared(const Vec3& point,const Vec3& start,const Vec3& end) {
  const float dx=end.x-start.x,dz=end.z-start.z;
  const float square=dx*dx+dz*dz;
  const float t=square>0?std::clamp(((point.x-start.x)*dx+(point.z-start.z)*dz)/square,0.f,1.f):0.f;
  const float x=point.x-start.x-t*dx,z=point.z-start.z-t*dz;
  return x*x+z*z;
}
float segmentSquareXZSquared(const Vec3& start,const Vec3& end,float x0,float z0,float x1,float z1) {
  const std::array<float,2> a={start.x,start.z},delta={end.x-start.x,end.z-start.z};
  const std::array<float,2> low={x0,z0},high={x1,z1};
  float enter=0,leave=1;
  bool intersects=true;
  for(size_t axis=0;axis<2;++axis) {
    if(std::abs(delta[axis])<.000001f) {
      if(a[axis]<low[axis] || a[axis]>high[axis]) {
        intersects=false;break;
      }
      continue;
    }
    float first=(low[axis]-a[axis])/delta[axis],last=(high[axis]-a[axis])/delta[axis];
    if(first>last) {
      std::swap(first,last);
    }
    enter=std::max(enter,first);leave=std::min(leave,last);
    if(enter>leave) {
      intersects=false;break;
    }
  }
  if(intersects) {
    return 0;
  }
  const auto pointSquare=[&](const Vec3& point) {
    const float dx=std::max({0.f,x0-point.x,point.x-x1});
    const float dz=std::max({0.f,z0-point.z,point.z-z1});
    return dx*dx+dz*dz;
  };
  float square=std::min(pointSquare(start),pointSquare(end));
  for(float x:{x0,x1}) {
    for(float z:{z0,z1}) {
      square=std::min(square,pointSegmentXZSquared(Vec3(x,0,z),start,end));
    }
  }
  return square;
}
void oracleCorridor(World& world,std::ostream& out,const std::string& label,
                    const std::string& from,float yaw,const std::string& to) {
  constexpr float Begin=500,End=1500,Radius=15;
  const auto start=demoPoint(world,from),goal=demoPoint(world,to);
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(start),meters(goal));
  const float pathCm=path.length*100.f;
  const char* status=!path.complete?"incomplete_path":!path.transitions.empty()?"offmesh_transition":
                     pathCm<=Begin?"short_path":"ok";
  NavMesh::LocalRaster raster;
  const bool evaluated=std::string_view(status)=="ok";
  if(evaluated) {
    raster=mesh.rasterLocal(meters(start),yaw);
    if(!raster.supportValid) {
      status="unsupported_pose";
    }
  }
  const bool available=std::string_view(status)=="ok";
  std::vector<std::pair<Vec3,Vec3>> corridor;
  if(available) {
    float offset=0;
    const float radians=yaw*Pi/180.f;
    for(size_t i=1;i<path.corners.size();++i) {
      const auto a=cm(path.corners[i-1]),b=cm(path.corners[i]);
      const float extent=(b-a).length();
      if(extent>0 && offset+extent>Begin && offset<End) {
        const float first=std::clamp((Begin-offset)/extent,0.f,1.f);
        const float last=std::clamp((End-offset)/extent,0.f,1.f);
        const auto near=local(a+(b-a)*first-start,radians),far=local(a+(b-a)*last-start,radians);
        corridor.emplace_back(Vec3(near[0],0,near[2]),Vec3(far[0],0,far[2]));
      }
      offset+=extent;
      if(offset>=End) {
        break;
      }
    }
  }
  out<<"{\"label\":"<<std::quoted(label)<<",\"start\":"<<std::quoted(from)<<",\"goal\":"<<std::quoted(to)
     <<",\"start_cm\":";vector(out,start);out<<",\"goal_cm\":";vector(out,goal);
  out<<",\"yaw_deg\":"<<yaw<<",\"complete\":"<<path.complete<<",\"reason\":"<<std::quoted(path.reason)
     <<",\"length_cm\":";jsonNumber(out,pathCm);
  out<<",\"transition_count\":"<<path.transitions.size()<<",\"corridor_begin_cm\":"<<Begin
     <<",\"corridor_end_cm\":"<<End<<",\"radius_cm\":"<<Radius
     <<",\"corridor_arclength\":\"detour_3d\",\"corridor_available\":"<<available
     <<",\"corridor_status\":"<<std::quoted(status)<<",\"raster_evaluated\":"<<evaluated<<",\"raster_support_valid\":";
  if(evaluated) {
    out<<raster.supportValid;
  }
  else {
    out<<"null";
  }
  out<<",\"raster_support_exact\":";
  if(evaluated) {
    out<<raster.supportExact;
  }
  else {
    out<<"null";
  }
  out<<",\"raster_support_snap_cm\":";
  if(evaluated && raster.supportValid) {
    jsonNumber(out,raster.supportSnapDistance*100.f);
  }
  else {
    out<<"null";
  }
  out<<",\"raster_support_horizontal_cm\":";
  if(evaluated && raster.supportValid) {
    jsonNumber(out,raster.supportHorizontalDistance*100.f);
  }
  else {
    out<<"null";
  }
  out<<",\"raster_support_vertical_offset_cm\":";
  if(evaluated && raster.supportValid) {
    jsonNumber(out,raster.supportVerticalOffset*100.f);
  }
  else {
    out<<"null";
  }
  out<<",\"corners_cm\":[";
  for(size_t i=0;i<path.corners.size();++i) {
    if(i!=0) {
      out<<',';
    }
    vector(out,cm(path.corners[i]));
  }
  out<<"],\"native_cells\":[";
  size_t count=0;
  if(available) {
    const auto layer=[&](const NavMesh::RasterLayer& value,bool near) {
      if(value.size==0 || value.cells.size()!=value.size*value.size) {
        throw std::runtime_error("navdemos oracle corridor raster shape mismatch");
      }
      const float step=value.cellSize*100.f,half=float(value.size)*.5f;
      for(size_t row=0;row<value.size;++row) {
        for(size_t column=0;column<value.size;++column) {
          const float x0=(float(column)-half)*step,z0=(float(row)-half)*step;
          if(!near && std::abs(x0+step*.5f)<800 && std::abs(z0+step*.5f)<800) {
            continue;
          }
          const bool touched=std::any_of(corridor.begin(),corridor.end(),[&](const auto& segment) {
            return segmentSquareXZSquared(segment.first,segment.second,x0,z0,x0+step,z0+step)<=Radius*Radius;
          });
          if(!touched) {
            continue;
          }
          if(count!=0) {
            out<<',';
          }
          const auto& cell=value.cells[row*value.size+column];
          if(cell.heightBoundary && cell.traversable) {
            throw std::runtime_error("navdemos oracle corridor has a traversable height boundary");
          }
          if(cell.traversable && !cell.heightValid) {
            throw std::runtime_error("navdemos oracle corridor has traversable ground without height");
          }
          out<<"{\"layer\":"<<std::quoted(near?"near":"far")<<",\"row\":"<<row<<",\"column\":"<<column
             <<",\"truth_passable\":"<<cell.traversable<<",\"height_valid\":"<<cell.heightValid
             <<",\"ground_height_cm\":";jsonNumber(out,cell.heightValid?cell.groundHeight*100.f:0.f);
          out<<",\"height_boundary\":"<<cell.heightBoundary<<'}';
          ++count;
        }
      }
    };
    layer(raster.near,true);layer(raster.far,false);
  }
  out<<"],\"native_cell_count\":"<<count<<"}\n";
  if(!out) {
    throw std::runtime_error("navdemos oracle-corridors write failed");
  }
}
void probeObject(World& world,Npc& npc,std::ostream& out,const std::string& label,
                 const std::string& from,const std::string& to,
                 const std::string& openingIn={},const std::string& openingOut={}) {
  const auto start=demoPoint(world,from),goal=demoPoint(world,to);
  Vec3 openingNormal=start-goal;
  if(!openingIn.empty() || !openingOut.empty()) {
    const auto* inside=world.findPoint(openingIn,false);
    const auto* outside=world.findPoint(openingOut,false);
    if(!inside || !outside) {
      throw std::runtime_error("navdemos object probe missing exact opening IN/OUT alias");
    }
    openingNormal=outside->groundPos-inside->groundPos;
  }
  openingNormal.y=0;
  const float normalLength=openingNormal.length();
  if(normalLength<.001f) {
    throw std::runtime_error("navdemos object probe opening normal has no horizontal direction");
  }
  openingNormal*=1.f/normalLength;
  const Vec3 tangent(-openingNormal.z,0,openingNormal.x);
  const auto old=npc.position();const float yaw=npc.rotation();
  npc.setPosition(start);npc.updateTransform();npc.setDirection(goal-start);
  const auto eyes=npc.navigationEyePose().origin;
  // Keep the original centre ray exactly. Extra samples stay within a
  // 130-cm-wide vertical opening at the unchanged authored floor goal.
  const auto look=goal+Vec3(0,100,0);
  const auto hit=navigationRay(*world.physic(),eyes,look,&npc);
  const bool visibleCentre=!hit.hasCol || (hit.point-look).length()<5.f;
  bool visibleEdge=false;
  size_t visibleSamples=visibleCentre?1u:0u;
  std::ostringstream samples;
  samples<<std::setprecision(9)<<std::boolalpha;
  const auto sample=[&](const Vec3& point,bool hasCollision,const Vec3& hitPoint,bool visible) {
    samples<<"{\"look_cm\":";vector(samples,point);
    samples<<",\"visible\":"<<visible<<",\"has_collision\":"<<hasCollision;
    if(hasCollision) {
      samples<<",\"hit_cm\":";vector(samples,hitPoint);
    }
    samples<<'}';
  };
  sample(look,hit.hasCol,hit.point,visibleCentre);
  for(float height:{40.f,100.f,180.f}) {
    for(float width:{-65.f,0.f,65.f}) {
      if(width==0 && height==100.f) {
        continue;
      }
      const auto edgeLook=goal+tangent*width+Vec3(0,height,0);
      const auto edgeHit=navigationRay(*world.physic(),eyes,edgeLook,&npc);
      const bool visible=!edgeHit.hasCol || (edgeHit.point-edgeLook).length()<5.f;
      visibleEdge=visibleEdge || visible;
      visibleSamples+=visible?1u:0u;
      samples<<',';
      sample(edgeLook,edgeHit.hasCol,edgeHit.point,visible);
    }
  }
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(start),meters(goal));
  const bool blocked=!mesh.straightWalkable(meters(start),meters(goal));
  const auto floor=world.physic()->landRay(start+Vec3(0,20,0),40.f);
  out<<"{\"label\":"<<std::quoted(label)<<",\"start\":"<<std::quoted(from)<<",\"goal\":"<<std::quoted(to)
     <<",\"visible_centre\":"<<visibleCentre<<",\"visible_edge\":"<<visibleEdge
     <<",\"visible\":"<<(visibleCentre || visibleEdge)
     <<",\"visibility_criterion\":\"centre_or_opening_edge_v1\",\"visible_samples\":"<<visibleSamples
     <<",\"opening_in\":"<<std::quoted(openingIn)<<",\"opening_out\":"<<std::quoted(openingOut)
     <<",\"opening_normal\":";vector(out,openingNormal);
  out<<",\"look_samples\":["<<samples.str()<<"],\"detour_complete\":"<<path.complete
     <<",\"detour_length_cm\":"<<path.length*100.f<<",\"direct_cm\":"<<(goal-start).length()
     <<",\"straight_blocked\":"<<blocked<<",\"start_supported\":"
     <<(floor.hasCol && floor.n.y>=.65f && std::abs(floor.v.y-start.y)<=20.f)
     <<",\"eyes_cm\":";vector(out,eyes);out<<",\"look_cm\":";vector(out,look);
  out<<",\"detour_cm\":[";
  for(size_t i=0;i<path.corners.size();++i) {if(i)out<<',';vector(out,cm(path.corners[i]));}
  out<<"]}\n";
  npc.setPosition(old);npc.updateTransform();npc.setDirection(yaw);
  if(!out)throw std::runtime_error("navdemos object probe write failed");
}

void commandProbes(const std::vector<std::string>& a,World& w,const std::string& output,bool& handled) {
  using NavDemos::running;
  handled=true;
  if(a.size()>=2 && a[1]=="oracle-corridors") {
    if(running() || NavBench::running()) {
      throw std::runtime_error("navdemos cannot query oracle corridors during episode");
    }
    if(a.size()!=3) {
      throw std::runtime_error("navdemos oracle-corridors FILE (rows LABEL START YAW_DEG GOAL)");
    }
    std::ifstream input(a[2]);
    if(!input) {
      throw std::runtime_error("navdemos cannot read oracle-corridors file");
    }
    std::ofstream out(std::filesystem::path(output)/"oracle-corridors.jsonl");
    if(!out) {
      throw std::runtime_error("navdemos cannot write oracle-corridors.jsonl");
    }
    out<<std::setprecision(9)<<std::boolalpha;
    std::string line;
    while(std::getline(input,line)) {
      std::istringstream row(line);
      std::string label,from,to,extra;
      float yaw=0;
      if(!(row>>label) || label[0]=='#') {
        continue;
      }
      if(!(row>>from>>yaw>>to) || (row>>extra) || !std::isfinite(yaw)) {
        throw std::runtime_error("navdemos oracle-corridors rows LABEL START YAW_DEG GOAL");
      }
      oracleCorridor(w,out,label,from,yaw,to);
    }
    if(!input.eof()) {
      throw std::runtime_error("navdemos oracle-corridors read failed");
    }
    out.close();
    if(!out) {
      throw std::runtime_error("navdemos oracle-corridors write failed");
    }
    return;
  }
  if(a.size()>=2 && a[1]=="detour-plans-v3") {
    if(running() || NavBench::running()) {
      throw std::runtime_error("navdemos cannot query Detour plans during episode");
    }
    if(a.size()!=3) {
      throw std::runtime_error("navdemos detour-plans-v3 FILE (rows LABEL START GOAL)");
    }
    std::ifstream input(a[2]);
    if(!input) {
      throw std::runtime_error("navdemos cannot read detour-plans-v3 file");
    }
    std::ofstream out(std::filesystem::path(output)/"detour-plans-v3.jsonl");
    if(!out) {
      throw std::runtime_error("navdemos cannot write detour-plans-v3.jsonl");
    }
    out<<std::setprecision(9)<<std::boolalpha;
    std::string line;
    while(std::getline(input,line)) {
      std::istringstream row(line);
      std::string label,from,to,extra;
      if(!(row>>label) || label[0]=='#') {
        continue;
      }
      if(!(row>>from>>to) || (row>>extra)) {
        throw std::runtime_error("navdemos detour-plans-v3 rows LABEL START GOAL");
      }
      detourPlanV3(w,out,label,from,to);
    }
    if(!input.eof()) {
      throw std::runtime_error("navdemos detour-plans-v3 read failed");
    }
    out.close();
    if(!out) {
      throw std::runtime_error("navdemos detour-plans-v3 write failed");
    }
    return;
  }
  if(a.size()>=2 && a[1]=="points-v3") {
    if(running()) {
      throw std::runtime_error("navdemos cannot export points during episode");
    }
    if(a.size()!=2) {
      throw std::runtime_error("navdemos points-v3");
    }
    std::ofstream out(std::filesystem::path(output)/"points-v3.jsonl");
    if(!out) {
      throw std::runtime_error("navdemos cannot write points-v3.jsonl");
    }
    out<<std::setprecision(9)<<std::boolalpha;
    w.forEachWayPoint([&](const WayPoint& point) {
      out<<"{\"name\":"<<std::quoted(point.name)<<",\"position_cm\":";vector(out,point.groundPos);
      out<<",\"connected\":"<<point.isConnected()<<",\"freepoint\":"<<point.isFreePoint()<<"}\n";
    });
    if(!out) {
      throw std::runtime_error("navdemos points-v3 write failed");
    }
    return;
  }
  if(a.size()>=2 && (a[1]=="probe-v3" || a[1]=="probes-v3")) {
    if(running()) {
      throw std::runtime_error("navdemos cannot probe during episode");
    }
    const bool batch=a[1]=="probes-v3";
    if((batch && a.size()!=4) || (!batch && a.size()!=5 && a.size()!=6 && a.size()!=8)) {
      throw std::runtime_error("navdemos probe-v3 NPC START GOAL [LABEL [IN_ALIAS OUT_ALIAS]] | probes-v3 NPC FILE");
    }
    auto* npc=w.findNpcByInstance(w.script().findSymbolIndex(a[2]));
    if(!npc) {
      throw std::runtime_error("navdemos probe-v3 missing NPC");
    }
    std::ofstream out(std::filesystem::path(output)/"object-candidates.jsonl",std::ios::app);
    if(!out) {
      throw std::runtime_error("navdemos cannot write object-candidates.jsonl");
    }
    out<<std::setprecision(9)<<std::boolalpha;
    if(!batch) {
      probeObject(w,*npc,out,a.size()>=6?a[5]:"0",a[3],a[4],a.size()==8?a[6]:"",a.size()==8?a[7]:"");
    }
    else {
      std::ifstream in(a[3]);
      if(!in) {
        throw std::runtime_error("navdemos cannot read probes-v3 file");
      }
      std::string line;
      while(std::getline(in,line)) {
        if(line.empty() || line[0]=='#') {
          continue;
        }
        std::istringstream row(line);std::string label,from,to,openingIn,openingOut,extra;
        if(!(row>>label>>from>>to)) {
          throw std::runtime_error("navdemos probes-v3 rows LABEL START GOAL [IN_ALIAS OUT_ALIAS]");
        }
        if(row>>openingIn) {
          if(!(row>>openingOut) || (row>>extra)) {
            throw std::runtime_error("navdemos probes-v3 rows LABEL START GOAL [IN_ALIAS OUT_ALIAS]");
          }
        }
        probeObject(w,*npc,out,label,from,to,openingIn,openingOut);
      }
    }
    return;
  }
  if(a.size()>=2 && (a[1]=="plan-v2" || a[1]=="plans-v2")) {
    if(running())throw std::runtime_error("navdemos cannot query a new route during an episode");
    const bool batch=a[1]=="plans-v2";
    if((batch && a.size()!=4) || (!batch && a.size()!=5 && a.size()!=6))
      throw std::runtime_error("navdemos plan-v2 NPC FROM TO [LABEL] | plans-v2 NPC PAIRS_FILE (LABEL FROM TO)");
    auto* npc=w.findNpcByInstance(w.script().findSymbolIndex(a[2]));
    if(!npc)throw std::runtime_error("navdemos plan-v2 missing NPC");
    std::ofstream out(std::filesystem::path(output)/"plans-v2.jsonl",std::ios::app);
    if(!out)throw std::runtime_error("navdemos cannot write plans-v2.jsonl");
    out<<std::setprecision(9)<<std::boolalpha;
    if(!batch)planV2(w,*npc,out,a.size()==6?a[5]:"0",a[3],a[4]);
    else {
      std::ifstream input(a[3]);
      if(!input)throw std::runtime_error("navdemos cannot read plans-v2 pairs");
      std::string line;
      while(std::getline(input,line)) {
        if(line.empty() || line[0]=='#')continue;
        std::istringstream row(line);std::string label,from,to,extra;
        if(!(row>>label>>from>>to) || (row>>extra))
          throw std::runtime_error("navdemos plans-v2 pairs require LABEL FROM TO per line");
        planV2(w,*npc,out,label,from,to);
      }
    }
    return;
  }
  handled=false;
}
}
