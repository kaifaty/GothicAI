#pragma once

#include "storygraph/nativeexec.h"

#include <memory>
#include <span>

namespace daedalus {
  class DaedalusInstance;
  class DaedalusSymbol;
  class DaedalusVm;
}

namespace storygraph {

class BoundNativeCall final {
  public:
    using Instance = std::shared_ptr<daedalus::DaedalusInstance>;
    using ResolveActor = std::function<Instance(NativeInstance)>;
    using BindActor = std::function<NativeInstance(const Instance&)>;
    using MapReference = std::function<std::optional<NativeSlot>(const daedalus::DaedalusSymbol&, uint16_t, const Instance&)>;

    BoundNativeCall(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& symbol);
    const NativeFunction& signature() const { return function; }
    bool active() const;
    std::optional<NativeCall> invoke(std::span<const NativeValue> args, const ResolveActor& resolve,
                                     const BindActor& bind, const MapReference& map, std::string& error) const;

  private:
    daedalus::DaedalusVm& vm;
    daedalus::DaedalusSymbol& symbol;
    NativeFunction function;
  };

}
