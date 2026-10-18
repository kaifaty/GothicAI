#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "game/gamescript.h"

#include "expr.h"
#include "program.h"
#include "storyeffects.h"
#include "storyruntime.h"

class Npc;
class World;

namespace storygraph {
  class Interpreter;
  }

// The story graph in the game (docs/story-graph.md, stage 3): which dialogs npc's offer, through the
// engine's dialog hooks (Gothic::dialogFilterFn...). Without patches every dialog keeps its script
// condition. The server's patches (graph_patch) switch a dialog or a choice off, add a condition to it,
// or add dialogs of its own, whose lines and effects the bridge plays.
// With -devmode (or GOTHIC_AI_STORYCHECK=1) every script condition is also computed from its lifted form
// and differences are logged: the check of stage 2, on the live game.
// The script functions of dialogs (conditions, information, choices) run as their programs (program.h), by
// the Interpreter of the StoryRuntime, not by the VM; script functions they call still run in the VM. GOTHIC_AI_STORYEXEC=0 turns
// it off. A patch may put a program of the
// director in place of a dialog's own (op "program"), and a dialog of the director may have one: those run
// in any case.
class DialogGraph final {
  public:
    // send: a message to the server; addLine: registers a runtime line (npc symbol, text in the game
    // codepage) and returns its id; rt: what programs and conditions run on, shared with the triggers
    DialogGraph(std::function<void(std::string&&)> send,
                std::function<std::string(const std::string&,const std::string&)> addLine,
                StoryRuntime& rt);
    ~DialogGraph();

    // an operation of a graph_patch message (all but the triggers'); empty, or why it was not applied
    std::string applyOp(const rapidjson::Value& op);
    // a load, or patches anew: the server sends the branch's patches again
    void clear();
    // what the patches change: "2 off, 0 with conditions, 3 new, 0 programs replaced"
    std::string summary() const;
    // a dialog or a choice ran (dialog_chosen): the npc and the node, for the triggers
    std::function<void(Npc& npc, const std::string& node)> chosen;

    // the dialogs the npc offers the player now, as the menu would list them (node ids)
    std::vector<std::string> offered(World& world, Npc& npc);
    // a dialog the npc offers now, or a choice of a dialog of the director it offers now, as if the player chose
    // it in the menu; false if it is not offered
    bool choose(World& world, Npc& npc, std::string_view node, std::string& error);
    // the choices a dialog of the director offers after its lines, as the menu asks for them: (id, text UTF-8)
    std::vector<std::pair<std::string,std::string>> choices(World& world, Npc& npc, std::string_view node);
    // an expression with self = npc, other = the hero, as JSON (5, "text", null, "PC_HERO"); false and `error`
    // if it can't be computed
    bool evaluate(World& world, Npc* npc, const rapidjson::Value& expr, std::string& json, std::string& error);
    // every npc of the world builds its dialog menu once, with the check on: the stage 2 check in the
    // live game. Checking runs the conditions, with their effects (C_Beklauen, Npc_SetRefuseTalk...).
    // JSON: {"npcs", "computed", "differ", "differences": [dialog ids]}
    std::string checkWorld(World& world);
    std::string status() const;
    // the program a dialog's script function runs now, as JSON: {"node", "fn", "returns", "patched", "steps"}.
    // `node`: a dialog (C_INFO; `part` "condition" or "information") or a choice's function; `own`: the scripts'
    // program even when the director's is in place. Empty and `error` if there is none
    std::string program(std::string_view node, std::string_view part, bool own, std::string& error);

  private:
    struct Line final {
      bool        hero = false;   // spoken by the hero ("other"), else by the npc
      std::string id;             // an original line (DIA_..._15_00), or empty
      std::string text;           // a line of its own, game codepage
      };
    // a dialog made by the director
    struct Node final {
      std::string                       id;
      std::string                       npc;          // script symbol
      std::string                       description;  // game codepage
      int32_t                           nr = 0;
      bool                              important = false, permanent = false;
      storygraph::ExprP                 condition;    // nullptr: always
      std::vector<Line>                 lines;
      std::vector<StoryEffect>          effects;
      storygraph::Program               program;      // runs after the effects; no steps: none
      };
    // a choice a dialog of the director offers now (a Choice step of its program ran)
    struct Choice final {
      std::string                       id;
      std::string                       node;         // the dialog that offers it
      std::string                       text;         // game codepage
      std::vector<storygraph::Step>     steps;        // run when the hero picks it
      };
    // a program of the director in place of a script function's own (op "program")
    struct Replaced final {
      std::string                       node;
      bool                              condition = false;  // the dialog's condition, else its information
      storygraph::Program               program;
      };

    void        onFilter(const zenkit::IInfo& info, bool& visible);
    bool        onChoiceFilter(const zenkit::IInfo& info, int32_t fn);
    void        onExtra(const zenkit::INpc& npc, bool important, std::vector<GameScript::DlgChoice>& out);
    void        onExec(const GameScript::DlgChoice& dlg, Npc& player, Npc& npc);
    void        onChoices(const GameScript::DlgChoice& dlg, std::vector<GameScript::DlgChoice>& out);
    // runs steps of a dialog of the director: its program, or a choice it offered; `node` gets the choices
    void        runOwn(const std::string& node, const std::vector<storygraph::Step>& steps);
    void        onChosen(const GameScript::DlgChoice& dlg, Npc& player, Npc& npc);
    // a dialog's condition, information or choice: run as its program (docs/story-graph.md, «Исполнение без VM»)
    bool        onScript(zenkit::DaedalusSymbol& fn, int32_t& result);

    std::string readNode(const rapidjson::Value& v, Node& n);
    // the script function behind a dialog's condition or information, or a choice's function
    zenkit::DaedalusSymbol* functionOf(zenkit::DaedalusVm& v, const std::string& node, bool condition,
                                       std::string& error) const;
    // the director's program in place of this function, nullptr if there is none
    const storygraph::Program* replacedProgram(zenkit::DaedalusVm& v, uint32_t fn);
    // the interpreter for this VM; a new VM: the lifted programs are lifted again
    storygraph::Interpreter& interpreter(zenkit::DaedalusVm& v);
    void        check(const zenkit::IInfo& info, bool visible);
    zenkit::DaedalusVm* vm() const { return rt.vm(); }

    std::function<void(std::string&&)>                               send;
    std::function<std::string(const std::string&,const std::string&)> addLine;
    StoryRuntime&                                                    rt;

    std::set<std::string>                                  disabled;    // dialogs and choices
    std::map<std::string,std::vector<storygraph::ExprP>>   conditions;  // added to script dialogs
    std::map<std::string,Node>                             nodes;       // made by the director
    std::set<std::string>                                  told;        // of those, the ones the hero chose
    std::map<std::string,std::vector<Choice>>              offering;    // the choices each dialog of the director offers
    std::map<std::string,std::string>                      choiceOf;    // choice id -> its dialog, also once picked
    std::string                                            runningOwn;  // the dialog of the director whose steps run
    std::map<std::string,Replaced>                         replaced;    // by node, or node + ":CONDITION"
    std::unordered_map<uint32_t,const storygraph::Program*> replacedFns; // by function symbol, for replacedFor
    zenkit::DaedalusVm*                                    replacedFor = nullptr;

    // the graph runs dialogs' script functions in place of the VM; GOTHIC_AI_STORYEXEC=0: the VM runs them
    bool                                                   running = true;
    zenkit::DaedalusVm*                                    programsFor = nullptr;
    std::unordered_map<uint32_t,storygraph::Program>       programs;   // by function symbol, lifted when it first runs
    size_t                                                 ran = 0, notPrograms = 0, runErrors = 0, replacedRuns = 0;

    bool                                                   checking = false;
    size_t                                                 checked = 0, differ = 0;
    std::set<std::string>                                  reported;   // differences, logged once
    std::vector<std::string>                               differences;
  };
