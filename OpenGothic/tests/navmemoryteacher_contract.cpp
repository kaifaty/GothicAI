#include "navmemoryteacher.h"

#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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
// the replay teacher receives exactly the same quantized learner memory.
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

  void decode(const std::vector<uint8_t>& bytes,uint32_t formatVersion) {
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
        cell.familiar=formatVersion==3 ? in.u8() : 0;
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
        version=in.u32();
        require(version==2 || version==3,"memory teacher replay requires NAVDEMO v2 or v3");
        require(in.u32()==NavDemoWriter::recordBytesForVersion(version) && in.u32()==NavDemoWriter::rayCount &&
                in.u32()==NavDemoWriter::nearSize && in.u32()==NavDemoWriter::farSize &&
                in.u32()==100,"unsupported NAVDEMO dimensions or decision period");
        }
      catch(...) { gzclose(stream);stream=nullptr;throw; }
      }
    ~Episode() { if(stream!=nullptr) gzclose(stream); }
    Episode(const Episode&)=delete;
    Episode& operator=(const Episode&)=delete;
    uint32_t formatVersion() const { return version; }

    bool next(std::vector<uint8_t>& bytes) {
      bytes.resize(NavDemoWriter::recordBytesForVersion(version));
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
    uint32_t version=0;
  };

struct Totals final {
  uint64_t records=0,mismatches=0,previousMismatches=0,backward=0,strafe=0,episodes=0;

  void json() const {
    std::cout<<"{\"contract\":\"navmemoryteacher\",\"records\":"<<records
             <<",\"decisions\":"<<records<<",\"matches\":"<<(records-mismatches)
             <<",\"mismatches\":"<<mismatches<<",\"previous_mismatches\":"<<previousMismatches
             <<",\"backward\":"<<backward<<",\"strafe\":"<<strafe<<",\"episodes\":"<<episodes<<"}\n";
    }
  };

void replay(const std::string& path,uint64_t limit,Totals& totals) {
  Episode episode(path);
  NavMemoryTeacher teacher;teacher.reset();
  Observation observation;
  std::vector<uint8_t> bytes;
  NpcKeys::KeyState last;
  uint64_t localRecords=0;
  ++totals.episodes;
  while(totals.records<limit && episode.next(bytes)) {
    observation.decode(bytes,episode.formatVersion());
    const auto actual=teacher.decide(observation.input);
    const bool previousMismatch=localRecords>0 && !equal(observation.input.previous,last);
    const bool mismatch=previousMismatch || !equal(actual,observation.expected);
    if(previousMismatch) ++totals.previousMismatches;
    if(mismatch) {
      ++totals.mismatches;
      if(totals.mismatches<=5)
        std::cerr<<path<<": decision "<<localRecords<<": "
                 <<(previousMismatch ? "previous keys differ" : "teacher keys differ")<<'\n';
      }
    totals.backward+=actual.forward<0;
    totals.strafe+=actual.strafe!=0;
    ++totals.records;++localRecords;
    last=actual;
    }
  }

NavMemoryTeacher::Input target(float right,float forward,float distance=600.f) {
  NavMemoryTeacher::Input input;
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

void hazard(NavMemoryTeacher::Input& input,int right,int forward,size_t channel=2) {
  require(right>=0 && right<64 && forward>=0 && forward<64,"invalid synthetic cell");
  auto& cell=input.near[size_t(forward)*64+size_t(right)];
  cell.channels[0]=255;cell.channels[channel]=255;
  }

float segmentDistance(float x,float y,const NavMemoryTeacher::Point& a,const NavMemoryTeacher::Point& b) {
  const float dx=b.rightCm-a.rightCm,dy=b.forwardCm-a.forwardCm;
  const float length=dx*dx+dy*dy;
  const float t=length>0 ? std::clamp(((x-a.rightCm)*dx+(y-a.forwardCm)*dy)/length,0.f,1.f) : 0.f;
  return std::hypot(x-a.rightCm-t*dx,y-a.forwardCm-t*dy);
  }

float planDistance(const std::vector<NavMemoryTeacher::Point>& plan,float right,float forward) {
  require(!plan.empty(),"synthetic plan is empty");
  NavMemoryTeacher::Point previous;
  float result=std::numeric_limits<float>::infinity();
  for(const auto& point:plan) {
    result=std::min(result,segmentDistance(right,forward,previous,point));
    previous=point;
    }
  return result;
  }

void noNativeAction(const NpcKeys::KeyState& keys) {
  require(!keys.jump && !keys.climb,"memory teacher must never jump or climb");
  require(std::isfinite(keys.forward) && std::isfinite(keys.strafe) && std::isfinite(keys.turn),
          "memory teacher produced a non-finite key");
  require(std::abs(keys.forward)<=1 && std::abs(keys.strafe)<=1 && std::abs(keys.turn)<=1,
          "memory teacher produced an invalid locomotion axis");
  }

void selfTest() {
  unsetenv("OPENGOTHIC_NAV_FAMILIAR_MAP");
  unsigned tests=0;
  NavMemoryTeacher teacher;
  auto input=target(0,1);
  auto keys=teacher.decide(input);
  require(keys.forward>0 && !teacher.plan().empty(),"unknown ground must admit an optimistic forward plan");
  noNativeAction(keys);++tests;

  // The optimistic route is allowed, but remembered free ground must be cheaper.
  input=target(0,1);
  for(auto& cell:input.near) { cell.channels[0]=255;cell.channels[1]=255; }
  for(int z=36;z<52;++z)
    for(int x=29;x<35;++x) input.near[size_t(z)*64+size_t(x)].channels={};
  teacher.reset();keys=teacher.decide(input);
  float knownDetour=0;
  for(const auto& point:teacher.plan()) knownDetour=std::max(knownDetour,std::abs(point.rightCm));
  require(knownDetour>=75.f,"unknown ground must cost more than a small remembered-free detour");
  noNativeAction(keys);++tests;

  input=target(0,1);
  for(int x=24;x<40;++x) hazard(input,x,43);
  teacher.reset();keys=teacher.decide(input);
  float widest=0;
  for(const auto& point:teacher.plan()) widest=std::max(widest,std::abs(point.rightCm));
  require(widest>=200.f,"memory wall must produce a lateral detour");
  for(int x=24;x<40;++x)
    require(planDistance(teacher.plan(),(float(x)+.5f-32)*25,287.5f)>25.f,
            "wall detour cuts through the body clearance margin");
  noNativeAction(keys);++tests;

  for(size_t channel=2;channel<7;++channel) {
    input=target(0,1);hazard(input,32,40,channel);
    teacher.reset();keys=teacher.decide(input);
    require(planDistance(teacher.plan(),12.5f,212.5f)>35.f,
            "danger channel was ignored or not dilated for body radius");
    noNativeAction(keys);++tests;
    }

  // With no opt-in, static familiar evidence is exactly unknown to the teacher,
  // while the unmodified learner input retains every familiar channel.
  for(const char* setting:{static_cast<const char*>(nullptr),"0"}) {
    if(setting) setenv("OPENGOTHIC_NAV_FAMILIAR_MAP",setting,1);
    else unsetenv("OPENGOTHIC_NAV_FAMILIAR_MAP");
    input=target(100,2100,std::hypot(100.f,2100.f));
    for(int x=24;x<40;++x) hazard(input,x,43);
    auto journey=input;
    for(size_t i=0;i<input.far.size();++i) {
      auto& tile=input.far[i];
      tile.channels[0]=89;tile.channels[1]=89;tile.familiar=255;
      if(i%2==0) tile.channels[2+i%4]=89;
      }
    const auto baseline=NavMemoryTeacher::probe(journey,NavMemoryTeacher::DiagnosticsOptions{});
    const auto ignored=NavMemoryTeacher::probe(input,NavMemoryTeacher::DiagnosticsOptions{});
    const auto samePath=[](const auto& a,const auto& b) {
      return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const auto& x,const auto& y) {
        return x.rightCm==y.rightCm && x.forwardCm==y.forwardCm;
        });
      };
    require(ignored.planCost==baseline.planCost && ignored.planLengthCm==baseline.planLengthCm &&
            ignored.actualTargetReached==baseline.actualTargetReached &&
            ignored.blockedFraction==baseline.blockedFraction &&
            ignored.knownFreeFraction==baseline.knownFreeFraction && samePath(ignored.path,baseline.path),
            "disabled familiar evidence differs from journey unknown ground");
    teacher.reset();NavMemoryTeacher journeyTeacher;
    for(unsigned tick=0;tick<3;++tick) {
      keys=teacher.decide(input);
      require(equal(keys,journeyTeacher.decide(journey)) && samePath(teacher.plan(),journeyTeacher.plan()),
              "disabled familiar evidence changed production actions or route");
      input.previous=keys;journey.previous=keys;
      }
    require(std::all_of(input.far.begin(),input.far.end(),[](const auto& tile) {
      return tile.familiar==255 && tile.channels[0]==89 && tile.channels[1]==89;
      }),"teacher fallback changed the learner familiar input");
    input.far[60*100+50].channels[6]=1;
    require(!NavMemoryTeacher::probe(input,NavMemoryTeacher::DiagnosticsOptions{}).targetClear,
            "disabled familiar static evidence hid live movable danger");
    ++tests;
    }

  // Familiar static hazards are an explicitly enabled soft prior, while live evidence
  // (even at one quantization unit) and live movable evidence retain a veto.
  setenv("OPENGOTHIC_NAV_FAMILIAR_MAP","1",1);
  for(size_t channel=2;channel<7;++channel) {
    input=target(100,2100,std::hypot(100.f,2100.f));
    auto& tile=input.far[60*100+50];
    tile.channels[0]=89;tile.channels[1]=89;tile.channels[channel]=89;tile.familiar=255;
    auto soft=NavMemoryTeacher::probe(input,NavMemoryTeacher::DiagnosticsOptions{});
    require(soft.targetClear==(channel!=6),"familiar static danger blocked or live movable danger was ignored");
    if(channel!=6) require(soft.actualTargetReached,"familiar danger must remain traversable");
    tile.familiar=0;tile.channels[channel]=1;
    const auto live=NavMemoryTeacher::probe(input,NavMemoryTeacher::DiagnosticsOptions{});
    require(!live.targetClear,"live danger must remain a hard veto regardless of confidence");
    ++tests;
    }

  input=target(100,2100,std::hypot(100.f,2100.f));
  const auto unknown=NavMemoryTeacher::probe(input,NavMemoryTeacher::DiagnosticsOptions{});
  for(auto& tile:input.far) { tile.channels[0]=89;tile.channels[1]=89;tile.familiar=255; }
  const auto familiarFree=NavMemoryTeacher::probe(input,NavMemoryTeacher::DiagnosticsOptions{});
  for(auto& tile:input.far) tile.channels[2]=89;
  const auto familiarHazard=NavMemoryTeacher::probe(input,NavMemoryTeacher::DiagnosticsOptions{});
  require(familiarFree.actualTargetReached && familiarHazard.actualTargetReached &&
          familiarFree.planCost<unknown.planCost && familiarHazard.planCost>unknown.planCost,
          "familiar free / unknown / familiar hazard costs are not ordered");
  ++tests;
  unsetenv("OPENGOTHIC_NAV_FAMILIAR_MAP");

  // Sparse visible water at the shore cannot borrow the occupied-start wall
  // exemption or leave a one-cell slit for the next physical walking step.
  for(const char* setting:{"0","1"}) {
    setenv("OPENGOTHIC_NAV_FAMILIAR_MAP",setting,1);
    input=target(0,1);hazard(input,32,33,5);
    teacher.reset();keys=teacher.decide(input);
    require(keys.forward==0 && keys.strafe==0,"shore water margin must stop movement before entry");
    noNativeAction(keys);++tests;
    input=target(0,1);hazard(input,32,32,5);
    teacher.reset();keys=teacher.decide(input);
    require(keys.forward==0 && keys.strafe==0,"raw water must not use an occupied-start escape");
    noNativeAction(keys);++tests;
    }
  unsetenv("OPENGOTHIC_NAV_FAMILIAR_MAP");

  // A hazard touching the current footprint must retain raw occupancy but
  // must not inflate back over an otherwise raw-free body origin.
  input=target(0,1);hazard(input,31,32);
  teacher.reset();keys=teacher.decide(input);
  auto diagnostic=NavMemoryTeacher::probe(input,NavMemoryTeacher::DiagnosticsOptions{});
  require(diagnostic.startClear && diagnostic.startFound && !diagnostic.startEscapeRequired &&
          diagnostic.overlapDangerCells==1,"a footprint neighbour was inflated over the raw-free body origin");
  require(teacher.lastPlanStatus().startFound && !teacher.lastPlanStatus().escapeRequired,
          "production start status differs from the footprint diagnostic");
  noNativeAction(keys);++tests;

  // An occupied raw start is not teleported to a graph node: the FIRST keys
  // physically retreat toward that nearby free node before distant steering.
  input=target(0,1);hazard(input,32,32);
  teacher.reset();keys=teacher.decide(input);
  const auto backwardExit=teacher.lastPlanStatus();
  require(backwardExit.startFound && backwardExit.escapeRequired && keys.forward<0 &&
          keys.strafe==0 && keys.turn==0 && std::hypot(backwardExit.escapeTarget.rightCm,
          backwardExit.escapeTarget.forwardCm)<=100,"raw occupied start did not first step backwards to a nearby free exit");
  noNativeAction(keys);
  input=target(0,1);
  for(int z=0;z<64;++z)for(int x=0;x<33;++x) hazard(input,x,z);
  teacher.reset();keys=teacher.decide(input);
  require(teacher.lastPlanStatus().startFound && teacher.lastPlanStatus().escapeRequired &&
          keys.strafe>0 && keys.forward==0 && keys.turn==0,
          "a right-only raw-start exit did not first apply an actual side step");
  noNativeAction(keys);++tests;

  // Free cells behind a further raw wall are not an escape from the body
  // footprint. This rejects graph-start teleporting through arbitrary walls.
  input=target(0,1);
  for(int z=0;z<64;++z)for(int x=0;x<64;++x) hazard(input,x,z);
  for(int z=31;z<=33;++z)for(int x=34;x<=36;++x) input.near[size_t(z)*64+size_t(x)]={};
  teacher.reset();keys=teacher.decide(input);
  require(!teacher.lastPlanStatus().startFound && teacher.lastPlanStatus().escapeRequired &&
          keys.forward==0 && keys.strafe==0 && keys.turn==0,
          "raw-start escape tunneled through danger outside the initial body footprint");
  noNativeAction(keys);++tests;

  // Staggered 75-cm openings retain a 35-cm diagonal throat: the 15-cm
  // margin passes it, while 30-cm inflation legitimately closes that throat.
  input=target(0,1);
  for(int x=0;x<64;++x) {
    if(x<31 || x>33) hazard(input,x,40);
    if(x<32 || x>34) hazard(input,x,41);
    }
  NavMemoryTeacher::DiagnosticsOptions narrow;narrow.nearOnly=true;
  const auto pass15=NavMemoryTeacher::probe(input,narrow);
  narrow.bodyRadiusCm=30;
  const auto pass30=NavMemoryTeacher::probe(input,narrow);
  require(pass15.actualTargetReached && !pass30.planComplete,
          "15-cm planning failed the narrow diagonal opening or 30-cm inflation did not close it");
  ++tests;

  input=target(-1,0);
  input.roads.carrotDirection={1,0,0};input.roads.carrotDistanceCm=600;
  input.roads.beyondKnown=false;
  teacher.reset();keys=teacher.decide(input);
  require(!teacher.plan().empty() && teacher.plan().back().rightCm>0,"known roads must guide toward the carrot");
  input.roads.beyondKnown=true;
  teacher.reset();keys=teacher.decide(input);
  require(!teacher.plan().empty() && teacher.plan().back().rightCm<0,"beyond-known plan must use the goal");
  noNativeAction(keys);++tests;

  input=target(0,-1);teacher.reset();keys=teacher.decide(input);
  require(keys.forward<0,"a plan behind the body must demonstrate a backwards step");
  noNativeAction(keys);++tests;

  bool reverse=false,sideways=false;
  input=target(0,1);input.previous.forward=1;teacher.reset();
  for(unsigned tick=0;tick<40;++tick) {
    keys=teacher.decide(input);noNativeAction(keys);
    reverse|=keys.forward<0;sideways|=keys.strafe!=0;
    input.previous=keys;
    }
  require(reverse && sideways,"observed immobility under movement must trigger backwards and sideways recovery");
  input=target(0,1);input.body.velocityCmS={0,0,100};input.previous.forward=1;teacher.reset();
  for(unsigned tick=0;tick<40;++tick) {
    keys=teacher.decide(input);noNativeAction(keys);
    require(keys.forward>=0,"healthy forward motion was mistaken for a stuck body");
    input.previous=keys;
    }
  ++tests;

  // There is no goal-kind input: movement is inferred from the changing
  // relative target after accounting for the learner's own body movement.
  input=target(0,1,600);teacher.reset();
  keys=teacher.decide(input);input.previous=keys;
  for(unsigned tick=1;tick<=5;++tick) {
    input.roads.goalDistanceCm=600+float(tick)*20;
    input.roads.carrotDistanceCm=input.roads.goalDistanceCm;
    keys=teacher.decide(input);noNativeAction(keys);
    require(keys.forward>0 && !keys.walk,"a receding target beyond four metres must be caught at a run");
    input.previous=keys;
    }
  ++tests;

  input=target(0,1,390);teacher.reset();
  keys=teacher.decide(input);input.previous=keys;
  float previousDistance=390;
  const std::array<float,9> distances={410,390,350,310,290,310,350,390,410};
  const std::array<bool,9> walking={false,false,false,false,true,true,true,true,false};
  for(size_t i=0;i<distances.size();++i) {
    // A target walking at 100 cm/s; changes in separation also contain the
    // learner's own translation. Gait must not confuse the two velocities.
    input.body.velocityCmS={0,0,100.f-(distances[i]-previousDistance)*10.f};
    input.roads.goalDistanceCm=distances[i];input.roads.carrotDistanceCm=distances[i];
    keys=teacher.decide(input);noNativeAction(keys);
    require(keys.walk==walking[i],"moving target gait did not retain the recorded walk/run hysteresis");
    input.previous=keys;previousDistance=distances[i];
    }
  ++tests;

  input=target(0,1,320);teacher.reset();
  keys=teacher.decide(input);input.previous=keys;
  input.roads.goalDistanceCm=350;input.roads.carrotDistanceCm=350;
  keys=teacher.decide(input);noNativeAction(keys);
  require(!keys.walk,"an observed running target inside four metres must be matched at a run");
  input.previous=keys;input.roads.goalDistanceCm=280;input.roads.carrotDistanceCm=280;
  keys=teacher.decide(input);noNativeAction(keys);
  require(keys.walk,"a running target must not retain running below three metres");
  teacher.reset();input=target(0,1,350);
  keys=teacher.decide(input);noNativeAction(keys);
  require(keys.walk,"an episode reset retained a previous target's running observation");
  ++tests;

  input=target(0,1,230);input.roads.beyondKnown=false;input.roads.carrotDistanceCm=0;teacher.reset();
  keys=teacher.decide(input);input.previous=keys;
  input.roads.goalDistanceCm=250;
  keys=teacher.decide(input);noNativeAction(keys);
  require(keys.forward==0 && keys.strafe==0 && keys.turn==0,"a nearby moving target must retain following distance");
  input.previous=keys;
  for(unsigned tick=1;tick<=6;++tick) {
    keys=teacher.decide(input);noNativeAction(keys);
    require(keys.forward==0 && keys.strafe==0,"a reached follow carrot must remain stopped when the target pauses");
    input.previous=keys;
    }
  ++tests;

  input=target(0,1,210);input.roads.beyondKnown=false;input.roads.carrotDistanceCm=0;teacher.reset();
  keys=teacher.decide(input);input.previous=keys;
  input.roads.goalDistanceCm=190;
  keys=teacher.decide(input);noNativeAction(keys);
  require(keys.forward<0 || keys.strafe!=0,"a moving target approaching inside two metres must trigger a safe retreat");
  ++tests;

  input=target(2,1,210);input.roads.beyondKnown=false;input.roads.carrotDistanceCm=0;
  // Both target components point forward/right. A left-side remembered wall
  // blocks the first away axis, while straight backwards remains clear.
  for(int z=30;z<33;++z)hazard(input,29,z);
  teacher.reset();keys=teacher.decide(input);input.previous=keys;
  input.roads.goalDistanceCm=190;
  keys=teacher.decide(input);noNativeAction(keys);
  require(keys.forward<0 && keys.strafe==0,"following retreat did not try a clear alternate away axis");
  ++tests;

  input=target(0,1,230);teacher.reset();
  keys=teacher.decide(input);input.previous=keys;
  input.roads.goalDistanceCm=250;input.roads.carrotDistanceCm=250;
  keys=teacher.decide(input);noNativeAction(keys);
  require(keys.forward>0,"approaching a nearby moving target must not inherit following stand-off");
  ++tests;

  input=target(0,1,600);input.body.velocityCmS={0,0,100};teacher.reset();
  for(unsigned tick=0;tick<12;++tick) {
    input.roads.goalDistanceCm=600-float(tick)*10;
    input.roads.carrotDistanceCm=input.roads.goalDistanceCm;
    keys=teacher.decide(input);noNativeAction(keys);
    require(keys.forward>0 && keys.walk,"coherent own translation must not turn a stationary target into a moving one");
    input.previous=keys;
    }
  ++tests;

  std::vector<NavMemoryTeacher::Input> inputs;
  std::vector<NpcKeys::KeyState> expected;
  input=target(0,1);teacher.reset();
  for(unsigned tick=0;tick<32;++tick) {
    if(tick==10) hazard(input,32,40);
    if(tick==20) input.roads.goalDirection={-1,0,0};
    inputs.push_back(input);
    keys=teacher.decide(input);expected.push_back(keys);noNativeAction(keys);
    input.previous=keys;
    }
  teacher.reset();
  NavMemoryTeacher fresh;
  for(size_t i=0;i<inputs.size();++i) {
    require(equal(teacher.decide(inputs[i]),expected[i]),"reset did not reproduce the full decision stream");
    require(equal(fresh.decide(inputs[i]),expected[i]),"fresh teacher shares hidden episode state");
    }
  ++tests;

  NavigationMemory::View memory;
  memory.near.size=64;memory.near.cellCm=25;memory.near.cells.resize(4096);
  memory.far.size=100;memory.far.cellCm=200;memory.far.cells.resize(10000);
  memory.near.cells[40*64+32].wall=1;
  memory.near.cells[40*64+33].free=.5f;
  memory.near.cells[40*64+33].groundHeightCm=123.25f;
  memory.far.cells[60*100+50].water=1;
  memory.far.cells[60*100+50].familiar=1;
  memory.far.cells[60*100+51].movable=.25f;
  input=target(0,1);
  const auto converted=NavMemoryTeacher::Input::fromLearner(input.body,input.roads,input.previous,memory);
  require(converted.near[40*64+32].channels[2]==255 && converted.near[40*64+33].channels[2]==0 &&
          converted.near[40*64+33].channels[1]==128,"adjacent full-resolution near cells were merged or quantized differently");
  require(converted.far[60*100+50].channels[5]==255 && converted.far[60*100+50].familiar==255 &&
          converted.far[60*100+51].familiar==0 && converted.far[60*100+51].channels[5]==0 &&
          converted.far[60*100+51].channels[6]==64,"adjacent full-resolution far cells were merged or quantized differently");
  require(converted.near[40*64+33].groundHeightCm==123.25f && converted.near[40*64+33].heightValid==1 &&
          converted.near[0].groundHeightCm==0 && converted.near[0].heightValid==0,"memory height encoding differs from NAVDEMO");
  memory.pose.position={99999,-12345,67890};memory.pose.forward={0,0,-1};
  memory.pose.gameMs=123456789;memory.traveledCm=987654;
  const auto moved=NavMemoryTeacher::Input::fromLearner(input.body,input.roads,input.previous,memory);
  teacher.reset();fresh.reset();
  require(equal(teacher.decide(converted),fresh.decide(moved)),"world memory annotations leaked into a teacher decision");
  ++tests;

  input=target(0,1,5000);
  auto& farWall=input.far[60*100+50];
  farWall.channels[0]=255;farWall.channels[2]=255;
  teacher.reset();keys=teacher.decide(input);
  require(planDistance(teacher.plan(),100,2100)>100.f,"a far-only wall must affect the full-resolution plan");
  noNativeAction(keys);++tests;

  std::cout<<"{\"contract\":\"navmemoryteacher\",\"self_tests\":"<<tests<<",\"mismatches\":0}\n";
  }
}

int main(int argc,char** argv) {
  try {
    if(argc==2 && std::string(argv[1])=="--self-test") {
      selfTest();return 0;
      }
    uint64_t limit=std::numeric_limits<uint64_t>::max();
    std::vector<std::string> paths;
    for(int i=1;i<argc;++i) {
      const std::string argument=argv[i];
      if(argument=="--limit") {
        require(i+1<argc,"missing --limit value");
        const std::string value=argv[++i];
        size_t consumed=0;
        require(!value.empty() && value.front()!='-',"--limit must be a positive integer");
        limit=std::stoull(value,&consumed);
        require(consumed==value.size() && limit>0,"--limit must be a positive integer");
        }
      else {
        require(argument.empty() || argument.front()!='-',"unknown option "+argument);
        paths.push_back(argument);
        }
      }
    if(paths.empty()) {
      std::cerr<<"usage: gothic-navmemoryteacher-contract --self-test\n"
               <<"       gothic-navmemoryteacher-contract [--limit N] samples.bin.gz [other files...]\n";
      return 2;
      }
    Totals totals;
    for(const auto& path:paths) {
      if(totals.records>=limit) break;
      replay(path,limit,totals);
      }
    require(totals.records>0,"no recorded teacher decisions");
    totals.json();
    return totals.mismatches==0 ? 0 : 1;
    }
  catch(const std::exception& error) {
    std::cerr<<"navmemoryteacher contract: "<<error.what()<<'\n';
    return 1;
    }
  }
