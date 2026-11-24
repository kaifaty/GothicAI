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

#include "storygraph/expr.h"
#include "storygraph/program.h"
#include "storyeffects.h"
#include "storyruntime.h"

class Npc;
class World;

// The director may filter stock dialogs, replace their native programs or add its own dialogs.
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

    void        onFilter(const daedalus::IInfo& info, bool& visible);
    bool        onChoiceFilter(const daedalus::IInfo& info, int32_t fn);
    void        onExtra(const daedalus::INpc& npc, bool important, std::vector<GameScript::DlgChoice>& out);
    void        onExec(const GameScript::DlgChoice& dlg, Npc& player, Npc& npc);
    void        onChoices(const GameScript::DlgChoice& dlg, std::vector<GameScript::DlgChoice>& out);
    // runs steps of a dialog of the director: its program, or a choice it offered; `node` gets the choices
    void        runOwn(const std::string& node, const std::vector<storygraph::Step>& steps, Npc& npc, Npc& player);
    void        onChosen(const GameScript::DlgChoice& dlg, Npc& player, Npc& npc);
    bool        onScript(uint32_t fn, int32_t& result);

    std::string readNode(const rapidjson::Value& v, Node& n);
    std::optional<uint32_t> functionOf(GameScript& script, const std::string& node, bool condition,
                                       std::string& error) const;
    const storygraph::Program* replacedProgram(GameScript& script, uint32_t fn);
    void        check(const daedalus::IInfo& info, bool visible);

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
    GameScript*                                            replacedFor = nullptr;

    size_t                                                 ran = 0, notPrograms = 0, runErrors = 0, replacedRuns = 0;

    bool                                                   checking = false;
    size_t                                                 checked = 0, differ = 0;
    std::set<std::string>                                  reported;   // differences, logged once
    std::vector<std::string>                               differences;
  };
