#include "storyeffects.h"

#include <Tempest/Log>

#include <cctype>
#include <optional>

#include <zenkit/DaedalusVm.hh>

#include "game/gamescript.h"
#include "game/herodeed.h"
#include "game/inventory.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "gothic.h"
#include "world/aiqueue.h"

#include "eval.h"
#include "jsonread.h"
#include "npcchests.h"
#include "textcodec.h"

using namespace Tempest;
using zenkit::DaedalusDataType;
using storygraph::Value;

static std::string upper(std::string_view s) {
  std::string r(s);
  for(auto& c:r)
    c = char(std::toupper(static_cast<unsigned char>(c)));
  return r;
  }

namespace {

// What an npc teaches (v17): the scripts' own teacher function, and for those that teach one thing of a
// talent (a rune, a potion...), the talent their learn cost is asked for (B_GetLearnCostTalent).
struct Teaching {
  std::string_view kind;
  const char*      fn;
  const char*      talent;
  };

constexpr Teaching Teachings[] = {
  {"attribute", "B_TEACHATTRIBUTEPOINTS",              nullptr},
  {"fight",     "B_TEACHFIGHTTALENTPERCENT",           nullptr},
  {"thief",     "B_TEACHTHIEFTALENT",                  nullptr},
  {"circle",    "B_TEACHMAGICCIRCLE",                  "NPC_TALENT_MAGE"},
  {"rune",      "B_TEACHPLAYERTALENTRUNES",            "NPC_TALENT_RUNES"},
  {"alchemy",   "B_TEACHPLAYERTALENTALCHEMY",          "NPC_TALENT_ALCHEMY"},
  {"smith",     "B_TEACHPLAYERTALENTSMITH",            "NPC_TALENT_SMITH"},
  {"trophy",    "B_TEACHPLAYERTALENTTAKEANIMALTROPHY", "NPC_TALENT_TAKEANIMALTROPHY"},
  };

// no teacher's own limit: how far the npc goes is the npc's to say (the server's deal)
constexpr int32_t AttributeMax = 1000;
constexpr int32_t FightMax     = 100;

const Teaching* teaching(std::string_view kind) {
  for(auto& t:Teachings)
    if(t.kind==kind)
      return &t;
  return nullptr;
  }

int32_t intConst(zenkit::DaedalusVm& vm, std::string_view name) {
  auto* s = vm.find_symbol_by_name(name);
  return s!=nullptr && s->type()==DaedalusDataType::INT ? s->get_int(0) : 0;
  }

}

std::string StoryEffect::read(const rapidjson::Value& x, zenkit::DaedalusVm* vm, StoryEffect& e) {
  e.kind   = std::string(JsonRead::str(x,"do"));
  e.item   = upper(JsonRead::str(x,"item"));
  e.chest  = upper(JsonRead::str(x,"chest"));
  e.teach  = std::string(JsonRead::str(x,"teach"));
  e.arg    = upper(JsonRead::str(x,"arg"));
  e.var    = upper(JsonRead::str(x,"var"));
  e.reason = upper(JsonRead::str(x,"reason"));
  e.attitude = upper(JsonRead::str(x,"attitude"));
  e.info   = upper(JsonRead::str(x,"info"));
  e.topic  = TextCodec::fromUtf8(JsonRead::str(x,"topic"));
  e.text   = TextCodec::fromUtf8(JsonRead::str(x,"text"));
  e.status = upper(JsonRead::str(x,"status"));
  e.target  = upper(JsonRead::str(x,"npc"));
  e.routine = upper(JsonRead::str(x,"routine"));
  e.count  = int32_t(JsonRead::integer(x,"count",JsonRead::integer(x,"amount",
                     JsonRead::integer(x,"value",JsonRead::integer(x,"seconds",1)))));
  e.day    = int32_t(JsonRead::integer(x,"day",0));
  e.toNpc  = JsonRead::str(x,"to")=="npc";
  e.toHero = JsonRead::str(x,"to")!="self" && !e.toNpc;
  auto* item = vm ? vm->find_symbol_by_name(e.item) : nullptr;
  auto* var  = vm ? vm->find_symbol_by_name(e.var) : nullptr;
  if(e.kind=="give" || e.kind=="create" || e.kind=="chest_take") {
    if(vm && (item==nullptr || item->type()!=DaedalusDataType::INSTANCE))
      return e.kind + ": no item " + e.item;
    if(e.count<=0)
      return e.kind + ": count must be positive";
    if(e.kind=="chest_take" && e.chest.empty())
      return "chest_take: needs a chest";
    if(e.kind=="give" && e.toNpc) {
      auto* t = vm && !e.target.empty() ? vm->find_symbol_by_name(e.target) : nullptr;
      if(e.target.empty() || (vm && (t==nullptr || t->type()!=DaedalusDataType::INSTANCE)))
        return "give: no npc " + e.target;
      }
    return {};
    }
  if(e.kind=="teach") {
    auto* t = teaching(e.teach);
    if(t==nullptr)
      return "teach: unknown teaching " + e.teach;
    if(vm && vm->find_symbol_by_name(t->fn)==nullptr)
      return std::string("teach: the scripts have no ") + t->fn;
    if(e.teach=="circle")
      return e.count<1 || e.count>6 ? "teach: circle must be 1..6" : "";
    auto* a = vm && !e.arg.empty() ? vm->find_symbol_by_name(e.arg) : nullptr;
    if(e.arg.empty() || (vm && (a==nullptr || !a->is_const() || a->type()!=DaedalusDataType::INT)))
      return "teach: no constant " + e.arg;
    if((e.teach=="attribute" || e.teach=="fight") && e.count<=0)
      return "teach: count must be positive";
    return {};
    }
  if(e.kind=="set") {
    if(vm && (var==nullptr || var->type()!=DaedalusDataType::INT || var->is_const() || var->is_member()))
      return "set: no variable " + e.var;
    return {};
    }
  if(e.kind=="crime_memory") {
    if(e.count<0 || e.count>4 || e.day<0)
      return "crime_memory: value must be 0..4 and day nonnegative";
    if(vm) {
      for(auto name:{"AIV_NPCSAWPLAYERCOMMIT","AIV_NPCSAWPLAYERCOMMITDAY","AIV_CRIMEABSOLUTIONLEVEL"}) {
        auto* s = vm->find_symbol_by_name(name);
        if(s==nullptr || !s->is_const() || s->type()!=DaedalusDataType::INT ||
           s->get_int(0)<0 || uint32_t(s->get_int(0))>=zenkit::INpc::aivar_count)
          return std::string("crime_memory: no AIV constant ")+name;
        }
      if(vm->find_symbol_by_name("B_GETCURRENTABSOLUTIONLEVEL")==nullptr)
        return "crime_memory: no B_GetCurrentAbsolutionLevel";
      }
    return {};
    }
  if(e.kind=="journal") {
    if(e.status.empty())
      e.status = "LOG_RUNNING";
    auto* st = vm ? vm->find_symbol_by_name(e.status) : nullptr;
    if(vm && (st==nullptr || !st->is_const() || st->type()!=DaedalusDataType::INT))
      return "journal: no status " + e.status;
    return e.topic.empty() ? "journal: needs a topic" : "";
    }
  if(e.kind=="attack") {
    if(e.reason.empty())
      e.reason = "AR_NONE";
    auto* r = vm ? vm->find_symbol_by_name(e.reason) : nullptr;
    if(vm && (r==nullptr || !r->is_const() || r->type()!=DaedalusDataType::INT))
      return "attack: no reason " + e.reason;
    return {};
    }
  if(e.kind=="calm") {
    if(e.attitude.empty())
      e.attitude = "ATT_NEUTRAL";
    auto* a = vm ? vm->find_symbol_by_name(e.attitude) : nullptr;
    if(vm && (a==nullptr || !a->is_const() || a->type()!=DaedalusDataType::INT))
      return "calm: no attitude " + e.attitude;
    return {};
    }
  if(e.kind=="refuse_talk")
    return e.count<=0 ? "refuse_talk: seconds must be positive" : "";
  if(e.kind=="routine") {
    auto* t = vm && !e.target.empty() ? vm->find_symbol_by_name(e.target) : nullptr;
    if(vm && !e.target.empty() && (t==nullptr || t->type()!=DaedalusDataType::INSTANCE))
      return "routine: no npc " + e.target;
    return e.routine.empty() ? "routine: needs a routine" : "";
    }
  if(e.kind=="knows") {
    auto* i = vm ? vm->find_symbol_by_name(e.info) : nullptr;
    if(vm && (i==nullptr || i->type()!=DaedalusDataType::INSTANCE || !i->is_instance_of<zenkit::IInfo>()))
      return "knows: no dialog " + e.info;
    return e.info.empty() ? "knows: needs a dialog" : "";
    }
  if(e.kind=="party") {
    if(vm && vm->find_symbol_by_name("AIV_PARTYMEMBER")==nullptr)
      return "party: the scripts have no AIV_PARTYMEMBER";
    return {};
    }
  if(e.kind=="xp" || e.kind=="end")
    return {};
  return "unknown effect " + e.kind;
  }

void StoryEffect::run(zenkit::DaedalusVm& vm, Npc& npc, Npc& player, std::string_view who) const {
  auto call = [&](const char* fn, std::vector<Value> args) {
    std::string err;
    auto* s = vm.find_symbol_by_name(fn);
    auto  r = s ? storygraph::callFunction(vm,*s,args,err) : std::nullopt;
    if(!r)
      Log::e("[story] ",who,": ",fn," failed: ",s ? err : "no such function");
    return r;
    };
  auto constant = [&](std::string_view name, int32_t def) {
    auto* s = vm.find_symbol_by_name(name);
    return s!=nullptr && s->type()==DaedalusDataType::INT ? s->get_int(0) : def;
    };
  // the scripts' own functions read self and other
  auto* self  = vm.global_self();
  auto* other = vm.global_other();
  auto  prevSelf  = self->get_instance();
  auto  prevOther = other->get_instance();
  self->set_instance(npc.handlePtr());
  other->set_instance(player.handlePtr());

  if(kind=="chest_take") {
    // v48: an outcome far from the hero. Direct transfer sends neither perceptions nor npc_deed;
    // Npc::addItem(chest) is deliberately avoided because it also captures a marked physical deed.
    auto& world = npc.world();
    auto* source = NpcChests::find(world,chest);
    const auto id = world.script().findSymbolIndex(item);
    if(source!=nullptr && id!=size_t(-1))
      Inventory::transfer(const_cast<Inventory&>(npc.inventory()),source->inventory(),nullptr,
                          id,size_t(count),world);
    }
  else if(kind=="give" && toNpc) {
    // to another npc (v39): an errand far from the hero, the walk taken as done (errands.py)
    auto* s = vm.find_symbol_by_name(item);
    auto* t = vm.find_symbol_by_name(target);
    auto  inst = t!=nullptr ? t->get_instance() : nullptr;
    if(s!=nullptr && inst!=nullptr)
      call("B_GIVEINVITEMS",{Value::instance(npc.handlePtr()),Value::instance(inst),
                             Value::integer(int32_t(s->index())),Value::integer(count)}); else
      Log::e("[story] ",who,": give to ",target,": no such npc in the world");
    }
  else if(kind=="give") {
    if(auto* s = vm.find_symbol_by_name(item)) {
      auto& from = toHero ? npc : player;
      auto& to   = toHero ? player : npc;
      call("B_GIVEINVITEMS",{Value::instance(from.handlePtr()),Value::instance(to.handlePtr()),
                             Value::integer(int32_t(s->index())),Value::integer(count)});
      }
    }
  else if(kind=="create") {
    // into the npc's own hands first: a thing it keeps elsewhere (the server's stash), given after this
    if(auto* s = vm.find_symbol_by_name(item))
      call("CREATEINVITEMS",{Value::instance(npc.handlePtr()),Value::integer(int32_t(s->index())),
                             Value::integer(count)});
    }
  else if(kind=="teach") {
    // the scripts' teacher function, as a teacher's dialog calls it: it takes the learning points itself
    if(auto* t = teaching(teach)) {
      std::vector<Value> args = {Value::instance(npc.handlePtr()),Value::instance(player.handlePtr())};
      if(teach=="circle")
        args.push_back(Value::integer(count)); else
        args.push_back(Value::integer(intConst(vm,arg)));
      if(teach=="attribute" || teach=="fight") {
        args.push_back(Value::integer(count));
        args.push_back(Value::integer(teach=="attribute" ? AttributeMax : FightMax));
        }
      auto r = call(t->fn,std::move(args));
      if(r && r->i==0)
        Log::e("[story] ",who,": ",t->fn," ",arg," ",count,": the scripts refused");
      }
    }
  else if(kind=="xp") {
    call("B_GIVEPLAYERXP",{Value::integer(count)});
    }
  else if(kind=="set") {
    if(auto* s = vm.find_symbol_by_name(var))
      s->set_int(count);
    }
  else if(kind=="crime_memory") {
    // B_MemorizePlayerCrime also writes PETZ counters. Project only its NPC-local fields, with
    // the current native absolution stamp so a new deed after payment remains readable.
    auto level = call("B_GETCURRENTABSOLUTIONLEVEL",{Value::instance(npc.handlePtr())});
    if(level) {
      auto& aiv = npc.handlePtr()->aivar;
      aiv[size_t(intConst(vm,"AIV_NPCSAWPLAYERCOMMIT"))] = count;
      aiv[size_t(intConst(vm,"AIV_NPCSAWPLAYERCOMMITDAY"))] = day;
      aiv[size_t(intConst(vm,"AIV_CRIMEABSOLUTIONLEVEL"))] = level->i;
      }
    }
  else if(kind=="journal") {
    call("LOG_CREATETOPIC",{Value::str(topic),Value::integer(constant("LOG_MISSION",0))});
    call("LOG_SETTOPICSTATUS",{Value::str(topic),Value::integer(constant(status,1))});
    if(!text.empty())
      call("B_LOGENTRY",{Value::str(topic),Value::str(text)});
    }
  else if(kind=="end") {
    call("AI_STOPPROCESSINFOS",{Value::instance(npc.handlePtr())});
    }
  else if(kind=="attack") {
    // the scripts' own attack: the reason decides how far it goes (AR_NONE: until the hero is down)
    call("B_ATTACK",{Value::instance(npc.handlePtr()),Value::instance(player.handlePtr()),
                     Value::integer(constant(reason,0)),Value::integer(0)});
    }
  else if(kind=="calm") {
    // the npc calls off its fight with the hero (v20, talked down: "Воу, ты чего, спокуха") and keeps to it:
    // its attitude to him as the scripts set it (B_SetAttitude), so their B_AssessEnemy sees no enemy in
    // him any more; and a fight with him ends now, the npc back to its routine
    call("B_SETATTITUDE",{Value::instance(npc.handlePtr()),Value::integer(constant(attitude,2))});
    auto* target = npc.target();
    if(target!=nullptr && (target==&player || HeroDeed::aivar(*target,"AIV_PARTYMEMBER")!=0) && !npc.isDown()) {
      npc.clearAiQueue();
      npc.clearState(true);
      npc.setTarget(nullptr);
      npc.resumeAiRoutine();
      npc.aiPush(AiQueue::aiRemoveWeapon());
      }
    }
  else if(kind=="knows") {
    // the dialog counts as heard, as when it is played: the menu no longer offers it, conditions see it
    auto* s    = vm.find_symbol_by_name(info);
    auto  dlg  = s!=nullptr ? std::dynamic_pointer_cast<zenkit::IInfo>(s->get_instance()) : nullptr;
    auto* w    = Gothic::inst().world();
    if(dlg!=nullptr && w!=nullptr)
      w->script().setNpcInfoKnown(player.handle(),*dlg); else
      Log::e("[story] ",who,": knows ",info,": no such dialog in the world");
    }
  else if(kind=="party") {
    // the npc goes with the hero (v30), as a companion's dialog marks it: whom it kills gives him the
    // experience (ZS_Dead), its blows at him are friendly fire
    const int32_t aiv = intConst(vm,"AIV_PARTYMEMBER");
    if(aiv>=0 && uint32_t(aiv)<zenkit::INpc::aivar_count)
      npc.handlePtr()->aivar[size_t(aiv)] = count!=0 ? 1 : 0;
    }
  else if(kind=="refuse_talk") {
    call("NPC_SETREFUSETALK",{Value::instance(npc.handlePtr()),Value::integer(count)});
    }
  else if(kind=="routine") {
    // as the dialogs do it (B_StartOtherRoutine): the symbol holds the npc the world made of it
    auto* s    = target.empty() ? nullptr : vm.find_symbol_by_name(target);
    auto  inst = s!=nullptr ? s->get_instance() : npc.handlePtr();
    if(inst!=nullptr)
      call("B_STARTOTHERROUTINE",{Value::instance(inst),Value::str(routine)}); else
      Log::e("[story] ",who,": routine of ",target,": no such npc in the world");
    }

  self->set_instance(prevSelf);
  other->set_instance(prevOther);
  }

int32_t StoryEffect::learnCost(zenkit::DaedalusVm& vm, Npc& player, std::string& error) const {
  auto* t = teaching(teach);
  if(t==nullptr) {
    error = "teach: unknown teaching " + teach;
    return -1;
    }
  // as the teacher function itself asks it (B_Teach*: disassembled from GOTHIC.DAT)
  const char*        fn   = teach=="attribute" ? "B_GETLEARNCOSTATTRIBUTE" : "B_GETLEARNCOSTTALENT";
  std::vector<Value> args = {Value::instance(player.handlePtr())};
  if(teach=="attribute") {
    args.push_back(Value::integer(intConst(vm,arg)));
    }
  else if(t->talent==nullptr) {  // fight, thief: the talent itself, skill 1
    args.push_back(Value::integer(intConst(vm,arg)));
    args.push_back(Value::integer(1));
    }
  else {  // circle: the circle; a rune, a potion...: what is taught
    args.push_back(Value::integer(intConst(vm,t->talent)));
    args.push_back(Value::integer(teach=="circle" ? count : intConst(vm,arg)));
    }
  auto* s = vm.find_symbol_by_name(fn);
  auto  r = s ? storygraph::callFunction(vm,*s,args,error) : std::nullopt;
  if(!r) {
    if(s==nullptr)
      error = std::string("the scripts have no ") + fn;
    return -1;
    }
  return teach=="attribute" || teach=="fight" ? r->i*count : r->i;
  }
