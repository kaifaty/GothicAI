#pragma once

#include "expr.h"
#include "predicates.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace daedalus {
  class DaedalusVm;
  }

namespace storygraph {

class Bytecode;
class Lifter;
struct Lifted;

// One thing a dialog does. `kind` names it (line, set, give, xp, journal_entry, choice, call...);
// expression fields and text fields depend on the kind. Text is UTF-8.
struct Effect final {
  std::string                                kind;
  std::vector<std::pair<std::string,ExprP>>  exprs;
  std::vector<std::pair<std::string,std::string>> texts;
  std::vector<ExprP>                         args;    // call: the arguments
  std::vector<std::string>                   writes;  // call: globals the script function may write
  ExprP                                      when;    // nullptr: always

  const std::string* text(const std::string& name) const;
  ExprP              expr(const std::string& name) const;
  };

// A dialog (C_INFO) or a choice offered by Info_AddChoice.
struct Node final {
  std::string              id;           // C_INFO instance, or the choice's function
  bool                     choice = false;
  std::string              npc;
  std::string              description;  // UTF-8
  int32_t                  nr = 0;
  bool                     important = false, permanent = false, trade = false;
  std::vector<std::string> parents;      // choice: the nodes that offer it
  ExprP                    condition;    // dialog: when it is offered; choice: nullptr
  std::string              notLifted;    // why the condition is not fully known; empty if it is
  std::vector<Effect>      conditionEffects;  // what checking the condition changes (C_BEKLAUEN stores globals)
  std::vector<Effect>      effects;
  std::string              error;        // why the effects could not be lifted
  std::set<std::string>    reads, knows, writes;
  };

struct VarInfo final {
  std::set<std::string> readers;  // nodes whose conditions read it
  std::set<std::string> writers;  // nodes that may write it
  std::set<std::string> others;   // other script functions that write it (ZS_DEAD, B_KAPITELWECHSEL...)
  };

// A quest: its MIS_* variable and the nodes that move it.
struct Quest final {
  std::string                        var;
  std::set<std::string>              topics;   // TOPIC_* constants
  std::map<std::string,std::string>  titles;   // TOPIC_* -> title, UTF-8
  std::set<std::string>              running, success, failed, obsolete, other;
  };

struct Model final {
  std::vector<Node>                       nodes;
  std::unordered_map<std::string,size_t>  byId;
  std::map<std::string,Helper>            helpers;
  std::map<std::string,std::string>       instances;  // global instance variable -> npc instance
  std::map<std::string,VarInfo>           vars;
  std::map<std::string,Quest>             quests;

  const Node* node(const std::string& id) const;
  };

// lines of OU.BIN: line name (uppercase) -> subtitle in the game codepage
using LineTexts = std::unordered_map<std::string,std::string>;

Model       buildModel(daedalus::DaedalusVm& vm, const Bytecode& code, const Lifter& lifter, const LineTexts& lines);

std::string toJson(const Model& m, bool pretty);
std::string toJson(const ExprP& e);
// readable views, UTF-8
std::string describeNode (const Model& m, const std::string& id, int depth = 3);
std::string describeQuest(const Model& m, const std::string& var);

}
