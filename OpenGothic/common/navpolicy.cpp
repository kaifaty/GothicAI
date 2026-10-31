#include "navpolicy.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
#include <rapidjson/document.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {
using Json = rapidjson::Value;

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error("nav policy: "+message);
  }

const Json& field(const Json& object, const char* name) {
  if(!object.IsObject() || !object.HasMember(name))
    fail(std::string("missing weight header field ")+name);
  return object[name];
  }

void expectString(const Json& value, std::string_view expected) {
  if(!value.IsString() || std::string_view(value.GetString(),value.GetStringLength())!=expected)
    fail("unsupported weight header value (expected "+std::string(expected)+")");
  }

void expectInt(const Json& value, uint64_t expected) {
  if(!value.IsUint64() || value.GetUint64()!=expected)
    fail("unsupported weight header integer");
  }

void expectShape(const Json& value, std::initializer_list<size_t> expected) {
  if(!value.IsArray() || value.Size()!=expected.size())
    fail("incorrect weight tensor or architecture shape");
  size_t index = 0;
  for(size_t dimension:expected)
    expectInt(value[rapidjson::SizeType(index++)],dimension);
  }

void expectStrings(const Json& value, std::initializer_list<const char*> expected) {
  if(!value.IsArray() || value.Size()!=expected.size())
    fail("unsupported architecture order");
  size_t index = 0;
  for(const char* item:expected)
    expectString(value[rapidjson::SizeType(index++)],item);
  }

bool uniqueKeys(const Json& value) {
  if(value.IsObject()) {
    std::unordered_set<std::string> names;
    for(auto i=value.MemberBegin(); i!=value.MemberEnd(); ++i)
      if(!names.emplace(i->name.GetString(),i->name.GetStringLength()).second || !uniqueKeys(i->value))
        return false;
    }
  else if(value.IsArray()) {
    for(const auto& item:value.GetArray())
      if(!uniqueKeys(item))
        return false;
    }
  return true;
  }

size_t parseVectorInputSize(const Json& preprocessing, uint32_t& demoVersion, uint32_t weightVersion) {
  const auto& order = field(preprocessing,"vector_order");
  if(!order.IsArray() || (order.Size()!=3 && order.Size()!=4))
    fail("unsupported preprocessing vector_order");
  if(preprocessing.HasMember("navdemo_version")) {
    const auto& version = preprocessing["navdemo_version"];
    if(!version.IsUint() || (version.GetUint()!=1 && version.GetUint()!=2 &&
                           !(weightVersion==4 && version.GetUint()==3)))
      fail("unsupported NAVDEMO input version");
    demoVersion = version.GetUint();
    }
  if(weightVersion==4 && demoVersion!=3)
    fail("unsupported NAVDEMO input version");
  if(demoVersion>=2) {
    expectStrings(order,{"eye","roads","body"});
    if(weightVersion>=3)
      expectStrings(field(preprocessing,"input_fields"),{"body","roads","eye","memory_near","memory_far","feet_height_cm"});
    else
      expectStrings(field(preprocessing,"input_fields"),{"body","roads","eye","memory_near","memory_far"});
    expectString(field(preprocessing,"roads_schema"),"carrot_v2");
    expectStrings(field(preprocessing,"roads_fields"),{"carrot_direction","carrot_distance_cm",
      "goal_direction","goal_distance_cm","remaining_known_cm","beyond_known"});
    }
  else if(preprocessing.HasMember("roads_schema")) {
    expectString(preprocessing["roads_schema"],"next_v1");
    }
  if(order.Size()==3) {
    expectStrings(order,{"eye","roads","body"});
    return NavPolicy::vectorSize;
    }
  expectStrings(order,{"eye","roads","body","previous_keys"});
  return NavPolicy::legacyVectorSize;
  }

void validateHdPreprocessing(const Json& preprocessing, uint32_t demoVersion, uint32_t weightVersion) {
  if(weightVersion==3 && demoVersion!=2)
    fail("HD weights require NAVDEMO v2 inputs");
  expectString(field(preprocessing,"input_variant"),weightVersion==4 ? "hd-familiar" : "hd");
  if(weightVersion==4)
    expectStrings(field(preprocessing,"map_order"),{"confidence","free","wall","ledge","drop","water","movable",
      "relative_ground_height_max","relative_ground_height_min","height_valid_fraction","familiar"});
  else
    expectStrings(field(preprocessing,"map_order"),{"confidence","free","wall","ledge","drop","water","movable",
      "relative_ground_height_max","relative_ground_height_min","height_valid_fraction"});
  expectString(field(preprocessing,"memory"),
    "7 probability channels unpooled; near 64x64 at 25cm; far forward rows[50:100] and all 100 columns at 200cm; one recorded height per cell");
  expectString(field(preprocessing,"height"),
    "known ground max and min minus physical feet height, /200cm, clip [-10,10]; unknown=0; unpooled recorded max=min");
  expectString(field(preprocessing,"height_reference"),
    "own physical feet height only; no absolute world position, x/z, yaw or teacher_hint access");
  expectString(field(preprocessing,"cache_quantization"),
    "vector/height extrema float16, map probabilities/validity round(255*x) uint8, labels uint8");
  }

void validateArchitecture(const Json& architecture, size_t& hidden, size_t vectorSize, uint32_t version) {
  expectString(field(architecture,"name"),"map-cnn-vector-gru");
  expectInt(field(architecture,"version"),1);
  const auto& inputs = field(architecture,"inputs");
  const size_t channels = version==4 ? 11 : version==3 ? 10 : 9;
  if(version>=3) {
    expectShape(field(inputs,"near"),{channels,64,64});
    expectShape(field(inputs,"far"),{channels,50,100});
    }
  else {
    expectShape(field(inputs,"near"),{9,32,32});
    expectShape(field(inputs,"far"),{9,25,25});
    }
  expectShape(field(inputs,"vector"),{vectorSize});
  expectString(field(inputs,"dtype"),"float32");
  expectString(field(inputs,"map_layout"),"CHW");
  const auto& encoders = field(architecture,"map_encoders");
  if(!field(encoders,"separate_weights").IsTrue())
    fail("map encoders must have separate weights");
  const auto& layers = field(encoders,"layers");
  if(!layers.IsArray() || layers.Size()!=4)
    fail("unsupported map encoder layers");
  for(unsigned i=0; i<2; ++i) {
    const auto& conv = layers[i];
    expectString(field(conv,"type"),"conv2d");
    expectInt(field(conv,"in_channels"),i==0 ? channels : 8);
    expectInt(field(conv,"out_channels"),i==0 ? 8 : 16);
    expectInt(field(conv,"kernel"),3);
    expectInt(field(conv,"stride"),2);
    expectInt(field(conv,"padding"),1);
    expectString(field(conv,"activation"),"relu");
    }
  expectString(field(layers[2],"type"),"adaptive_avg_pool2d");
  expectShape(field(layers[2],"output"),{2,2});
  expectString(field(layers[2],"bins"),"start=floor(i*input/output); end=ceil((i+1)*input/output)");
  expectString(field(layers[3],"type"),"flatten");
  expectString(field(layers[3],"order"),"CHW");
  expectInt(field(layers[3],"output"),64);
  const auto& vector = field(architecture,"vector_encoder");
  expectInt(field(vector,"in"),vectorSize);
  expectInt(field(vector,"out"),128);
  expectString(field(vector,"activation"),"relu");
  const auto& fusion = field(architecture,"fusion");
  expectStrings(field(fusion,"concat"),{"near","far","vector"});
  expectInt(field(fusion,"in"),256);
  expectInt(field(fusion,"out"),128);
  expectString(field(fusion,"activation"),"relu");
  const auto& gru = field(architecture,"gru");
  expectInt(field(gru,"input_size"),128);
  expectInt(field(gru,"layers"),1);
  expectStrings(field(gru,"gate_order"),{"reset","update","new"});
  expectString(field(gru,"initial_hidden"),"zeros");
  const auto& hiddenSize = field(gru,"hidden_size");
  if(!hiddenSize.IsUint() || hiddenSize.GetUint()==0 || hiddenSize.GetUint()>1024)
    fail("unsupported GRU hidden size");
  hidden = hiddenSize.GetUint();
  const auto& heads = field(architecture,"heads");
  const size_t headCount = version>=2 ? NavPolicy::bc3HeadNames.size() : NavPolicy::headNames.size();
  if(!heads.IsObject() || heads.MemberCount()!=headCount)
    fail("incorrect policy heads");
  for(size_t i=0; i<headCount; ++i) {
    const auto& head = field(heads,version>=2 ? NavPolicy::bc3HeadNames[i] : NavPolicy::headNames[i]);
    const size_t classes = i<3 || (version>=2 && i==4) ? 3 : 2;
    expectInt(field(head,"classes"),classes);
    const auto& values = field(head,"values");
    if(!values.IsArray() || values.Size()!=classes)
      fail("incorrect head values");
    for(rapidjson::SizeType j=0; j<values.Size(); ++j)
      if(!values[j].IsInt() || values[j].GetInt()!=int(j)-(i<3 ? 1 : 0))
        fail("unsupported head class mapping");
    }
  }

float dot(const float* left, const float* right, size_t size) {
  // Independent accumulators permit instruction-level parallelism without
  // fast-math, SIMD dependencies, or changing the stored float32 arithmetic.
  float a = 0, b = 0, c = 0, d = 0;
  size_t i = 0;
  for(; i+3<size; i+=4) {
    a += left[i]*right[i];
    b += left[i+1]*right[i+1];
    c += left[i+2]*right[i+2];
    d += left[i+3]*right[i+3];
    }
  float result = (a+b)+(c+d);
  for(; i<size; ++i)
    result += left[i]*right[i];
  return result;
  }

float sigmoid(float value) {
  value = std::clamp(value,-80.f,80.f);
  return 1.f/(1.f+std::exp(-value));
  }

bool nearestHeight(const NavigationMemory::Layer& layer, float& result) {
  size_t nearest = std::numeric_limits<size_t>::max();
  bool known = false;
  for(size_t z=0; z<layer.size; ++z) {
    for(size_t x=0; x<layer.size; ++x) {
      const float height = layer.cells[z*layer.size+x].groundHeightCm;
      if(std::isnan(height))
        continue;
      if(!std::isfinite(height))
        fail("infinite memory ground height");
      // Twice the centre offset is integral; strict comparison keeps row-order
      // tie breaking identical to NumPy's stable radius sort.
      const int dx = int(2*x+1)-int(layer.size), dz = int(2*z+1)-int(layer.size);
      const size_t distance = size_t(dx*dx+dz*dz);
      if(distance<nearest) {
        nearest = distance;
        result = height;
        known = true;
        }
      }
    }
  return known;
  }

void validateLayer(const NavigationMemory::Layer& layer, size_t expected, float cellCm) {
  if(layer.size!=expected || layer.cells.size()!=expected*expected || layer.cellCm!=cellCm)
    fail("incorrect memory shape or resolution");
  }

void poolMemory(const NavigationMemory::Layer& layer, size_t factor, float reference, float* output) {
  const size_t size = layer.size/factor, area = size*size;
  const float denominator = float(factor*factor);
  for(size_t z=0; z<size; ++z) {
    for(size_t x=0; x<size; ++x) {
      float probability[7] = {}, height = 0, valid = 0;
      for(size_t dz=0; dz<factor; ++dz) {
        for(size_t dx=0; dx<factor; ++dx) {
          const auto& cell = layer.cells[(z*factor+dz)*layer.size+x*factor+dx];
          const float channels[] = {cell.confidence,cell.free,cell.wall,cell.ledge,
                                    cell.drop,cell.water,cell.movable};
          for(size_t c=0; c<7; ++c) {
            if(!std::isfinite(channels[c]) || channels[c]<0 || channels[c]>1)
              fail("memory channel outside [0,1]");
            // NAVDEMO serializes before Python encodes: retain that exact
            // rounding boundary instead of averaging the live probabilities.
            probability[c] += float(std::lround(channels[c]*255.f))/255.f;
            }
          if(std::isfinite(cell.groundHeightCm)) {
            height += cell.groundHeightCm-reference;
            valid += 1;
            }
          }
        }
      const size_t index = z*size+x;
      for(size_t c=0; c<7; ++c)
        output[c*area+index] = probability[c]/denominator;
      output[7*area+index] = valid>0 ? std::clamp(height/(valid*200.f),-10.f,10.f) : 0.f;
      output[8*area+index] = valid/denominator;
      }
    }
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
}

NavPolicy::NavPolicy(const std::string& path) {
  static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559);
  std::ifstream stream(path,std::ios::binary);
  if(!stream)
    fail("cannot read weights "+path);
  std::string line;
  for(char character; stream.get(character);) {
    if(character=='\n')
      break;
    if(line.size()>=4*1024*1024)
      fail("weight JSON header exceeds limit");
    line.push_back(character);
    }
  if(!stream)
    fail("weight file has no bounded JSON header line");
  rapidjson::Document header;
  header.Parse<rapidjson::kParseValidateEncodingFlag>(line.data(),line.size());
  if(header.HasParseError() || !header.IsObject() || !uniqueKeys(header))
    fail("invalid or duplicate-key weight JSON header");
  expectString(field(header,"format"),"gothic-nav-bc");
  const auto& version = field(header,"version");
  if(!version.IsUint() || (version.GetUint()!=1 && version.GetUint()!=2 && version.GetUint()!=3 && version.GetUint()!=4))
    fail("unsupported navigation weight version");
  formatVersion = version.GetUint();
  expectString(field(header,"byte_order"),"little");
  expectString(field(header,"offset_origin"),"first byte after JSON header newline");
  const auto& preprocessing = field(header,"preprocessing");
  const size_t vectorInputs = parseVectorInputSize(preprocessing,demoVersion,formatVersion);
  if(formatVersion>=3)
    validateHdPreprocessing(preprocessing,demoVersion,formatVersion);
  if(preprocessing.HasMember("navigation_memory_version")) {
    const auto& memory = preprocessing["navigation_memory_version"];
    if(!memory.IsUint() || (memory.GetUint()!=3 && memory.GetUint()!=4))
      fail("unsupported navigation memory version");
    memoryVersion = memory.GetUint();
    }
  if(formatVersion==4 && memoryVersion!=4)
    fail("BC4 weights require navigation memory version 4");
  validateArchitecture(field(header,"architecture"),hidden,vectorInputs,formatVersion);
  if(header.HasMember("temperatures")) {
    const auto& values = header["temperatures"];
    if(!values.IsObject() || values.MemberCount()!=actionHeadCount())
      fail("incorrect policy temperatures");
    for(size_t i=0; i<actionHeadCount(); ++i) {
      const auto& value = field(values,headName(i));
      if(!value.IsNumber() || !std::isfinite(value.GetDouble()) || value.GetDouble()<=0)
        fail("policy temperatures must be finite and positive");
      temperatures[i] = value.GetDouble();
      }
    }
  const auto& length = field(header,"data_nbytes");
  if(!length.IsUint64() || length.GetUint64()>64*1024*1024)
    fail("invalid weight payload size");
  std::vector<uint8_t> payload(size_t(length.GetUint64()));
  stream.read(reinterpret_cast<char*>(payload.data()),std::streamsize(payload.size()));
  if(size_t(stream.gcount())!=payload.size() || stream.peek()!=std::char_traits<char>::eof())
    fail("weight payload length differs from header");

  struct Tensor final {
    std::vector<float> values;
    std::vector<size_t> shape;
    };
  std::unordered_map<std::string,Tensor> tensors;
  std::vector<std::pair<size_t,size_t>> ranges;
  const auto& entries = field(header,"tensors");
  if(!entries.IsArray() || entries.Size()!=16+2*actionHeadCount())
    fail("incorrect number of policy tensors");
  for(const auto& entry:entries.GetArray()) {
    const auto& nameValue = field(entry,"name");
    if(!nameValue.IsString())
      fail("invalid tensor name");
    const std::string name(nameValue.GetString(),nameValue.GetStringLength());
    expectString(field(entry,"dtype"),"float32");
    const auto& shape = field(entry,"shape");
    if(!shape.IsArray() || shape.Empty() || shape.Size()>4)
      fail("invalid tensor shape");
    Tensor tensor;
    size_t count = 1;
    for(const auto& dimension:shape.GetArray()) {
      if(!dimension.IsUint() || dimension.GetUint()==0 || dimension.GetUint()>payload.size()/4/count)
        fail("invalid tensor dimension");
      count *= dimension.GetUint();
      tensor.shape.push_back(dimension.GetUint());
      }
    const auto& offsetValue = field(entry,"offset");
    const auto& bytesValue = field(entry,"nbytes");
    if(!offsetValue.IsUint64() || !bytesValue.IsUint64() || bytesValue.GetUint64()!=count*4 ||
       offsetValue.GetUint64()>payload.size() || count*4>payload.size()-size_t(offsetValue.GetUint64()) ||
       offsetValue.GetUint64()%4!=0)
      fail("invalid payload range for tensor "+name);
    const size_t offset = size_t(offsetValue.GetUint64());
    ranges.emplace_back(offset,offset+count*4);
    tensor.values.resize(count);
    for(size_t i=0; i<count; ++i) {
      const auto* bytes = payload.data()+offset+4*i;
      const uint32_t bits = uint32_t(bytes[0]) | (uint32_t(bytes[1])<<8) |
                            (uint32_t(bytes[2])<<16) | (uint32_t(bytes[3])<<24);
      std::memcpy(&tensor.values[i],&bits,4);
      if(!std::isfinite(tensor.values[i]))
        fail("non-finite weight in tensor "+name);
      }
    if(!tensors.emplace(name,std::move(tensor)).second)
      fail("duplicate tensor "+name);
    }
  std::sort(ranges.begin(),ranges.end());
  size_t end = 0;
  for(const auto& range:ranges) {
    if(range.first!=end)
      fail("overlapping or incomplete tensor payload");
    end = range.second;
    }
  if(end!=payload.size())
    fail("unclaimed weight payload bytes");
  const auto take = [&](const std::string& name, std::initializer_list<size_t> expected) {
    auto found = tensors.find(name);
    if(found==tensors.end() || found->second.shape!=std::vector<size_t>(expected))
      fail("missing or incorrectly shaped tensor "+name);
    auto values = std::move(found->second.values);
    tensors.erase(found);
    return values;
    };
  const auto loadLinear = [&](Linear& layer, const std::string& name, size_t in, size_t out) {
    layer.inputs = in;
    layer.outputs = out;
    layer.weight = take(name+".weight",{out,in});
    layer.bias = take(name+".bias",{out});
    };
  const auto loadMap = [&](MapEncoder& encoder, const char* prefix) {
    const size_t channels = formatVersion==4 ? 11 : formatVersion==3 ? 10 : 9;
    encoder.first.inputs = channels*3*3;
    encoder.first.outputs = 8;
    encoder.first.weight = take(std::string(prefix)+".0.weight",{8,channels,3,3});
    encoder.first.bias = take(std::string(prefix)+".0.bias",{8});
    encoder.second.inputs = 8*3*3;
    encoder.second.outputs = 16;
    encoder.second.weight = take(std::string(prefix)+".2.weight",{16,8,3,3});
    encoder.second.bias = take(std::string(prefix)+".2.bias",{16});
    };
  loadMap(nearEncoder,"near_encoder");
  loadMap(farEncoder,"far_encoder");
  loadLinear(vectorEncoder,"vector_encoder.0",vectorInputs,128);
  loadLinear(fusion,"fusion.0",256,128);
  gruInput.inputs = 128;
  gruInput.outputs = hidden*3;
  gruInput.weight = take("gru.weight_ih_l0",{hidden*3,128});
  gruInput.bias = take("gru.bias_ih_l0",{hidden*3});
  gruHidden.inputs = hidden;
  gruHidden.outputs = hidden*3;
  gruHidden.weight = take("gru.weight_hh_l0",{hidden*3,hidden});
  gruHidden.bias = take("gru.bias_hh_l0",{hidden*3});
  for(size_t i=0; i<actionHeadCount(); ++i)
    loadLinear(heads[i],std::string("head_")+headName(i),hidden,headSize(i));
  if(!tensors.empty())
    fail("unknown weight tensors");
  }

void NavPolicy::configureMemory(NavigationMemory& memory, const char* journeyOverride) const {
  bool journey = memoryVersion==4;
  if(journeyOverride) {
    const std::string_view value(journeyOverride);
    if(value!="0" && value!="1")
      fail("OPENGOTHIC_NAV_JOURNEY_MEMORY must be 0 or 1");
    journey = value=="1";
    }
  memory.setJourneyMemory(journey);
  // Each episode starts without inherited familiarity. The stand may attach
  // the resident's map only after this selects v4 memory.
  memory.setFamiliarMap(nullptr,{});
  }

NavPolicy::Features NavPolicy::encode(const NavDemoWriter::Snapshot& observation) const {
  if(observation.eye==nullptr || observation.eye->size()!=NavDemoWriter::rayCount || observation.memory==nullptr)
    fail("missing eye or memory observation");
  const auto& near = observation.memory->near;
  const auto& far = observation.memory->far;
  validateLayer(near,NavDemoWriter::nearSize,NavigationMemory::Constants::nearCellCm);
  validateLayer(far,NavDemoWriter::farSize,NavigationMemory::Constants::farCellCm);
  Features features;
  features.vector.resize(vectorInputSize());
  if(formatVersion>=3) {
    if(!std::isfinite(observation.position[1]))
      fail("non-finite feet height");
    features.near.resize(nearInputSize());
    features.far.resize(farInputSize());
    nativeMemory(near,0,64,observation.position[1],formatVersion==4,features.near.data());
    nativeMemory(far,50,50,observation.position[1],formatVersion==4,features.far.data());
    }
  else {
    float nearReference = 0, farReference = 0;
    const bool nearKnown = nearestHeight(near,nearReference);
    nearestHeight(far,farReference);
    const float reference = nearKnown ? nearReference : farReference;
    poolMemory(near,2,reference,features.near.data());
    poolMemory(far,4,reference,features.far.data());
    }
  size_t cursor = 0;
  auto& vector = features.vector;
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
  for(float value:demoVersion>=2 ? observation.roads.carrotDirection : observation.roads.nextDirection)
    vector[cursor++] = value;
  vector[cursor++] = distance(demoVersion>=2 ? observation.roads.carrotDistanceCm : observation.roads.nextDistanceCm);
  for(float value:observation.roads.goalDirection)
    vector[cursor++] = value;
  vector[cursor++] = distance(observation.roads.goalDistanceCm);
  vector[cursor++] = distance(demoVersion>=2 ? observation.roads.remainingKnownCm : observation.roads.remainingVisualCm);
  vector[cursor++] = (demoVersion>=2 ? observation.roads.beyondKnown : observation.roads.goalInKnown) ? 1.f : 0.f;
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
  if(vectorInputSize()==legacyVectorSize) {
    vector[cursor++] = observation.previous.forward;
    vector[cursor++] = observation.previous.strafe;
    vector[cursor++] = observation.previous.turn;
    vector[cursor++] = observation.previous.headYaw/180.f;
    vector[cursor++] = observation.previous.headPitch/90.f;
    vector[cursor++] = observation.previous.walk ? 1.f : 0.f;
    vector[cursor++] = observation.previous.jump ? 1.f : 0.f;
    vector[cursor++] = observation.previous.climb ? 1.f : 0.f;
    }
  if(cursor!=vectorInputSize())
    fail("internal feature size mismatch");
  for(float value:vector)
    if(!std::isfinite(value))
      fail("non-finite learner feature");
  return features;
  }

std::vector<float> NavPolicy::linear(const float* input, const Linear& layer, bool relu) {
  std::vector<float> result(layer.outputs);
  for(size_t out=0; out<layer.outputs; ++out) {
    const float value = dot(input,layer.weight.data()+out*layer.inputs,layer.inputs)+layer.bias[out];
    result[out] = relu ? std::max(value,0.f) : value;
    }
  return result;
  }

std::array<float,64> NavPolicy::map(const float* input, size_t rows, size_t columns, const MapEncoder& encoder) {
  const auto convolution = [](const float* data, size_t height, size_t width, const Linear& layer) {
    const size_t outputHeight = (height+1)/2, outputWidth = (width+1)/2;
    const size_t area = outputHeight*outputWidth, channels = layer.inputs/9;
    std::vector<float> result(layer.outputs*area);
    // A zero-padded window is shared by every output channel. OIHW tap order
    // agrees with the reference's im2col cross-correlation (no kernel flip).
    std::array<float,11*3*3> window;
    for(size_t z=0; z<outputHeight; ++z) {
      for(size_t x=0; x<outputWidth; ++x) {
        size_t tap = 0;
        for(size_t channel=0; channel<channels; ++channel) {
          for(int dz=-1; dz<=1; ++dz) {
            const int sourceZ = int(2*z)+dz;
            for(int dx=-1; dx<=1; ++dx) {
              const int sourceX = int(2*x)+dx;
              window[tap++] = sourceZ>=0 && sourceX>=0 && sourceZ<int(height) && sourceX<int(width) ?
                              data[(channel*height+size_t(sourceZ))*width+size_t(sourceX)] : 0.f;
              }
            }
          }
        for(size_t out=0; out<layer.outputs; ++out)
          result[out*area+z*outputWidth+x] = std::max(0.f,dot(window.data(),layer.weight.data()+out*layer.inputs,
                                                          layer.inputs)+layer.bias[out]);
        }
      }
    return result;
    };
  auto first = convolution(input,rows,columns,encoder.first);
  rows = (rows+1)/2;columns = (columns+1)/2;
  auto second = convolution(first.data(),rows,columns,encoder.second);
  rows = (rows+1)/2;columns = (columns+1)/2;
  std::array<float,64> result;
  for(size_t channel=0; channel<16; ++channel) {
    for(size_t z=0; z<2; ++z) {
      const size_t startZ = z*rows/2, endZ = ((z+1)*rows+1)/2;
      for(size_t x=0; x<2; ++x) {
        const size_t startX = x*columns/2, endX = ((x+1)*columns+1)/2;
        float sum = 0;
        for(size_t sourceZ=startZ; sourceZ<endZ; ++sourceZ)
          for(size_t sourceX=startX; sourceX<endX; ++sourceX)
            sum += second[(channel*rows+sourceZ)*columns+sourceX];
        result[channel*4+z*2+x] = sum/float((endZ-startZ)*(endX-startX));
        }
      }
    }
  return result;
  }

NavPolicy::Result NavPolicy::step(const Features& features, State& state) const {
  if(features.near.size()!=nearInputSize() || features.far.size()!=farInputSize() ||
     features.vector.size()!=vectorInputSize())
    fail("incorrect learner feature size");
  for(float value:features.near)
    if(!std::isfinite(value)) fail("non-finite near map feature");
  for(float value:features.far)
    if(!std::isfinite(value)) fail("non-finite far map feature");
  for(float value:features.vector)
    if(!std::isfinite(value)) fail("non-finite vector feature");
  if(state.hidden.empty())
    state.hidden.assign(hidden,0.f);
  if(state.hidden.size()!=hidden)
    fail("incorrect GRU state size");
  for(float value:state.hidden)
    if(!std::isfinite(value)) fail("non-finite GRU state");
  const auto near = map(features.near.data(),formatVersion>=3 ? 64 : 32,formatVersion>=3 ? 64 : 32,nearEncoder);
  const auto far = map(features.far.data(),formatVersion>=3 ? 50 : 25,formatVersion>=3 ? 100 : 25,farEncoder);
  const auto vector = linear(features.vector.data(),vectorEncoder,true);
  std::array<float,256> joined;
  std::copy(near.begin(),near.end(),joined.begin());
  std::copy(far.begin(),far.end(),joined.begin()+64);
  std::copy(vector.begin(),vector.end(),joined.begin()+128);
  const auto fused = linear(joined.data(),fusion,true);
  const auto inputGates = linear(fused.data(),gruInput);
  const auto hiddenGates = linear(state.hidden.data(),gruHidden);
  for(size_t i=0; i<hidden; ++i) {
    const float reset = sigmoid(inputGates[i]+hiddenGates[i]);
    const float update = sigmoid(inputGates[hidden+i]+hiddenGates[hidden+i]);
    const float next = std::tanh(inputGates[2*hidden+i]+reset*hiddenGates[2*hidden+i]);
    state.hidden[i] = (1.f-update)*next+update*state.hidden[i];
    }
  Result result;
  std::array<size_t,headCount> action = {};
  for(size_t i=0; i<actionHeadCount(); ++i) {
    result.logits[i] = linear(state.hidden.data(),heads[i]);
    action[i] = size_t(std::max_element(result.logits[i].begin(),result.logits[i].end())-result.logits[i].begin());
    }
  result.keys.forward = float(int(action[0])-1);
  result.keys.strafe = float(int(action[1])-1);
  result.keys.turn = float(int(action[2])-1);
  result.keys.walk = action[3]!=0;
  result.keys.jump = formatVersion>=2 ? action[4]==1 : action[4]!=0;
  result.keys.climb = formatVersion>=2 ? action[4]==2 : action[5]!=0;
  // Head yaw and pitch stay at the KeyState default: straight ahead.
  return result;
  }

NavPolicy::Result NavPolicy::step(const NavDemoWriter::Snapshot& observation, State& state) const {
  return step(encode(observation),state);
  }
