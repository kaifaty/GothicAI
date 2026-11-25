#include "baseimport.h"

#include "daedalus/classes.h"
#include "daedalus/vm.h"
#include "definitionjson.h"
#include "game/definitions/cameradefinitiondata.h"
#include "programdata.h"

#include <bit>

void BaseImport::convertCameraDefinitions(const std::filesystem::path& base) {
  using DefinitionJson::require;
  using namespace CameraDefinitionData;
  using namespace daedalus;

  const auto index = ProgramData::readIndex(base/"programs");
  const auto* source = ProgramData::cameraProgram(index);
  if(source==nullptr || source->recordSha256!=stockProgram)
    return;

  std::string sha;
  auto program = ProgramData::readProgram(index.path(*source),&sha);
  require(sha==source->recordSha256,"camera definitions: program record differs from index");

  auto script = DaedalusScript::from_program(std::move(program));
  ICamera::register_(script);
  DaedalusVm vm(std::move(script),DaedalusVmExecutionFlag::ALLOW_NULL_INSTANCE_ACCESS);
  vm.observe_externals([](const auto& symbol) {
    throw std::runtime_error("camera definition calls an engine function: "+symbol.name());
    });
  vm.observe_reads([](const auto& symbol) {
    require(symbol.is_member(),"camera definition reads shared script state: "+symbol.name());
    });
  vm.observe_writes([](const auto& symbol) {
    require(symbol.is_member(),"camera definition writes shared script state: "+symbol.name());
    });
  const DaedalusInstance* owner = nullptr;
  vm.observe_member_reads([&](const auto& symbol, uint16_t, const auto* instance) {
    require(instance==owner,"camera definition reads another instance: "+symbol.name());
    });
  vm.observe_member_writes([&](const auto& symbol, uint16_t, const auto* instance) {
    require(instance==owner,"camera definition writes another instance: "+symbol.name());
    });

  Presets presets;
  vm.enumerate_instances_by_class_name("CCAMSYS",[&](auto& symbol) {
    const auto instance = std::make_shared<ICamera>();
    owner = instance.get();
    vm.init_instance(instance,&symbol);
    Def::Camera camera{};
#define CAMERA_COPY(type, field, extent, member) camera.field = instance->field;
    DEF_CAMERA_FIELDS(CAMERA_COPY)
#undef CAMERA_COPY
    presets.emplace_back(symbol.name(),camera);
    });
  require(presets.size()==stockPresetCount,"camera definitions: unexpected stock preset count");

  write(base,sha,presets);
  const auto roundTrip = read(base,sha);
  require(roundTrip.size()==presets.size(),"camera definitions: preset count differs after reading");
  for(size_t i=0; i<presets.size(); ++i) {
    const auto& [name,expected] = presets[i];
    const auto& [actualName,actual] = roundTrip[i];
    require(name==actualName,"camera definitions: preset order differs after reading");
#define CAMERA_COMPARE(type, field, extent, member) \
    require(std::bit_cast<uint32_t>(expected.field)==std::bit_cast<uint32_t>(actual.field), \
            "camera definitions: field " #field " differs after reading "+name);
    DEF_CAMERA_FIELDS(CAMERA_COMPARE)
#undef CAMERA_COMPARE
    }
  }
