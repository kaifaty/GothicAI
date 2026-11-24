#pragma once

#include "program.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace storygraph {

namespace execution { struct Core; }

using NativeInstance = uint64_t;

struct NativeValue final {
  enum Type : uint8_t { Int, Float, Str, Inst };
  Type           type = Int;
  int32_t        i = 0;
  float          f = 0;
  std::string    s;
  NativeInstance inst = 0;

  static NativeValue integer(int32_t v);
  static NativeValue real(float v);
  static NativeValue str(std::string v);
  static NativeValue instance(NativeInstance v);
  };

struct NativeSlot final {
  enum Kind : uint8_t { Variable, Instance, Member };
  Kind             kind = Variable;
  std::string_view name;
  uint16_t         index = 0;
  NativeInstance   context = 0;
  };

struct NativeFunction final {
  std::vector<NativeValue::Type> parameters;
  std::optional<NativeValue::Type> result; // empty for a void function
  const Program* program = nullptr;       // null for a host function
  };

class NativeExecutionHost {
  public:
    virtual ~NativeExecutionHost() = default;
    virtual bool hasVariable(std::string_view name, bool instanceOnly) const = 0;
    virtual std::optional<int32_t> symbolIndex(std::string_view name) const = 0;
    virtual std::optional<NativeValue> read(const NativeSlot& slot, std::string& error) = 0;
    virtual bool write(const NativeSlot& slot, const NativeValue& value, std::string& error) = 0;
    // Returns a program or a supported host function. Programs must outlive the enclosing run.
    virtual std::optional<NativeFunction> findFunction(std::string_view name) const = 0;
    virtual std::optional<NativeValue> invoke(std::string_view name, const std::vector<NativeValue>& args,
                                              std::string& error) = 0;
    virtual std::string instanceName(NativeInstance) const { return {}; }
  };

class NativeInterpreter final {
  public:
    explicit NativeInterpreter(NativeExecutionHost& host) : host(host) {}

    struct Result final {
      bool        ok = true;
      std::string error;
      NativeValue returned;
      bool        started = false; // Only preflight failures are safe to retry elsewhere.
      };
    Result run(const Program& p, const std::vector<NativeValue>& args = {});

    std::function<std::string(const std::string& npc, const std::string& text)> ownLine;
    std::function<bool(const Step&)> choices;

  private:
    friend struct execution::Core;
    struct Item final {
      NativeValue               value;
      std::optional<NativeSlot> ref;
      };
    struct Frame final {
      const std::vector<NativeValue>& args;
      std::optional<Item>             returned;
      std::string                     error;
      };

    std::string preflight(const Program& p, size_t arguments) const;
    Frame execute(const Program& p, const std::vector<NativeValue>& args, bool readReturn);
    bool store(const Step& s);
    bool say(const Step& s);
    std::optional<NativeValue> eval(const Expr& expr);
    std::optional<Item> item(const Expr& expr);
    std::optional<NativeValue> read(const Item& item);
    std::optional<Item> call(const Expr& expr);
    std::optional<Item> op(const Expr& expr);
    std::nullopt_t fail(std::string error);

    NativeExecutionHost& host;
    Frame*               frame = nullptr;
    unsigned             depth = 0;
  };

}
