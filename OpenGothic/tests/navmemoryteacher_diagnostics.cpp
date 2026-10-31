#include "navmemoryteacher.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition,const std::string& message) {
  if(!condition) throw std::runtime_error(message);
  }

// Read the packed little-endian format, never a native struct with padding.
// World pose, timestamps, rays and privileged hints are deliberately skipped:
// both replay and diagnostics receive exactly the same quantized learner memory.
struct Cursor final {
  const uint8_t* current;
  const uint8_t* end;

  explicit Cursor(const std::vector<uint8_t>& bytes):current(bytes.data()),end(bytes.data()+bytes.size()) {}
  void skip(size_t count) {
    require(size_t(end-current)>=count,"truncated NAVDEMO value");
    current+=count;
    }
  uint8_t u8() {
    require(current<end,"truncated NAVDEMO byte");
    return *current++;
    }
  bool bit() {
    const auto value=u8();
    require(value<=1,"NAVDEMO boolean outside 0..1");
    return value!=0;
    }
  uint32_t u32() {
    uint32_t value=0;
    for(unsigned i=0;i<4;++i) value|=uint32_t(u8())<<(8*i);
    return value;
    }
  float f32() {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    const auto bits=u32();
    float value=0;
    std::memcpy(&value,&bits,sizeof(value));
    require(std::isfinite(value),"non-finite NAVDEMO learner value");
    return value;
    }
  std::array<float,3> vec() { return {f32(),f32(),f32()}; }
  NpcKeys::KeyState keys() {
    NpcKeys::KeyState value;
    value.forward=f32();value.strafe=f32();value.turn=f32();
    value.headYaw=f32();value.headPitch=f32();
    value.walk=bit();value.jump=bit();value.climb=bit();
    return value;
    }
  };

bool equal(const NpcKeys::KeyState& a,const NpcKeys::KeyState& b) {
  // Exact scalar equality also accepts either IEEE representation of zero.
  return a.forward==b.forward && a.strafe==b.strafe && a.turn==b.turn &&
         a.headYaw==b.headYaw && a.headPitch==b.headPitch &&
         a.walk==b.walk && a.jump==b.jump && a.climb==b.climb;
  }

struct Observation final {
  NavMemoryTeacher::Input input;
  NpcKeys::KeyState expected;

  void decode(const std::vector<uint8_t>& bytes) {
    Cursor in(bytes);
    in.skip(8+3*4+4); // time_ms, position_cm, body_yaw_deg: never read.
    input.body.velocityCmS=in.vec();
    for(auto& flag:input.body.flags) flag=uint8_t(in.bit());
    input.body.groundNormal=in.vec();input.body.groundSlopeDeg=in.f32();
    input.body.headYawDeg=in.f32();input.body.headPitchDeg=in.f32();
    input.roads.carrotDirection=in.vec();input.roads.carrotDistanceCm=in.f32();
    input.roads.goalDirection=in.vec();input.roads.goalDistanceCm=in.f32();
    input.roads.remainingKnownCm=in.f32();input.roads.beyondKnown=in.bit();
    input.previous=in.keys();expected=in.keys();
    in.skip(NavDemoWriter::rayCount*19); // eye: valid learner input, unused by v3.
    const auto memory=[&](auto& cells) {
      for(auto& cell:cells) {
        for(auto& channel:cell.channels) channel=in.u8();
        cell.groundHeightCm=in.f32();cell.heightValid=uint8_t(in.bit());
        require(cell.heightValid || cell.groundHeightCm==0,"unknown NAVDEMO height is not zero");
        }
      };
    memory(input.near);memory(input.far);
    in.skip(4*4); // teacher corner and Detour path length: never read.
    require(in.current==in.end,"NAVDEMO record layout mismatch");
    }
  };

class Episode final {
  public:
    explicit Episode(const std::string& path):stream(gzopen(path.c_str(),"rb")) {
      require(stream!=nullptr,"cannot open NAVDEMO "+path);
      try {
        std::vector<uint8_t> header(NavDemoWriter::headerBytes);
        require(read(header.data(),header.size())==header.size(),"truncated NAVDEMO header");
        const char magic[8]={'N','A','V','D','E','M','O','\0'};
        require(std::memcmp(header.data(),magic,sizeof(magic))==0,"incorrect NAVDEMO magic");
        Cursor in(header);in.skip(8);
        require(in.u32()==2,"memory teacher replay requires NAVDEMO v2");
        require(in.u32()==NavDemoWriter::recordBytes && in.u32()==NavDemoWriter::rayCount &&
                in.u32()==NavDemoWriter::nearSize && in.u32()==NavDemoWriter::farSize &&
                in.u32()==100,"unsupported NAVDEMO dimensions or decision period");
        }
      catch(...) { gzclose(stream);stream=nullptr;throw; }
      }
    ~Episode() { if(stream!=nullptr) gzclose(stream); }
    Episode(const Episode&)=delete;
    Episode& operator=(const Episode&)=delete;

    bool next(std::vector<uint8_t>& bytes) {
      bytes.resize(NavDemoWriter::recordBytes);
      const auto count=read(bytes.data(),bytes.size());
      if(count==0) return false;
      require(count==bytes.size(),"truncated NAVDEMO record");
      return true;
      }

  private:
    size_t read(void* target,size_t size) {
      require(size<=size_t(std::numeric_limits<unsigned>::max()),"oversized NAVDEMO block");
      const int result=gzread(stream,target,unsigned(size));
      if(result<0) {
        int code=Z_OK;
        const char* message=gzerror(stream,&code);
        throw std::runtime_error(std::string("NAVDEMO gzip read failed: ")+message);
        }
      if(size_t(result)<size) require(gzeof(stream)!=0,"damaged NAVDEMO gzip payload");
      return size_t(result);
      }
    gzFile stream=nullptr;
  };

using Input = NavMemoryTeacher::Input;
using Point = NavMemoryTeacher::Point;
using Options = NavMemoryTeacher::DiagnosticsOptions;
using Diagnostics = NavMemoryTeacher::Diagnostics;

void number(float value) {
  if(std::isfinite(value)) std::cout<<value;
  else std::cout<<"null";
  }

void point(Point value) {
  std::cout<<'[';number(value.rightCm);std::cout<<',';number(value.forwardCm);std::cout<<']';
  }

void keys(const NpcKeys::KeyState& value) {
  std::cout<<"{\"forward\":"<<value.forward<<",\"strafe\":"<<value.strafe<<",\"turn\":"<<value.turn
           <<",\"head_yaw_deg\":"<<value.headYaw<<",\"head_pitch_deg\":"<<value.headPitch
           <<",\"walk\":"<<value.walk<<",\"jump\":"<<value.jump<<",\"climb\":"<<value.climb<<'}';
  }

void diagnostic(const std::string& name,const std::string& targetKind,
                const Options& options,const Diagnostics& value,bool paths) {
  std::cout<<"{\"name\":\""<<name<<"\",\"target_kind\":\""<<targetKind
           <<"\",\"options\":{\"body_radius_cm\":"<<options.bodyRadiusCm
           <<",\"unknown_cost\":"<<options.unknownCost<<",\"near_only\":"<<options.nearOnly
           <<",\"known_free_only\":"<<options.knownFreeOnly<<"},\"plan_complete\":"<<value.planComplete
           <<",\"actual_target_reached\":"<<value.actualTargetReached
           <<",\"target_outside_map\":"<<value.targetOutsideMap<<",\"target_clamped\":"<<value.targetClamped
           <<",\"target_scale\":";number(value.targetScale);
  std::cout<<",\"target_clear\":"<<value.targetClear<<",\"start_clear\":"<<value.startClear
           <<",\"start_found\":"<<value.startFound<<",\"start_escape_required\":"<<value.startEscapeRequired
           <<",\"start_escape_target\":";point(value.startEscapeTarget);
  std::cout<<",\"overlap_danger_cells\":"<<value.overlapDangerCells<<",\"target\":";point(value.target);
  std::cout<<",\"local_target\":";point(value.localTarget);
  std::cout<<",\"snapped_target\":";point(value.snappedTarget);
  std::cout<<",\"snapped_target_distance_cm\":";number(value.snappedTargetDistanceCm);
  std::cout<<",\"endpoint_distance_to_actual_target_cm\":";number(value.endpointDistanceToActualTargetCm);
  std::cout<<",\"plan_length_cm\":";number(value.planLengthCm);
  std::cout<<",\"plan_cost\":";number(value.planCost);
  std::cout<<",\"near_danger_fraction\":";number(value.dangerFraction[0]);
  std::cout<<",\"far_danger_fraction\":";number(value.dangerFraction[1]);
  std::cout<<",\"near_blocked_fraction\":";number(value.blockedFraction[0]);
  std::cout<<",\"far_blocked_fraction\":";number(value.blockedFraction[1]);
  std::cout<<",\"near_known_free_fraction\":";number(value.knownFreeFraction[0]);
  std::cout<<",\"far_known_free_fraction\":";number(value.knownFreeFraction[1]);
  std::cout<<",\"reachable_nodes\":"<<value.reachableNodes;
  if(paths) {
    std::cout<<",\"path\":[";
    bool first=true;
    for(const auto& p:value.path) {
      if(!first) std::cout<<',';
      first=false;point(p);
      }
    std::cout<<']';
    }
  std::cout<<'}';
  }

void diagnose(const Input& input,uint64_t index,const NpcKeys::KeyState& replayKeys,
              const NavMemoryTeacher::PlanStatus& status,bool keysMatch,bool previousMatch,bool paths) {
  std::cout<<"{\"type\":\"record\",\"index\":"<<index<<",\"keys_match\":"<<keysMatch
           <<",\"previous_match\":"<<previousMatch<<",\"beyond_known\":"<<input.roads.beyondKnown
           <<",\"steering_start_found\":"<<status.startFound<<",\"steering_escape_required\":"<<status.escapeRequired
           <<",\"replay_keys\":";
  keys(replayKeys);std::cout<<",\"diagnostics\":[";
  bool first=true;
  const auto output=[&](const std::string& name,const std::string& targetKind,
                        const Input& observation,const Options& options) {
    const auto result=NavMemoryTeacher::probe(observation,options);
    if(!first) std::cout<<',';
    first=false;
    diagnostic(name,targetKind,options,result,paths);
    };
  for(float radius:{30.f,15.f,0.f})for(float unknown:{1.f,2.f,4.f,8.f,16.f}) {
    Options options;
    options.bodyRadiusCm=radius;options.unknownCost=unknown;
    output("r"+std::to_string(int(radius))+"-u"+std::to_string(int(unknown)),"steering",input,options);
    }
  Options options;
  options.bodyRadiusCm=30;
  options.knownFreeOnly=true;
  output("r30-u4-known-free","steering",input,options);
  options.knownFreeOnly=false;options.nearOnly=true;
  output("r30-u4-near","steering",input,options);
  options.knownFreeOnly=true;
  output("r30-u4-near-known-free","steering",input,options);
  // The production teacher still steers to familiar-road carrots. These
  // additional probes answer a separate question about the final goal.
  Input goalInput=input;
  goalInput.roads.beyondKnown=true;
  for(float radius:{30.f,15.f,0.f}) {
    Options goalOptions;goalOptions.bodyRadiusCm=radius;
    output("goal-r"+std::to_string(int(radius))+"-u4","goal",goalInput,goalOptions);
    }
  Options goalKnown;goalKnown.bodyRadiusCm=30;goalKnown.knownFreeOnly=true;
  output("goal-r30-u4-known-free","goal",goalInput,goalKnown);
  output("production-steering","steering",input,Options{});
  output("production-goal","goal",goalInput,Options{});
  std::cout<<"]}\n";
  }

std::set<uint64_t> indices(const std::string& path) {
  std::ifstream stream(path);
  require(stream.good(),"cannot open record indices "+path);
  std::set<uint64_t> result;
  std::string line;
  size_t lineNumber=0;
  while(std::getline(stream,line)) {
    ++lineNumber;
    std::istringstream words(line);
    std::string value,extra;
    if(!(words>>value)) continue;
    require(!(words>>extra),"expected one record index per line at "+path+":"+std::to_string(lineNumber));
    require(value.front()!='-',"negative record index at "+path+":"+std::to_string(lineNumber));
    size_t consumed=0;
    const auto index=std::stoull(value,&consumed);
    require(consumed==value.size(),"invalid record index at "+path+":"+std::to_string(lineNumber));
    result.insert(index);
    }
  require(!stream.bad(),"cannot read record indices "+path);
  return result;
  }

int replay(const std::string& path,const std::set<uint64_t>& selected,bool paths,bool allowMismatch) {
  Episode episode(path);
  NavMemoryTeacher teacher;teacher.reset();
  Observation observation;
  std::vector<uint8_t> bytes;
  NpcKeys::KeyState previous;
  uint64_t records=0,mismatches=0,previousMismatches=0,diagnosed=0;
  uint64_t startFoundFalse=0,escapeRequired=0;
  while(episode.next(bytes)) {
    observation.decode(bytes);
    const auto actual=teacher.decide(observation.input);
    const auto status=teacher.lastPlanStatus();
    startFoundFalse+=!status.startFound;
    escapeRequired+=status.escapeRequired;
    const bool keysMatch=equal(actual,observation.expected);
    const bool previousMatch=records==0 || equal(observation.input.previous,previous);
    if(!previousMatch) ++previousMismatches;
    if(!keysMatch || !previousMatch) ++mismatches;
    if(selected.count(records)!=0) {
      diagnose(observation.input,records,actual,status,keysMatch,previousMatch,paths);
      ++diagnosed;
      }
    previous=actual;++records;
    }
  require(records>0,"no recorded teacher decisions");
  require(selected.empty() || *selected.rbegin()<records,"requested record index exceeds the episode");
  std::cout<<"{\"type\":\"summary\",\"records\":"<<records<<",\"mismatches\":"<<mismatches
           <<",\"previous_mismatches\":"<<previousMismatches<<",\"diagnosed_records\":"<<diagnosed
           <<",\"counterfactual\":"<<allowMismatch<<",\"steering_start_found_false\":"<<startFoundFalse
           <<",\"steering_start_evaluable_records\":"<<records
           <<",\"steering_escape_required_records\":"<<escapeRequired<<"}\n";
  return allowMismatch || mismatches==0 ? 0 : 1;
  }

Input target(float right,float forward,float distance=600.f) {
  Input input;
  const float length=std::hypot(right,forward);
  require(length>0,"invalid synthetic target");
  input.roads.goalDirection={right/length,0,forward/length};
  input.roads.goalDistanceCm=distance;
  input.roads.carrotDirection=input.roads.goalDirection;
  input.roads.carrotDistanceCm=distance;
  input.roads.beyondKnown=true;
  input.body.groundNormal={0,1,0};
  return input;
  }

bool samePath(const std::vector<Point>& a,const std::vector<Point>& b) {
  if(a.size()!=b.size()) return false;
  for(size_t i=0;i<a.size();++i)
    if(a[i].rightCm!=b[i].rightCm || a[i].forwardCm!=b[i].forwardCm) return false;
  return true;
  }

void hazard(Input& input,int right,int forward) {
  auto& cell=input.near[size_t(forward)*64+size_t(right)];
  cell.channels[0]=255;cell.channels[2]=255;
  }

void selfTest() {
  unsigned tests=0;
  Options defaults;
  NavMemoryTeacher teacher;
  auto input=target(0,1);
  for(int right=24;right<40;++right) hazard(input,right,43);
  teacher.decide(input);
  auto result=NavMemoryTeacher::probe(input,defaults);
  require(samePath(teacher.plan(),result.path) && teacher.planComplete()==result.planComplete &&
          teacher.planLengthCm()==result.planLengthCm,"default diagnostic changed the production route");
  require(result.planComplete && result.actualTargetReached,"clear reachable target was not recognized exactly");
  ++tests;

  input=target(0,1);hazard(input,32,40);
  std::array<Diagnostics,3> radii;
  for(size_t i=0;i<radii.size();++i) {
    Options options;options.bodyRadiusCm=float(i)*15;
    radii[i]=NavMemoryTeacher::probe(input,options);
    require(radii[i].dangerFraction[0]==1.f/4096,"thin occupied cell was lost in the diagnostic map");
    }
  require(radii[0].blockedFraction[0]<radii[1].blockedFraction[0] &&
          radii[1].blockedFraction[0]<=radii[2].blockedFraction[0],"body-radius hazard dilation is not monotonic");
  ++tests;

  input=target(0,1,5000);
  result=NavMemoryTeacher::probe(input,defaults);
  Options near;near.nearOnly=true;
  const auto local=NavMemoryTeacher::probe(input,near);
  require(result.actualTargetReached && !result.targetOutsideMap,"full far memory failed to retain an in-map target");
  require(local.targetOutsideMap && local.targetClamped && !local.actualTargetReached &&
          local.endpointDistanceToActualTargetCm>4000,"near-only proxy falsely claimed to reach the actual goal");
  ++tests;

  input=target(0,1);
  for(auto& cell:input.near) {cell.channels[0]=255;cell.channels[1]=255;}
  for(int right=0;right<64;++right) input.near[43*64+size_t(right)].channels={};
  const auto optimistic=NavMemoryTeacher::probe(input,defaults);
  Options known;known.knownFreeOnly=true;
  const auto conservative=NavMemoryTeacher::probe(input,known);
  require(optimistic.actualTargetReached && !conservative.actualTargetReached && !conservative.planComplete,
          "known-free diagnostic crossed an unknown memory barrier");
  require(conservative.reachableNodes<optimistic.reachableNodes,"known-free restriction did not reduce the reachable component");
  ++tests;

  input=target(0,1,500);
  for(int z=50;z<=53;++z)for(int x=30;x<=33;++x) hazard(input,x,z);
  result=NavMemoryTeacher::probe(input,defaults);
  require(!result.targetClear && !result.actualTargetReached && result.endpointDistanceToActualTargetCm>1,
          "a snapped accessible cell falsely claimed the occupied actual target");
  ++tests;

  input=target(0,1,500);
  input.roads.carrotDirection={1,0,0};input.roads.carrotDistanceCm=500;
  input.roads.beyondKnown=false;
  for(int z=50;z<=53;++z)for(int x=30;x<=33;++x) hazard(input,x,z);
  const auto originalRoads=input.roads;
  const auto steeringTarget=NavMemoryTeacher::probe(input,defaults);
  Input goalInput=input;goalInput.roads.beyondKnown=true;
  const auto finalTarget=NavMemoryTeacher::probe(goalInput,defaults);
  require(steeringTarget.actualTargetReached && !finalTarget.actualTargetReached && !finalTarget.targetClear,
          "reachable steering carrot concealed the occupied final goal");
  require(input.roads.beyondKnown==originalRoads.beyondKnown &&
          input.roads.carrotDirection==originalRoads.carrotDirection &&
          input.roads.goalDirection==originalRoads.goalDirection &&
          input.roads.carrotDistanceCm==originalRoads.carrotDistanceCm &&
          input.roads.goalDistanceCm==originalRoads.goalDistanceCm,
          "final-goal probe mutated original learner road input");
  ++tests;

  input=target(0,1,5000);
  for(int right=0;right<100;++right) {
    auto& cell=input.far[55*100+size_t(right)];
    cell.channels[0]=255;cell.channels[2]=255;
    }
  result=NavMemoryTeacher::probe(input,defaults);
  const auto nearBarrier=NavMemoryTeacher::probe(input,near);
  require(!result.actualTargetReached && !nearBarrier.actualTargetReached,"far barrier or near proxy falsely reported the actual goal");
  require(!result.targetOutsideMap && nearBarrier.targetOutsideMap &&
          result.dangerFraction[1]>0,"far-only barrier did not remain visible to full-memory diagnostics");
  ++tests;

  input=target(0,1);
  NavMemoryTeacher baseline;
  std::vector<Input> sequence;
  std::vector<NpcKeys::KeyState> expectedKeys;
  std::vector<std::vector<Point>> expectedPaths;
  for(unsigned tick=0;tick<10;++tick) {
    sequence.push_back(input);
    const auto expected=baseline.decide(input);
    expectedKeys.push_back(expected);expectedPaths.push_back(baseline.plan());
    input.previous=expected;
    }
  NavMemoryTeacher withProbes;
  for(size_t tick=0;tick<sequence.size();++tick) {
    Options options;
    options.bodyRadiusCm=float(tick%3)*15;
    options.unknownCost=1.f+float(tick);
    options.nearOnly=tick%2!=0;options.knownFreeOnly=tick%3==0;
    NavMemoryTeacher::probe(sequence[tick],options);
    const auto actual=withProbes.decide(sequence[tick]);
    require(equal(expectedKeys[tick],actual) && samePath(expectedPaths[tick],withProbes.plan()),
            "probe options mutated the live teacher's decision state");
    }
  ++tests;
  std::cout<<"{\"type\":\"self_test\",\"tests\":"<<tests<<",\"passed\":true}\n";
  }
}

int main(int argc,char** argv) {
  try {
    std::cout<<std::boolalpha<<std::setprecision(std::numeric_limits<float>::max_digits10);
    std::string samples,indexFile;
    bool paths=false,selfTests=false,allowMismatch=false;
    for(int i=1;i<argc;++i) {
      const std::string argument=argv[i];
      if(argument=="--paths") paths=true;
      else if(argument=="--self-test") selfTests=true;
      else if(argument=="--allow-mismatch") allowMismatch=true;
      else if(argument=="--indices") {
        require(i+1<argc,"missing --indices filename");
        require(indexFile.empty(),"duplicate --indices option");
        indexFile=argv[++i];
        }
      else {
        require(!argument.empty() && argument.front()!='-',"unknown option "+argument);
        require(samples.empty(),"expected exactly one NAVDEMO episode");
        samples=argument;
        }
      }
    if(selfTests) selfTest();
    if(samples.empty() && selfTests && indexFile.empty()) return 0;
    if(samples.empty() || indexFile.empty()) {
      std::cerr<<"usage: gothic-navmemoryteacher-diagnostics SAMPLES.bin.gz --indices FILE [--paths] [--self-test] [--allow-mismatch]\n"
               <<"       gothic-navmemoryteacher-diagnostics --self-test\n";
      return 2;
      }
    return replay(samples,indices(indexFile),paths,allowMismatch);
    }
  catch(const std::exception& error) {
    std::cerr<<"navmemoryteacher diagnostics: "<<error.what()<<'\n';
    return 2;
    }
  }
