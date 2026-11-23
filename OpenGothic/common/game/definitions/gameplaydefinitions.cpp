#include "gameplaydefinitions.h"

using namespace DefinitionJson;
using daedalus::DaedalusDataType;

std::shared_ptr<const GameplayDefinitions> GameplayDefinitions::load(const std::filesystem::path& base) {
  const auto path = section(base);
  if(!std::filesystem::exists(path))
    return {};
  auto rows = DefinitionJson::read(path);
  object(rows,{"format","version","program_sha256","instances"});
  require(string(rows["format"])==format && integer(rows["version"])==version &&
          string(rows["program_sha256"])==stockProgram && rows["instances"].IsObject(),
          "gameplay definitions: invalid format or program in "+path.string());
  return std::shared_ptr<const GameplayDefinitions>(new GameplayDefinitions(std::move(rows)));
  }

void GameplayDefinitions::restore(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& symbol,
                                  daedalus::DaedalusInstance& instance) const {
  const auto& row = at(rows["instances"],symbol.name().c_str());
  const auto members = vm.find_class_members(instanceClass(vm,symbol));
  require(row.IsObject() && row.MemberCount()==members.size(),
          "gameplay definitions: unexpected member count for "+symbol.name());
  for(auto* member : members) {
    const auto& values = at(row,member->name().c_str());
    require(values.IsArray() && values.Size()==member->count(),
            "gameplay definitions: unexpected value count for "+member->name());
    for(uint16_t index=0; index<member->count(); ++index) {
      const auto& value = values[index];
      switch(member->type()) {
        case DaedalusDataType::INT: member->set_int(integer(value),index,&instance); break;
        case DaedalusDataType::FLOAT:
          require(value.IsFloat(),"gameplay definitions: expected a float for "+member->name());
          member->set_float(value.GetFloat(),index,&instance);
          break;
        case DaedalusDataType::STRING: member->set_string(bytes(value),index,&instance); break;
        default: throw std::runtime_error("gameplay definitions: unsupported member "+member->name());
        }
      }
    }
  }
