#include "baseimport.h"

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "definitionjson.h"
#include "game/definitions/musicdefinitiondata.h"
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

void BaseImport::convertMusicThemes(const std::filesystem::path& base) {
  using DefinitionJson::require;
  using namespace MusicDefinitionData;
  using namespace daedalus;

  const auto index = ProgramData::readIndex(base/"programs");
  const auto* source = ProgramData::musicProgram(index);
  if(source==nullptr || source->recordSha256!=stockProgram)
    return;

  std::string sha;
  auto program = ProgramData::readProgram(index.path(*source),&sha);
  require(sha==source->recordSha256,"music themes: program record differs from index");

  auto script = DaedalusScript::from_program(std::move(program));
  IMusicTheme::register_(script);
  DaedalusVm vm(std::move(script),DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  vm.observe_externals([](const auto& symbol) {
    throw std::runtime_error("music theme calls an engine function: "+symbol.name());
    });
  vm.observe_reads([](const auto& symbol) {
    const auto type = symbol.type();
    const bool constant = symbol.is_const() && (type==DaedalusDataType::INT || type==DaedalusDataType::STRING);
    require(symbol.is_member() || constant,"music theme reads shared script state: "+symbol.name());
    });
  vm.observe_writes([](const auto& symbol) {
    require(symbol.is_member(),"music theme writes shared script state: "+symbol.name());
    });
  const DaedalusInstance* owner = nullptr;
  vm.observe_member_reads([&](const auto& symbol, uint16_t, const auto* instance) {
    require(instance==owner,"music theme reads another instance: "+symbol.name());
    });
  vm.observe_member_writes([&](const auto& symbol, uint16_t, const auto* instance) {
    require(instance==owner,"music theme writes another instance: "+symbol.name());
    });

  Themes themes;
  themes.reserve(stockThemeCount);
  vm.enumerate_instances_by_class_name("C_MUSICTHEME",[&](auto& symbol) {
    const auto instance = std::make_shared<IMusicTheme>();
    owner = instance.get();
    vm.init_instance(instance,&symbol);
    Def::MusicTheme value{};
#define MUSIC_COPY(type, field, extent, member) value.field = static_cast<type>(instance->field);
    DEF_MUSIC_THEME_FIELDS(MUSIC_COPY)
#undef MUSIC_COPY
    themes.push_back({symbol.index(),symbol.name(),std::move(value)});
    });
  require(themes.size()==stockThemeCount,"music themes: unexpected stock theme count");

  write(base,sha,themes);
  const auto roundTrip = read(base,sha);
  require(roundTrip.size()==themes.size(),"music themes: theme count differs after reading");
  for(size_t i=0; i<themes.size(); ++i) {
    const auto& [id,name,expected] = themes[i];
    const auto& [actualId,actualName,actual] = roundTrip[i];
    require(id==actualId && name==actualName,"music themes: theme identity differs after reading");
#define MUSIC_COMPARE(type, field, extent, member) \
    require(sameField(expected.field,actual.field),"music themes: field " #field " differs after reading "+name);
    DEF_MUSIC_THEME_FIELDS(MUSIC_COMPARE)
#undef MUSIC_COMPARE
    }
  }
