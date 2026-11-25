#pragma once

#include "program.h"

#include <exception>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>

namespace storygraph::execution {

std::optional<int32_t> integerOperator(std::string_view op, std::span<const int32_t> operands, std::string& error);

struct Core final {
  enum class Order : uint8_t { Arguments, Operators };

  template<class Runner, class Args>
  static auto execute(Runner& runner, const Program& program, const Args& args, bool readReturn,
                      std::string_view failure, std::string_view exceptionPrefix) -> typename Runner::Frame {
    if(!program.error.empty())
      return {args, {}, "not a program: " + program.error};
    if(runner.depth==64)
      return {args, {}, "program call depth exceeded"};

    // A host function may reenter the executor. Restore the caller's frame on all exit paths.
    typename Runner::Frame state{args, {}, {}};
    struct Restore final {
      Runner& runner;
      typename Runner::Frame* parent;
      ~Restore() {
        runner.frame = parent;
        --runner.depth;
        }
      } restore{runner, std::exchange(runner.frame,&state)};
    ++runner.depth;
    try {
      if(steps(runner,program.steps)==Flow::Fail && state.error.empty())
        state.error = failure;
      if(readReturn && state.returned && state.error.empty())
        if(auto value = runner.read(*state.returned))
          state.returned->value = std::move(*value);
      }
    catch(const std::exception& e) {
      runner.fail(std::string(exceptionPrefix)+e.what());
      }
    return state;
    }

  template<class Runner>
  static auto push(Runner& runner, const std::vector<ExprP>& args, Order order)
      -> std::optional<std::vector<typename Runner::Item>> {
    std::vector<typename Runner::Item> pushed(args.size());
    for(size_t i=0; i<args.size(); ++i) {
      const auto index = order==Order::Arguments ? i : args.size()-i-1;
      auto item = runner.item(*args[index]);
      if(!item)
        return std::nullopt;
      pushed[index] = std::move(*item);
      }
    return pushed;
    }

  template<class Runner>
  static std::optional<int32_t> arithmetic(Runner& runner, const Expr& e) {
    auto pushed = push(runner,e.args,Order::Operators);
    if(!pushed)
      return std::nullopt;
    std::vector<int32_t> operands;
    operands.reserve(pushed->size());
    for(const auto& item : *pushed) {
      auto value = runner.read(item);
      if(!value)
        return std::nullopt;
      using Value = std::remove_cvref_t<decltype(*value)>;
      if(value->type!=Value::Int) {
        runner.fail("arithmetic on a non-int: " + e.str());
        return std::nullopt;
        }
      operands.push_back(value->i);
      }
    std::string error;
    auto result = integerOperator(e.name,operands,error);
    if(!result)
      runner.fail(std::move(error));
    return result;
    }

  private:
    enum class Flow : uint8_t { Next, Return, Fail };

    template<class Runner>
    static Flow steps(Runner& runner, const std::vector<Step>& list) {
      for(const auto& s : list) {
        switch(s.kind) {
          case Step::Call:
            if(!runner.eval(*s.call))
              return Flow::Fail;
            break;
          case Step::Store:
            if(!runner.store(s))
              return Flow::Fail;
            break;
          case Step::If: {
            auto value = runner.eval(*s.cond);
            using Value = std::remove_cvref_t<decltype(*value)>;
            if(!value || value->type!=Value::Int) {
              if(value)
                runner.fail("condition is not an int: " + s.cond->str());
              return Flow::Fail;
              }
            const auto flow = steps(runner,value->i!=0 ? s.then : s.otherwise);
            if(flow!=Flow::Next)
              return flow;
            break;
            }
          case Step::Return:
            if(s.value) {
              runner.frame->returned = runner.item(*s.value);
              if(!runner.frame->returned)
                return Flow::Fail;
              }
            return Flow::Return;
          case Step::Say:
            if(!runner.say(s))
              return Flow::Fail;
            break;
          case Step::Choice:
          case Step::ClearChoices:
            if(!runner.choices || !runner.choices(s)) {
              runner.fail("choices of its own are for the director's dialogs");
              return Flow::Fail;
              }
            break;
          }
        }
      return Flow::Next;
      }
  };

}
