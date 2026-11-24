#include "gamescript_nativecall.h"

#include "daedalus/vm.h"

#include <stdexcept>
#include <utility>

using namespace storygraph;

static NativeValue::Type valueType(daedalus::DaedalusDataType type) {
  using enum daedalus::DaedalusDataType;
  switch(type) {
    case FUNCTION:
    case INT:      return NativeValue::Int;
    case FLOAT:    return NativeValue::Float;
    case STRING:   return NativeValue::Str;
    case INSTANCE: return NativeValue::Inst;
    default:       throw std::runtime_error("Unsupported native function parameter or result type");
    }
  }

BoundNativeCall::BoundNativeCall(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol& symbol)
  : vm(vm), symbol(symbol) {
  if(symbol.type()!=daedalus::DaedalusDataType::FUNCTION)
    throw std::runtime_error("Cannot bind non-function " + symbol.name());
  for(const auto& parameter : vm.find_parameters_for_function(&symbol))
    function.parameters.push_back(valueType(parameter.type()));
  if(symbol.has_return())
    function.result = valueType(symbol.rtype());
  }

bool BoundNativeCall::active() const {
  return vm.has_bound_native(symbol);
  }

std::optional<NativeCall> BoundNativeCall::invoke(std::span<const NativeValue> args, const ResolveActor& resolve,
                                                const BindActor& bind, const MapReference& map, std::string& error) const {
  error.clear();
  if(args.size()!=function.parameters.size()) {
    error = "Incorrect argument count for " + symbol.name();
    return std::nullopt;
    }
  for(size_t index=0; index<args.size(); ++index) {
    if(args[index].type!=function.parameters[index]) {
      error = "Incorrect argument type for " + symbol.name() + " argument " + std::to_string(index+1);
      return std::nullopt;
      }
    }

  NativeCall result;
  try {
    const bool invoked = vm.call_bound_native(symbol, [&] {
      for(const auto& argument : args) {
        switch(argument.type) {
          case NativeValue::Int:   vm.push_int(argument.i); break;
          case NativeValue::Float: vm.push_float(argument.f); break;
          case NativeValue::Str:   vm.push_string(argument.s); break;
          case NativeValue::Inst: {
            auto actor = resolve(argument.inst);
            if(argument.inst!=0 && actor==nullptr)
              throw std::runtime_error("Unbound instance argument");
            vm.push_instance(std::move(actor));
            break;
            }
          }
        }
      }, [&] {
      if(!function.result)
        return;
      if(!symbol.is_external() && vm.top_is_named_reference()) {
        const auto [slot, index, context] = vm.pop_reference();
        result.reference = map(*slot, index, context);
        if(!result.reference)
          error = "Unbound reference returned by " + symbol.name() + ": " + slot->name();
        return;
        }
      switch(*function.result) {
        case NativeValue::Int:   result.value = NativeValue::integer(vm.pop_int()); break;
        case NativeValue::Float: result.value = NativeValue::real(vm.pop_float()); break;
        case NativeValue::Str:   result.value = NativeValue::str(vm.pop_string()); break;
        case NativeValue::Inst:  result.value = NativeValue::instance(bind(vm.pop_instance())); break;
        }
      });
    if(!invoked)
      error = "No active native implementation for " + symbol.name();
    }
  catch(const std::exception& ex) {
    error = "Native function " + symbol.name() + " failed: " + ex.what();
    }
  if(!error.empty())
    return std::nullopt;
  return result;
  }
