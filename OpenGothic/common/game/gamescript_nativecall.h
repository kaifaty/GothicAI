#pragma once

#include "storygraph/nativeexec.h"
#include "world/records/recordinstance.h"

#include <concepts>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace storygraph {

std::string nativeOperationName(std::string_view name);

struct NativeExternal final {
  using Instance = std::shared_ptr<RecordInstance>;
  using ResolveActor = std::function<Instance(NativeInstance)>;
  using BindActor = std::function<NativeInstance(const Instance&)>;

  NativeFunction signature;
  std::function<NativeCall(std::span<const NativeValue>, const ResolveActor&, const BindActor&)> call;
  bool external=true;
  };

using NativeExternals=std::map<std::string,NativeExternal,std::less<>>;

namespace detail {

template<class T>
concept RecordPointer = std::same_as<T,std::shared_ptr<typename T::element_type>> &&
                        std::derived_from<typename T::element_type,RecordInstance>;

template<class T>
constexpr NativeValue::Type externalType() {
  if constexpr(std::is_integral_v<T>) {
    return NativeValue::Int;
    } else if constexpr(std::is_same_v<T,float>) {
    return NativeValue::Float;
    } else if constexpr(std::is_same_v<T,std::string_view> || std::is_same_v<T,std::string>) {
    return NativeValue::Str;
    } else {
    static_assert(RecordPointer<T>);
    return NativeValue::Inst;
    }
  }

template<class T>
T externalArgument(const NativeValue& value, const NativeExternal::ResolveActor& resolve) {
  if constexpr(std::is_integral_v<T>) {
    return static_cast<T>(value.i);
    } else if constexpr(std::is_same_v<T,float>) {
    return value.f;
    } else if constexpr(std::is_same_v<T,std::string_view> || std::is_same_v<T,std::string>) {
    return value.s;
    } else {
    using Instance = typename T::element_type;
    auto instance = resolve(value.inst);
    if(value.inst!=0 && instance==nullptr)
      throw std::runtime_error("Unbound instance argument");
    auto record = std::dynamic_pointer_cast<Instance>(instance);
    if(instance!=nullptr && record==nullptr)
      throw std::runtime_error("Instance argument has the wrong type");
    return record;
    }
  }

}

template<class R, class... P>
NativeExternal makeNativeExternal(const std::function<R(P...)>& callback) {
  NativeExternal external;
  external.signature.parameters = {detail::externalType<P>()...};
  if constexpr(!std::is_void_v<R>)
    external.signature.result = detail::externalType<R>();
  external.call = [callback](std::span<const NativeValue> args, const NativeExternal::ResolveActor& resolve,
                             const NativeExternal::BindActor& bind) -> NativeCall {
    const auto parameters = [&]<size_t... I>(std::index_sequence<I...>) {
      return std::tuple<P...>{detail::externalArgument<P>(args[I],resolve)...};
      }(std::index_sequence_for<P...>{});
    if constexpr(std::is_void_v<R>) {
      std::apply(callback,parameters);
      return {};
      } else if constexpr(detail::RecordPointer<R>) {
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
NativeExternal makeNativeInternal(const std::function<R(P...)>& callback) {
  auto operation = makeNativeExternal(callback);
  operation.external = false;
  return operation;
  }

}
