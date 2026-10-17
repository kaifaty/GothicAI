#include "navpolicy.h"

#include <rapidjson/document.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::array<size_t,6> classes={3,3,3,2,2,2};
constexpr std::array<const char*,6> heads={"forward","strafe","turn","walk","jump","climb"};

void require(bool condition,const std::string& message) {
  if(!condition) throw std::runtime_error(message);
  }

// The fixture carries the actual packed NAVDEMO bytes, rather than a second
// hand-written preprocessing path. This reader only restores the live structs.
struct Cursor final {
  const uint8_t* current;
  const uint8_t* end;

  explicit Cursor(const std::vector<uint8_t>& bytes):current(bytes.data()),end(bytes.data()+bytes.size()) {}
  uint8_t u8() {
    require(current<end,"short fixture value");
    return *current++;
    }
  uint32_t u32() {
    uint32_t value=0;
    for(unsigned i=0;i<4;++i) value|=uint32_t(u8())<<(8*i);
    return value;
    }
  uint64_t u64() {
    uint64_t value=0;
    for(unsigned i=0;i<8;++i) value|=uint64_t(u8())<<(8*i);
    return value;
    }
  float f32() {
    static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
    const auto bits=u32();
    float value=0;
    std::memcpy(&value,&bits,sizeof(value));
    require(std::isfinite(value),"non-finite fixture value");
    return value;
    }
  std::array<float,3> vec() { return {f32(),f32(),f32()}; }
  NpcKeys::KeyState keys() {
    NpcKeys::KeyState value;
    value.forward=f32();value.strafe=f32();value.turn=f32();
    value.headYaw=f32();value.headPitch=f32();
    value.walk=u8()!=0;value.jump=u8()!=0;value.climb=u8()!=0;
    return value;
    }
  };

struct Observation final {
  NavDemoWriter::Snapshot snapshot;
  std::vector<NavigationEye::Ray> eye;
  NavigationMemory::View memory;

  void decode(const std::vector<uint8_t>& bytes,const std::vector<std::array<float,3>>& layout) {
    Cursor in(bytes);
    snapshot.gameMs=in.u64();snapshot.position=in.vec();snapshot.bodyYawDeg=in.f32();
    snapshot.body.velocityCmS=in.vec();
    for(auto& flag:snapshot.body.flags) flag=in.u8();
    snapshot.body.groundNormal=in.vec();snapshot.body.groundSlopeDeg=in.f32();
    snapshot.body.headYawDeg=in.f32();snapshot.body.headPitchDeg=in.f32();
    snapshot.roads.nextDirection=in.vec();snapshot.roads.nextDistanceCm=in.f32();
    snapshot.roads.goalDirection=in.vec();snapshot.roads.goalDistanceCm=in.f32();
    snapshot.roads.remainingVisualCm=in.f32();snapshot.roads.goalInKnown=in.u8()!=0;
    snapshot.previous=in.keys();snapshot.keys=in.keys();
    eye.resize(NavDemoWriter::rayCount);
    for(size_t i=0;i<eye.size();++i) {
      auto& ray=eye[i];
      ray.yawDeg=layout[i][0];ray.pitchDeg=layout[i][1];ray.maxRangeCm=layout[i][2];
      ray.distanceCm=in.f32();ray.kind=NavigationEye::HitKind(in.u8());ray.normalY=in.f32();
      const auto age=in.u32(),nearAge=in.u32();
      require(age<=snapshot.gameMs && nearAge<=snapshot.gameMs,"invalid fixture ray age");
      ray.sampledAtMs=snapshot.gameMs-age;ray.nearSampledAtMs=snapshot.gameMs-nearAge;
      ray.complete=in.u8()!=0;ray.hit=in.u8()!=0;
      }
    layer(in,memory.near,NavDemoWriter::nearSize,NavigationMemory::Constants::nearCellCm);
    layer(in,memory.far,NavDemoWriter::farSize,NavigationMemory::Constants::farCellCm);
    snapshot.teacherCorner=in.vec();snapshot.teacherPathCm=in.f32();
    require(in.current==in.end,"NAVDEMO fixture layout mismatch");
    memory.pose.gameMs=snapshot.gameMs;
    snapshot.eye=&eye;snapshot.memory=&memory;
    }

  static void layer(Cursor& in,NavigationMemory::Layer& value,size_t size,float cellCm) {
    value.size=size;value.cellCm=cellCm;value.cells.resize(size*size);
    for(auto& cell:value.cells) {
      cell.confidence=float(in.u8())/255.f;cell.free=float(in.u8())/255.f;
      cell.wall=float(in.u8())/255.f;cell.ledge=float(in.u8())/255.f;
      cell.drop=float(in.u8())/255.f;cell.water=float(in.u8())/255.f;cell.movable=float(in.u8())/255.f;
      const float height=in.f32();
      cell.groundHeightCm=in.u8()!=0 ? height : std::numeric_limits<float>::quiet_NaN();
      }
    }
  };

struct Fixture final {
  gzFile stream=nullptr;
  size_t steps=0,hidden=0,readSteps=0;
  std::vector<std::array<float,3>> layout;
  std::string source;
  std::vector<uint8_t> raw;
  NavPolicy::Features expected;
  std::array<std::vector<float>,6> logits;
  std::vector<float> hiddenState;

  explicit Fixture(const std::string& path) {
    stream=gzopen(path.c_str(),"rb");
    require(stream!=nullptr,"cannot open fixture "+path);
    try {
      std::string line;
      for(int c=gzgetc(stream);c!='\n';c=gzgetc(stream)) {
        require(c!=-1 && line.size()<1024*1024,"missing bounded fixture header");
        line.push_back(char(c));
        }
      rapidjson::Document header;
      header.Parse(line.c_str());
      require(!header.HasParseError() && header.IsObject(),"invalid fixture header JSON");
      require(header.HasMember("format") && header["format"].IsString() &&
              std::string(header["format"].GetString())=="navpolicy-parity" &&
              header.HasMember("version") && header["version"].IsUint() && header["version"].GetUint()==1,
              "unsupported fixture format");
      auto number=[&](const char* name) {
        require(header.HasMember(name) && header[name].IsUint(),std::string("invalid fixture ")+name);
        return size_t(header[name].GetUint());
        };
      steps=number("steps");hidden=number("hidden_size");
      require(steps>0 && steps<=100000 && hidden>0 && hidden<=4096,"fixture dimensions out of bounds");
      const auto vectorSize=number("vector_size");
      require(number("record_bytes")==NavDemoWriter::recordBytes &&
              (vectorSize==NavPolicy::vectorSize || vectorSize==NavPolicy::legacyVectorSize),
              "fixture input dimensions differ from NAVDEMO");
      expected.vector.resize(vectorSize);
      require(header.HasMember("carry_hidden") && header["carry_hidden"].IsTrue(),"fixture must carry GRU state");
      require(header.HasMember("heads") && header["heads"].IsArray() && header["heads"].Size()==heads.size(),
              "fixture head count mismatch");
      require(header.HasMember("classes") && header["classes"].IsArray() && header["classes"].Size()==classes.size(),
              "fixture class count mismatch");
      for(size_t i=0;i<heads.size();++i) {
        require(header["heads"][rapidjson::SizeType(i)].IsString() &&
                std::string(header["heads"][rapidjson::SizeType(i)].GetString())==heads[i],"fixture head order mismatch");
        require(header["classes"][rapidjson::SizeType(i)].IsUint() &&
                header["classes"][rapidjson::SizeType(i)].GetUint()==classes[i],"fixture head classes mismatch");
        logits[i].resize(classes[i]);
        }
      require(header.HasMember("eye_layout") && header["eye_layout"].IsArray() &&
              header["eye_layout"].Size()==NavDemoWriter::rayCount,"fixture eye layout mismatch");
      for(const auto& row:header["eye_layout"].GetArray()) {
        require(row.IsArray() && row.Size()==3,"fixture eye layout row mismatch");
        std::array<float,3> ray;
        for(size_t i=0;i<3;++i) {
          require(row[rapidjson::SizeType(i)].IsNumber(),"invalid fixture eye layout scalar");
          ray[i]=row[rapidjson::SizeType(i)].GetFloat();
          require(std::isfinite(ray[i]),"non-finite fixture eye layout");
          }
        require(ray[2]>0,"invalid fixture ray range");
        layout.push_back(ray);
        }
      if(header.HasMember("source") && header["source"].IsString()) source=header["source"].GetString();
      raw.resize(NavDemoWriter::recordBytes);hiddenState.resize(hidden);
      }
    catch(...) { gzclose(stream);stream=nullptr;throw; }
    }
  ~Fixture() { if(stream) gzclose(stream); }
  Fixture(const Fixture&)=delete;
  Fixture& operator=(const Fixture&)=delete;

  void bytes(void* target,size_t size) {
    require(size<=std::numeric_limits<unsigned>::max(),"oversized fixture block");
    require(gzread(stream,target,unsigned(size))==int(size),"truncated fixture block");
    }
  template<class Values> void floats(Values& values) {
    std::vector<uint8_t> encoded(values.size()*4);
    bytes(encoded.data(),encoded.size());
    Cursor cursor(encoded);
    for(auto& value:values) value=cursor.f32();
    }
  bool next() {
    if(readSteps==steps) return false;
    bytes(raw.data(),raw.size());
    floats(expected.near);floats(expected.far);floats(expected.vector);
    for(auto& head:logits) floats(head);
    floats(hiddenState);
    ++readSteps;
    return true;
    }
  void finish() { require(gzgetc(stream)==-1 && gzeof(stream),"trailing or damaged fixture payload"); }
  };

template<class A,class B> double difference(const A& a,const B& b) {
  require(a.size()==b.size(),"inference result size mismatch");
  double maximum=0;
  for(size_t i=0;i<a.size();++i) {
    require(std::isfinite(a[i]) && std::isfinite(b[i]),"non-finite inference output");
    maximum=std::max(maximum,std::abs(double(a[i])-double(b[i])));
    }
  return maximum;
  }

void parity(const std::string& weights,const std::string& path) {
  const NavPolicy policy(weights);
  Fixture fixture(path);
  require(policy.hiddenSize()==fixture.hidden,"weight/fixture hidden size mismatch");
  require(policy.vectorInputSize()==fixture.expected.vector.size(),"weight/fixture vector size mismatch");
  NavPolicy::State state;
  Observation observation;
  double featureError=0,logitError=0,hiddenError=0;
  while(fixture.next()) {
    observation.decode(fixture.raw,fixture.layout);
    const auto actual=policy.encode(observation.snapshot);
    const double localFeature=std::max({difference(actual.near,fixture.expected.near),
        difference(actual.far,fixture.expected.far),difference(actual.vector,fixture.expected.vector)});
    featureError=std::max(featureError,localFeature);
    require(localFeature<=1e-6,"encoded features differ at step "+std::to_string(fixture.readSteps)+
            ": "+std::to_string(localFeature));
    const auto result=policy.step(actual,state);
    double localLogit=0;
    for(size_t h=0;h<heads.size();++h)
      localLogit=std::max(localLogit,difference(result.logits[h],fixture.logits[h]));
    logitError=std::max(logitError,localLogit);
    const double localHidden=difference(state.hidden,fixture.hiddenState);
    hiddenError=std::max(hiddenError,localHidden);
    require(localLogit<=1e-4,"logits differ at step "+std::to_string(fixture.readSteps)+": "+std::to_string(localLogit));
    require(localHidden<=1e-4,"hidden state differs at step "+std::to_string(fixture.readSteps)+": "+std::to_string(localHidden));
    if(fixture.readSteps==1) {
      // The raw record has privileged annotations, but encode must ignore them.
      observation.snapshot.position={99999,-77777,33333};observation.snapshot.bodyYawDeg=-177;
      observation.snapshot.gameMs+=9876;
      observation.snapshot.keys.forward=12345;observation.snapshot.keys.strafe=-12345;
      observation.snapshot.keys.turn=999;observation.snapshot.keys.headYaw=888;observation.snapshot.keys.headPitch=-777;
      observation.snapshot.keys.walk=!observation.snapshot.keys.walk;
      observation.snapshot.keys.jump=!observation.snapshot.keys.jump;
      observation.snapshot.keys.climb=!observation.snapshot.keys.climb;
      if(policy.vectorInputSize()==NavPolicy::vectorSize) {
        observation.snapshot.previous.forward=std::numeric_limits<float>::quiet_NaN();
        observation.snapshot.previous.strafe=12345;observation.snapshot.previous.turn=-12345;
        observation.snapshot.previous.headYaw=999;observation.snapshot.previous.headPitch=-999;
        observation.snapshot.previous.walk=!observation.snapshot.previous.walk;
        observation.snapshot.previous.jump=!observation.snapshot.previous.jump;
        observation.snapshot.previous.climb=!observation.snapshot.previous.climb;
        }
      observation.memory.pose.position={12345,-54321,99999};
      observation.memory.pose.forward={0,0,-1};observation.memory.pose.gameMs+=5432;
      observation.memory.traveledCm=987654;
      observation.snapshot.teacherCorner={-99,88,77};
      observation.snapshot.teacherPathCm=88888;
      const auto other=policy.encode(observation.snapshot);
      require(difference(actual.near,other.near)==0 && difference(actual.far,other.far)==0 &&
              difference(actual.vector,other.vector)==0,"privileged annotation leaked into features");
      NavPolicy::State fresh;
      const auto reset=policy.step(observation.snapshot,fresh);
      for(size_t h=0;h<heads.size();++h)
        require(difference(result.logits[h],reset.logits[h])==0,"NPC state is shared across evaluations");
      }
    }
  fixture.finish();
  std::cout<<"{\"contract\":\"navpolicy\",\"steps\":"<<fixture.readSteps
           <<",\"carry_hidden\":true,\"max_feature_error\":"<<featureError
           <<",\"max_logit_error\":"<<logitError<<",\"max_hidden_error\":"<<hiddenError<<"}\n";
  }

double percentile(std::vector<double> values,double fraction) {
  require(!values.empty(),"no benchmark samples");
  std::sort(values.begin(),values.end());
  const double index=fraction*double(values.size()-1);
  const auto low=size_t(index),high=std::min(low+1,values.size()-1);
  return values[low]+(values[high]-values[low])*(index-double(low));
  }

void benchmark(const std::string& weights,const std::string& path,size_t npcs,size_t decisions) {
  require(npcs>0 && npcs<=1000 && decisions>=1000,"benchmark needs 1..1000 NPCs and >=1000 decisions");
  const NavPolicy policy(weights);
  Fixture fixture(path);
  std::vector<NavPolicy::Features> features;
  std::vector<std::vector<uint8_t>> records;
  Observation observation;
  for(size_t i=0;i<16 && fixture.next();++i) {
    observation.decode(fixture.raw,fixture.layout);
    features.push_back(policy.encode(observation.snapshot));
    records.push_back(fixture.raw);
    }
  require(!features.empty(),"empty benchmark fixture");
  std::vector<NavPolicy::State> states(npcs);
  double checksum=0;
  for(size_t tick=0;tick<30;++tick)
    for(size_t npc=0;npc<npcs;++npc)
      checksum+=policy.step(features[(tick*npcs+npc)%features.size()],states[npc]).logits[0][0];
  using Clock=std::chrono::steady_clock;
  const auto micros=[](Clock::time_point a,Clock::time_point b) {
    return std::chrono::duration<double,std::micro>(b-a).count();
    };
  std::vector<double> stepUs,tickUs,encodeUs;
  const size_t ticks=(decisions+npcs-1)/npcs;
  stepUs.reserve(ticks*npcs);tickUs.reserve(ticks);encodeUs.reserve(decisions);
  for(size_t tick=0;tick<ticks;++tick) {
    const auto begin=Clock::now();
    for(size_t npc=0;npc<npcs;++npc) {
      const auto& input=features[(tick*npcs+npc)%features.size()];
      const auto before=Clock::now();
      const auto result=policy.step(input,states[npc]);
      const auto after=Clock::now();
      stepUs.push_back(micros(before,after));
      checksum+=result.logits[0][0];
      }
    tickUs.push_back(micros(begin,Clock::now()));
    }
  // Decode and I/O are excluded; live sensor preprocessing is reported separately.
  for(size_t i=0;i<decisions;++i) {
    observation.decode(records[i%records.size()],fixture.layout);
    const auto before=Clock::now();
    const auto encoded=policy.encode(observation.snapshot);
    const auto after=Clock::now();
    encodeUs.push_back(micros(before,after));
    checksum+=encoded.vector[i%encoded.vector.size()];
    }
  std::cout<<"{\"benchmark\":\"navpolicy\",\"threads\":1,\"npcs\":"<<npcs
           <<",\"warmup_ticks\":30,\"decisions\":"<<stepUs.size()<<",\"ticks\":"<<ticks
           <<",\"decision_median_us\":"<<percentile(stepUs,.5)<<",\"decision_p95_us\":"<<percentile(stepUs,.95)
           <<",\"tick_median_us\":"<<percentile(tickUs,.5)<<",\"tick_p95_us\":"<<percentile(tickUs,.95)
           <<",\"encode_median_us\":"<<percentile(encodeUs,.5)<<",\"encode_p95_us\":"<<percentile(encodeUs,.95)
           <<",\"checksum\":"<<checksum<<"}\n";
  }
}

int main(int argc,char** argv) {
  try {
    if(argc==3) {
      parity(argv[1],argv[2]);
      return 0;
      }
    if(argc>=4 && std::string(argv[1])=="--benchmark") {
      size_t npcs=20,decisions=2000;
      for(int i=4;i<argc;i+=2) {
        require(i+1<argc,"missing benchmark option value");
        const std::string option=argv[i];
        if(option=="--npcs") npcs=std::stoul(argv[i+1]);
        else if(option=="--decisions") decisions=std::stoul(argv[i+1]);
        else throw std::runtime_error("unknown benchmark option "+option);
        }
      benchmark(argv[2],argv[3],npcs,decisions);
      return 0;
      }
    std::cerr<<"usage: gothic-navpolicy-contract WEIGHTS FIXTURE\n"
             <<"       gothic-navpolicy-contract --benchmark WEIGHTS FIXTURE [--npcs 20] [--decisions 2000]\n";
    return 2;
    }
  catch(const std::exception& error) {
    std::cerr<<"navpolicy contract: "<<error.what()<<'\n';
    return 1;
    }
  }
