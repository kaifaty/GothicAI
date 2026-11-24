#include "baseimport.h"

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "definitionjson.h"
#include "game/definitions/sfxdefinitiondata.h"
#include "programdata.h"

#include <bit>
#include <type_traits>

namespace {
template<class T>
bool sameField(const T& expected, const T& actual) {
  if constexpr (std::is_same_v<T,float>)
    return std::bit_cast<uint32_t>(expected)==std::bit_cast<uint32_t>(actual);
  return expected==actual;
  }
}

void BaseImport::convertSfxDefinitions(const std::filesystem::path& base) {
  using DefinitionJson::require;
  using namespace SfxDefinitionData;
  using namespace daedalus;

  const auto index = ProgramData::readIndex(base/"programs");
  const auto* source = ProgramData::sfxProgram(index);
  if(source==nullptr || source->recordSha256!=stockProgram)
    return;

  std::string sha;
  auto program = ProgramData::readProgram(index.path(*source),&sha);
  require(sha==source->recordSha256,"sfx definitions: program record differs from index");

  auto script = DaedalusScript::from_program(std::move(program));
  ISoundEffect::register_(script);
  DaedalusVm vm(std::move(script),DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  vm.observe_externals([](const auto& symbol) {
    throw std::runtime_error("sfx definition calls an engine function: "+symbol.name());
    });
  vm.observe_reads([](const auto& symbol) {
    const bool stringConstant = symbol.type()==DaedalusDataType::STRING && symbol.is_const();
    require(symbol.is_member() || stringConstant,"sfx definition reads shared script state: "+symbol.name());
    });
  vm.observe_writes([](const auto& symbol) {
    require(symbol.is_member(),"sfx definition writes shared script state: "+symbol.name());
    });
  const DaedalusInstance* owner = nullptr;
  vm.observe_member_reads([&](const auto& symbol, uint16_t, const auto* instance) {
    require(instance==owner,"sfx definition reads another instance: "+symbol.name());
    });
  vm.observe_member_writes([&](const auto& symbol, uint16_t, const auto* instance) {
    require(instance==owner,"sfx definition writes another instance: "+symbol.name());
    });

  Presets presets;
  presets.reserve(stockPresetCount);
  vm.enumerate_instances_by_class_name("C_SFX",[&](auto& symbol) {
    const auto instance = std::make_shared<ISoundEffect>();
    owner = instance.get();
    vm.init_instance(instance,&symbol);
    Def::SoundEffect sfx{};
#define SFX_COPY(type, field, extent, member) sfx.field = instance->field;
    DEF_SOUND_EFFECT_FIELDS(SFX_COPY)
#undef SFX_COPY
    presets.emplace_back(symbol.name(),std::move(sfx));
    });
  require(presets.size()==stockPresetCount,"sfx definitions: unexpected stock preset count");

  write(base,sha,presets);
  const auto roundTrip = read(base,sha);
  require(roundTrip.size()==presets.size(),"sfx definitions: preset count differs after reading");
  for(size_t i=0; i<presets.size(); ++i) {
    const auto& [name,expected] = presets[i];
    const auto& [actualName,actual] = roundTrip[i];
    require(name==actualName,"sfx definitions: preset order differs after reading");
#define SFX_COMPARE(type, field, extent, member) \
    require(sameField(expected.field,actual.field),"sfx definitions: field " #field " differs after reading "+name);
    DEF_SOUND_EFFECT_FIELDS(SFX_COMPARE)
#undef SFX_COMPARE
    }
  }
