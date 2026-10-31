#include "navperceptionbench.h"

#include "navigationeye.h"
#include "navigationmemory.h"
#include "game/gamescript.h"
#include "navbench.h"
#include "physics/navmesh.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {
using Tempest::Vec3;
using Clock=std::chrono::steady_clock;
constexpr float Deg=3.14159265358979323846f/180.f;
constexpr float Begin=500,End=1500,Radius=15;
constexpr uint64_t Epoch=1000000000;
using Counts=std::map<std::string,uint64_t>;

struct Request final {
  std::string label;
  uint64_t elapsed=0;
  Vec3 position,goal;
  float yaw=0;
  bool sample=true;
};

std::vector<Request> readRequests(const std::string& path) {
  if(!std::filesystem::path(path).is_absolute())
    throw std::runtime_error("perception-replay input must be absolute");
  std::ifstream input(path);
  if(!input)throw std::runtime_error("perception-replay cannot read input");
  std::vector<Request> requests;
  std::set<std::string> labels;
  std::string line;
  while(std::getline(input,line)) {
    const auto first=line.find_first_not_of(" \t\r");
    if(first==std::string::npos || line[first]=='#')continue;
    Request request;
    std::string elapsed,flag,extra;
    std::istringstream row(line);
    if(!(row>>request.label>>elapsed>>request.position.x>>request.position.y>>request.position.z
         >>request.yaw>>request.goal.x>>request.goal.y>>request.goal.z))
      throw std::runtime_error("perception-replay rows: LABEL ELAPSED_MS X Y Z YAW GX GY GZ [SAMPLE=0|1]");
    if(elapsed.empty() || elapsed.find_first_not_of("0123456789")!=std::string::npos)
      throw std::runtime_error("perception-replay elapsed must be an unsigned integer");
    request.elapsed=std::stoull(elapsed);
    if(row>>flag) {
      if(flag=="0" || flag=="SAMPLE=0")request.sample=false;
      else if(flag!="1" && flag!="SAMPLE=1")throw std::runtime_error("perception-replay SAMPLE must be 0 or 1");
      if(row>>extra)throw std::runtime_error("perception-replay extra input field");
    }
    for(float value:{request.position.x,request.position.y,request.position.z,request.yaw,
                     request.goal.x,request.goal.y,request.goal.z})
      if(!std::isfinite(value))throw std::runtime_error("perception-replay nonfinite pose");
    if(request.label.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=std::string::npos ||
       request.elapsed>86400000 || request.elapsed%100!=0)
      throw std::runtime_error("perception-replay invalid label or elapsed (100 ms grid, at most one day)");
    if(requests.empty() || requests.back().label!=request.label) {
      if(!labels.insert(request.label).second)throw std::runtime_error("perception-replay labels must be contiguous");
    } else if(request.elapsed<=requests.back().elapsed)
      throw std::runtime_error("perception-replay elapsed must increase within each label");
    requests.push_back(request);
    if(requests.size()>500000)throw std::runtime_error("perception-replay exceeds 500000 pose limit");
  }
  if(!input.eof() || requests.empty())throw std::runtime_error("perception-replay empty or unreadable input");
  return requests;
}

NavMesh::Point meters(const Vec3& value) { return {value.x*.01f,value.y*.01f,value.z*.01f}; }
Vec3 centimeters(const NavMesh::Point& value) { return Vec3(value[0]*100,value[1]*100,value[2]*100); }
Vec3 local(const Vec3& point,const Request& request) {
  const auto delta=point-request.position;
  const float c=std::cos(request.yaw*Deg),s=std::sin(request.yaw*Deg);
  return Vec3(-delta.x*s+delta.z*c,delta.y,delta.x*c+delta.z*s);
}

float pointSegmentSquared(const Vec3& point,const Vec3& start,const Vec3& end) {
  const float dx=end.x-start.x,dz=end.z-start.z,square=dx*dx+dz*dz;
  const float t=square>0?std::clamp(((point.x-start.x)*dx+(point.z-start.z)*dz)/square,0.f,1.f):0.f;
  const float x=point.x-start.x-t*dx,z=point.z-start.z-t*dz;
  return x*x+z*z;
}

// Same native-cell intersection as navdemos oracle-corridors: a radius-15 cm
// corridor along the 3D Detour arclength from 5 to 15 metres.
float segmentSquareSquared(const Vec3& start,const Vec3& end,float x0,float z0,float step) {
  const std::array<float,2> a={start.x,start.z},delta={end.x-start.x,end.z-start.z};
  const std::array<float,2> low={x0,z0},high={x0+step,z0+step};
  float enter=0,leave=1;
  bool intersects=true;
  for(size_t axis=0;axis<2;++axis) {
    if(std::abs(delta[axis])<.000001f) {
      if(a[axis]<low[axis] || a[axis]>high[axis]) {intersects=false;break;}
      continue;
    }
    float first=(low[axis]-a[axis])/delta[axis],last=(high[axis]-a[axis])/delta[axis];
    if(first>last)std::swap(first,last);
    enter=std::max(enter,first);leave=std::min(leave,last);
    if(enter>leave) {intersects=false;break;}
  }
  if(intersects)return 0;
  const auto pointSquare=[&](const Vec3& point) {
    const float dx=std::max({0.f,x0-point.x,point.x-x0-step});
    const float dz=std::max({0.f,z0-point.z,point.z-z0-step});
    return dx*dx+dz*dz;
  };
  float square=std::min(pointSquare(start),pointSquare(end));
  for(float x:{x0,x0+step})for(float z:{z0,z0+step})
    square=std::min(square,pointSegmentSquared(Vec3(x,0,z),start,end));
  return square;
}

std::array<uint8_t,7> quantize(const NavigationMemory::Cell& cell) {
  const std::array<float,7> channels={cell.confidence,cell.free,cell.wall,cell.ledge,cell.drop,cell.water,cell.movable};
  std::array<uint8_t,7> result={};
  for(size_t i=0;i<channels.size();++i) {
    if(!std::isfinite(channels[i]) || channels[i]<0 || channels[i]>1)
      throw std::runtime_error("perception-replay memory channel outside [0,1]");
    result[i]=uint8_t(std::lround(channels[i]*255.f));
  }
  return result;
}

void classify(Counts& counts,const NavMesh::RasterCell& truth,const NavigationMemory::Cell& memory,float step) {
  const auto channels=quantize(memory);
  const bool danger=std::any_of(channels.begin()+2,channels.end(),[](uint8_t value) {return value>0;});
  const bool unknown=channels[0]==0 || channels[1]==0;
  const std::string category=danger?"danger":unknown?"unknown":"free";
  const uint64_t area=uint64_t(step*step);
  ++counts["cells"];counts["area_cm2"]+=area;
  ++counts[category];counts[category+"_area_cm2"]+=area;
  counts["confidence_quantized_sum"]+=channels[0];counts["free_quantized_sum"]+=channels[1];
  counts["unknown_exposure_quantized_sum"]+=255-std::min(channels[0],channels[1]);
  counts["truth_passable"]+=truth.traversable;
  counts["truth_passable_"+category]+=truth.traversable;
  counts["truth_unpassable"]+=!truth.traversable;
  counts["truth_unpassable_"+category]+=!truth.traversable;
  counts["truth_height_boundary"]+=truth.heightBoundary;
  counts["false_danger"]+=danger && truth.traversable;
  counts["false_danger_area_cm2"]+=uint64_t(danger && truth.traversable)*area;
  counts["true_danger"]+=danger && !truth.traversable;
  counts["unknown_zero_confidence"]+=category=="unknown" && channels[0]==0;
  counts["unknown_positive_confidence_zero_free"]+=category=="unknown" && channels[0]>0 && channels[1]==0;
  const std::array<const char*,5> names={"wall","ledge","drop","water","movable"};
  for(size_t i=0;i<names.size();++i) {
    const std::string name=names[i];
    counts["danger_"+name]+=channels[i+2]>0;
    counts["false_danger_"+name]+=channels[i+2]>0 && truth.traversable;
    counts["true_danger_"+name]+=channels[i+2]>0 && !truth.traversable;
  }
  const bool comparable=std::isfinite(memory.groundHeightCm) && truth.heightValid;
  const bool mismatch=comparable && std::abs(memory.groundHeightCm-truth.groundHeight*100.f)>100;
  counts["height_comparable"]+=comparable;counts["height_mismatch_gt100"]+=mismatch;
  counts["false_danger_height_mismatch_gt100"]+=mismatch && danger && truth.traversable;
}

void countsJson(std::ostream& out,const Counts& counts) {
  out<<'{';bool first=true;
  for(const auto& [key,value]:counts) {if(!first)out<<',';first=false;out<<std::quoted(key)<<':'<<value;}
  out<<'}';
}

bool detailed(const Request& request) {
  return (request.label=="point-0156131ae7538da6" && request.elapsed==5200) ||
         (request.label=="object-39a83107a39adc9e" && request.elapsed==87700) ||
         (request.label=="object-15cce3111679129f" && request.elapsed==13300);
}

void evaluate(std::ostream& out,World& world,const Request& request,const NavigationMemory::View& view,
              const std::string& mode,const NavigationEye& eye) {
  // The entire perception update and readout have finished before this access.
  const auto& mesh=NavBench::navigationMesh(world);
  const auto path=mesh.findPath(meters(request.position),meters(request.goal));
  const auto raster=mesh.rasterLocal(meters(request.position),request.yaw);
  const char* status=!path.complete?"incomplete_path":!path.transitions.empty()?"offmesh_transition":
                     path.length*100<=Begin?"short_path":!raster.supportValid?"unsupported_pose":"ok";
  const bool available=std::string(status)=="ok";
  std::vector<std::pair<Vec3,Vec3>> corridor;
  if(available) {
    float offset=0;
    for(size_t i=1;i<path.corners.size();++i) {
      const auto a=centimeters(path.corners[i-1]),b=centimeters(path.corners[i]);
      const float extent=(b-a).length();
      if(extent>0 && offset+extent>Begin && offset<End) {
        const float first=std::clamp((Begin-offset)/extent,0.f,1.f),last=std::clamp((End-offset)/extent,0.f,1.f);
        corridor.emplace_back(local(a+(b-a)*first,request),local(a+(b-a)*last,request));
      }
      offset+=extent;if(offset>=End)break;
    }
  }
  Counts corridorCounts,bandCounts;
  std::array<Counts,2> corridorLayers;
  std::ostringstream cells;
  size_t cellDetails=0;
  const bool details=detailed(request);
  const auto layer=[&](const NavMesh::RasterLayer& truth,const NavigationMemory::Layer& memory,bool near) {
    if(truth.size!=memory.size || truth.cells.size()!=memory.cells.size() ||
       std::abs(truth.cellSize*100-memory.cellCm)>.001f)
      throw std::runtime_error("perception-replay truth/memory raster mismatch");
    const float step=memory.cellCm,half=float(memory.size)*.5f;
    for(size_t row=0;row<memory.size;++row)for(size_t column=0;column<memory.size;++column) {
      const float x0=(float(column)-half)*step,z0=(float(row)-half)*step;
      const float x=x0+step*.5f,z=z0+step*.5f;
      if(!near && std::abs(x)<800 && std::abs(z)<800)continue;
      const auto& real=truth.cells[row*truth.size+column];
      const auto& seen=memory.cells[row*memory.size+column];
      if((real.heightValid && !std::isfinite(real.groundHeight)) ||
         (!std::isfinite(seen.groundHeightCm) && !std::isnan(seen.groundHeightCm)))
        throw std::runtime_error("perception-replay invalid ground height");
      const bool touched=available && std::any_of(corridor.begin(),corridor.end(),[&](const auto& segment) {
        return segmentSquareSquared(segment.first,segment.second,x0,z0,step)<=Radius*Radius;
      });
      const bool band=raster.supportValid && z>=0 && x*x+z*z>=Begin*Begin && x*x+z*z<=End*End;
      if(touched) {classify(corridorCounts,real,seen,step);classify(corridorLayers[near?0:1],real,seen,step);}
      if(band)classify(bandCounts,real,seen,step);
      const auto channels=quantize(seen);
      if(!details || (!touched && !(band && real.traversable && channels[2]>0)))continue;
      if(cellDetails++)cells<<',';
      cells<<"{\"layer\":"<<std::quoted(near?"near":"far")<<",\"row\":"<<row<<",\"column\":"<<column
           <<",\"corridor\":"<<touched<<",\"truth_passable\":"<<real.traversable
           <<",\"height_valid\":"<<real.heightValid<<",\"ground_height_cm\":"<<real.groundHeight*100
           <<",\"height_boundary\":"<<real.heightBoundary<<",\"channels\":[";
      for(size_t i=0;i<channels.size();++i) {if(i)cells<<',';cells<<unsigned(channels[i]);}
      cells<<"],\"memory_height_valid\":"<<std::isfinite(seen.groundHeightCm)
           <<",\"memory_ground_height_cm\":"<<(std::isfinite(seen.groundHeightCm)?seen.groundHeightCm:0)
           <<",\"local_cm\":["<<x<<",0,"<<z<<"]}";
    }
  };
  layer(raster.near,view.near,true);layer(raster.far,view.far,false);
  out<<"{\"label\":"<<std::quoted(request.label)<<",\"elapsed_ms\":"<<request.elapsed<<",\"mode\":"<<std::quoted(mode)
     <<",\"sampled\":"<<request.sample
     <<",\"position_cm\":["<<request.position.x<<','<<request.position.y<<','<<request.position.z<<']'
     <<",\"yaw_deg\":"<<request.yaw<<",\"gaze_pitch_deg\":"<<eye.gazePitchDeg()
     <<",\"corridor_status\":"<<std::quoted(status)<<",\"corridor_available\":"<<available
     <<",\"raster_support_valid\":"<<raster.supportValid<<",\"corridor\":";
  countsJson(out,corridorCounts);out<<",\"corridor_layers\":{\"near\":";countsJson(out,corridorLayers[0]);
  out<<",\"far\":";countsJson(out,corridorLayers[1]);out<<"},\"forward_band\":";countsJson(out,bandCounts);
  out<<",\"native_cells\":["<<cells.str()<<"],\"eye_hit_rays\":[";
  size_t rayDetails=0;
  if(details)for(size_t i=0;i<eye.memorySamples().size();++i) {
    const auto& ray=eye.memorySamples()[i];
    if(!ray.hit)continue;
    if(rayDetails++)out<<',';
    const auto vector=[&](const Vec3& value) {out<<'['<<value.x<<','<<value.y<<','<<value.z<<']';};
    out<<"{\"index\":"<<i<<",\"cached_far_slot\":"<<(i>=eye.rayCount())
       <<",\"yaw_deg\":"<<ray.yawDeg<<",\"pitch_deg\":"<<ray.pitchDeg
       <<",\"hit_kind\":"<<std::quoted(NavigationEye::hitKindName(ray.kind))
       <<",\"distance_cm\":"<<ray.distanceCm<<",\"normal_y\":"<<ray.normalY
       <<",\"sampled_at_ms\":"<<ray.sampledAtMs<<",\"near_sampled_at_ms\":"<<ray.nearSampledAtMs
       <<",\"complete\":"<<ray.complete<<",\"origin_cm\":";vector(ray.origin);
    out<<",\"direction\":";vector(ray.direction);out<<",\"point_cm\":";vector(ray.point);out<<'}';
  }
  out<<"]}\n";
  if(!out)throw std::runtime_error("perception-replay frame write failed");
}

void distribution(std::ostream& out,std::vector<double> values) {
  double sum=0;for(double value:values)sum+=value;
  std::sort(values.begin(),values.end());
  out<<"{\"samples\":"<<values.size()<<",\"sum_us\":"<<sum<<",\"mean_us\":"<<(values.empty()?0:sum/double(values.size()))
     <<",\"median_us\":"<<(values.empty()?0:values[values.size()/2])
     <<",\"p95_us\":"<<(values.empty()?0:values[size_t(std::ceil(double(values.size())*.95))-1])<<'}';
}
}

void NavPerceptionBench::command(const std::vector<std::string>& args,World& world,Npc& player,
                                  const std::string& outputDirectory) {
  if(args.size()!=4 || (args[3]!="baseline" && args[3]!="gaze" && args[3]!="rows"))
    throw std::runtime_error("navdemos perception-replay FILE baseline|gaze|rows");
  if(!std::filesystem::path(outputDirectory).is_absolute())throw std::runtime_error("perception-replay output must be absolute");
  const auto requests=readRequests(args[2]);
  const auto mode=args[3];
  NavigationEye::Config config;config.layout=250;
  config.floorSampling=mode=="gaze"?NavigationEye::FloorSampling::WalkingGazeDown:
                       mode=="rows"?NavigationEye::FloorSampling::ForwardRows:NavigationEye::FloorSampling::Baseline;
  NavigationEye eye(config);
  NavigationMemory memory;
  NavigationMemory::View view;
  std::ofstream frames(std::filesystem::path(outputDirectory)/"perception-frames.jsonl");
  std::ofstream timing(std::filesystem::path(outputDirectory)/"perception-timing.json");
  if(!frames || !timing)throw std::runtime_error("perception-replay cannot open output");
  frames<<std::setprecision(10)<<std::boolalpha;timing<<std::setprecision(12)<<std::boolalpha;
  const auto instance=world.script().findSymbolIndex("BAU_954_MALETH");
  if(instance==size_t(-1))throw std::runtime_error("perception-replay human fixture unavailable");
  auto* npc=world.addNpc(instance,requests.front().position);
  if(!npc)throw std::runtime_error("perception-replay fixture creation failed");
  struct Restore final {
    World& world;Npc& player;Npc* fixture;
    Vec3 position;float yaw;
    ~Restore() {player.setPosition(position);player.setDirection(yaw);player.updateTransform();world.removeNpc(*fixture);}
  } restore{world,player,npc,player.position(),player.rotation()};
  npc->setInteraction(nullptr,true);npc->clearAiQueue();npc->clearState(false);npc->clearSpeed();
  npc->clearGoTo();npc->attachToPoint(nullptr);npc->setExternalControl(true);
  const auto& guild=world.script().guildVal();
  const float step=float(guild.step_height[npc->guild()]);
  const float slope=std::acos(std::clamp(std::cos(float(guild.slide_angle[npc->guild()])*Deg),0.f,1.f))/Deg;
  std::vector<double> eyeCosts,memoryCosts,nearCosts,fullCosts;
  uint64_t casts=0,nearCasts=0,farCasts=0,episodes=0,samples=0;
  std::string previous;
  for(const auto& request:requests) {
    if(previous!=request.label) {eye.reset();memory.reset();previous=request.label;++episodes;}
    const float yaw=request.yaw*Deg;
    const Vec3 forward(std::cos(yaw),0,std::sin(yaw)),right(-forward.z,0,forward.x);
    npc->setPosition(request.position);npc->setDirection(request.yaw);npc->updateTransform();
    // Keep the hero behind the human view and within the living-rig birth range.
    player.setPosition(request.position-forward*500);player.updateTransform();
    const auto eyeBegan=Clock::now();
    const auto head=npc->navigationEyePose();
    NavigationEye::Pose pose;
    pose.origin=head.origin;pose.position=request.position;
    // A saved body yaw defines the reproducible neutral head frame. Animated
    // gaze from an unrelated loaded save must not rotate the replay trajectory.
    pose.forward=forward;pose.right=right;pose.up=Vec3(0,1,0);
    const auto update=eye.update(pose,*world.physic(),npc,Epoch+request.elapsed);
    const double eyeUs=std::chrono::duration<double,std::micro>(Clock::now()-eyeBegan).count();
    if(!update.updated)throw std::runtime_error("perception-replay source poses are faster than the eye");
    eyeCosts.push_back(eyeUs);(update.farUpdated?fullCosts:nearCosts).push_back(eyeUs);
    casts+=update.casts;nearCasts+=update.nearCasts;farCasts+=update.farCasts;
    const auto began=Clock::now();
    NavigationMemory::Pose memoryPose;
    memoryPose.position=request.position;memoryPose.forward=forward;memoryPose.gameMs=Epoch+request.elapsed;
    memoryPose.stepHeightCm=step>0.f?step:50.f;memoryPose.maxSlopeDegrees=slope;
    memory.update(eye.memorySamples(),memoryPose);
    memory.view(view);
    memoryCosts.push_back(std::chrono::duration<double,std::micro>(Clock::now()-began).count());
    if(request.sample || detailed(request))evaluate(frames,world,request,view,mode,eye);
    samples+=request.sample;
  }
  timing<<"{\"mode\":"<<std::quoted(mode)<<",\"episodes\":"<<episodes<<",\"updates\":"<<requests.size()
        <<",\"navigation_memory_version\":"<<NavigationMemory::RepresentationVersion
        <<",\"sampled_frames\":"<<samples<<",\"ray_count\":"<<eye.rayCount()
        <<",\"step_height_cm\":"<<(step>0.f?step:50.f)<<",\"max_slope_degrees\":"<<slope
        <<",\"near_period_ms\":"<<config.nearPeriodMs<<",\"far_period_ms\":"<<config.farPeriodMs
        <<",\"casts\":"<<casts<<",\"near_casts\":"<<nearCasts<<",\"far_casts\":"<<farCasts
        <<",\"head_frame\":\"neutral body yaw, fixture head height\",\"eye\":";
  distribution(timing,eyeCosts);timing<<",\"near_only\":";distribution(timing,nearCosts);
  timing<<",\"full\":";distribution(timing,fullCosts);timing<<",\"memory_and_view\":";distribution(timing,memoryCosts);
  timing<<"}\n";
  if(!timing)throw std::runtime_error("perception-replay timing write failed");
}
