#include "navpolicy.h"
#include "navlivetrace.h"

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition,const std::string& message) {
  if(!condition) throw std::runtime_error(message);
  }

void liveFormatContract() {
  struct Temporary final {
    std::filesystem::path path=std::filesystem::temp_directory_path()/
      ("gothic-nav-live-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Temporary() {std::error_code error;std::filesystem::remove_all(path,error);}
  } temporary;
  require(std::filesystem::create_directory(temporary.path),"cannot create live trace fixture directory");
  NavigationMemory::View memory;
  memory.near.size=64;memory.near.cellCm=25;memory.near.cells.resize(64*64);
  memory.far.size=100;memory.far.cellCm=200;memory.far.cells.resize(100*100);
  memory.near.cells[0].wall=.5f;memory.far.cells[0].familiar=1;
  std::vector<NavigationEye::Ray> rays(NavDemoWriter::rayCount);
  rays[0].point={100,200,300};rays[0].hit=true;rays[0].kind=NavigationEye::HitKind::World;
  NavDemoWriter::Snapshot sample;sample.gameMs=1000;sample.memory=&memory;sample.eye=&rays;
  NavMesh::Path path;path.complete=true;path.corners={{1,2,3},{4,5,6}};
  NavLiveTrace trace(temporary.path,nullptr,path,rays.size(),true,{},false,true);
  trace.observation(sample,nullptr,0,0);
  sample.gameMs=1100;trace.observation(sample,nullptr,100,1);
  sample.gameMs=1400;trace.observation(sample,nullptr,400,4);trace.finish();
  std::ifstream meta(temporary.path/"live-format.json");
  const std::string json{std::istreambuf_iterator<char>(meta),std::istreambuf_iterator<char>()};
  rapidjson::Document format;format.Parse(json.c_str());
  require(!format.HasParseError() && format["schema"].GetUint()==1,"invalid live trace metadata");
  require(format["maps"]["near"]["shape"][0].GetUint()==8 && format["maps"]["far"]["shape"][1].GetUint()==100,
          "teacher trace map shape mismatch");
  require(format["detour_cm"][0][0].GetFloat()==100,"live Detour path is not in centimetres");
  const auto& target=format["target_script"];
  require(std::string(target["mode"].GetString())=="player-walk" && target["walk_cm_s"].GetInt()==100 &&
          target["run_cm_s"].GetInt()==0 && target["run_leg_index"].IsNull() && target["run_leg_prefix_ms"].GetInt()==0,
          "live player-walk metadata must match the 100cm/s script without a run burst");
  const size_t recordBytes=16+(64*64+100*100)*8+rays.size()*55;
  std::vector<uint8_t> bytes(12+recordBytes*2+1);
  gzFile input=gzopen((temporary.path/"live-observations.bin.gz").string().c_str(),"rb");
  require(input!=nullptr,"cannot read live trace fixture");
  const int read=gzread(input,bytes.data(),unsigned(bytes.size()));
  const int closed=gzclose(input);
  require(read==int(bytes.size()-1) && closed==Z_OK,"live trace record count/size mismatch");
  require(std::memcmp(bytes.data(),"NAVLIVE\0",8)==0 && bytes[8]==1,"live trace header mismatch");
  require(bytes[12+16+2*64*64]==128,"live memory probability quantization mismatch");
  require(bytes[12+16+64*64*8+7*100*100]==255,"live familiar channel mismatch");
  require(bytes[12+recordBytes]==144 && bytes[13+recordBytes]==1,"live observation cadence mismatch");
  std::cout<<"{\"contract\":\"nav_live_format\",\"records\":2,\"ray_bytes\":55,\"record_bytes\":"<<recordBytes<<"}\n";
}

void memoryContract(const std::string& weights) {
  const NavPolicy original(weights);
  require(original.navDemoVersion()>=1 && original.navDemoVersion()<=3,"memory contract requires a supported policy");
  std::ifstream input(weights,std::ios::binary);
  std::string headerText;
  require(bool(std::getline(input,headerText)),"cannot read memory contract weight header");
  const std::string payload{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
  struct Temporary final {
    std::filesystem::path directory;
    bool created=false;
    ~Temporary() {if(created) {std::error_code error;std::filesystem::remove_all(directory,error);}}
  } temporary{std::filesystem::temp_directory_path()/
      ("gothic-navpolicy-memory-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))};
  require(std::filesystem::create_directory(temporary.directory),"cannot create memory contract temporary directory");
  temporary.created=true;
  const auto path=temporary.directory/"fixture.navbc";
  const auto variant=[&](const char* memoryVersion,uint32_t navdemoVersion=0) {
    rapidjson::Document header;header.Parse(headerText.c_str());
    auto& preprocessing=header["preprocessing"];
    preprocessing.RemoveMember("navigation_memory_version");
    if(memoryVersion) {
      rapidjson::Document value;value.Parse(memoryVersion);
      require(!value.HasParseError(),"invalid memory contract metadata literal");
      rapidjson::Value copied;copied.CopyFrom(value,header.GetAllocator());
      preprocessing.AddMember("navigation_memory_version",copied,header.GetAllocator());
    }
    if(navdemoVersion) {
      preprocessing.RemoveMember("navdemo_version");
      preprocessing.AddMember("navdemo_version",navdemoVersion,header.GetAllocator());
    }
    rapidjson::StringBuffer buffer;rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);header.Accept(writer);
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(buffer.GetString(),std::streamsize(buffer.GetSize()));out.put('\n');
    out.write(payload.data(),std::streamsize(payload.size()));out.close();
    require(bool(out),"cannot write memory contract weight variant");
  };

  using Memory=NavigationMemory;
  Memory::FamiliarMap familiar;
  Memory::FamiliarTile tile;tile.x=25;tile.z=0;tile.channels=Memory::FamiliarTile::Wall;
  familiar.tiles.push_back(tile);familiar.finish();
  NavigationEye::Ray rock;
  rock.origin={0,150,0};rock.point={5000,150,0};rock.direction={1,0,0};
  rock.distanceCm=rock.maxRangeCm=5000;rock.kind=NavigationEye::HitKind::World;rock.hit=rock.complete=true;
  const auto strongest=[](const Memory::View& view,float Memory::Cell::* field) {
    float result=0;for(const auto& cell:view.far.cells) result=std::max(result,cell.*field);return result;
  };
  const char* versions[]={nullptr,"3","4"};
  const char* overrides[]={nullptr,"0","1"};
  size_t cases=0;float legacyConfidence=0,journeyConfidence=0;
  for(const char* version:versions) {
    if(original.weightVersion()==4 && (!version || std::string(version)!="4")) {
      variant(version);bool rejected=false;
      try {const NavPolicy policy(path.string());}
      catch(const std::runtime_error& error) {rejected=std::string(error.what())=="nav policy: BC4 weights require navigation memory version 4";}
      require(rejected,"BC4 accepted missing or legacy memory metadata");
      continue;
    }
    variant(version);
    const NavPolicy policy(path.string());
    const uint32_t expectedVersion=version && std::string(version)=="4" ? 4u : 3u;
    require(policy.navigationMemoryVersion()==expectedVersion,"policy memory metadata selection differs");
    for(const char* overrideValue:overrides) {
      const bool journey=overrideValue ? std::string(overrideValue)=="1" : expectedVersion==4;
      Memory memory;Memory::View view;
      memory.setFamiliarMap(&familiar,{1});
      memory.update({},{{0,0,0},{1,0,0},0});memory.view(view);
      require(strongest(view,&Memory::Cell::familiar)==1,"memory contract familiar fixture is not active");
      policy.configureMemory(memory,overrideValue);
      require(memory.journeyMemoryEnabled()==journey && memory.representationVersion()==(journey?4u:3u),
              "policy memory configuration differs from metadata or override");
      require(!memory.familiarMapEnabled(),"policy configuration inherited familiar knowledge");
      memory.view(view);
      require(strongest(view,&Memory::Cell::familiar)==0 && strongest(view,&Memory::Cell::wall)==0,
              "configured policy sees an unseen familiar rock");
      memory.update({rock},{{0,0,0},{1,0,0},0});
      memory.update({},{{7500,0,0},{1,0,0},60000});
      memory.update({},{{0,0,0},{1,0,0},120000});memory.view(view);
      const float confidence=strongest(view,&Memory::Cell::wall);
      const float expected=float(std::exp(journey?-.9:-9.));
      require(std::abs(confidence-expected)<1.e-6f,
              "policy-configured rock decay differs after 120 seconds and 150 metres");
      (journey?journeyConfidence:legacyConfidence)=confidence;
      ++cases;
    }
  }
  for(const char* invalid:{"2","5","null","true","\"4\"","4.0"}) {
    variant(invalid);bool rejected=false;
    try {const NavPolicy policy(path.string());}
    catch(const std::runtime_error& error) {rejected=std::string(error.what())=="nav policy: unsupported navigation memory version";}
    require(rejected,"invalid navigation memory metadata was not rejected");
  }
  variant("4",original.weightVersion()==4 ? 2u : 3u);bool rejectedDemo=false;
  try {const NavPolicy policy(path.string());}
  catch(const std::runtime_error& error) {rejectedDemo=std::string(error.what())=="nav policy: unsupported NAVDEMO input version";}
  require(rejectedDemo,"unsupported policy NAVDEMO version was not rejected");
  variant("4");const NavPolicy policy(path.string());
  for(const char* invalid:{"","2","-1","true"}) {
    Memory memory;bool rejected=false;
    try {policy.configureMemory(memory,invalid);}
    catch(const std::runtime_error& error) {rejected=std::string(error.what())=="nav policy: OPENGOTHIC_NAV_JOURNEY_MEMORY must be 0 or 1";}
    require(rejected,"invalid journey override was not rejected");
  }
  std::cout<<"{\"contract\":\"navpolicy_memory\",\"navdemo_version\":"<<original.navDemoVersion()
           <<",\"weight_version\":"<<original.weightVersion()
           <<",\"configuration_cases\":"<<cases<<",\"invalid_metadata_cases\":6,\"invalid_override_cases\":4"
           <<",\"legacy_confidence\":"<<legacyConfidence<<",\"journey_confidence\":"<<journeyConfidence<<"}\n";
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

  void decode(const std::vector<uint8_t>& bytes,const std::vector<std::array<float,3>>& layout,uint32_t version) {
    Cursor in(bytes);
    snapshot.gameMs=in.u64();snapshot.position=in.vec();snapshot.bodyYawDeg=in.f32();
    snapshot.body.velocityCmS=in.vec();
    for(auto& flag:snapshot.body.flags) flag=in.u8();
    snapshot.body.groundNormal=in.vec();snapshot.body.groundSlopeDeg=in.f32();
    snapshot.body.headYawDeg=in.f32();snapshot.body.headPitchDeg=in.f32();
    if(version>=2) {
      snapshot.roads.carrotDirection=in.vec();snapshot.roads.carrotDistanceCm=in.f32();
      }
    else {
      snapshot.roads.nextDirection=in.vec();snapshot.roads.nextDistanceCm=in.f32();
      }
    snapshot.roads.goalDirection=in.vec();snapshot.roads.goalDistanceCm=in.f32();
    if(version>=2) {
      snapshot.roads.remainingKnownCm=in.f32();snapshot.roads.beyondKnown=in.u8()!=0;
      }
    else {
      snapshot.roads.remainingVisualCm=in.f32();snapshot.roads.goalInKnown=in.u8()!=0;
      }
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
    layer(in,memory.near,NavDemoWriter::nearSize,NavigationMemory::Constants::nearCellCm,version);
    layer(in,memory.far,NavDemoWriter::farSize,NavigationMemory::Constants::farCellCm,version);
    snapshot.teacherCorner=in.vec();snapshot.teacherPathCm=in.f32();
    require(in.current==in.end,"NAVDEMO fixture layout mismatch");
    memory.pose.gameMs=snapshot.gameMs;
    snapshot.eye=&eye;snapshot.memory=&memory;
    }

  static void layer(Cursor& in,NavigationMemory::Layer& value,size_t size,float cellCm,uint32_t version) {
    value.size=size;value.cellCm=cellCm;value.cells.resize(size*size);
    for(auto& cell:value.cells) {
      cell.confidence=float(in.u8())/255.f;cell.free=float(in.u8())/255.f;
      cell.wall=float(in.u8())/255.f;cell.ledge=float(in.u8())/255.f;
      cell.drop=float(in.u8())/255.f;cell.water=float(in.u8())/255.f;cell.movable=float(in.u8())/255.f;
      cell.familiar=version==3 ? float(in.u8())/255.f : 0.f;
      const float height=in.f32();
      cell.groundHeightCm=in.u8()!=0 ? height : std::numeric_limits<float>::quiet_NaN();
      }
    }
  };

struct Fixture final {
  gzFile stream=nullptr;
  size_t steps=0,hidden=0,readSteps=0;
  uint32_t demoVersion=1;
  uint32_t weightVersion=1;
  std::vector<std::string> heads;
  std::vector<size_t> classes;
  std::vector<double> temperatures;
  std::vector<std::array<float,3>> layout;
  std::string source;
  std::vector<uint8_t> raw;
  NavPolicy::Features expected;
  std::vector<std::vector<float>> logits;
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
      if(header.HasMember("navdemo_version")) demoVersion=uint32_t(number("navdemo_version"));
      if(header.HasMember("weight_version")) weightVersion=uint32_t(number("weight_version"));
      require(weightVersion>=1 && weightVersion<=4,"unsupported fixture weight version");
      if(weightVersion>=2) {
        heads={"forward","strafe","turn","walk","maneuver"};classes={3,3,3,2,3};
        }
      else {
        heads={"forward","strafe","turn","walk","jump","climb"};classes={3,3,3,2,2,2};
        }
      logits.resize(heads.size());temperatures.assign(heads.size(),1.);
      require(demoVersion>=1 && demoVersion<=3,"unsupported fixture NAVDEMO version");
      require(steps>0 && steps<=100000 && hidden>0 && hidden<=4096,"fixture dimensions out of bounds");
      const auto vectorSize=number("vector_size");
      require(number("record_bytes")==NavDemoWriter::recordBytesForVersion(demoVersion) &&
              (vectorSize==NavPolicy::vectorSize || vectorSize==NavPolicy::legacyVectorSize),
              "fixture input dimensions differ from NAVDEMO");
      if(weightVersion>=3) {
        expected.near.resize(weightVersion==4 ? NavPolicy::bc4NearSize : NavPolicy::hdNearSize);
        expected.far.resize(weightVersion==4 ? NavPolicy::bc4FarSize : NavPolicy::hdFarSize);
        }
      const auto shape=[&](const char* name,std::initializer_list<size_t> dimensions) {
        if(!header.HasMember(name)) return;
        const auto& value=header[name];
        require(value.IsArray() && value.Size()==dimensions.size(),std::string("fixture shape mismatch: ")+name);
        size_t index=0;
        for(const auto dimension:dimensions) {
          const auto& item=value[rapidjson::SizeType(index++)];
          require(item.IsUint() && item.GetUint()==dimension,std::string("fixture shape mismatch: ")+name);
          }
        };
      if(weightVersion>=3) {
        const size_t channels=weightVersion==4 ? 11 : 10;
        shape("near_shape",{channels,64,64});shape("far_shape",{channels,50,100});
        }
      else { shape("near_shape",{9,32,32});shape("far_shape",{9,25,25}); }
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
        if(header.HasMember("temperatures")) {
          const auto& values=header["temperatures"];
          require(values.IsObject() && values.HasMember(heads[i].c_str()) && values[heads[i].c_str()].IsNumber(),
                  "fixture temperatures mismatch");
          temperatures[i]=values[heads[i].c_str()].GetDouble();
          require(std::isfinite(temperatures[i]) && temperatures[i]>0,"invalid fixture temperature");
          }
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
      raw.resize(NavDemoWriter::recordBytesForVersion(demoVersion));hiddenState.resize(hidden);
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
  // NumPy's multi-axis 4x4 height reduction groups row sums; the live encoder
  // accumulates cells in row order. Float32 rounding can differ by a few ulps
  // (1.9e-6 on recorded relative heights). Inference remains checked at 1e-4.
  constexpr double featureTolerance=1e-5;
  const NavPolicy policy(weights);
  Fixture fixture(path);
  require(policy.hiddenSize()==fixture.hidden,"weight/fixture hidden size mismatch");
  require(policy.nearInputSize()==fixture.expected.near.size() &&
          policy.farInputSize()==fixture.expected.far.size(),"weight/fixture map size mismatch");
  require(policy.vectorInputSize()==fixture.expected.vector.size(),"weight/fixture vector size mismatch");
  require(policy.navDemoVersion()==fixture.demoVersion,"weight/fixture NAVDEMO version mismatch");
  require(policy.weightVersion()==fixture.weightVersion,"weight/fixture policy version mismatch");
  require(policy.actionHeadCount()==fixture.heads.size(),"weight/fixture head count mismatch");
  for(size_t i=0;i<fixture.heads.size();++i) {
    require(policy.headName(i)==fixture.heads[i] && policy.headSize(i)==fixture.classes[i],"weight/fixture head schema mismatch");
    require(policy.headTemperature(i)==fixture.temperatures[i],"weight/fixture temperature mismatch");
    }
  NavPolicy::State state;
  Observation observation;
  double featureError=0,logitError=0,hiddenError=0;
  while(fixture.next()) {
    observation.decode(fixture.raw,fixture.layout,fixture.demoVersion);
    const auto actual=policy.encode(observation.snapshot);
    const double localFeature=std::max({difference(actual.near,fixture.expected.near),
        difference(actual.far,fixture.expected.far),difference(actual.vector,fixture.expected.vector)});
    featureError=std::max(featureError,localFeature);
    if(localFeature>featureTolerance) {
      std::ostringstream message;
      message<<std::setprecision(10)<<"encoded features differ at step "<<fixture.readSteps
             <<": "<<localFeature;
      const auto locate=[&](const auto& actualValues,const auto& expectedValues,const char* name) {
        for(size_t i=0;i<actualValues.size();++i)
          if(std::abs(double(actualValues[i])-double(expectedValues[i]))==localFeature) {
            message<<" ("<<name<<'['<<i<<"] actual="<<actualValues[i]<<", expected="<<expectedValues[i]<<')';
            break;
            }
        };
      locate(actual.near,fixture.expected.near,"near");
      locate(actual.far,fixture.expected.far,"far");
      locate(actual.vector,fixture.expected.vector,"vector");
      throw std::runtime_error(message.str());
      }
    const auto result=policy.step(actual,state);
    double localLogit=0;
    for(size_t h=0;h<fixture.heads.size();++h)
      localLogit=std::max(localLogit,difference(result.logits[h],fixture.logits[h]));
    if(policy.weightVersion()>=2) {
      const auto& maneuver=result.logits[4];
      const auto action=size_t(std::max_element(maneuver.begin(),maneuver.end())-maneuver.begin());
      require(result.keys.jump==(action==1) && result.keys.climb==(action==2),"maneuver key decode mismatch");
      }
    logitError=std::max(logitError,localLogit);
    const double localHidden=difference(state.hidden,fixture.hiddenState);
    hiddenError=std::max(hiddenError,localHidden);
    require(localLogit<=1e-4,"logits differ at step "+std::to_string(fixture.readSteps)+": "+std::to_string(localLogit));
    require(localHidden<=1e-4,"hidden state differs at step "+std::to_string(fixture.readSteps)+": "+std::to_string(localHidden));
    if(fixture.readSteps==1) {
      // HD uses physical feet Y only as the local height origin. All policies
      // ignore absolute X/Z, yaw and the record's teacher annotations.
      const float feetY=observation.snapshot.position[1];
      observation.snapshot.position={99999,policy.weightVersion()>=3 ? feetY : -77777,33333};
      observation.snapshot.bodyYawDeg=-177;
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
      if(policy.navDemoVersion()>=2) {
        observation.snapshot.roads.nextDirection={-99,88,77};
        observation.snapshot.roads.nextDistanceCm=std::numeric_limits<float>::quiet_NaN();
        observation.snapshot.roads.remainingVisualCm=std::numeric_limits<float>::quiet_NaN();
        observation.snapshot.roads.goalInKnown=!observation.snapshot.roads.goalInKnown;
        }
      else {
        observation.snapshot.roads.carrotDirection={-99,88,77};
        observation.snapshot.roads.carrotDistanceCm=std::numeric_limits<float>::quiet_NaN();
        observation.snapshot.roads.remainingKnownCm=std::numeric_limits<float>::quiet_NaN();
        observation.snapshot.roads.beyondKnown=!observation.snapshot.roads.beyondKnown;
        }
      const auto other=policy.encode(observation.snapshot);
      require(difference(actual.near,other.near)==0 && difference(actual.far,other.far)==0 &&
              difference(actual.vector,other.vector)==0,"privileged annotation leaked into features");
      NavPolicy::State fresh;
      const auto reset=policy.step(observation.snapshot,fresh);
      for(size_t h=0;h<fixture.heads.size();++h)
        require(difference(result.logits[h],reset.logits[h])==0,"NPC state is shared across evaluations");
      }
    }
  fixture.finish();
  std::cout<<"{\"contract\":\"navpolicy\",\"steps\":"<<fixture.readSteps
           <<",\"navdemo_version\":"<<fixture.demoVersion
           <<",\"weight_version\":"<<fixture.weightVersion
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
    observation.decode(fixture.raw,fixture.layout,fixture.demoVersion);
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
  std::vector<double> stepUs,tickUs,encodeUs,policyUs;
  const size_t ticks=(decisions+npcs-1)/npcs;
  stepUs.reserve(ticks*npcs);tickUs.reserve(ticks);encodeUs.reserve(decisions);policyUs.reserve(decisions);
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
    observation.decode(records[i%records.size()],fixture.layout,fixture.demoVersion);
    const auto before=Clock::now();
    const auto encoded=policy.encode(observation.snapshot);
    const auto after=Clock::now();
    encodeUs.push_back(micros(before,after));
    checksum+=encoded.vector[i%encoded.vector.size()];
    }
  for(size_t i=0;i<decisions;++i) {
    observation.decode(records[i%records.size()],fixture.layout,fixture.demoVersion);
    const auto before=Clock::now();
    const auto result=policy.step(observation.snapshot,states[i%npcs]);
    const auto after=Clock::now();
    policyUs.push_back(micros(before,after));
    checksum+=result.logits[0][0];
    }
  std::cout<<"{\"benchmark\":\"navpolicy\",\"threads\":1,\"npcs\":"<<npcs
           <<",\"warmup_ticks\":30,\"decisions\":"<<stepUs.size()<<",\"ticks\":"<<ticks
           <<",\"decision_median_us\":"<<percentile(stepUs,.5)<<",\"decision_p95_us\":"<<percentile(stepUs,.95)
           <<",\"tick_median_us\":"<<percentile(tickUs,.5)<<",\"tick_p95_us\":"<<percentile(tickUs,.95)
           <<",\"encode_median_us\":"<<percentile(encodeUs,.5)<<",\"encode_p95_us\":"<<percentile(encodeUs,.95)
           <<",\"policy_median_us\":"<<percentile(policyUs,.5)<<",\"policy_p95_us\":"<<percentile(policyUs,.95)
           <<",\"checksum\":"<<checksum<<"}\n";
  }
}

int main(int argc,char** argv) {
  try {
    if(argc==2 && std::string(argv[1])=="--live-format") {
      liveFormatContract();
      return 0;
      }
    if(argc==3 && std::string(argv[1])=="--memory") {
      memoryContract(argv[2]);
      return 0;
      }
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
             <<"       gothic-navpolicy-contract --live-format\n"
             <<"       gothic-navpolicy-contract --memory WEIGHTS\n"
             <<"       gothic-navpolicy-contract --benchmark WEIGHTS FIXTURE [--npcs 20] [--decisions 2000]\n";
    return 2;
    }
  catch(const std::exception& error) {
    std::cerr<<"navpolicy contract: "<<error.what()<<'\n';
    return 1;
    }
  }
