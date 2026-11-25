#include "storyeffects.h"

#include <Tempest/Log>

#include <cctype>
#include <optional>
#include <utility>
#include <vector>

#include "game/gamescript.h"
#include "game/inventory.h"
#include "world/objects/interactive.h"
#include "world/objects/npc.h"
#include "world/world.h"
#include "world/aiqueue.h"

#include "jsonread.h"
#include "npcchests.h"
#include "textcodec.h"

using namespace Tempest;
using storygraph::StoryArgument;

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

}

std::string StoryEffect::read(const rapidjson::Value& x, GameScript* script, StoryEffect& e) {
  using SymbolKind = GameScript::SymbolKind;
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
  if(e.kind=="knockout" || e.kind=="wound") {
    e.by = upper(JsonRead::str(x,"by"));
    if(e.by.empty())
      return e.kind+": needs by";
    const auto loss = x.FindMember("loss");
    if(loss==x.MemberEnd() || !loss->value.IsInt64() || loss->value.GetInt64()<0)
      return e.kind+": loss must be a nonnegative integer";
    e.loss = loss->value.GetInt64();
    if(e.kind=="wound") {
      const auto byLoss = x.FindMember("by_loss");
      if(byLoss==x.MemberEnd() || !byLoss->value.IsInt64() || byLoss->value.GetInt64()<0)
        return "wound: by_loss must be a nonnegative integer";
      e.byLoss = byLoss->value.GetInt64();
      }
    return {};
    }
  if(e.kind=="give" || e.kind=="create" || e.kind=="chest_take") {
    if(script!=nullptr && !script->findItemInstance(e.item))
      return e.kind + ": no item " + e.item;
    if(e.count<=0)
      return e.kind + ": count must be positive";
    if(e.kind=="chest_take" && e.chest.empty())
      return "chest_take: needs a chest";
    if(e.kind=="give" && e.toNpc) {
      if(e.target.empty() || (script!=nullptr && !script->storyNpcId(e.target)))
        return "give: no npc " + e.target;
      }
    return {};
    }
  if(e.kind=="teach") {
    auto* t = teaching(e.teach);
    if(t==nullptr)
      return "teach: unknown teaching " + e.teach;
    if(script!=nullptr && !script->storyFunction(t->fn))
      return std::string("teach: no function ") + t->fn;
    if(e.teach=="circle")
      return e.count<1 || e.count>6 ? "teach: circle must be 1..6" : "";
    if(e.arg.empty() || (script!=nullptr && !script->intConstant(e.arg)))
      return "teach: no constant " + e.arg;
    if((e.teach=="attribute" || e.teach=="fight") && e.count<=0)
      return "teach: count must be positive";
    return {};
    }
  if(e.kind=="set") {
    if(script!=nullptr && (script->storySymbolKind(e.var)!=SymbolKind::Variable || !script->globalIntSymbol(e.var)))
      return "set: no variable " + e.var;
    return {};
    }
  if(e.kind=="crime_memory") {
    if(e.count<0 || e.count>4 || e.day<0)
      return "crime_memory: value must be 0..4 and day nonnegative";
    if(script!=nullptr) {
      for(auto name:{"AIV_NPCSAWPLAYERCOMMIT","AIV_NPCSAWPLAYERCOMMITDAY","AIV_CRIMEABSOLUTIONLEVEL"}) {
        const auto index = script->intConstant(name);
        if(!index || *index<0 || size_t(*index)>=NpcRecord::aivar_count)
          return std::string("crime_memory: no AIV constant ")+name;
        }
      if(!script->storyFunction("B_GETCURRENTABSOLUTIONLEVEL"))
        return "crime_memory: no B_GetCurrentAbsolutionLevel";
      }
    return {};
    }
  if(e.kind=="journal") {
    if(e.status.empty())
      e.status = "LOG_RUNNING";
    if(script!=nullptr && !script->intConstant(e.status))
      return "journal: no status " + e.status;
    return e.topic.empty() ? "journal: needs a topic" : "";
    }
  if(e.kind=="attack") {
    if(e.reason.empty())
      e.reason = "AR_NONE";
    if(script!=nullptr && !script->intConstant(e.reason))
      return "attack: no reason " + e.reason;
    return {};
    }
  if(e.kind=="calm") {
    if(e.attitude.empty())
      e.attitude = "ATT_NEUTRAL";
    if(script!=nullptr && !script->intConstant(e.attitude))
      return "calm: no attitude " + e.attitude;
    return {};
    }
  if(e.kind=="refuse_talk")
    return e.count<=0 ? "refuse_talk: seconds must be positive" : "";
  if(e.kind=="routine") {
    if(script!=nullptr && !e.target.empty() && !script->storyNpcId(e.target))
      return "routine: no npc " + e.target;
    return e.routine.empty() ? "routine: needs a routine" : "";
    }
  if(e.kind=="knows") {
    if(script!=nullptr && !script->storyDialogFunction(e.info))
      return "knows: no dialog " + e.info;
    return e.info.empty() ? "knows: needs a dialog" : "";
    }
  if(e.kind=="party") {
    if(script!=nullptr && !script->intConstant("AIV_PARTYMEMBER"))
      return "party: no AIV_PARTYMEMBER constant";
    return {};
    }
  if(e.kind=="xp" || e.kind=="end")
    return {};
  return "unknown effect " + e.kind;
  }

std::string StoryEffect::read(const rapidjson::Value& x, GameScript& script, StoryEffect& e) {
  return read(x,&script,e);
  }

void StoryEffect::run(GameScript& script, Npc& npc, Npc& player, std::string_view who) const {
  auto call = [&](std::string_view name, const std::vector<StoryArgument>& args) {
    auto result = script.callStoryFunction(name,&npc,&player,args);
    if(!result.ok)
      Log::e("[story] ",who,": ",name," failed: ",result.error);
    return result;
    };

  if(kind=="set") {
    if(!script.setGlobalIntSymbol(var,count))
      Log::e("[story] ",who,": set ",var,": no such variable");
    }
  else if(kind=="party") {
    script.setPartyMember(npc,count!=0);
    }
  else if(kind=="knows") {
    if(!script.markNpcInfoKnown(player,info))
      Log::e("[story] ",who,": knows ",info,": no such dialog in the world");
    }
  else if(kind=="refuse_talk") {
    npc.setRefuseTalkSeconds(count);
    }
  else if(kind=="end") {
    script.endDirectorDialog(npc);
    }
  else if(kind=="chest_take") {
    // A remote transfer sends neither perceptions nor npc_deed.
    auto& world  = npc.world();
    auto* source = NpcChests::find(world,chest);
    const auto id = script.findItemInstance(item);
    if(source!=nullptr && id)
      Inventory::transfer(const_cast<Inventory&>(npc.inventory()),source->inventory(),nullptr,*id,size_t(count),world);
    }
  else if(kind=="give") {
    auto* giver = toHero || toNpc ? &npc : &player;
    auto* taker = toHero ? &player : &npc;
    if(toNpc)
      taker = script.directorNpc(target);
    const auto id = script.findItemInstance(item);
    if(taker==nullptr)
      Log::e("[story] ",who,": give to ",target,": no such npc in the world");
    else if(id)
      call("B_GIVEINVITEMS",{giver,taker,int32_t(*id),count});
    }
  else if(kind=="create") {
    if(const auto id = script.findItemInstance(item))
      call("CREATEINVITEMS",{&npc,int32_t(*id),count});
    }
  else if(kind=="teach") {
    auto* lesson = teaching(teach);
    if(lesson==nullptr)
      return;
    const auto skill = teach=="circle" ? std::optional<int32_t>(count) : script.intConstant(arg);
    if(!skill) {
      Log::e("[story] ",who,": teach: no constant ",arg);
      return;
      }
    std::vector<StoryArgument> args = {&npc,&player,*skill};
    if(teach=="attribute" || teach=="fight") {
      args.push_back(count);
      args.push_back(teach=="attribute" ? AttributeMax : FightMax);
      }
    auto result = call(lesson->fn,args);
    if(result.ok && result.returned.i==0)
      Log::e("[story] ",who,": ",lesson->fn," ",arg," ",count,": the teacher refused");
    }
  else if(kind=="xp") {
    call("B_GIVEPLAYERXP",{count});
    }
  else if(kind=="crime_memory") {
    // Project only the NPC-local fields that B_MemorizePlayerCrime writes.
    const auto crime = script.intConstant("AIV_NPCSAWPLAYERCOMMIT");
    const auto when  = script.intConstant("AIV_NPCSAWPLAYERCOMMITDAY");
    const auto stamp = script.intConstant("AIV_CRIMEABSOLUTIONLEVEL");
    const auto valid = [](std::optional<int32_t> index) {
      return index && *index>=0 && size_t(*index)<NpcRecord::aivar_count;
      };
    if(!valid(crime) || !valid(when) || !valid(stamp)) {
      Log::e("[story] ",who,": crime_memory: missing NPC crime fields");
      return;
      }
    auto result = call("B_GETCURRENTABSOLUTIONLEVEL",{&npc});
    if(result.ok) {
      auto& aiv = npc.handlePtr()->aivar;
      aiv[size_t(*crime)] = count;
      aiv[size_t(*when)]  = day;
      aiv[size_t(*stamp)] = result.returned.i;
      }
    }
  else if(kind=="journal") {
    call("LOG_CREATETOPIC",{topic,script.intConstant("LOG_MISSION").value_or(0)});
    call("LOG_SETTOPICSTATUS",{topic,script.intConstant(status).value_or(1)});
    if(!text.empty())
      call("B_LOGENTRY",{topic,text});
    }
  else if(kind=="attack") {
    call("B_ATTACK",{&npc,&player,script.intConstant(reason).value_or(0),int32_t(0)});
    }
  else if(kind=="calm") {
    call("B_SETATTITUDE",{&npc,script.intConstant(attitude).value_or(2)});
    auto* opponent = npc.target();
    if(opponent!=nullptr && (opponent==&player || script.isPartyMember(*opponent)) && !npc.isDown()) {
      npc.clearAiQueue();
      npc.clearState(true);
      npc.setTarget(nullptr);
      npc.resumeAiRoutine();
      npc.aiPush(AiQueue::aiRemoveWeapon());
      }
    }
  else if(kind=="routine") {
    auto* actor = target.empty() ? &npc : script.directorNpc(target);
    if(actor!=nullptr)
      call("B_STARTOTHERROUTINE",{actor,routine});
    else
      Log::e("[story] ",who,": routine of ",target,": no such npc in the world");
    }
  }

int32_t StoryEffect::learnCost(GameScript& script, Npc& teacher, Npc& player, std::string& error) const {
  auto* lesson = teaching(teach);
  if(lesson==nullptr) {
    error = "teach: unknown teaching " + teach;
    return -1;
    }
  const auto skill = teach=="circle" ? std::optional<int32_t>(count) : script.intConstant(arg);
  if(!skill) {
    error = "teach: no constant " + arg;
    return -1;
    }
  const auto talent = lesson->talent==nullptr ? skill : script.intConstant(lesson->talent);
  if(!talent) {
    error = std::string("teach: no constant ") + lesson->talent;
    return -1;
    }
  const char* fn = teach=="attribute" ? "B_GETLEARNCOSTATTRIBUTE" : "B_GETLEARNCOSTTALENT";
  std::vector<StoryArgument> args = {&player,*talent};
  if(teach!="attribute")
    args.push_back(lesson->talent==nullptr ? 1 : *skill);
  auto result = script.callStoryFunction(fn,&teacher,&player,args);
  if(!result.ok) {
    error = std::move(result.error);
    return -1;
    }
  const auto cost = result.returned.i;
  return teach=="attribute" || teach=="fight" ? cost*count : cost;
  }
