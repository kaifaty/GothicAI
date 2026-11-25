#include "dialoggraph.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

#include "commandline.h"
#include "gothic.h"
#include "world/aiqueue.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include "exprjson.h"

#include "jsonread.h"
#include "jsonwriter.h"
#include "textcodec.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace Tempest;
using storygraph::ExprP;
using storygraph::Expr;

namespace {

std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

std::string str(const rapidjson::Value& v, const char* key) {
  return std::string(JsonRead::str(v,key));
  }

bool usesRandom(const ExprP& e) {
  bool r = false;
  storygraph::ex::walk(e, [&r](const Expr& x) { r |= (x.kind==Expr::Call && x.name=="HLP_RANDOM"); });
  return r;
  }

}

DialogGraph::DialogGraph(std::function<void(std::string&&)> send,
                         std::function<std::string(const std::string&,const std::string&)> addLine,
                         StoryRuntime& rt)
  :send(std::move(send)), addLine(std::move(addLine)), rt(rt) {
  const char* env = std::getenv("GOTHIC_AI_STORYCHECK");
  checking = env!=nullptr ? std::string_view(env)=="1" : CommandLine::inst().isDevMode();

  auto& g = Gothic::inst();
  g.dialogFilterFn       = [this](const daedalus::IInfo& info, bool& visible) { onFilter(info,visible); };
  g.dialogChoiceFilterFn = [this](const daedalus::IInfo& info, int32_t fn) { return onChoiceFilter(info,fn); };
  g.dialogExtraFn        = [this](const daedalus::INpc& npc, bool important, std::vector<GameScript::DlgChoice>& out) {
    onExtra(npc,important,out);
    };
  g.dialogExtraExecFn    = [this](const GameScript::DlgChoice& dlg, Npc& player, Npc& npc) { onExec(dlg,player,npc); };
  g.dialogExtraChoicesFn = [this](const GameScript::DlgChoice& dlg, std::vector<GameScript::DlgChoice>& out) {
    onChoices(dlg,out);
    };
  g.onDialogChosen.bind(this,&DialogGraph::onChosen);
  g.dialogScriptFn       = [this](uint32_t fn, int32_t& result) { return onScript(fn,result); };
  g.dialogStockResultFn  = [this](bool ranProgram, bool failed) {
    ran += size_t(ranProgram);
    notPrograms += size_t(!ranProgram);
    runErrors += size_t(failed);
    };
  // a choice step of a program: the dialog of the director that runs offers it
  rt.choices = [this](const storygraph::Step& s) {
    if(runningOwn.empty())
      return false;
    auto& list = offering[runningOwn];
    if(s.kind==storygraph::Step::ClearChoices) {
      list.clear();
      return true;
      }
    // offered again: once in the menu
    list.erase(std::remove_if(list.begin(),list.end(),[&s](const Choice& c) { return c.id==s.id; }),list.end());
    list.push_back(Choice{s.id,runningOwn,s.text,s.then});
    choiceOf[s.id] = runningOwn;
    return true;
    };
  if(checking)
    Log::i("[story] conditions are checked against their lifted form");
  }

DialogGraph::~DialogGraph() {
  auto& g = Gothic::inst();
  g.dialogFilterFn       = nullptr;
  g.dialogChoiceFilterFn = nullptr;
  g.dialogExtraFn        = nullptr;
  g.dialogExtraExecFn    = nullptr;
  g.dialogExtraChoicesFn = nullptr;
  g.dialogScriptFn       = nullptr;
  g.dialogStockResultFn  = nullptr;
  g.onDialogChosen.ubind(this,&DialogGraph::onChosen);
  rt.choices = nullptr;
  }

void DialogGraph::clear() {
  disabled.clear();
  conditions.clear();
  nodes.clear();
  told.clear();
  offering.clear();
  choiceOf.clear();
  replaced.clear();
  replacedFns.clear();
  replacedFor = nullptr;
  reported.clear();
  }

std::string DialogGraph::summary() const {
  return std::to_string(disabled.size()) + " off, " + std::to_string(conditions.size()) + " with conditions, " +
         std::to_string(nodes.size()) + " new, " + std::to_string(replaced.size()) + " programs replaced";
  }

std::string DialogGraph::applyOp(const rapidjson::Value& op) {
  const auto kind = str(op,"op");
  if(kind=="vars") {
    // the director's story variables (v29): globals of the scripts' kind that the save keeps
    auto it = op.FindMember("vars");
    if(it==op.MemberEnd() || !it->value.IsObject())
      return "vars: no vars";
    auto* sc = rt.script();
    for(auto& m : it->value.GetObject()) {
      const auto name = upper(std::string(m.name.GetString(),m.name.GetStringLength()));
      if(!m.value.IsInt())
        return "vars: " + name + " is not an integer";
      if(name.rfind(OwnSymbols::Prefix,0)!=0)
        return "vars: " + name + ": a variable of the director's is named DIR_...";
      if(sc!=nullptr) {
        if(auto err = StoryRuntime::whyNotVar(*sc,name); !err.empty())
          return "vars: " + err;
        }
      }
    for(auto& m : it->value.GetObject())
      if(auto err = rt.addVar(upper(std::string(m.name.GetString(),m.name.GetStringLength())),m.value.GetInt()); !err.empty())
        return "vars: " + err;
    return {};
    }
  if(kind=="add_node") {
    auto it = op.FindMember("node");
    if(it==op.MemberEnd() || !it->value.IsObject())
      return "add_node: no node";
    Node n;
    auto err = readNode(it->value,n);
    if(!err.empty())
      return err;
    auto id = n.id;
    nodes[id] = std::move(n);
    return {};
    }

  const auto node = upper(str(op,"node"));
  if(node.empty())
    return kind + ": no node";
  if(kind=="disable") {
    disabled.insert(node);
    return {};
    }
  if(kind=="enable") {
    disabled.erase(node);
    return {};
    }
  if(kind=="remove_node") {
    nodes.erase(node);
    told.erase(node);
    return {};
    }
  if(kind=="told") {
    told.insert(node);
    return {};
    }
  if(kind=="program") {
    const auto part = str(op,"part");
    if(!part.empty() && part!="information" && part!="condition")
      return "program: part is information or condition, not " + part;
    Replaced r;
    r.node      = node;
    r.condition = part=="condition";
    const auto key = node + (r.condition ? ":CONDITION" : "");
    replacedFor = nullptr;  // functions are found again
    auto it = op.FindMember("steps");
    if(it==op.MemberEnd() || it->value.IsNull()) {
      replaced.erase(key);  // the scripts' own program again
      return {};
      }
    // a condition returns whether the dialog is offered; an information or a choice returns nothing
    r.program = storygraph::readProgram(it->value, r.condition);
    if(!r.program.error.empty())
      return "program: " + r.program.error;
    if(auto* sc = rt.script()) {
      // with a world, names are checked now; without, when the program first runs
      std::string err;
      auto fn = functionOf(*sc,node,r.condition,err);
      if(!fn)
        return "program: " + err;
      err = rt.checkProgram(r.program,0,false);
      if(!err.empty())
        return "program: " + err;
      }
    replaced[key] = std::move(r);
    return {};
    }
  if(kind=="condition") {
    auto it = op.FindMember("expr");
    if(it==op.MemberEnd() || it->value.IsNull()) {
      conditions.erase(node);  // no expression: the ones added before are dropped
      return {};
      }
    std::string err;
    auto e = storygraph::readExpr(it->value,err);
    if(e==nullptr)
      return "condition: " + err;
    err = rt.checkExpr(e);
    if(!err.empty())
      return "condition: " + err;
    conditions[node].push_back(e);
    return {};
    }
  return "unknown op " + kind;
  }

std::string DialogGraph::readNode(const rapidjson::Value& v, Node& n) {
  n.id          = upper(str(v,"id"));
  n.npc         = upper(str(v,"npc"));
  n.description = TextCodec::fromUtf8(JsonRead::str(v,"description"));
  n.nr          = int32_t(JsonRead::integer(v,"nr",0));
  n.important   = JsonRead::boolean(v,"important",false);
  n.permanent   = JsonRead::boolean(v,"permanent",false) && !n.important;
  if(n.id.empty() || n.npc.empty())
    return "add_node: needs id and npc";
  if(!n.important && n.description.empty())
    return "add_node: a dialog the player picks needs a description";
  auto* sc = rt.script();
  if(sc!=nullptr) {
    if(sc->storySymbolIndex(n.id))
      return "add_node: " + n.id + " is a script symbol";
    if(!sc->storyNpcId(n.npc))
      return "add_node: no npc " + n.npc;
    }

  if(auto it = v.FindMember("condition"); it!=v.MemberEnd() && !it->value.IsNull()) {
    std::string err;
    n.condition = storygraph::readExpr(it->value,err);
    if(n.condition==nullptr)
      return "add_node condition: " + err;
    err = rt.checkExpr(n.condition);
    if(!err.empty())
      return "add_node condition: " + err;
    }

  if(auto it = v.FindMember("lines"); it!=v.MemberEnd() && it->value.IsArray()) {
    for(auto& l : it->value.GetArray()) {
      Line line;
      line.hero = JsonRead::str(l,"speaker")=="other";
      line.id   = upper(str(l,"line"));
      line.text = TextCodec::fromUtf8(JsonRead::str(l,"text"));
      if(line.id.empty() && line.text.empty())
        return "add_node line: needs line or text";
      if(!line.id.empty() && Gothic::inst().messageByName(line.id).empty())
        return "add_node line: no original line " + line.id;
      n.lines.push_back(std::move(line));
      }
    }

  if(auto it = v.FindMember("effects"); it!=v.MemberEnd() && it->value.IsArray()) {
    for(auto& x : it->value.GetArray()) {
      StoryEffect e;
      auto err = StoryEffect::read(x,sc,e);
      if(!err.empty())
        return "add_node " + err;
      n.effects.push_back(std::move(e));
      }
    }

  if(auto it = v.FindMember("program"); it!=v.MemberEnd() && !it->value.IsNull()) {
    n.program = storygraph::readProgram(it->value,false);
    if(!n.program.error.empty())
      return "add_node program: " + n.program.error;
    // choices without an id are the dialog's id and their number, in the order they are written
    size_t number = 0;
    std::function<void(std::vector<storygraph::Step>&)> name = [&](std::vector<storygraph::Step>& steps) {
      for(auto& st : steps) {
        if(st.kind==storygraph::Step::Choice && st.id.empty())
          st.id = n.id + "." + std::to_string(++number);
        name(st.then);
        name(st.otherwise);
        }
      };
    name(n.program.steps);
    if(auto err = rt.checkProgram(n.program); !err.empty())
      return "add_node program: " + err;
    }
  return {};
  }

std::optional<uint32_t> DialogGraph::functionOf(GameScript& script, const std::string& node, bool condition,
                                                std::string& error) const {
  if(auto fn = script.storyDialogFunction(node,condition))
    return fn;
  const auto kind = script.storySymbolKind(node);
  if(kind==GameScript::SymbolKind::Info) {
    error = node + " has no " + (condition ? "condition" : "information");
    return std::nullopt;
    }
  if(!condition)
    if(auto id = script.storyScriptFunctionId(node))
      return id; // A stock choice.
  error = "no dialog or choice " + node;
  return std::nullopt;
  }

const storygraph::Program* DialogGraph::replacedProgram(GameScript& sc, uint32_t fn) {
  if(replacedFor!=&sc) {
    replacedFns.clear();
    for(auto& [key,r] : replaced) {
      std::string err;
      auto f = functionOf(sc,r.node,r.condition,err);
      if(f)
        err = rt.checkProgram(r.program,0,false);
      if(!err.empty()) {
        if(reported.insert("program:" + key).second)
          Log::e("[story] program for ",key,": ",err);
        continue;
        }
      if(f)
        replacedFns[*f] = &r.program;
      }
    replacedFor = &sc;
    }
  auto it = replacedFns.find(fn);
  return it!=replacedFns.end() ? it->second : nullptr;
  }

void DialogGraph::check(const daedalus::IInfo& info, bool visible) {
  auto* sc = rt.script();
  if(sc==nullptr || info.condition==0)
    return;
  if(!replaced.empty() && replacedProgram(*sc,uint32_t(info.condition))!=nullptr)
    return;

  const auto name = std::string(sc->storyDialogName(info.symbol_index()));
  const auto e = rt.originalCondition(name);
  if(!e || usesRandom(e))
    return;
  std::string error;
  const auto [self,other] = sc->storyActors();
  const auto r = rt.evaluate(e,error,self,other);
  checked++;
  if(r && r->type==storygraph::NativeValue::Int && (r->i!=0)==visible)
    return;
  differ++;
  if(reported.insert("check:" + name).second) {
    differences.push_back(name);
    Log::e("[story] check: ",name," actual ",int(visible),", model ",
           r ? std::to_string(r->i) : "? (" + error + ")",": ",e->str());
    }
  }

void DialogGraph::onFilter(const daedalus::IInfo& info, bool& visible) {
  if(checking)
    check(info,visible);
  if(disabled.empty() && conditions.empty())
    return;
  auto* sc = rt.script();
  if(sc==nullptr)
    return;
  const auto name = std::string(sc->storyDialogName(info.symbol_index()));
  if(disabled.count(name)) {
    visible = false;
    return;
    }
  if(!visible)
    return;
  auto it = conditions.find(name);
  if(it==conditions.end())
    return;
  const auto [self,other] = sc->storyActors();
  for(const auto& c : it->second) {
    // A condition that can't be computed leaves the stock visibility alone.
    auto r = rt.test(c,name,self,other);
    if(r && !*r) {
      visible = false;
      return;
      }
    }
  }

bool DialogGraph::onChoiceFilter(const daedalus::IInfo&, int32_t fn) {
  auto* sc = rt.script();
  return disabled.empty() || sc==nullptr || disabled.count(std::string(sc->storyProgramName(uint32_t(fn))))==0;
  }

void DialogGraph::onExtra(const daedalus::INpc& npc, bool important, std::vector<GameScript::DlgChoice>& out) {
  auto* sc = rt.script();
  if(nodes.empty() || sc==nullptr)
    return;
  const auto name = sc->storyNpcName(npc.symbol_index());
  if(name.empty())
    return;
  const auto [self,other] = sc->storyActors();
  for(auto& [id,n] : nodes) {
    if(n.npc!=name || n.important!=important || disabled.count(id))
      continue;
    if(told.count(id) && !n.permanent)
      continue;
    if(n.condition!=nullptr) {
      auto r = rt.test(n.condition,id,self,other);
      if(!r || !*r)
        continue;
      }
    GameScript::DlgChoice ch;
    ch.title    = important ? std::string() : n.description;
    ch.sort     = n.nr;
    ch.external = id;
    out.push_back(std::move(ch));
    }
  }

void DialogGraph::onExec(const GameScript::DlgChoice& dlg, Npc& player, Npc& npc) {
  for(auto& [node,list] : offering)
    for(size_t i=0; i<list.size(); ++i) {
      if(list[i].id!=dlg.external)
        continue;
      // a choice it offered: picked, it is no longer offered (as a script dialog's), and its steps run
      auto picked = std::move(list[i]);
      list.erase(list.begin()+std::ptrdiff_t(i));
      runOwn(picked.node,picked.steps,npc,player);
      return;
      }
  auto it = nodes.find(dlg.external);
  if(it==nodes.end())
    return;
  auto& n = it->second;
  told.insert(n.id);
  offering.erase(n.id);  // what it offers comes from this run

  // lines are queued like AI_Output in an info function; effects happen right away, like there
  auto* sc = rt.script();
  for(auto& l : n.lines) {
    Npc& speaker  = l.hero ? player : npc;
    Npc& listener = l.hero ? npc : player;
    auto id = l.id;
    if(id.empty()) {
      const auto name = sc ? std::string(sc->storyNpcName(speaker.instanceSymbol())) : std::string{};
      id = addLine(name,l.text);
      }
    speaker.aiPush(AiQueue::aiOutput(listener,id,0));
    }
  if(sc==nullptr)
    return;
  for(auto& e : n.effects)
    e.run(*sc,npc,player,n.id);
  if(!n.program.steps.empty())
    runOwn(n.id,n.program.steps,npc,player);
  }

void DialogGraph::runOwn(const std::string& node, const std::vector<storygraph::Step>& steps, Npc& npc, Npc& player) {
  storygraph::Program p;
  p.steps = steps;
  const auto outer = std::exchange(runningOwn,node);
  auto r = rt.run(p,&npc,&player,true);
  runningOwn = outer;
  replacedRuns++;
  if(!r.ok) {
    runErrors++;
    Log::e("[story] program of ",node,": ",r.error);
    }
  }

void DialogGraph::onChoices(const GameScript::DlgChoice& dlg, std::vector<GameScript::DlgChoice>& out) {
  // after a dialog of the director, or after one of its choices (picked, so no longer offered): what the
  // dialog offers now
  auto owner = choiceOf.find(dlg.external);
  auto it    = offering.find(owner!=choiceOf.end() ? owner->second : dlg.external);
  if(it==offering.end())
    return;
  int32_t i = 0;
  for(auto& c : it->second) {
    if(disabled.count(c.id))
      continue;
    GameScript::DlgChoice ch;
    ch.title    = c.text;
    ch.sort     = i++;  // in the order they were offered (Info_AddChoice shows the last first)
    ch.external = c.id;
    out.push_back(std::move(ch));
    }
  }

bool DialogGraph::onScript(uint32_t fn, int32_t& result) {
  auto* sc = rt.script();
  if(sc==nullptr || replaced.empty())
    return false;
  const auto* program = replacedProgram(*sc,fn);
  if(program==nullptr)
    return false;
  const auto [self,other] = sc->storyActors();
  auto r = rt.run(*program,self,other);
  replacedRuns++;
  if(!r.ok) {
    runErrors++;
    Log::e("[story] program for ",sc->storyProgramName(fn),": ",r.error);
    }
  result = r.returned.type==storygraph::NativeValue::Int ? r.returned.i : 0;
  return true;
  }

void DialogGraph::onChosen(const GameScript::DlgChoice& dlg, Npc& player, Npc& npc) {
  auto* sc = rt.script();
  if(sc==nullptr || &player==&npc)
    return;
  std::string node = dlg.external;
  if(node.empty() && dlg.handle!=nullptr)
    node = dlg.scriptFn==uint32_t(dlg.handle->information) ? sc->storyDialogName(dlg.handle->symbol_index())
                                                          : sc->storyProgramName(dlg.scriptFn);
  const auto npcName = sc->storyNpcName(npc.instanceSymbol());
  if(node.empty() || npcName.empty())
    return;
  send(JsonWriter("dialog_chosen")
         .str("npc",npcName)
         .str("node",node)
         .boolean("made",!dlg.external.empty())
         .finish());
  if(chosen)
    chosen(npc,node);
  }

std::vector<std::string> DialogGraph::offered(World& world, Npc& npc) {
  std::vector<std::string> ret;
  Npc* player = world.player();
  auto& sc    = world.script();
  if(player==nullptr)
    return ret;
  // important dialogs start on their own and hide the others; list both
  for(bool important : {true,false}) {
    for(auto& ch : npc.dialogChoices(*player,{},important)) {
      std::string id = ch.external;
      if(id.empty() && ch.handle!=nullptr)
        id = sc.storyDialogName(ch.handle->symbol_index());
      if(!id.empty() && std::find(ret.begin(),ret.end(),id)==ret.end())
        ret.push_back(id);
      }
    }
  return ret;
  }

bool DialogGraph::choose(World& world, Npc& npc, std::string_view node, std::string& error) {
  Npc* player = world.player();
  auto& sc    = world.script();
  if(player==nullptr) {
    error = "no world";
    return false;
    }
  const auto id = upper(node);
  for(bool important : {true,false}) {
    for(auto& ch : npc.dialogChoices(*player,{},important)) {
      if(ch.external==id || (ch.handle!=nullptr && sc.storyDialogName(ch.handle->symbol_index())==id)) {
        Gothic::inst().dialogExec(ch,*player,npc);
        return true;
        }
      }
    }
  // a choice that a dialog of the director, this npc's, offers now
  const auto self = sc.storyNpcName(npc.instanceSymbol());
  for(auto& [node,list] : offering) {
    auto made = nodes.find(node);
    if(self.empty() || made==nodes.end() || made->second.npc!=self)
      continue;
    for(auto& c : list)
      if(c.id==id) {
        GameScript::DlgChoice ch;
        ch.title    = c.text;
        ch.external = c.id;
        Gothic::inst().dialogExec(ch,*player,npc);
        return true;
        }
    }
  error = id + " is not offered";
  return false;
  }

std::vector<std::pair<std::string,std::string>> DialogGraph::choices(World& world, Npc& npc, std::string_view node) {
  std::vector<std::pair<std::string,std::string>> ret;
  Npc* player = world.player();
  if(player==nullptr)
    return ret;
  GameScript::DlgChoice dlg;
  dlg.external = upper(node);
  // as the dialog menu asks after the lines
  for(auto& ch : Gothic::inst().updateDialog(dlg,*player,npc))
    ret.emplace_back(ch.external.empty() ? std::to_string(ch.scriptFn) : ch.external, TextCodec::toUtf8(ch.title));
  return ret;
  }

bool DialogGraph::evaluate(World& world, Npc* npc, const rapidjson::Value& expr, std::string& json,
                           std::string& error) {
  auto e = storygraph::readExpr(expr,error);
  if(e==nullptr)
    return false;
  error = rt.checkExpr(e);
  if(!error.empty())
    return false;
  auto [self,other] = world.script().storyActors();
  std::string instanceName;
  auto r = rt.evaluate(e,error,npc!=nullptr ? npc : self,world.player()!=nullptr ? world.player() : other,&instanceName);
  if(!r)
    return false;
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  switch(r->type) {
    case storygraph::NativeValue::Int:   w.Int(r->i);    break;
    case storygraph::NativeValue::Float: w.Double(r->f); break;
    case storygraph::NativeValue::Str: {
      auto s = TextCodec::toUtf8(r->s);
      w.String(s.data(),rapidjson::SizeType(s.size()));
      break;
      }
    case storygraph::NativeValue::Inst:
      if(r->inst!=0)
        w.String(instanceName.data(),rapidjson::SizeType(instanceName.size()));
      else
        w.Null();
      break;
    }
  json.assign(buf.GetString(),buf.GetSize());
  return true;
  }

std::string DialogGraph::checkWorld(World& world) {
  const bool   was    = checking;
  const size_t before = checked, differBefore = differ;
  uint32_t     npcs   = 0;
  checking = true;
  for(uint32_t i=0; i<world.npcCount(); ++i) {
    Npc* npc = world.npcById(i);
    if(npc==nullptr || npc->isPlayer())
      continue;
    offered(world,*npc);
    npcs++;
    }
  checking = was;
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  w.StartObject();
  w.Key("npcs");     w.Uint(npcs);
  w.Key("computed"); w.Uint64(checked-before);
  w.Key("differ");   w.Uint64(differ-differBefore);
  w.Key("differences");
  w.StartArray();
  for(auto& d : differences)
    w.String(d.data(),rapidjson::SizeType(d.size()));
  w.EndArray();
  // The "vm" wire field counts stock functions without a native program.
  w.Key("programs");
  w.StartObject();
  w.Key("on");     w.Bool(true);
  w.Key("runs");   w.Uint64(ran);
  w.Key("failed"); w.Uint64(runErrors);
  w.Key("vm");     w.Uint64(notPrograms);
  w.Key("replaced"); w.Uint64(replaced.size());
  w.Key("director"); w.Uint64(replacedRuns);
  w.EndObject();
  w.EndObject();
  Log::i("[story] check of the world: ",npcs," npcs, ",checked-before," conditions computed, ",
         differ-differBefore," differ");
  return std::string(buf.GetString(),buf.GetSize());
  }

std::string DialogGraph::status() const {
  return "story graph: " + std::to_string(disabled.size()) + " off, " + std::to_string(conditions.size()) +
         " with conditions, " + std::to_string(nodes.size()) + " new (" + std::to_string(told.size()) +
         " told); check " + (checking ? "on" : "off") + ": " + std::to_string(checked) + " computed, " +
         std::to_string(differ) + " differ; native programs: " + std::to_string(ran) +
         " runs, " + std::to_string(runErrors) + " failed, " + std::to_string(notPrograms) + " not programs; " +
         std::to_string(replaced.size()) + " replaced, " + std::to_string(replacedRuns) + " director's runs; " +
         std::to_string(rt.varCount()) + " vars";
  }

std::string DialogGraph::program(std::string_view node, std::string_view part, bool own, std::string& error) {
  auto* sc = rt.script();
  if(sc==nullptr) {
    error = "no world";
    return {};
    }
  if(!part.empty() && part!="information" && part!="condition") {
    error = "part is information or condition, not " + std::string(part);
    return {};
    }
  const auto id = upper(node);
  auto fn = functionOf(*sc,id,part=="condition",error);
  if(!fn)
    return {};
  storygraph::Program scripts;
  const storygraph::Program* p = replaced.empty() ? nullptr : replacedProgram(*sc,*fn);
  const bool patched = p!=nullptr;
  if(p==nullptr || own) {
    scripts = rt.originalProgram(*fn);
    p       = &scripts;
    }
  if(!p->error.empty()) {
    error = std::string(sc->storyProgramName(*fn)) + " is not a program: " + p->error;
    return {};
    }
  auto steps = storygraph::toJson(*p);
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  w.StartObject();
  w.Key("node");    w.String(id.c_str(),rapidjson::SizeType(id.size()));
  auto name = storygraph::ident(std::string(sc->storyProgramName(*fn)));
  w.Key("fn");      w.String(name.c_str(),rapidjson::SizeType(name.size()));
  w.Key("returns"); w.Bool(p->returns);
  w.Key("patched"); w.Bool(patched);
  w.Key("steps");   w.RawValue(steps.c_str(),steps.size(),rapidjson::kArrayType);
  w.EndObject();
  return std::string(buf.GetString(),buf.GetSize());
  }
