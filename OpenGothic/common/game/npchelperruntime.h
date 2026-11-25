#pragma once

#include "asset/npchelperdefinitions.h"
#include "daedalus/classes.h"
#include "daedalus/vm.h"

#include <filesystem>
#include <functional>
#include <unordered_map>

namespace NativeBaseCatalog { struct Selection; }

namespace NpcHelperRuntime {
struct Definitions {
  std::unordered_map<uint32_t,NpcHelperDefinitions::Definition> helpers;
  std::map<std::string,std::pair<uint32_t,uint8_t>> literals;
  };
bool enabled();
Definitions resolve(const NpcHelperDefinitions::Library& library,const daedalus::DaedalusScript& script);
Definitions load(const std::filesystem::path& base,const daedalus::DaedalusScript& script,std::string_view sha256);
Definitions resolve(const NpcHelperDefinitions::Library& library,const NativeBaseCatalog::Selection& catalog);
Definitions load(const std::filesystem::path& base,const NativeBaseCatalog::Selection& catalog,std::string_view sha256);
// Arguments have already been pushed by NPC record playback. A fallback leaves them untouched.
bool invoke(daedalus::DaedalusVm& vm,const std::shared_ptr<daedalus::INpc>& npc,
            const daedalus::DaedalusSymbol* function,const Definitions& definitions);
// The prepared path consumes record values directly. The optional observer
// receives the same argument frames as the oracle path without executing it.
using InvokeExternal=std::function<int32_t(const daedalus::DaedalusSymbol&, std::span<const NpcHelperDefinitions::Value>,
                                           const std::function<void()>& observe)>;
bool invokePrepared(daedalus::DaedalusVm& vm,const std::shared_ptr<daedalus::INpc>& npc,
                    uint32_t function,const Definitions& definitions,std::span<const NpcHelperDefinitions::Value> args,
                    const InvokeExternal& invokeExternal={});
bool invokePrepared(uint32_t function,const Definitions& definitions,
                    std::span<const NpcHelperDefinitions::Value> args,NpcHelperDefinitions::Host& host);
}
