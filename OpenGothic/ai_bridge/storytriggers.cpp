#include "storytriggers.h"

#include <Tempest/Log>

#include <algorithm>
#include <cctype>

#include "game/gamescript.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include "exprjson.h"

#include "jsonread.h"
#include "jsonwriter.h"
#include "storyruntime.h"

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using namespace Tempest;

namespace {

constexpr uint64_t StateMs  = 250;  // how often the conditions of state triggers are computed
constexpr uint64_t DeathsMs = 500;  // how often deaths are looked for

std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

std::string symbolOf(World& world, const Npc* npc) {
  if(npc==nullptr)
    return {};
  return std::string(world.script().storyNpcName(npc->instanceSymbol()));
  }

Npc* bySymbol(World& world, const std::string& symbol) {
  const auto id = world.script().storyNpcId(symbol);
  return id ? world.findNpcByInstance(*id) : nullptr;
  }

const char* nameOf(int on) {
  switch(on) {
    case 1:  return "death";
    case 2:  return "dialog";
    default: return "state";
    }
  }

}

StoryTriggers::StoryTriggers(StoryRuntime& rt, std::function<void(std::string&&)> send)
  :rt(rt), send(std::move(send)) {
  }

void StoryTriggers::clear() {
  triggers.clear();
  }

void StoryTriggers::clearWorld() {
  deaths.clear();
  nextState = nextDeaths = 0;
  }

std::string StoryTriggers::apply(const rapidjson::Value& op) {
  Trigger t;
  t.id = upper(JsonRead::str(op,"id"));
  if(t.id.empty())
    return "trigger: no id";
  auto at = std::find_if(triggers.begin(),triggers.end(),[&t](const Trigger& x) { return x.id==t.id; });
  auto steps = op.FindMember("steps");
  if(steps==op.MemberEnd() || steps->value.IsNull()) {
    // no program: the trigger is taken away; its count stays, as a variable of the save
    if(at!=triggers.end())
      triggers.erase(at);
    return {};
    }

  const auto on = JsonRead::str(op,"on");
  if(on=="state")
    t.on = On::State;
  else if(on=="death")
    t.on = On::Death;
  else if(on=="dialog")
    t.on = On::Dialog;
  else
    return "trigger " + t.id + ": no event " + std::string(on) + " (state, death, dialog)";
  t.npc    = upper(JsonRead::str(op,"npc"));
  t.node   = upper(JsonRead::str(op,"node"));
  t.world  = upper(JsonRead::str(op,"world"));
  t.repeat = JsonRead::boolean(op,"repeat",false);
  if(!t.node.empty() && t.on!=On::Dialog)
    return "trigger " + t.id + ": a node is for dialog triggers";
  if(auto it = op.FindMember("when"); it!=op.MemberEnd() && !it->value.IsNull()) {
    std::string err;
    t.when = storygraph::readExpr(it->value,err);
    if(t.when==nullptr)
      return "trigger " + t.id + " when: " + err;
    }
  t.program = storygraph::readProgram(steps->value,false);
  if(!t.program.error.empty())
    return "trigger " + t.id + " steps: " + t.program.error;

  // its count: a variable of the director's, made at 0; one a save brought back keeps its value
  if(auto err = rt.addVar(t.id,0); !err.empty())
    return "trigger " + t.id + ": " + err;
  if(auto* sc = rt.script()) {
    // with a world, names are checked now; without, when it first runs
    if(auto err = check(t,*sc); !err.empty())
      return "trigger " + t.id + ": " + err;
    t.checkedFor = sc;
    }
  if(at!=triggers.end())
    *at = std::move(t); else
    triggers.push_back(std::move(t));
  return {};
  }

std::string StoryTriggers::check(const Trigger& t, GameScript& sc) const {
  if(!t.npc.empty() && !sc.storyNpcId(t.npc))
    return "no npc " + t.npc;
  if(t.when!=nullptr) {
    if(auto err = rt.checkExpr(t.when); !err.empty())
      return "when: " + err;
    }
  // outside a dialog: there is no menu for choices
  if(auto err = rt.checkProgram(t.program,0,false); !err.empty())
    return "steps: " + err;
  return {};
  }

bool StoryTriggers::ready(Trigger& t, GameScript& sc) {
  if(t.checkedFor!=&sc) {
    t.checkedFor = &sc;
    t.broken     = check(t,sc);
    if(!t.broken.empty())
      Log::e("[story] trigger ",t.id,": ",t.broken);
    }
  return t.broken.empty();
  }

bool StoryTriggers::inWorld(const Trigger& t, World& world) const {
  return t.world.empty() || t.world==upper(world.name());
  }

bool StoryTriggers::spent(const Trigger& t) {
  return !t.repeat && rt.intOf(t.id).value_or(1)!=0;
  }

std::optional<bool> StoryTriggers::holds(const Trigger& t, std::pair<Npc*,Npc*>& actors) {
  if(t.when==nullptr)
    return true;
  return rt.test(t.when,t.id,actors.first,actors.second,&actors);
  }

void StoryTriggers::tick(World& world) {
  auto*          sc  = rt.script();
  Npc*           pl  = world.player();
  const uint64_t now = world.tickCount();
  if(sc==nullptr || pl==nullptr)
    return;
  if(now>=nextDeaths || nextDeaths>now+DeathsMs) {
    nextDeaths = now+DeathsMs;
    // looked for even without death triggers: one put later must not take old deaths for news
    for(auto& d : deaths.look(world))
      onDeath(world,*d.npc,d.killer);
    }
  if(now<nextState && nextState<=now+StateMs)
    return;
  nextState = now+StateMs;
  for(size_t i=0; i<triggers.size(); ++i) {
    auto& t = triggers[i];
    if(t.on!=On::State || !inWorld(t,world) || !ready(t,*sc) || spent(t))
      continue;
    Npc* self = t.npc.empty() ? pl : bySymbol(world,t.npc);
    if(self==nullptr)
      continue;  // not in this world, or not yet
    auto actors = std::pair{self,pl};
    const bool is = holds(t,actors).value_or(false);
    if(t.repeat) {
      const bool came = t.was.has_value() && !*t.was && is;
      t.was = is;
      if(!came)
        continue;
      }
    else if(!is) {
      continue;
      }
    fire(world,t,self,pl,actors);
    }
  }

void StoryTriggers::onDeath(World& world, Npc& dead, Npc* killer) {
  auto* sc = rt.script();
  if(sc==nullptr)
    return;
  const auto who = symbolOf(world,&dead);
  for(size_t i=0; i<triggers.size(); ++i) {
    auto& t = triggers[i];
    if(t.on!=On::Death || (!t.npc.empty() && t.npc!=who) || !inWorld(t,world) || !ready(t,*sc) || spent(t))
      continue;
    auto actors = std::pair{&dead,killer};
    if(holds(t,actors).value_or(false))
      fire(world,t,&dead,killer,actors);
    }
  }

void StoryTriggers::onDialog(World& world, Npc& npc, const std::string& node) {
  auto* sc = rt.script();
  Npc*  pl = world.player();
  if(sc==nullptr || pl==nullptr)
    return;
  const auto who = symbolOf(world,&npc);
  for(size_t i=0; i<triggers.size(); ++i) {
    auto& t = triggers[i];
    if(t.on!=On::Dialog || (!t.npc.empty() && t.npc!=who) || (!t.node.empty() && t.node!=node) ||
       !inWorld(t,world) || !ready(t,*sc) || spent(t))
      continue;
    auto actors = std::pair{&npc,pl};
    if(holds(t,actors).value_or(false))
      fire(world,t,&npc,pl,actors);
    }
  }

void StoryTriggers::fire(World& world, Trigger& t, Npc* self, Npc* other, const std::pair<Npc*,Npc*>& actors) {
  auto* sc = rt.script();
  if(sc==nullptr)
    return;
  // the count first: the program sees itself fired, and one that fails half way does not run again
  const int32_t count = rt.intOf(t.id).value_or(0)+1;
  rt.setInt(t.id,count);
  // A query such as NPC_GETTARGET can replace OTHER for the trigger program.
  auto r = rt.run(t.program,actors.first,actors.second);
  fired++;
  JsonWriter j("trigger_fired");
  j.str("id",t.id).integer("count",count).str("npc",symbolOf(world,self)).str("other",symbolOf(world,other));
  if(!r.ok) {
    failed++;
    Log::e("[story] trigger ",t.id,": ",r.error);
    j.str("error",r.error);
    } else {
    Log::i("[story] trigger ",t.id," fired (",count,")");
    }
  send(j.finish());
  }

std::string StoryTriggers::list(World& world) {
  auto* sc = rt.script();
  Npc*  pl = world.player();
  rapidjson::StringBuffer                    buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  auto key = [&w](const char* k, const std::string& s) {
    w.Key(k);
    w.String(s.c_str(),rapidjson::SizeType(s.size()));
    };
  w.StartArray();
  for(auto& t : triggers) {
    w.StartObject();
    key("id",t.id);
    key("on",nameOf(int(t.on)));
    key("npc",t.npc);
    key("node",t.node);
    w.Key("repeat"); w.Bool(t.repeat);
    w.Key("count");  w.Int(rt.intOf(t.id).value_or(0));
    w.Key("holds");
    Npc* self = t.npc.empty() ? pl : bySymbol(world,t.npc);
    if(sc!=nullptr && pl!=nullptr && self!=nullptr && t.on==On::State && ready(t,*sc)) {
      auto actors = std::pair{self,pl};
      auto is = holds(t,actors);
      if(is)
        w.Bool(*is); else
        w.Null();
      } else {
      w.Null();
      }
    if(!t.broken.empty())
      key("broken",t.broken);
    w.EndObject();
    }
  w.EndArray();
  return std::string(buf.GetString(),buf.GetSize());
  }

std::string StoryTriggers::status() const {
  return std::to_string(triggers.size()) + " triggers, " + std::to_string(fired) + " fired, " +
         std::to_string(failed) + " failed";
  }
