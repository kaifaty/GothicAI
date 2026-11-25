#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "storygraph/nativeexec.h"
#include "storygraph/predicates.h"

class GameScript;
class Npc;

// The director's programs, read-only queries and story variables shared by dialogs and triggers.
class StoryRuntime final {
  public:
    // ownLine registers a runtime line (npc symbol, text in the game codepage) and returns its id.
    explicit StoryRuntime(std::function<std::string(const std::string&,const std::string&)> ownLine);
    ~StoryRuntime();

    GameScript* script() const;

    // Restored variables keep their saved values.
    std::string            addVar(const std::string& name, int32_t first);
    static std::string     whyNotVar(GameScript& script, const std::string& name);
    size_t                 varCount() const { return vars.size(); }
    std::optional<int32_t> intOf(const std::string& name);
    bool                   setInt(const std::string& name, int32_t value);

    std::string checkExpr(const storygraph::ExprP& e);
    std::string checkProgram(const storygraph::Program& p, size_t arguments = 0, bool allowChoices = true);

    std::optional<storygraph::NativeValue> evaluate(const storygraph::ExprP& e, std::string& error, Npc* self,
                                                   Npc* other, std::string* instanceName = nullptr,
                                                   std::pair<Npc*,Npc*>* resultingActors = nullptr);
    std::optional<bool> test(const storygraph::ExprP& e, std::string_view what, Npc* self, Npc* other,
                             std::pair<Npc*,Npc*>* resultingActors = nullptr);
    storygraph::NativeInterpreter::Result run(const storygraph::Program& p, Npc* self, Npc* other,
                                               bool allowChoices = false,
                                               const std::vector<storygraph::NativeValue>& args = {});
    std::function<bool(const storygraph::Step&)> choices;

    storygraph::Program originalProgram(uint32_t fn);
    storygraph::ExprP   originalCondition(std::string_view dialog);

    void clear();

  private:
    void ensureVars();
    void ensurePredicates();
    std::string checkPredicate(const storygraph::ExprP& e) const;

    std::function<std::string(const std::string&,const std::string&)> ownLine;

    std::map<std::string,int32_t>                  vars;
    std::set<std::string>                          varsMade;
    GameScript*                                    varsFor = nullptr;

    GameScript*                                    preparedFor = nullptr;
    std::map<std::string,storygraph::Helper>       preparedHelpers;
    std::map<std::string,storygraph::ExprP,std::less<>> preparedConditions;
    std::set<std::string>                          reported;
  };
