#pragma once

#include "daedalus/vm.h"
#include "storygraph/nativeexec.h"

#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace storygraph {

NativeValue::Type nativeValueType(daedalus::DaedalusDataType type);

class BoundNativeCall final {
  public:
    using Instance = std::shared_ptr<daedalus::DaedalusInstance>;
    using ResolveActor = std::function<Instance(NativeInstance)>;
    using BindActor = std::function<NativeInstance(const Instance&)>;
    using MapReference = std::function<std::optional<NativeSlot>(const daedalus::DaedalusSymbol&, uint16_t, const Instance&)>;

    BoundNativeCall(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& symbol);
    const NativeFunction& signature() const { return function; }
    bool active() const;
    static void observeExternal(daedalus::DaedalusVm& vm, const daedalus::DaedalusSymbol& symbol,
                                std::span<const NativeValue> args, const ResolveActor& resolve);
    std::optional<NativeCall> invoke(std::span<const NativeValue> args, const ResolveActor& resolve,
                                     const BindActor& bind, const MapReference& map, std::string& error) const;

  private:
    daedalus::DaedalusVm& vm;
    daedalus::DaedalusSymbol& symbol;
    NativeFunction function;
  };

struct NativeExternal final {
  NativeFunction signature;
  std::function<NativeCall(std::span<const NativeValue>, const BoundNativeCall::ResolveActor&,
                           const BoundNativeCall::BindActor&)> call;
  bool observe=true;
  };

using NativeExternals=std::map<std::string,NativeExternal,std::less<>>;

namespace detail {

template<class T>
constexpr NativeValue::Type externalType() {
  if constexpr(std::is_integral_v<T>) {
    return NativeValue::Int;
    } else if constexpr(std::is_same_v<T,float>) {
    return NativeValue::Float;
    } else if constexpr(std::is_same_v<T,std::string_view> || std::is_same_v<T,std::string>) {
    return NativeValue::Str;
    } else {
    static_assert(daedalus::DaedalusInstancePointer<T>);
    return NativeValue::Inst;
    }
  }

template<class T>
T externalArgument(const NativeValue& value, const BoundNativeCall::ResolveActor& resolve) {
  if constexpr(std::is_integral_v<T>) {
    return static_cast<T>(value.i);
    } else if constexpr(std::is_same_v<T,float>) {
    return value.f;
    } else if constexpr(std::is_same_v<T,std::string_view>) {
    return value.s;
    } else {
    using Instance = typename T::element_type;
    auto instance = resolve(value.inst);
    if(value.inst!=0 && instance==nullptr)
      throw std::runtime_error("Unbound instance argument");
    if constexpr(!std::is_same_v<Instance,daedalus::DaedalusInstance>) {
      if(instance!=nullptr && (instance->instance_type()==nullptr || *instance->instance_type()!=typeid(Instance)))
        throw std::runtime_error("Instance argument has the wrong type");
      }
    return std::static_pointer_cast<Instance>(std::move(instance));
    }
  }

}

template<class R, class... P>
NativeExternal makeNativeExternal(const std::function<R(P...)>& callback,bool observe=true) {
  NativeExternal external;
  external.observe=observe;
  external.signature.parameters = {detail::externalType<P>()...};
  if constexpr(!std::is_void_v<R>)
    external.signature.result = detail::externalType<R>();
  external.call = [callback](std::span<const NativeValue> args, const BoundNativeCall::ResolveActor& resolve,
                             const BoundNativeCall::BindActor& bind) -> NativeCall {
    const auto parameters = [&]<size_t... I>(std::index_sequence<I...>) {
      return std::tuple<P...>{detail::externalArgument<P>(args[I],resolve)...};
      }(std::index_sequence_for<P...>{});
    if constexpr(std::is_void_v<R>) {
      std::apply(callback,parameters);
      return {};
      } else if constexpr(daedalus::DaedalusInstancePointer<R>) {
      return {NativeValue::instance(bind(std::apply(callback,parameters))),{}};
      } else if constexpr(std::is_same_v<R,float>) {
      return {NativeValue::real(std::apply(callback,parameters)),{}};
      } else if constexpr(std::is_same_v<R,std::string>) {
      return {NativeValue::str(std::apply(callback,parameters)),{}};
      } else {
      static_assert(std::is_integral_v<R>);
      return {NativeValue::integer(int32_t(std::apply(callback,parameters))),{}};
      }
    };
  return external;
  }

template<class R, class... P>
void registerNativeExternal(daedalus::DaedalusVm& vm,NativeExternals* registry,std::string_view name,
                            const std::function<R(P...)>& callback,bool observe=true) {
  vm.register_external(name,callback);
  if(registry==nullptr)
    return;
  const auto* symbol=vm.find_symbol_by_name(name);
  // Function locals require the VM's call frame.
  if(symbol==nullptr || symbol->has_local_variables_enabled())
    return;
  registry->insert_or_assign(symbol->name(),makeNativeExternal(callback,observe));
  }

}
