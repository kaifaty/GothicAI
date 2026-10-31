#include "smallnet.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace {
using Json = rapidjson::Value;

[[noreturn]] void fail(const std::string& reason) {
  throw std::runtime_error("small net: "+reason);
  }

const Json& field(const Json& object, const char* name) {
  if(!object.IsObject() || !object.HasMember(name))
    fail(std::string("missing tensor header field: ")+name);
  return object[name];
  }

bool equal(const Json& value, std::string_view expected) {
  return value.IsString() && std::string_view(value.GetString(),value.GetStringLength())==expected;
  }

bool uniqueKeys(const Json& value) {
  if(value.IsObject()) {
    std::unordered_set<std::string> names;
    for(const auto& member:value.GetObject())
      if(!names.emplace(member.name.GetString(),member.name.GetStringLength()).second || !uniqueKeys(member.value))
        return false;
    }
  else if(value.IsArray()) {
    for(const auto& item:value.GetArray())
      if(!uniqueKeys(item))
        return false;
    }
  return true;
  }

float dot(const float* left, const float* right, size_t size) {
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
}

SmallNet::TensorFile::TensorFile(const std::string& path) {
  static_assert(sizeof(float)==4 && std::numeric_limits<float>::is_iec559,
                "small nets require IEEE-754 float32");
  std::ifstream stream(path,std::ios::binary);
  if(!stream)
    fail("cannot open weight file: "+path);
  std::string line;
  char byte = 0;
  bool newline = false;
  while(line.size()<1024*1024 && stream.get(byte)) {
    if(byte=='\n') {
      newline = true;
      break;
      }
    line.push_back(byte);
    }
  if(!newline || line.size()>=1024*1024)
    fail("weight file has no bounded JSON header line");
  header.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseFullPrecisionFlag>(line.data(),line.size());
  if(header.HasParseError() || !header.IsObject() || !uniqueKeys(header))
    fail("invalid tensor JSON header (object with unique keys required)");
  if(!equal(field(header,"byte_order"),"little"))
    fail("unsupported tensor byte order");
  if(!equal(field(header,"offset_origin"),"first byte after JSON header newline"))
    fail("unsupported tensor offset origin");

  const std::vector<unsigned char> payload{std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
  if(stream.bad())
    fail("cannot read weight payload: "+path);
  const auto& dataBytes = field(header,"data_nbytes");
  if(!dataBytes.IsUint64() || dataBytes.GetUint64()!=payload.size())
    fail("data_nbytes differs from payload length (truncated or extra data)");
  const auto& entries = field(header,"tensors");
  if(!entries.IsArray())
    fail("tensor list required");
  size_t offset = 0;
  for(const auto& entry:entries.GetArray()) {
    const auto& jsonName = field(entry,"name");
    if(!jsonName.IsString())
      fail("tensor name must be a string");
    const std::string name(jsonName.GetString(),jsonName.GetStringLength());
    if(tensors.count(name)!=0)
      fail("duplicate tensor name: "+name);
    const auto& shape = field(entry,"shape");
    if(!shape.IsArray() || shape.Empty())
      fail("invalid tensor shape: "+name);
    if(!equal(field(entry,"dtype"),"float32"))
      fail("unsupported tensor dtype: "+name);
    Tensor value;
    size_t count = 1;
    for(const auto& dimension:shape.GetArray()) {
      if(!dimension.IsUint64() || dimension.GetUint64()==0 ||
         dimension.GetUint64()>std::numeric_limits<size_t>::max()/count)
        fail("invalid tensor shape: "+name);
      const auto size = size_t(dimension.GetUint64());
      count *= size;
      value.shape.push_back(size);
      }
    if(count>std::numeric_limits<size_t>::max()/4)
      fail("tensor byte count overflow: "+name);
    const size_t bytes = count*4;
    const auto& jsonOffset = field(entry,"offset");
    if(!jsonOffset.IsUint64() || jsonOffset.GetUint64()!=offset)
      fail("invalid tensor offset: "+name);
    if(bytes>payload.size()-offset)
      fail("truncated tensor: "+name);
    if(entry.HasMember("nbytes") && (!entry["nbytes"].IsUint64() || entry["nbytes"].GetUint64()!=bytes))
      fail("invalid tensor nbytes: "+name);
    value.values.resize(count);
    for(size_t i=0; i<count; ++i) {
      const auto* p = payload.data()+offset+4*i;
      const uint32_t bits = uint32_t(p[0]) | (uint32_t(p[1])<<8) | (uint32_t(p[2])<<16) | (uint32_t(p[3])<<24);
      std::memcpy(&value.values[i],&bits,sizeof(bits));
      if(!std::isfinite(value.values[i]))
        fail("nonfinite tensor: "+name);
      }
    offset += bytes;
    tensors.emplace(name,std::move(value));
    }
  if(offset!=payload.size())
    fail("data_nbytes differs from tensor byte count");
  }

const SmallNet::Tensor& SmallNet::TensorFile::tensor(const std::string& name, std::initializer_list<size_t> shape) const {
  const auto found = tensors.find(name);
  if(found==tensors.end())
    fail("missing tensor: "+name);
  if(found->second.shape!=std::vector<size_t>(shape))
    fail("tensor shape mismatch: "+name);
  return found->second;
  }

std::vector<float> SmallNet::linear(const float* input, const Linear& layer, bool relu) {
  if(layer.inputs==0 || layer.outputs==0 || layer.outputs>std::numeric_limits<size_t>::max()/layer.inputs ||
     layer.weight.size()!=layer.inputs*layer.outputs || layer.bias.size()!=layer.outputs)
    fail("invalid dense layer shape");
  std::vector<float> result(layer.outputs);
  for(size_t out=0; out<layer.outputs; ++out) {
    const float value = dot(input,layer.weight.data()+out*layer.inputs,layer.inputs)+layer.bias[out];
    result[out] = relu ? std::max(value,0.f) : value;
    }
  return result;
  }

float SmallNet::sigmoid(float value) {
  const float exp = std::exp(-std::abs(value));
  return value>=0 ? 1.f/(1.f+exp) : exp/(1.f+exp);
  }

std::vector<float> SmallNet::gruCell(const float* input, const std::vector<float>& hidden,
                                  const Linear& inputLayer, const Linear& hiddenLayer) {
  const size_t size = hidden.size();
  if(size==0 || size>std::numeric_limits<size_t>::max()/3 || hiddenLayer.inputs!=size ||
     inputLayer.outputs!=3*size || hiddenLayer.outputs!=3*size)
    fail("invalid GRU cell shape");
  const auto inputGates = linear(input,inputLayer);
  const auto hiddenGates = linear(hidden.data(),hiddenLayer);
  std::vector<float> result(size);
  for(size_t i=0; i<size; ++i) {
    const float reset = sigmoid(inputGates[i]+hiddenGates[i]);
    const float update = sigmoid(inputGates[size+i]+hiddenGates[size+i]);
    const float next = std::tanh(inputGates[2*size+i]+reset*hiddenGates[2*size+i]);
    result[i] = next+update*(hidden[i]-next);
    }
  return result;
  }
