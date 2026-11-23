#include "model.h"

#include <algorithm>

#include "bytecode.h"
#include "lifter.h"
#include "predicates.h"
#include "textcodec.h"
#include "vocab.h"

#include "daedalus/vm.h"
#include "daedalus/classes.h"

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <deque>
#include <functional>

using namespace storygraph;
using daedalus::DaedalusDataType;

const std::string* Effect::text(const std::string& name) const {
  for(auto& [k,v] : texts)
    if(k==name)
      return &v;
  return nullptr;
  }

ExprP Effect::expr(const std::string& name) const {
  for(auto& [k,v] : exprs)
    if(k==name)
      return v;
  return nullptr;
  }

const Node* Model::node(const std::string& id) const {
  auto it = byId.find(id);
  return it==byId.end() ? nullptr : &nodes[it->second];
  }

namespace {

// What a script function and everything it calls may do.
struct Footprint final {
  std::set<std::string> writes;
  std::set<std::string> externals;
  bool                  pure = true;  // only predicates and pure engine functions, no writes
  };

class Builder final {
  public:
    Builder(daedalus::DaedalusVm& vm, const Bytecode& code, const Lifter& lifter, const LineTexts& lines)
      :vm(vm), dat(vm), code(code), lifter(lifter), lines(lines), predicates(vm, lifter) {}

    Model build();

  private:
    const Footprint&    footprint(const std::string& fn);
    void                collectReads(const ExprP& e, Node& n, std::set<std::string>& seen);
    std::vector<Effect> effects(const Lifted& l);
    Node                choiceNode(const std::string& fn);
    void                addQuests(Model& m);
    void                addVars(Model& m, const std::set<std::string>& dialogFns);

    daedalus::DaedalusVm&             vm;
    const daedalus::DaedalusScript&   dat;
    const Bytecode&                 code;
    const Lifter&                   lifter;
    const LineTexts&                lines;
    Predicates                      predicates;
    std::map<std::string,Footprint> footprints;
    std::set<std::string>           busy;
  };

std::string name(const daedalus::DaedalusSymbol* s) {
  return s==nullptr ? std::string("?") : s->name();
  }

std::string utf8(const ExprP& e) {
  return e && e->kind==Expr::Str ? TextCodec::toUtf8(e->text) : std::string();
  }

}

const Footprint& Builder::footprint(const std::string& fn) {
  auto it = footprints.find(fn);
  if(it!=footprints.end())
    return it->second;
  Footprint& f = footprints[fn];
  auto* sym = dat.find_symbol_by_name(fn);
  if(sym==nullptr || busy.count(fn))
    return f;
  busy.insert(fn);
  auto s = code.summarize(sym->index());
  f.writes    = s.writes;
  f.externals = s.externals;
  for(auto& c : s.calls) {
    auto& sub = footprint(c);
    f.writes.insert(sub.writes.begin(), sub.writes.end());
    f.externals.insert(sub.externals.begin(), sub.externals.end());
    }
  f.pure = f.writes.empty();
  for(auto& x : f.externals)
    if(!vocab::PredicateExternals.count(x) && !vocab::PureExternals.count(x))
      f.pure = false;
  busy.erase(fn);
  return f;
  }

void Builder::collectReads(const ExprP& e, Node& n, std::set<std::string>& seen) {
  std::vector<std::string> fns;
  ex::walk(e, [&](const Expr& x) {
    if(x.kind==Expr::Var)
      n.reads.insert(x.name);
    else if(x.kind==Expr::Call && x.name=="NPC_KNOWSINFO" && x.args.size()==2 && x.args[1]->kind==Expr::Sym)
      n.knows.insert(x.args[1]->name);
    else if(x.kind==Expr::Fn)
      fns.push_back(x.name);
    });
  for(auto& f : fns)
    if(seen.insert(f).second)
      collectReads(predicates.helper(f).body, n, seen);
  }

std::vector<Effect> Builder::effects(const Lifted& l) {
  std::vector<Effect> out;
  for(auto& e : l.events) {
    Effect f;
    f.when = e.guard.isAlways() ? nullptr : e.guard.expr();
    if(e.kind==Event::Store) {
      auto& t = *e.target;
      if(t.kind==Expr::Var) {
        f.kind = "set";
        f.texts.emplace_back("var", t.name);
        if(t.index>=0)
          f.exprs.emplace_back("index", ex::integer(t.index));
        } else if(t.kind==Expr::Field) {
        f.kind = "set_field";
        f.exprs.emplace_back("of", t.args[0]);
        f.texts.emplace_back("field", t.name);
        if(t.index>=0)
          f.exprs.emplace_back("index", ex::integer(t.index, t.text));
        } else if(t.kind==Expr::Inst) {
        f.kind = "set_instance";
        f.texts.emplace_back("var", t.name);
        } else if(t.kind==Expr::Local) {
        f.kind = "set_local";  // static: it keeps the value, later calls may read it
        f.texts.emplace_back("var", t.name);
        } else {
        continue;
        }
      f.exprs.emplace_back("value", e.value);
      out.push_back(std::move(f));
      continue;
      }

    const std::string& fn = e.callee->name();
    const auto&        a  = e.args;
    auto arg = [&a](size_t i) { return i<a.size() ? a[i] : ex::unknown("missing argument"); };
    auto symName = [&](size_t i) {
      auto x = arg(i);
      return x->kind==Expr::Sym || x->kind==Expr::Inst ? x->name : std::string();
      };

    if(fn=="AI_OUTPUT") {
      f.kind = "line";
      f.exprs = {{"speaker", arg(0)}, {"listener", arg(1)}};
      auto id = arg(2)->kind==Expr::Str ? arg(2)->text : std::string();
      for(auto& c : id)
        c = char(std::toupper(uint8_t(c)));
      f.texts.emplace_back("id", id);
      auto it = lines.find(id);
      if(it!=lines.end())
        f.texts.emplace_back("text", TextCodec::toUtf8(it->second));
      }
    else if(fn=="B_SAY") {
      f.kind  = "svm";
      f.exprs = {{"speaker", arg(0)}, {"listener", arg(1)}};
      f.texts.emplace_back("svm", utf8(arg(2)));
      }
    else if(fn=="LOG_CREATETOPIC") {
      f.kind = "journal_topic";
      f.texts.emplace_back("topic", arg(0)->name);
      f.texts.emplace_back("title", utf8(arg(0)));
      f.exprs.emplace_back("section", arg(1));
      }
    else if(fn=="LOG_SETTOPICSTATUS") {
      f.kind = "journal_status";
      f.texts.emplace_back("topic", arg(0)->name);
      f.exprs.emplace_back("status", arg(1));
      }
    else if(fn=="LOG_ADDENTRY" || fn=="B_LOGENTRY") {
      f.kind = "journal_entry";
      f.texts.emplace_back("topic", arg(0)->name);
      f.texts.emplace_back("text", utf8(arg(1)));
      }
    else if(fn=="B_GIVEPLAYERXP") {
      f.kind = "xp";
      f.exprs.emplace_back("amount", arg(0));
      }
    else if(fn=="B_GIVEINVITEMS") {
      f.kind  = "give";
      f.exprs = {{"from", arg(0)}, {"to", arg(1)}, {"item", arg(2)}, {"amount", arg(3)}};
      }
    else if(fn=="CREATEINVITEMS" || fn=="CREATEINVITEM") {
      f.kind  = "create_items";
      f.exprs = {{"npc", arg(0)}, {"item", arg(1)}, {"amount", fn=="CREATEINVITEM" ? ex::integer(1) : arg(2)}};
      }
    else if(fn=="NPC_REMOVEINVITEMS" || fn=="NPC_REMOVEINVITEM") {
      f.kind  = "remove_items";
      f.exprs = {{"npc", arg(0)}, {"item", arg(1)}, {"amount", fn=="NPC_REMOVEINVITEM" ? ex::integer(1) : arg(2)}};
      }
    else if(fn=="NPC_EXCHANGEROUTINE" || fn=="B_STARTOTHERROUTINE") {
      f.kind = "routine";
      f.exprs.emplace_back("npc", arg(0));
      f.texts.emplace_back("routine", utf8(arg(1)));
      }
    else if(fn=="WLD_INSERTNPC") {
      f.kind = "insert_npc";
      f.exprs.emplace_back("npc", arg(0));
      f.texts.emplace_back("waypoint", utf8(arg(1)));
      }
    else if(fn=="WLD_INSERTITEM") {
      f.kind = "insert_item";
      f.exprs.emplace_back("item", arg(0));
      f.texts.emplace_back("waypoint", utf8(arg(1)));
      }
    else if(fn=="B_ATTACK") {
      f.kind  = "attack";
      f.exprs = {{"npc", arg(0)}, {"target", arg(1)}, {"reason", arg(2)}};
      }
    else if(fn=="B_KAPITELWECHSEL") {
      f.kind = "chapter";
      f.exprs.emplace_back("chapter", arg(0));
      }
    else if(fn=="INFO_ADDCHOICE") {
      f.kind = "choice";
      f.texts.emplace_back("node", symName(2));
      f.texts.emplace_back("text", utf8(arg(1)));
      }
    else if(fn=="INFO_CLEARCHOICES") {
      f.kind = "clear_choices";
      }
    else if(fn=="AI_STOPPROCESSINFOS") {
      f.kind = "end";
      }
    else if(fn=="NPC_SETTRUEGUILD") {
      f.kind  = "set_guild";
      f.exprs = {{"npc", arg(0)}, {"guild", arg(1)}};
      }
    else if(e.callee->is_external()) {
      if(vocab::PredicateExternals.count(fn) || vocab::PureExternals.count(fn))
        continue;
      f.kind = "call";
      f.texts.emplace_back("fn", fn);
      f.args = a;
      }
    else {
      auto& fp = footprint(fn);
      if(fp.pure)
        continue;  // a predicate or a getter used in an expression
      f.kind = "call";
      f.texts.emplace_back("fn", fn);
      f.args   = a;
      f.writes = std::vector<std::string>(fp.writes.begin(), fp.writes.end());
      }
    out.push_back(std::move(f));
    }
  return out;
  }

Node Builder::choiceNode(const std::string& fn) {
  Node n;
  n.id     = fn;
  n.choice = true;
  auto* sym = dat.find_symbol_by_name(fn);
  if(sym==nullptr) {
    n.error = "no such function";
    return n;
    }
  auto l = lifter.lift(sym->index());
  n.error   = l.error;
  n.effects = effects(l);
  return n;
  }

static int questStatus(const ExprP& v) {
  if(v==nullptr || v->kind!=Expr::Int)
    return -1;
  return v->value;  // LOG_RUNNING 1, LOG_SUCCESS 2, LOG_FAILED 3, LOG_OBSOLETE 4
  }

void Builder::addQuests(Model& m) {
  auto quest = [&m](const std::string& var) -> Quest& {
    auto& q = m.quests[var];
    q.var = var;
    return q;
    };
  for(auto& n : m.nodes) {
    std::vector<std::string> started;
    for(auto& e : n.effects) {
      if(e.kind!="set" || e.text("var")==nullptr || e.text("var")->rfind("MIS_",0)!=0)
        continue;
      auto& q = quest(*e.text("var"));
      switch(questStatus(e.expr("value"))) {
        case 1: q.running.insert(n.id); started.push_back(q.var); break;
        case 2: q.success.insert(n.id); break;
        case 3: q.failed.insert(n.id); break;
        case 4: q.obsolete.insert(n.id); break;
        default: q.other.insert(n.id); break;
        }
      }
    if(started.empty())
      continue;
    // the topics created next to the start of a quest belong to it; with several quests, pair them in order
    std::vector<const Effect*> topics;
    for(auto& e : n.effects)
      if(e.kind=="journal_topic" && e.text("topic") && !e.text("topic")->empty())
        topics.push_back(&e);
    for(size_t i=0; i<started.size(); ++i) {
      auto& q = quest(started[i]);
      for(size_t k=0; k<topics.size(); ++k) {
        if(started.size()==topics.size() && k!=i)
          continue;
        q.topics.insert(*topics[k]->text("topic"));
        q.titles[*topics[k]->text("topic")] = *topics[k]->text("title");
        }
      }
    }
  // topic created elsewhere: the journal entries written where the quest starts or ends
  std::map<std::string,std::string> titles;
  for(auto& n : m.nodes)
    for(auto& e : n.effects)
      if(e.kind=="journal_topic" && e.text("topic"))
        titles[*e.text("topic")] = *e.text("title");
  for(auto& [var,q] : m.quests) {
    for(auto* nodes : {&q.running, &q.success, &q.failed}) {
      if(!q.topics.empty())
        break;
      for(auto& id : *nodes)
        for(auto& e : m.nodes[m.byId[id]].effects)
          if((e.kind=="journal_entry" || e.kind=="journal_status") && e.text("topic") && !e.text("topic")->empty()) {
            q.topics.insert(*e.text("topic"));
            q.titles[*e.text("topic")] = titles[*e.text("topic")];
            }
      }
    }
  for(auto& [var,v] : m.vars)
    if(var.rfind("MIS_",0)==0)
      quest(var);
  }

void Builder::addVars(Model& m, const std::set<std::string>& dialogFns) {
  for(auto& n : m.nodes) {
    for(auto& v : n.reads)
      m.vars[v].readers.insert(n.id);
    for(auto& v : n.writes)
      m.vars[v].writers.insert(n.id);
    }
  for(auto& s : dat.symbols()) {
    if(s.type()!=DaedalusDataType::FUNCTION || !Bytecode::hasCode(s) || dialogFns.count(s.name()))
      continue;
    for(auto& w : code.summarize(s.index()).writes) {
      auto it = m.vars.find(w);
      if(it!=m.vars.end())
        it->second.others.insert(s.name());
      }
    }
  }

Model Builder::build() {
  Model m;
  std::set<std::string> dialogFns;

  std::vector<std::shared_ptr<daedalus::IInfo>> infos;
  vm.enumerate_instances_by_class_name("C_INFO", [&](daedalus::DaedalusSymbol& sym){
    infos.push_back(vm.init_instance<daedalus::IInfo>(&sym));
    });

  std::deque<std::pair<std::string,std::string>> choices;  // function, offered by
  for(auto& info : infos) {
    Node n;
    n.id          = name(dat.find_symbol_by_index(info->symbol_index()));
    n.npc         = name(dat.find_symbol_by_index(uint32_t(info->npc)));
    n.description = TextCodec::toUtf8(info->description);
    n.nr          = info->nr;
    n.important   = info->important!=0;
    n.permanent   = info->permanent!=0;
    n.trade       = info->trade!=0;

    if(info->condition!=0) {
      auto l      = lifter.lift(uint32_t(info->condition));
      n.condition = predicates.condition(uint32_t(info->condition));
      // conditions may change the world too: C_BEKLAUEN stores the theft values, Npc_SetRefuseTalk...
      // (their own locals are part of the lifted value, not an effect)
      for(auto& e : effects(l))
        if(e.kind!="set_local")
          n.conditionEffects.push_back(std::move(e));
      dialogFns.insert(name(dat.find_symbol_by_index(uint32_t(info->condition))));
      } else {
      n.condition = ex::integer(1);
      }
    n.notLifted = predicates.whyNotLifted(n.condition);
    std::set<std::string> seen;
    collectReads(n.condition, n, seen);

    auto l = lifter.lift(uint32_t(info->information));
    dialogFns.insert(name(dat.find_symbol_by_index(uint32_t(info->information))));
    n.error   = l.error;
    n.effects = effects(l);
    for(auto& e : n.effects)
      if(e.kind=="choice" && e.text("node") && !e.text("node")->empty())
        choices.emplace_back(*e.text("node"), n.id);

    m.byId[n.id] = m.nodes.size();
    m.nodes.push_back(std::move(n));
    }

  while(!choices.empty()) {
    auto [fn, parent] = choices.front();
    choices.pop_front();
    auto it = m.byId.find(fn);
    if(it!=m.byId.end()) {
      auto& p = m.nodes[it->second].parents;
      if(std::find(p.begin(), p.end(), parent)==p.end())
        p.push_back(parent);
      continue;
      }
    Node n = choiceNode(fn);
    dialogFns.insert(fn);
    auto& from = m.nodes[m.byId[parent]];
    n.npc = from.npc;
    for(auto& e : from.effects)
      if(e.kind=="choice" && e.text("node") && *e.text("node")==fn)
        n.description = *e.text("text");
    n.parents.push_back(parent);
    for(auto& e : n.effects)
      if(e.kind=="choice" && e.text("node") && !e.text("node")->empty())
        choices.emplace_back(*e.text("node"), n.id);
    m.byId[n.id] = m.nodes.size();
    m.nodes.push_back(std::move(n));
    }

  for(auto& n : m.nodes)
    for(auto& e : n.effects) {
      if(e.kind=="set")
        n.writes.insert(*e.text("var"));
      n.writes.insert(e.writes.begin(), e.writes.end());
      }

  // global instance variables: HILDA = Hlp_GetNpc(BAU_951_HILDA)
  if(auto* init = dat.find_symbol_by_name("B_INITNPCGLOBALS")) {
    for(auto& e : lifter.lift(init->index()).events) {
      if(e.kind!=Event::Store || e.value->kind!=Expr::Call || e.value->name!="HLP_GETNPC")
        continue;
      if(e.value->args.size()==1 && e.value->args[0]->kind==Expr::Sym &&
         (e.target->kind==Expr::Inst || e.target->kind==Expr::Var))
        m.instances[e.target->name] = e.value->args[0]->name;
      }
    }

  addVars(m, dialogFns);
  addQuests(m);
  m.helpers = predicates.helpers();
  return m;
  }

Model storygraph::buildModel(daedalus::DaedalusVm& vm, const Bytecode& code, const Lifter& lifter, const LineTexts& lines) {
  return Builder(vm, code, lifter, lines).build();
  }

// ---- JSON

namespace {

template<class W>
void writeStr(W& w, const std::string& s) {
  w.String(s.data(), rapidjson::SizeType(s.size()));
  }

template<class W>
void writeExpr(W& w, const ExprP& e) {
  if(e==nullptr) {
    w.Null();
    return;
    }
  w.StartArray();
  auto tag = [&w](const char* t) { w.String(t); };
  switch(e->kind) {
    case Expr::Int:
      tag("int");
      w.Int(e->value);
      if(!e->name.empty())
        writeStr(w, ident(e->name));
      break;
    case Expr::Str:
      tag("str");
      writeStr(w, TextCodec::toUtf8(e->text));
      if(!e->name.empty())
        writeStr(w, ident(e->name));
      break;
    case Expr::Var:
      tag("var");
      writeStr(w, ident(e->name));
      if(e->index>=0)
        w.Int(e->index);
      break;
    case Expr::Field:
      tag("field");
      writeExpr(w, e->args[0]);
      writeStr(w, ident(e->name));
      if(e->index>=0) {
        w.Int(e->index);
        if(!e->text.empty())
          writeStr(w, ident(e->text));
        }
      break;
    case Expr::Inst:
      tag("inst");
      writeStr(w, ident(e->name));
      break;
    case Expr::Sym:
      tag("sym");
      writeStr(w, ident(e->name));
      break;
    case Expr::Param:
      tag("param");
      w.Int(e->index);
      break;
    case Expr::Local:
      tag("unknown");
      writeStr(w, "local " + ident(e->name));
      break;
    case Expr::Call:
    case Expr::Fn:
      tag(e->kind==Expr::Call ? "call" : "fn");
      writeStr(w, ident(e->name));
      for(auto& a : e->args)
        writeExpr(w, a);
      break;
    case Expr::Op:
      writeStr(w, e->name);
      for(auto& a : e->args)
        writeExpr(w, a);
      break;
    case Expr::Cases:
      tag("cases");
      for(auto& a : e->args)
        writeExpr(w, a);
      break;
    case Expr::Unknown:
      tag("unknown");
      writeStr(w, e->text);
      break;
    }
  w.EndArray();
  }

template<class W>
void writeNames(W& w, const char* key, const std::set<std::string>& names) {
  w.Key(key);
  w.StartArray();
  for(auto& n : names)
    writeStr(w, ident(n));
  w.EndArray();
  }

template<class W>
void writeEffect(W& w, const Effect& e) {
  w.StartObject();
  w.Key("do");
  writeStr(w, e.kind);
  for(auto& [k,v] : e.texts) {
    w.Key(k.c_str());
    bool symbol = (k=="var" || k=="field" || k=="topic" || k=="fn" || k=="node" || k=="id");
    writeStr(w, symbol ? ident(v) : v);
    }
  for(auto& [k,v] : e.exprs) {
    w.Key(k.c_str());
    writeExpr(w, v);
    }
  if(e.kind=="call") {
    w.Key("args");
    w.StartArray();
    for(auto& a : e.args)
      writeExpr(w, a);
    w.EndArray();
    if(!e.writes.empty())
      writeNames(w, "writes", std::set<std::string>(e.writes.begin(), e.writes.end()));
    }
  if(e.when) {
    w.Key("when");
    writeExpr(w, e.when);
    }
  w.EndObject();
  }

template<class W>
void writeModel(W& w, const Model& m) {
  w.StartObject();
  w.Key("version");
  w.Int(1);

  w.Key("nodes");
  w.StartArray();
  for(auto& n : m.nodes) {
    w.StartObject();
    w.Key("id");          writeStr(w, ident(n.id));
    w.Key("kind");        w.String(n.choice ? "choice" : "dialog");
    w.Key("npc");         writeStr(w, ident(n.npc));
    w.Key("description"); writeStr(w, n.description);
    if(!n.choice) {
      w.Key("nr");        w.Int(n.nr);
      w.Key("important"); w.Bool(n.important);
      w.Key("permanent"); w.Bool(n.permanent);
      w.Key("trade");     w.Bool(n.trade);
      w.Key("condition"); writeExpr(w, n.condition);
      w.Key("lifted");    w.Bool(n.notLifted.empty());
      if(!n.notLifted.empty()) {
        w.Key("not_lifted");
        writeStr(w, n.notLifted);
        }
      writeNames(w, "reads", n.reads);
      writeNames(w, "knows", n.knows);
      if(!n.conditionEffects.empty()) {
        w.Key("condition_effects");
        w.StartArray();
        for(auto& e : n.conditionEffects)
          writeEffect(w, e);
        w.EndArray();
        }
      } else {
      w.Key("parents");
      w.StartArray();
      for(auto& p : n.parents)
        writeStr(w, ident(p));
      w.EndArray();
      }
    writeNames(w, "writes", n.writes);
    if(!n.error.empty()) {
      w.Key("error");
      writeStr(w, n.error);
      }
    w.Key("effects");
    w.StartArray();
    for(auto& e : n.effects)
      writeEffect(w, e);
    w.EndArray();
    w.EndObject();
    }
  w.EndArray();

  w.Key("functions");
  w.StartObject();
  for(auto& [name,h] : m.helpers) {
    w.Key(ident(name).c_str());
    w.StartObject();
    w.Key("params"); w.Uint(h.params);
    w.Key("body");   writeExpr(w, h.body);
    w.Key("lifted"); w.Bool(h.notLifted.empty());
    if(!h.notLifted.empty()) {
      w.Key("not_lifted");
      writeStr(w, h.notLifted);
      }
    w.EndObject();
    }
  w.EndObject();

  w.Key("instances");
  w.StartObject();
  for(auto& [v,npc] : m.instances) {
    w.Key(ident(v).c_str());
    writeStr(w, ident(npc));
    }
  w.EndObject();

  w.Key("vars");
  w.StartObject();
  for(auto& [name,v] : m.vars) {
    w.Key(ident(name).c_str());
    w.StartObject();
    writeNames(w, "readers", v.readers);
    writeNames(w, "writers", v.writers);
    writeNames(w, "others",  v.others);
    w.EndObject();
    }
  w.EndObject();

  w.Key("quests");
  w.StartArray();
  for(auto& [var,q] : m.quests) {
    w.StartObject();
    w.Key("var"); writeStr(w, ident(var));
    w.Key("topics");
    w.StartArray();
    for(auto& t : q.topics) {
      w.StartObject();
      w.Key("topic"); writeStr(w, ident(t));
      w.Key("title"); writeStr(w, q.titles.count(t) ? q.titles.at(t) : std::string());
      w.EndObject();
      }
    w.EndArray();
    writeNames(w, "running",  q.running);
    writeNames(w, "success",  q.success);
    writeNames(w, "failed",   q.failed);
    writeNames(w, "obsolete", q.obsolete);
    writeNames(w, "other",    q.other);
    w.EndObject();
    }
  w.EndArray();

  w.EndObject();
  }

}

std::string storygraph::toJson(const Model& m, bool pretty) {
  rapidjson::StringBuffer buf;
  if(pretty) {
    rapidjson::PrettyWriter<rapidjson::StringBuffer> w(buf);
    w.SetIndent(' ', 1);
    w.SetFormatOptions(rapidjson::kFormatSingleLineArray);
    writeModel(w, m);
    } else {
    rapidjson::Writer<rapidjson::StringBuffer> w(buf);
    writeModel(w, m);
    }
  return std::string(buf.GetString(), buf.GetSize());
  }

std::string storygraph::toJson(const ExprP& e) {
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  writeExpr(w, e);
  return std::string(buf.GetString(), buf.GetSize());
  }

// ---- readable views

static std::string effectText(const Effect& e) {
  auto ex = [&e](const char* k) { auto x = e.expr(k); return x ? x->str() : std::string("?"); };
  auto tx = [&e](const char* k) { auto x = e.text(k); return x ? *x : std::string(); };
  const auto& k = e.kind;
  if(k=="line")          return ex("speaker") + ": «" + tx("text") + "» (" + tx("id") + ")";
  if(k=="svm")           return ex("speaker") + ": SVM " + tx("svm");
  if(k=="set")           return "set " + ident(tx("var")) + (e.expr("index") ? "[" + ex("index") + "]" : "") + " = " + ex("value");
  if(k=="set_field")     return "set " + ex("of") + "." + tx("field") + (e.expr("index") ? "[" + ex("index") + "]" : "") + " = " + ex("value");
  if(k=="set_instance")  return "set " + ident(tx("var")) + " = " + ex("value");
  if(k=="set_local")     return "set local " + ident(tx("var")) + " = " + ex("value");
  if(k=="journal_topic") return "journal: new topic " + ident(tx("topic")) + " «" + tx("title") + "» (" + ex("section") + ")";
  if(k=="journal_status")return "journal: " + ident(tx("topic")) + " -> " + ex("status");
  if(k=="journal_entry") return "journal: " + ident(tx("topic")) + ": «" + tx("text") + "»";
  if(k=="xp")            return "xp " + ex("amount");
  if(k=="give")          return "give " + ex("amount") + " " + ex("item") + ": " + ex("from") + " -> " + ex("to");
  if(k=="create_items")  return "create " + ex("amount") + " " + ex("item") + " for " + ex("npc");
  if(k=="remove_items")  return "remove " + ex("amount") + " " + ex("item") + " from " + ex("npc");
  if(k=="routine")       return "routine " + ex("npc") + " -> " + tx("routine");
  if(k=="insert_npc")    return "insert " + ex("npc") + " at " + tx("waypoint");
  if(k=="insert_item")   return "insert " + ex("item") + " at " + tx("waypoint");
  if(k=="attack")        return ex("npc") + " attacks " + ex("target") + " (" + ex("reason") + ")";
  if(k=="chapter")       return "chapter " + ex("chapter");
  if(k=="choice")        return "choice «" + tx("text") + "» -> " + ident(tx("node"));
  if(k=="clear_choices") return "clear choices";
  if(k=="end")           return "end dialog";
  if(k=="set_guild")     return "guild " + ex("npc") + " = " + ex("guild");
  std::string r = ident(tx("fn")) + "(";
  for(size_t i=0; i<e.args.size(); ++i)
    r += (i ? ", " : "") + e.args[i]->str();
  r += ")";
  if(!e.writes.empty()) {
    r += " writes";
    for(auto& w : e.writes)
      r += " " + ident(w);
    }
  return r;
  }

static void describe(const Model& m, const Node& n, int depth, const std::string& indent, std::string& out,
                     std::set<std::string>& shown) {
  out += indent + ident(n.id) + (n.choice ? " [choice]" : "") + "  " + ident(n.npc);
  if(!n.choice)
    out += " nr " + std::to_string(n.nr) + (n.important ? " important" : "") + (n.permanent ? " permanent" : "") +
           (n.trade ? " trade" : "");
  out += "\n";
  if(!n.description.empty())
    out += indent + "  «" + n.description + "»\n";
  if(n.condition)
    out += indent + "  if: " + n.condition->str() + (n.notLifted.empty() ? "" : "   [not lifted: " + n.notLifted + "]") + "\n";
  for(auto& e : n.conditionEffects)
    out += indent + "  checking it: " + (e.when ? "[" + e.when->str() + "] " : "") + effectText(e) + "\n";
  if(!n.error.empty())
    out += indent + "  error: " + n.error + "\n";
  if(!shown.insert(n.id).second) {
    out += indent + "  (shown above)\n";
    return;
    }
  for(auto& e : n.effects) {
    out += indent + "  ";
    if(e.when)
      out += "[" + e.when->str() + "] ";
    out += effectText(e) + "\n";
    if(e.kind=="choice" && depth>0)
      if(auto* c = m.node(*e.text("node")))
        describe(m, *c, depth-1, indent + "      ", out, shown);
    }
  }

std::string storygraph::describeNode(const Model& m, const std::string& id, int depth) {
  auto* n = m.node(id);
  if(n==nullptr)
    return "no node " + id + "\n";
  std::string           out;
  std::set<std::string> shown;
  describe(m, *n, depth, "", out, shown);
  return out;
  }

std::string storygraph::describeQuest(const Model& m, const std::string& var) {
  auto it = m.quests.find(var);
  if(it==m.quests.end())
    return "no quest " + var + "\n";
  auto& q = it->second;
  std::string out = ident(var) + "\n";
  for(auto& t : q.topics)
    out += "  topic " + ident(t) + " «" + (q.titles.count(t) ? q.titles.at(t) : std::string()) + "»\n";
  auto list = [&out](const char* what, const std::set<std::string>& s) {
    if(s.empty())
      return;
    out += std::string("  ") + what + ":";
    for(auto& x : s)
      out += " " + ident(x);
    out += "\n";
    };
  list("running",  q.running);
  list("success",  q.success);
  list("failed",   q.failed);
  list("obsolete", q.obsolete);
  list("other",    q.other);
  auto v = m.vars.find(var);
  if(v!=m.vars.end()) {
    list("read by",          v->second.readers);
    list("also written by",  v->second.others);
    }

  std::set<std::string> nodes;
  for(auto* s : {&q.running, &q.success, &q.failed, &q.obsolete, &q.other})
    nodes.insert(s->begin(), s->end());
  if(v!=m.vars.end())
    nodes.insert(v->second.readers.begin(), v->second.readers.end());
  std::set<std::string> shown;
  for(auto& id : nodes)
    if(auto* n = m.node(id)) {
      out += "\n";
      describe(m, *n, 0, "", out, shown);
      }
  return out;
  }
