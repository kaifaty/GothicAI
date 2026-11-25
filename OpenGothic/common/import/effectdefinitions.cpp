#include "baseimport.h"

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "definitionjson.h"
#include "game/definitions/effectdefinitiondata.h"
#include "programdata.h"

#include <algorithm>
#include <bit>
#include <type_traits>

namespace {
using DefinitionJson::require;
using namespace EffectDefinitionData;
using namespace daedalus;

template<class T, class Source, class Visit>
void visitFields(T& value, const Source& source, Visit&& visit) {
  using Record = std::remove_const_t<T>;
#define EFFECT_FIELD(type, field, extent, symbol) visit(value.field,source.field,#field);
  if constexpr (std::is_same_v<Record,Def::ParticleEffect>) {
    DEF_PARTICLE_EFFECT_FIELDS(EFFECT_FIELD)
    }
  else if constexpr (std::is_same_v<Record,Def::EffectBase>) {
    DEF_EFFECT_BASE_FIELDS(EFFECT_FIELD)
    }
  else {
    DEF_PARTICLE_EFFECT_EMIT_KEY_FIELDS(EFFECT_FIELD)
    }
#undef EFFECT_FIELD
  }

template<class T>
  requires (!std::is_array_v<T>)
void copyField(T& field, const T& from) {
  field = from;
  }

template<size_t N>
void copyField(std::string (&field)[N], const std::string (&from)[N]) {
  std::copy_n(from,N,field);
  }

template<class T>
bool sameField(const T& expected, const T& actual) {
  if constexpr (std::is_same_v<T,float>)
    return std::bit_cast<uint32_t>(expected)==std::bit_cast<uint32_t>(actual);
  else if constexpr (std::is_array_v<T>)
    return std::equal(expected,expected+std::extent_v<T>,actual);
  else
    return expected==actual;
  }

DaedalusScript readScript(const ProgramData::Index& index, const ProgramData::Source& source) {
  std::string sha;
  auto program = ProgramData::readProgram(index.path(source),&sha);
  require(sha==source.recordSha256,"effect definitions: program record differs from index");
  return DaedalusScript::from_program(std::move(program));
  }

void restrictInitializer(DaedalusVm& vm, const DaedalusInstance*& owner) {
  vm.observe_externals([](const auto& symbol) {
    throw std::runtime_error("effect definition calls an engine function: "+symbol.name());
    });
  vm.observe_reads([](const auto& symbol) {
    const bool stringConstant = symbol.type()==DaedalusDataType::STRING && symbol.is_const();
    require(symbol.is_member() || stringConstant,"effect definition reads shared script state: "+symbol.name());
    });
  vm.observe_writes([](const auto& symbol) {
    require(symbol.is_member(),"effect definition writes shared script state: "+symbol.name());
    });
  vm.observe_member_reads([&](const auto& symbol, uint16_t, const auto* instance) {
    require(instance==owner,"effect definition reads another instance: "+symbol.name());
    });
  vm.observe_member_writes([&](const auto& symbol, uint16_t, const auto* instance) {
    require(instance==owner,"effect definition writes another instance: "+symbol.name());
    });
  }

template<class T, class Binding>
std::vector<Entry<T>> capture(DaedalusVm& vm, const DaedalusInstance*& owner,
                             std::string_view className, size_t expectedCount) {
  std::vector<Entry<T>> result;
  result.reserve(expectedCount);
  vm.enumerate_instances_by_class_name(className,[&](auto& symbol) {
    const auto instance = std::make_shared<Binding>();
    if constexpr (std::is_same_v<T,Def::ParticleEffect>)
      instance->vis_tex_is_quadpoly = 1;
    owner = instance.get();
    vm.init_instance(instance,&symbol);
    T value{};
    visitFields(value,*instance,[](auto& field, const auto& from, const char*) { copyField(field,from); });
    result.push_back({symbol.index(),symbol.name(),std::move(value)});
    });
  require(result.size()==expectedCount,"effect definitions: unexpected stock count for "+std::string(className));
  return result;
  }

template<class T>
void verifyRoundTrip(const std::vector<Entry<T>>& expected, const std::vector<Entry<T>>& actual) {
  require(actual.size()==expected.size(),"effect definitions: count differs after reading");
  for(size_t i=0; i<expected.size(); ++i) {
    const auto& [id,name,value] = expected[i];
    const auto& [actualId,actualName,actualValue] = actual[i];
    require(id==actualId && name==actualName,"effect definitions: identity differs after reading");
    visitFields(value,actualValue,[&](const auto& a, const auto& b, const char* field) {
      require(sameField(a,b),"effect definitions: field "+std::string(field)+" differs after reading "+name);
      });
    }
  }

void convertParticles(const std::filesystem::path& base, const ProgramData::Index& index) {
  const auto* source = ProgramData::particleFxProgram(index);
  if(source==nullptr || source->recordSha256!=stockParticleProgram)
    return;
  auto script = readScript(index,*source);
  IParticleEffect::register_(script);
  DaedalusVm vm(std::move(script),DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  const DaedalusInstance* owner = nullptr;
  restrictInitializer(vm,owner);

  const auto effects = capture<Def::ParticleEffect,IParticleEffect>(vm,owner,"C_PARTICLEFX",stockParticleCount);
  writeParticles(base,stockParticleProgram,effects);
  verifyRoundTrip(effects,readParticles(base,stockParticleProgram));
  }

void convertVisuals(const std::filesystem::path& base, const ProgramData::Index& index) {
  const auto* source = ProgramData::visualFxProgram(index);
  if(source==nullptr || source->recordSha256!=stockVisualProgram)
    return;
  auto script = readScript(index,*source);
  IEffectBase::register_(script);
  IParticleEffectEmitKey::register_(script);
  DaedalusVm vm(std::move(script),DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  const DaedalusInstance* owner = nullptr;
  restrictInitializer(vm,owner);

  const Visuals effects = {capture<Def::EffectBase,IEffectBase>(vm,owner,"CFX_BASE",stockVisualCount),
                           capture<Def::ParticleEffectEmitKey,IParticleEffectEmitKey>(vm,owner,
                                                                                      "C_PARTICLEFXEMITKEY",stockKeyCount)};
  writeVisuals(base,stockVisualProgram,effects);
  const auto actual = readVisuals(base,stockVisualProgram);
  verifyRoundTrip(effects.effects,actual.effects);
  verifyRoundTrip(effects.keys,actual.keys);
  }
}

void BaseImport::convertEffectDefinitions(const std::filesystem::path& base) {
  const auto index = ProgramData::readIndex(base/"programs");
  convertParticles(base,index);
  convertVisuals(base,index);
  }
