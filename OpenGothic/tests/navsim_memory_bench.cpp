// Measurement-only replay. Copies of classification/encoding are checked against
// the engine on every readout; no game implementation is replaced.
#define NAV_MEMORY_COST_REPLAY_ONLY
#include "navmemorycosttrace.h"
#include "navpolicy.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <type_traits>
#include <utility>

namespace {
using Clock=std::chrono::steady_clock;
using Memory=NavigationMemory;
using Vec3=Tempest::Vec3;
int cellCoordinate(float value,float cell) { return int(std::floor(value/cell)); }
size_t ringCoordinate(int value,size_t size) {
  const int remainder=value%int(size);
  return size_t(remainder<0 ? remainder+int(size) : remainder);
  }
void require(bool ok,const std::string& message) { if(!ok)throw std::runtime_error(message); }
void fail(const std::string& message) { throw std::runtime_error(message); }
struct Event {
  uint8_t kind=0;
  NavigationMemory::Pose pose;
  uint32_t rays=0;
  std::vector<std::pair<uint32_t,NavigationEye::Ray>> changed;
  };
struct Capture {
  NavigationMemory::FamiliarMap map;
  NavigationMemory::FamiliarMask mask;
  std::vector<Event> events;
  uint64_t updates=0,views=0;
  double durationMs=0;
  };
Capture load(const std::string& path) {
  std::ifstream input(path,std::ios::binary);
  if(!input)throw std::runtime_error("cannot open navigation memory cost capture: "+path);
  std::array<char,8> magic;
  NavMemoryCost::read(input,magic);
  if(magic!=NavMemoryCost::Magic)throw std::runtime_error("invalid navigation memory cost capture");
  uint32_t poseBytes=0,rayBytes=0,tileBytes=0;
  NavMemoryCost::read(input,poseBytes);NavMemoryCost::read(input,rayBytes);NavMemoryCost::read(input,tileBytes);
  if(poseBytes!=sizeof(NavigationMemory::Pose) || rayBytes!=sizeof(NavigationEye::Ray) ||
     tileBytes!=sizeof(NavigationMemory::FamiliarTile))
    throw std::runtime_error("navigation memory cost capture ABI differs");
  Capture result;
  uint64_t tiles=0,words=0;
  NavMemoryCost::read(input,tiles);NavMemoryCost::read(input,words);
  if(words!=(tiles+63)/64)throw std::runtime_error("navigation memory cost capture mask differs from atlas");
  result.map.tiles.resize(size_t(tiles));result.mask.resize(size_t(words));
  for(auto& tile:result.map.tiles)NavMemoryCost::read(input,tile);
  for(auto& word:result.mask)NavMemoryCost::read(input,word);
  result.map.finish();
  uint64_t first=0,last=0;
  while(input.peek()!=std::char_traits<char>::eof()) {
    Event event;
    NavMemoryCost::read(input,event.kind);
    if(event.kind==NavMemoryCost::Update) {
      NavMemoryCost::read(input,event.pose);NavMemoryCost::read(input,event.rays);
      uint32_t changed=0;NavMemoryCost::read(input,changed);
      if(changed>event.rays)throw std::runtime_error("invalid navigation memory cost capture ray count");
      event.changed.resize(changed);
      for(auto& [index,ray]:event.changed) {
        NavMemoryCost::read(input,index);NavMemoryCost::read(input,ray);
        if(index>=event.rays)throw std::runtime_error("invalid navigation memory cost capture ray slot");
        }
      if(result.updates++==0)first=event.pose.gameMs;
      last=event.pose.gameMs;
      }
    else if(event.kind==NavMemoryCost::View)++result.views;
    else throw std::runtime_error("invalid navigation memory cost capture event");
    result.events.push_back(std::move(event));
    }
  if(result.updates==0 || result.views==0)throw std::runtime_error("navigation memory cost capture is empty");
  result.durationMs=double(last-first);
  return result;
  }

void nativeMemory(const NavigationMemory::Layer& layer, size_t firstRow, size_t rows,
                  float reference, bool familiar, float* output) {
  const size_t columns = layer.size, area = rows*columns;
  for(size_t z=0; z<rows; ++z) {
    for(size_t x=0; x<columns; ++x) {
      const auto& cell = layer.cells[(firstRow+z)*columns+x];
      const size_t index = z*columns+x;
      const float channels[] = {cell.confidence,cell.free,cell.wall,cell.ledge,
                                cell.drop,cell.water,cell.movable};
      for(size_t c=0; c<7; ++c) {
        if(!std::isfinite(channels[c]) || channels[c]<0 || channels[c]>1)
          fail("memory channel outside [0,1]");
        output[c*area+index] = float(std::lround(channels[c]*255.f))/255.f;
        }
      if(!std::isnan(cell.groundHeightCm) && !std::isfinite(cell.groundHeightCm))
        fail("infinite memory ground height");
      const bool valid = std::isfinite(cell.groundHeightCm);
      // Native cells contain one height sample. Max and min coincide until
      // an upstream memory representation supplies more than one sample.
      const float relative = valid ? std::clamp((cell.groundHeightCm-reference)/200.f,-10.f,10.f) : 0.f;
      output[7*area+index] = relative;
      output[8*area+index] = relative;
      output[9*area+index] = valid ? 1.f : 0.f;
      if(familiar) {
        if(!std::isfinite(cell.familiar) || cell.familiar<0 || cell.familiar>1)
          fail("memory familiar channel outside [0,1]");
        output[10*area+index] = float(std::lround(cell.familiar*255.f))/255.f;
        }
      }
    }
  }
void encodeVector(const NavPolicy& policy,const NavDemoWriter::Snapshot& observation,std::vector<float>& vector) {
  vector.resize(policy.vectorInputSize());
  size_t cursor = 0;

  for(const auto& ray:*observation.eye) {
    if(!std::isfinite(ray.maxRangeCm) || ray.maxRangeCm<=0 || !std::isfinite(ray.distanceCm) ||
       ray.distanceCm<0 || !std::isfinite(ray.normalY) || std::abs(ray.normalY)>1 || uint8_t(ray.kind)>5)
      fail("invalid eye range or hit kind");
    vector[cursor++] = std::clamp(ray.distanceCm/ray.maxRangeCm,0.f,1.f);
    for(size_t kind=0; kind<6; ++kind)
      vector[cursor++] = kind==uint8_t(ray.kind) ? 1.f : 0.f;
    vector[cursor++] = ray.normalY;
    vector[cursor++] = ray.hit ? 1.f : 0.f;
    }
  const auto distance = [](float value) { return std::clamp(value/1000.f,0.f,50.f); };
  for(float value:policy.navDemoVersion()>=2 ? observation.roads.carrotDirection : observation.roads.nextDirection)
    vector[cursor++] = value;
  vector[cursor++] = distance(policy.navDemoVersion()>=2 ? observation.roads.carrotDistanceCm : observation.roads.nextDistanceCm);
  for(float value:observation.roads.goalDirection)
    vector[cursor++] = value;
  vector[cursor++] = distance(observation.roads.goalDistanceCm);
  vector[cursor++] = distance(policy.navDemoVersion()>=2 ? observation.roads.remainingKnownCm : observation.roads.remainingVisualCm);
  vector[cursor++] = (policy.navDemoVersion()>=2 ? observation.roads.beyondKnown : observation.roads.goalInKnown) ? 1.f : 0.f;
  for(float value:observation.body.velocityCmS)
    vector[cursor++] = std::clamp(value/500.f,-4.f,4.f);
  for(uint8_t flag:observation.body.flags) {
    if(flag>1)
      fail("invalid body flag");
    vector[cursor++] = float(flag);
    }
  for(float value:observation.body.groundNormal)
    vector[cursor++] = value;
  vector[cursor++] = observation.body.groundSlopeDeg/90.f;
  vector[cursor++] = observation.body.headYawDeg/180.f;
  vector[cursor++] = observation.body.headPitchDeg/90.f;
  if(policy.paceInput()) {
    vector[cursor++] = 0.f;
    }
  if(policy.vectorInputSize()==NavPolicy::legacyVectorSize) {
    vector[cursor++] = observation.previous.forward;
    vector[cursor++] = observation.previous.strafe;
    vector[cursor++] = observation.previous.turn;
    vector[cursor++] = observation.previous.headYaw/180.f;
    vector[cursor++] = observation.previous.headPitch/90.f;
    vector[cursor++] = observation.previous.walk ? 1.f : 0.f;
    vector[cursor++] = observation.previous.jump ? 1.f : 0.f;
    vector[cursor++] = observation.previous.climb ? 1.f : 0.f;
    }
  if(cursor!=policy.vectorInputSize())
    fail("internal feature size mismatch");
  for(float value:vector)
    if(!std::isfinite(value))
      fail("non-finite learner feature");
  }
}
class NavMemoryCostTrace final {
  using Grid=Memory::Grid;
  using Constants=Memory::Constants;
  using FamiliarTile=Memory::FamiliarTile;
  using ClassificationLimits=Memory::ClassificationLimits;
  enum { Free=Memory::Free, Ground=Memory::Ground, Wall=Memory::Wall,
         Water=Memory::Water, Movable=Memory::Movable, ChannelCount=Memory::ChannelCount };
  public:
    static void read(const Memory& memory,bool far,Memory::Layer& layer) {
      memory.read(far ? memory.farGrid : memory.nearGrid,layer);
      }
static std::array<float,9> classify(const Memory& memory,const Grid& grid,int x,int z,const ClassificationLimits& limits) {
  std::array<float,9> output={};
  output[8]=std::numeric_limits<float>::quiet_NaN();
  const auto& input = grid.at(x,z);
  if(input.known==0)
    return output;
  std::array<float,ChannelCount> c = {};
  for(size_t channel=0; channel<ChannelCount; ++channel)
    if((input.known&(1u<<channel))!=0) {
      c[channel] = memory.confidence(input.evidence[channel],channel==Movable);
      output[0] = std::max(output[0],c[channel]);
      }
  output[1] = std::max(output[1],c[Free]);
  // A hit above an observed walking surface does not prove a wall in the
  // space occupied by the body. Nearby ground is evidence, not a guess at
  // the height of unseen terrain. Without it the wall remains conservative.
  bool overheadWall = false;
  if(c[Wall]>0.f) {
    const auto& wall = input.evidence[Wall];
    float nearest = std::numeric_limits<float>::infinity();
    for(int nz=z-1; nz<=z+1; ++nz)
      for(int nx=x-1; nx<=x+1; ++nx) {
        if(!grid.contains(nx,nz))
          continue;
        const auto& neighbour = grid.at(nx,nz);
        if((neighbour.known&(1u<<Ground))==0)
          continue;
        const auto& ground = neighbour.evidence[Ground];
        const float gx = ground.hitX-wall.hitX, gz = ground.hitZ-wall.hitZ;
        const float distance = gx*gx+gz*gz;
        if(distance<nearest) {
          nearest = distance;
          overheadWall = wall.heightCm>ground.heightCm+limits.bodyHeight;
          }
        }
    }
  if(!overheadWall)
    output[2] = std::max(output[2],c[Wall]);
  output[5] = std::max(output[5],c[Water]);
  output[6] = std::max(output[6],c[Movable]);
  if(c[Ground]>0.f) {
    const auto& ground = input.evidence[Ground];
    output[8] = ground.heightCm;
    output[1] = std::max(output[1],c[Ground]);
    // Height relative to the actor's feet says nothing about traversability
    // several metres ahead. A hazard needs a locally observed discontinuity
    // exceeding both the body's step and its maximum walking slope.
    for(int nz=z-1; nz<=z+1; ++nz)
      for(int nx=x-1; nx<=x+1; ++nx) {
        if((nx==x && nz==z) || !grid.contains(nx,nz))
          continue;
        const auto& neighbour = grid.at(nx,nz);
        if((neighbour.known&(1u<<Ground))==0)
          continue;
        const auto& other = neighbour.evidence[Ground];
        const float difference = ground.heightCm-other.heightCm;
        const float dx = ground.hitX-other.hitX, dz = ground.hitZ-other.hitZ;
        const float distance = std::sqrt(dx*dx+dz*dz);
        if(std::abs(difference)<=limits.step || std::abs(difference)<=distance*limits.slope)
          continue;
        const float certainty = std::min(c[Ground],memory.confidence(other,false));
        if(difference>0.f)
          output[3] = std::max(output[3],certainty);
        else
          output[4] = std::max(output[4],certainty);
        }
    }
  // Empty air below a ray is not a ground measurement: it cannot establish
  // a drop without a local observed change in the supporting surface.
  return output;
  }

static std::array<float,9> familiarCell(const Memory& memory,const FamiliarTile& tile,const std::array<float,9>& live) {
  if(live[0]>=Constants::familiarConfidence)
    return live;
  // A measured support surface can disprove this tile's floor. Without one,
  // retain the same two-dimensional limitation as the live far grid.
  const float bodyHeight=std::isfinite(memory.body.bodyHeightCm) && memory.body.bodyHeightCm>0.f ?
                         memory.body.bodyHeightCm : Constants::bodyHeightCm;
  if(std::isfinite(live[8]) && std::isfinite(tile.groundHeightCm) &&
     std::abs(live[8]-tile.groundHeightCm)>bodyHeight)
    return live;
  std::array<float,9> output={};
  output[8]=std::numeric_limits<float>::quiet_NaN();
  output[0]=Constants::familiarConfidence;
  output[7]=1.f;
  output[8]=tile.groundHeightCm;
  const auto value=[&](uint8_t channel) { return (tile.channels&channel)!=0 ? Constants::familiarConfidence : 0.f; };
  output[1]=value(FamiliarTile::Free);
  output[2]=value(FamiliarTile::Wall);
  output[3]=value(FamiliarTile::Ledge);
  output[4]=value(FamiliarTile::Drop);
  output[5]=value(FamiliarTile::Water);
  output[6]=live[6];
  return output;
  }

    // Quantization is monotone: max(Q(a),Q(b)) == Q(max(a,b)). Keep raw
    // confidence and familiar provenance in scratch for the .35 precedence
    // test, while splats write quantized values straight into network CHW.
    static void fused(const Memory& memory,bool far,std::vector<float>& output,std::vector<float>& provenance) {
      const auto& grid=far ? memory.farGrid : memory.nearGrid;
      const size_t firstRow=far ? 50 : 0, rows=far ? 50 : 64, area=rows*grid.size;
      output.resize(10*area);provenance.resize(2*area);
      std::fill(output.begin(),output.end(),0.f);
      std::fill(provenance.begin(),provenance.end(),0.f);
      const auto& body=memory.body;
      const Vec3 right(-body.forward.z,0,body.forward.x);
      const auto limits=memory.classificationLimits();
      const bool hasFamiliar=far && !memory.familiarWindow.empty();
      for(int z=grid.minZ;z<grid.minZ+int(grid.size);++z)
        for(int x=grid.minX;x<grid.minX+int(grid.size);++x) {
          const auto* familiar=hasFamiliar ? memory.familiarWindow[ringCoordinate(z,grid.size)*grid.size+ringCoordinate(x,grid.size)] : nullptr;
          if(grid.at(x,z).known==0 && !familiar)continue;
          const float dx=(float(x)+.5f)*grid.cellCm-body.position.x;
          const float dz=(float(z)+.5f)*grid.cellCm-body.position.z;
          const int vx=cellCoordinate(dx*right.x+dz*right.z+float(grid.size/2)*grid.cellCm,grid.cellCm);
          const int vz=cellCoordinate(dx*body.forward.x+dz*body.forward.z+float(grid.size/2)*grid.cellCm,grid.cellCm);
          // The HD far input omits the rear half; it cannot contribute to a
          // forward splat, so classification there is unnecessary.
          if(vx<0 || vz<int(firstRow) || vx>=int(grid.size) || vz>=int(firstRow+rows))continue;
          auto sample=classify(memory,grid,x,z,limits);
          if(familiar)sample=familiarCell(memory,*familiar,sample);
          if(sample[0]<=0.f)continue;
          const size_t index=(size_t(vz)-firstRow)*grid.size+size_t(vx);
          auto& rawConfidence=provenance[area+index];
          if(rawConfidence>0.f && provenance[index]!=sample[7]) {
            if(provenance[index]==0.f && rawConfidence>=Constants::familiarConfidence)continue;
            if(sample[7]==0.f && sample[0]>=Constants::familiarConfidence) {
              for(size_t c=0;c<10;++c)output[c*area+index]=0.f;
              provenance[index]=0.f;
              rawConfidence=0.f;
              }
            }
          for(size_t c=0;c<7;++c) {
            const float quantized=float(std::lround(sample[c]*255.f))/255.f;
            output[c*area+index]=std::max(output[c*area+index],quantized);
            }
          rawConfidence=std::max(rawConfidence,sample[0]);
          provenance[index]=std::max(provenance[index],sample[7]);
          if(std::isfinite(sample[8])) {
            const float height=std::clamp((sample[8]-body.position.y)/200.f,-10.f,10.f);
            output[7*area+index]=height;output[8*area+index]=height;output[9*area+index]=1.f;
            }
          }
      }
  };

namespace {
void requireEqual(const std::vector<float>& actual,const std::vector<float>& expected,const std::string& context) {
  require(actual.size()==expected.size(),context+": size differs");
  if(std::memcmp(actual.data(),expected.data(),actual.size()*sizeof(float))==0)return;
  for(size_t i=0;i<actual.size();++i)
    if(std::memcmp(&actual[i],&expected[i],sizeof(float))!=0)
      throw std::runtime_error(context+": bit mismatch at "+std::to_string(i)+": "+std::to_string(actual[i])+" vs "+std::to_string(expected[i]));
  }

void selfTest() {
  Memory memory;
  Memory::FamiliarMap atlas;
  for(int z=-30;z<=30;++z)for(int x=-30;x<=30;++x) {
    Memory::FamiliarTile tile;
    tile.x=x;tile.z=z;tile.groundHeightCm=float((x+z)%7)*60.f;
    tile.channels=uint8_t(1u<<unsigned((x*x+z*z)%5));
    atlas.tiles.push_back(tile);
    }
  atlas.finish();
  memory.setFamiliarMap(&atlas,Memory::FamiliarMask((atlas.tiles.size()+63)/64,~uint64_t(0)));
  Memory::View view;
  std::vector<float> actual,expected,provenance;
  for(unsigned step=0;step<96;++step) {
    Memory::Pose pose;
    const float yaw=float(step)*.137f;
    pose.forward={std::cos(yaw),0,std::sin(yaw)};
    pose.position={float(step)*43.f-1800.f,float(step%8)*35.f,float(step%13)*123.f-800.f};
    pose.gameMs=uint64_t(step)*8000;
    memory.setJourneyMemory(step%3!=0);
    std::vector<NavigationEye::Ray> rays;
    for(unsigned i=0;i<96;++i) {
      NavigationEye::Ray ray;
      ray.origin=pose.position+Vec3(0,150,0);
      const float angle=float(i)*.31f;
      const float distance=100.f+float((i*131)%6500);
      ray.point=pose.position+Vec3(std::cos(angle)*distance,float(int(i%9)-4)*75.f,std::sin(angle)*distance);
      ray.direction=Vec3::normalize(ray.point-ray.origin);
      ray.distanceCm=(ray.point-ray.origin).length();ray.maxRangeCm=ray.distanceCm;
      ray.kind=NavigationEye::HitKind(i%6);ray.normalY=i%3==0 ? 1.f : 0.f;
      ray.hit=i%6!=0;ray.complete=true;ray.sampledAtMs=ray.nearSampledAtMs=pose.gameMs;
      rays.push_back(ray);
      }
    memory.update(rays,pose);memory.view(view);
    for(bool far:{false,true}) {
      expected.resize(far ? NavPolicy::hdFarSize : NavPolicy::hdNearSize);
      nativeMemory(far ? view.far : view.near,far ? 50 : 0,far ? 50 : 64,pose.position.y,false,expected.data());
      NavMemoryCostTrace::fused(memory,far,actual,provenance);
      requireEqual(actual,expected,"synthetic step "+std::to_string(step));
      }
    }
  std::cout<<"{\"self_test\":\"passed\",\"decisions\":96,\"float32_bitwise_equal\":true}\n";
  }

enum Part { Update,ViewNear,ViewFar,EncodeNear,EncodeFar,EncodeVector,EncodeFull,Policy,FusedNear,FusedFar,PartCount };
constexpr std::array<const char*,PartCount> names={"update","view_near","view_far","encode_near","encode_far","encode_vector","encode_full","policy_scalar_cpp","fused_float32_near","fused_float32_far"};
struct Timing {
  std::array<double,PartCount> us={};
  uint64_t updates=0,decisions=0;
  double checksum=0;
  Timing& operator+=(const Timing& other) {
    for(size_t i=0;i<PartCount;++i)us[i]+=other.us[i];
    updates+=other.updates;decisions+=other.decisions;checksum+=other.checksum;return *this;
    }
  };
template<class F> void timePart(Timing& timing,Part part,F&& function) {
  const auto start=Clock::now();function();
  timing.us[part]+=std::chrono::duration<double,std::micro>(Clock::now()-start).count();
  }

Timing replay(const Capture& capture,const NavPolicy& policy,unsigned round) {
  Memory memory;
  policy.configureMemory(memory);
  if(!capture.mask.empty())memory.setFamiliarMap(&capture.map,capture.mask);
  Memory::View view;
  std::vector<NavigationEye::Ray> rays;
  std::vector<NavigationEye::Ray> eye(NavDemoWriter::rayCount);
  NavPolicy::Features features;
  features.near.resize(policy.nearInputSize());features.far.resize(policy.farInputSize());
  features.vector.resize(policy.vectorInputSize());
  std::vector<float> fusedNear(policy.nearInputSize()),fusedFar(policy.farInputSize()),provenanceNear(2*64*64),provenanceFar(2*50*100);
  NavPolicy::State state;
  Timing result;
  for(const auto& event:capture.events) {
    if(event.kind==NavMemoryCost::Update) {
      rays.resize(event.rays);
      for(const auto& [index,ray]:event.changed)rays[index]=ray;
      timePart(result,Update,[&] { memory.update(rays,event.pose); });
      ++result.updates;
      }
    else {
      view.pose=memory.pose();view.traveledCm=memory.traveledCm();
      const auto current=[&] {
        timePart(result,ViewNear,[&] { NavMemoryCostTrace::read(memory,false,view.near); });
        timePart(result,ViewFar,[&] { NavMemoryCostTrace::read(memory,true,view.far); });
        timePart(result,EncodeNear,[&] { nativeMemory(view.near,0,64,view.pose.position.y,false,features.near.data()); });
        timePart(result,EncodeFar,[&] { nativeMemory(view.far,50,50,view.pose.position.y,false,features.far.data()); });
        };
      const auto fused=[&] {
        timePart(result,FusedNear,[&] { NavMemoryCostTrace::fused(memory,false,fusedNear,provenanceNear); });
        timePart(result,FusedFar,[&] { NavMemoryCostTrace::fused(memory,true,fusedFar,provenanceFar); });
        };
      // AB/BA alternation across repeats counters the warm-cache ordering bias.
      if(round%2==0) { current();fused(); } else { fused();current(); }
      NavDemoWriter::Snapshot observation;
      // memorySamples is 2*N: the current eye followed by cached far rays.
      require(rays.size()==2*eye.size(),"capture does not have the current + cached eye layout");
      std::copy_n(rays.begin(),eye.size(),eye.begin());
      observation.eye=&eye;observation.memory=&view;
      observation.position={view.pose.position.x,view.pose.position.y,view.pose.position.z};
      // Memory traces contain eye + pose, but no roads/body. Those vector
      // fields retain Snapshot defaults; they are not inferred from the world.
      timePart(result,EncodeVector,[&] { encodeVector(policy,observation,features.vector); });
      std::optional<NavPolicy::Features> reference;
      timePart(result,EncodeFull,[&] { reference.emplace(policy.encode(observation,false)); });
      requireEqual(features.near,reference->near,"split near encode");requireEqual(features.far,reference->far,"split far encode");
      requireEqual(features.vector,reference->vector,"split vector encode");
      requireEqual(fusedNear,reference->near,"fused near");requireEqual(fusedFar,reference->far,"fused far");
      NavPolicy::Result action;
      timePart(result,Policy,[&] { action=policy.step(*reference,state); });
      result.checksum+=action.logits[0][0]+fusedNear[0]+fusedFar[0];
      ++result.decisions;
      }
    }
  require(result.decisions>0,"no decisions in replay");return result;
  }

double percentile(std::vector<double> values,double fraction) {
  std::sort(values.begin(),values.end());
  const double index=fraction*double(values.size()-1);
  const size_t lower=size_t(index),upper=std::min(lower+1,values.size()-1);
  return values[lower]+(values[upper]-values[lower])*(index-double(lower));
  }
void jsonTiming(const Timing& value) {
  std::cout<<"{\"updates\":"<<value.updates<<",\"decisions\":"<<value.decisions<<",\"checksum\":"<<value.checksum<<",\"us_per_decision\":{";
  for(size_t i=0;i<PartCount;++i) {
    if(i)std::cout<<',';
    std::cout<<std::quoted(names[i])<<':'<<value.us[i]/double(value.decisions);
    }
  std::cout<<"}}";
  }
void jsonDistribution(const std::vector<double>& values) {
  std::cout<<"{\"median\":"<<percentile(values,.5)<<",\"p90\":"<<percentile(values,.9)<<",\"repeats\":"<<values.size()<<'}';
  }
}

int main(int argc,char** argv) {
  try {
    if(argc==1) { selfTest();return 0; }
    require(argc>=3,"usage: gothic-navsim-memory-bench WEIGHTS [--repetitions N] CAPTURE...");
    NavPolicy policy(argv[1]);
    require(policy.weightVersion()==5,"benchmark requires v5 weights");
    unsigned repetitions=6;
    std::vector<std::string> paths;
    for(int i=2;i<argc;++i) {
      const std::string arg=argv[i];
      if(arg=="--repetitions" && i+1<argc)repetitions=unsigned(std::stoul(argv[++i]));
      else if(arg.starts_with("--"))throw std::runtime_error("unknown option: "+arg);
      else paths.push_back(arg);
      }
    require(repetitions>=2 && !paths.empty(),"at least two repeats and one capture required");
    std::vector<Timing> totals(repetitions);
    std::cout<<std::setprecision(10)<<"{\"weights\":"<<std::quoted(argv[1])<<",\"warmup_passes_per_capture\":1,\"repetitions\":"<<repetitions
             <<",\"vector_context\":\"captured eye and feet height; roads/body Snapshot defaults; forced run\",\"captures\":[";
    for(size_t i=0;i<paths.size();++i) {
      const auto capture=load(paths[i]);
      replay(capture,policy,0);
      if(i)std::cout<<',';
      std::cout<<"{\"path\":"<<std::quoted(paths[i])<<",\"updates\":"<<capture.updates<<",\"decisions\":"<<capture.views<<",\"rounds\":[";
      for(unsigned round=0;round<repetitions;++round) {
        const auto measured=replay(capture,policy,round);
        totals[round]+=measured;
        if(round)std::cout<<',';
        jsonTiming(measured);
        }
      std::cout<<"]}"<<std::flush;
      }
    std::cout<<"],\"rounds\":[";
    for(size_t i=0;i<totals.size();++i) { if(i)std::cout<<',';jsonTiming(totals[i]); }
    std::cout<<"],\"us_per_decision\":{";
    for(size_t part=0;part<PartCount;++part) {
      std::vector<double> values;
      for(const auto& t:totals)values.push_back(t.us[part]/double(t.decisions));
      if(part)std::cout<<',';
      std::cout<<std::quoted(names[part])<<':';jsonDistribution(values);
      }
    std::vector<double> current,fused,saved,percent;
    for(const auto& t:totals) {
      const double before=(t.us[ViewNear]+t.us[ViewFar]+t.us[EncodeNear]+t.us[EncodeFar])/double(t.decisions);
      const double after=(t.us[FusedNear]+t.us[FusedFar])/double(t.decisions);
      current.push_back(before);fused.push_back(after);saved.push_back(before-after);percent.push_back(100.*(before-after)/before);
      }
    std::cout<<"},\"memory_view_and_map_encode_us\":";jsonDistribution(current);
    std::cout<<",\"fused_float32_us\":";jsonDistribution(fused);
    std::cout<<",\"saved_us\":";jsonDistribution(saved);
    std::cout<<",\"saved_percent\":";jsonDistribution(percent);
    std::cout<<",\"float32_bitwise_equal\":true,\"checked_decisions_per_pass\":"<<totals[0].decisions<<"}\n";
    return 0;
    }
  catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
  }
