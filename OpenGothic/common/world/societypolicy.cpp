#include "societypolicy.h"

#include "game/serialize.h"

#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

const rapidjson::Value& field(const rapidjson::Value& header, const char* name) {
  const auto it = header.FindMember(name);
  if(it==header.MemberEnd())
    throw std::invalid_argument(std::string("society policy: missing field ")+name);
  return it->value;
  }

bool equal(const rapidjson::Value& value, std::string_view expected) {
  return value.IsString() && std::string_view(value.GetString(),value.GetStringLength())==expected;
  }

bool matches(const rapidjson::Value& values, const std::vector<std::string>& names) {
  if(!values.IsArray() || values.Size()!=names.size())
    return false;
  size_t index = 0;
  for(const auto& value:values.GetArray()) {
    if(!equal(value,names[index]))
      return false;
    ++index;
    }
  return true;
  }

SmallNet::Linear layer(const SmallNet::TensorFile& file, const char* weight, const char* bias,
                       size_t inputs, size_t outputs) {
  return {inputs,outputs,file.tensor(weight,{outputs,inputs}).values,file.tensor(bias,{outputs}).values};
  }

}

SocietyPolicy::SocietyPolicy(const std::filesystem::path& path, const Society::World& world)
  : menuSize(world.config.decisions.menu), hidden(world.npcs.size()) {
  const auto layout = Society::featureLayout(world);
  const SmallNet::TensorFile file(path);
  const auto& header = file.header;
  const auto& version = field(header,"version");
  const auto& featureVersion = field(header,"feature_version");
  const auto& declaredMenu = field(header,"menu_size");
  if(!equal(field(header,"format"),"gothic-society-net") ||
     !equal(field(header,"architecture"),"gru128-dot") || !version.IsUint() || version.GetUint()!=1 ||
     !featureVersion.IsUint() || featureVersion.GetUint()!=Society::FeatureVersion ||
     !declaredMenu.IsUint() || declaredMenu.GetUint()!=menuSize || !matches(field(header,"own_feature_names"),layout.own) ||
     !matches(field(header,"option_feature_names"),layout.options))
    throw std::invalid_argument("society policy: incompatible weight file or feature layout");
  input  = layer(file,"memory.weight_ih","memory.bias_ih",layout.own.size(),3*Hidden);
  memory = layer(file,"memory.weight_hh","memory.bias_hh",Hidden,3*Hidden);
  option = layer(file,"option.weight","option.bias",layout.options.size(),Hidden);
  for(const auto* tensor : {&input.weight,&input.bias,&memory.weight,&memory.bias,&option.weight,&option.bias}) {
    for(const auto value : *tensor) {
      const auto bits = std::bit_cast<uint32_t>(value);
      for(unsigned bit = 0; bit<32; bit+=8) {
        weightsHash ^= (bits >> bit) & 0xffu;
        weightsHash *= 1099511628211ull;
        }
      }
    }
  }

void SocietyPolicy::choose(const Society::DecisionBatch& batch, std::vector<Society::Choice>& out) {
  const size_t n = batch.size();
  if(batch.stride!=menuSize || batch.mask.size()!=n*menuSize || batch.own.size()!=n*input.inputs ||
     batch.options.size()!=n*menuSize*option.inputs)
    throw std::invalid_argument("society policy: invalid decision batch layout");
  out.clear();
  out.reserve(n);
  for(size_t row=0;row<n;++row) {
    const uint32_t npc = batch.npc[row];
    auto& previous = hidden.at(npc);
    if(previous.empty())
      previous.resize(Hidden,0.f);
    auto next = SmallNet::gruCell(batch.own.data()+row*input.inputs,previous,input,memory);
    size_t best = menuSize;
    float bestScore = -std::numeric_limits<float>::infinity();
    for(size_t i=0;i<menuSize;++i) {
      const size_t index = row*menuSize+i;
      if(batch.mask[index]==0)
        continue;
      const auto embedding = SmallNet::linear(batch.options.data()+index*option.inputs,option,true);
      float score = 0.f;
      for(size_t channel=0;channel<Hidden;++channel)
        score += next[channel]*embedding[channel];
      if(!std::isfinite(score))
        throw std::runtime_error("society policy: nonfinite action score");
      if(score>bestScore) {
        best = i;
        bestScore = score;
        }
      }
    if(best==menuSize)
      throw std::invalid_argument("society policy: no allowed action");
    previous = std::move(next);
    out.push_back({npc,uint16_t(best),Society::Chooser::Net});
    }
  }

void SocietyPolicy::saveState(Serialize& save) const {
  save.write(weightsHash,uint32_t(hidden.size()));
  for(const auto& memoryState : hidden) {
    save.write(uint8_t(!memoryState.empty()));
    if(!memoryState.empty())
      save.writeBytes(memoryState.data(),Hidden*sizeof(float));
    }
  }

void SocietyPolicy::restoreState(Serialize& save) {
  uint64_t hash = 0;
  uint32_t actors = 0;
  save.read(hash,actors);
  if(hash!=weightsHash || actors!=hidden.size())
    throw std::invalid_argument("society policy: saved weights or actor count mismatch");
  std::vector<std::vector<float>> restored(actors);
  for(auto& memoryState : restored) {
    uint8_t present = 0;
    save.read(present);
    if(present>1)
      throw std::invalid_argument("society policy: invalid saved hidden-state flag");
    if(present==0)
      continue;
    memoryState.resize(Hidden);
    save.readBytes(memoryState.data(),Hidden*sizeof(float));
    for(const auto value : memoryState)
      if(!std::isfinite(value) || std::abs(value)>1.00001f)
        throw std::invalid_argument("society policy: invalid saved hidden state");
    }
  hidden = std::move(restored);
  }
