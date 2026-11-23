#pragma once

#include "asset/npchelperdefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"

#include <filesystem>
#include <unordered_map>

namespace NpcHelperRuntime {
struct Definitions {
  std::unordered_map<uint32_t,NpcHelperDefinitions::Definition> helpers;
  std::map<std::string,std::pair<uint32_t,uint8_t>> literals;
  };
bool enabled();
Definitions resolve(const NpcHelperDefinitions::Library& library,const daedalus::DaedalusScript& script);
Definitions load(const std::filesystem::path& base,const daedalus::DaedalusScript& script,std::string_view sha256);
// Arguments have already been pushed by NPC record playback. A fallback leaves them untouched.
bool invoke(daedalus::DaedalusVm& vm,const std::shared_ptr<daedalus::INpc>& npc,
            const daedalus::DaedalusSymbol* function,const Definitions& definitions);
bool addFightSkill(daedalus::DaedalusVm& vm,const std::shared_ptr<daedalus::INpc>& npc,
                   int32_t talent,int32_t percent,const Definitions& definitions);
}
