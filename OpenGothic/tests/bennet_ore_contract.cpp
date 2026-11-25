#include "commandline.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "game/gamescript_programs.h"
#include "game/gamesession.h"
#include "game/savegameheader.h"
#include "game/serialize.h"
#include "gamemusic.h"
#include "gothic.h"
#include "resources.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/waypoint.h"
#include "world/world.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

struct NpcDefinitionContract {
  inline static size_t bennetScopes=0;

  static void watchBennetRoute(GameScript& script) {
    script.bennetLookupScope=[](bool entering) { bennetScopes+=size_t(entering); };
    }
  static bool isBennetOreDialog(const GameScript& script, const DialogInstance& info) {
    return script.bennetOreDialog(info)!=nullptr;
    }
  static auto& bennetAnswer(GameScript& script) { return script.bennetOre->dialogs.back().answer; }
  static auto& bennetTransferObserver(GameScript& script) { return script.bennetTransferObserver; }
  static int& outputOrder(GameScript& script) { return script.aiOutOrderId; }
  static AiQueue& queue(Npc& npc) { return npc.aiQueue; }
  static const auto& transferVariables(const GameScript& script) { return script.inventoryTransferVariables; }
  static ScriptVars::Object& alias(GameScript& script, std::string_view name) { return script.storyActorSlot(name); }
  };

namespace {

void require(bool condition, const char* message) {
  if(!condition)
    throw std::runtime_error(message);
  }

std::vector<std::string> activityQueue(Npc& npc) {
  auto queue=NpcDefinitionContract::queue(npc);
  std::vector<std::string> actions;
  while(queue.size()>0) {
    const auto action=queue.pop();
    actions.push_back(std::string(AiQueue::actionName(uint32_t(action.act)))+" "+std::to_string(action.i0)+" "+
                      std::to_string(action.i1)+" "+action.s0+" "+(action.target ? std::string(action.target->displayName()) : ""));
    }
  return actions;
  }

void requireNativeQuestBase() {
  const auto base=Resources::baseDir();
  require(std::filesystem::is_regular_file(NativeBaseCatalog::section(base)),"the quest requires a prepared native base");
  require(!std::filesystem::exists(base/"programs"),"the quest base must have no compatibility program payloads");
  for(const auto& file : std::filesystem::recursive_directory_iterator(base/"definitions")) {
    const auto name=file.path().filename().string();
    require(!name.starts_with("program-symbols.") && name!="symbols.json",
            "the quest base must have no full program symbol table");
    }
  std::puts("Native quest base: no program payloads or full symbol files");
  }

void bennetOreContract(const std::filesystem::path& caseDir) {
  using Contract = NpcDefinitionContract;
  using Counts = std::pair<size_t,size_t>;
  constexpr auto mission = "MIS_BENNET_BRINGORE";
  constexpr auto trained = "BENNET_TEACHSMITH";
  constexpr auto directorVar = "DIR_BENNET_SAVE_CONTRACT";
  constexpr auto directorNpc = "DIR_BENNET_WITNESS_CONTRACT";
  constexpr uint32_t startId = 23816, startFn = 23818, leadId = 23819, leadFn = 23821;
  constexpr uint32_t infoId = 23822, conditionFn = 23823, answerFn = 23824, ore = 6928;
  constexpr std::array starterLines = {"15_00", "06_07", "15_08", "06_09", "15_10", "06_11"};
  constexpr std::array leadLines = {"15_00", "06_01", "06_02", "06_03", "06_04"};
  constexpr std::array handInLines = {
    "DIA_Bennet_BringOre_15_00", "DIA_Bennet_BringOre_06_01",
    "DIA_Bennet_BringOre_06_02", "DIA_Bennet_BringOre_06_03"};
  constexpr std::array trainingLines = {"15_00", "06_12", "15_13", "06_14", "06_15", "06_16", "06_17"};
  require(caseDir.is_absolute(),"Bennet contract case directory must be absolute");
  const auto* game = std::getenv("GOTHIC_DIR");
  require(game && *game,"GOTHIC_DIR must point to prepared Gothic II settings");
  setenv("GOTHIC_AI","0",1);
  const char* args[] = {"gothic-bennet-ore-contract","-g",game};
  CommandLine commandLine(3,args);
  Resources resources;
  Gothic gothic;
  requireNativeQuestBase();
  gothic.isNpcInDialogFn = [](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  std::filesystem::create_directories(caseDir);

  {
    std::vector<uint8_t> oldBytes;
    {
      Tempest::MemWriter writer(oldBytes);
      Serialize out(writer);
      SaveGameHeader header;
      header.version=56;
      header.world="NEWWORLD.ZEN";
      out.setEntry("header");
      out.write(header);
    }
    Tempest::MemReader reader(oldBytes.data(),oldBytes.size());
    Serialize in(reader);
    bool rejected=false;
    try {
      GameSession legacy(in);
      }
    catch(const std::runtime_error& error) {
      rejected=std::string_view(error.what()).starts_with("Unsupported save version 56");
      }
    require(rejected && gothic.world()==nullptr,"the normal loader must reject v56 before creating a world");
    std::puts("Bennet save contract: v56 rejected before world creation");
  }

  const auto actors = [&] {
    auto& world=*gothic.world();
    const auto smithId=world.script().storyNpcId("SLD_809_BENNET");
    auto* smith=smithId ? world.findNpcByInstance(*smithId) : nullptr;
    require(world.player()!=nullptr && smith!=nullptr,"Bennet contract needs both the hero and Bennet");
    return std::pair{world.player(),smith};
    };
  const auto save = [&](const char* title, const std::filesystem::path& path) {
    std::vector<uint8_t> bytes;
    {
      Tempest::MemWriter writer(bytes);
      Serialize out(writer);
      Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
      std::memset(preview.data(),0,4);
      gothic.gameSession()->save(out,title,preview);
    }
    Tempest::MemReader reader(bytes.data(),bytes.size());
    Serialize in(reader);
    SaveGameHeader header;
    require(in.setEntry("header"),"Bennet save must have the normal save header");
    in.read(header);
    require(header.version==Serialize::Current && in.setEntry("game/bennet") && in.setEntry(ScriptVars::StateEntry) &&
            in.setEntry("game/director") && !in.setEntry("game/state") && !in.setEntry("game/daedalus") && !in.setEntry("game/symbols"),
            "Bennet save must use only the current native session layout");
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    require(bool(out),"Bennet contract could not write a new save");
    };
  const auto load = [&](const std::filesystem::path& path) {
    gothic.clearGame().reset();
    std::ifstream file(path,std::ios::binary);
    require(bool(file),"Bennet contract could not read its new save");
    const std::string bytes{std::istreambuf_iterator<char>{file},std::istreambuf_iterator<char>{}};
    Tempest::MemReader reader(bytes.data(),bytes.size());
    Serialize in(reader);
    gothic.setGame(std::make_unique<GameSession>(in));
    return actors();
    };
  const auto offer = [](Npc& hero, Npc& bennet, uint32_t scriptFn) -> std::optional<GameScript::DlgChoice> {
    const auto choices=hero.world().script().dialogChoices(hero.handlePtr(),bennet.handlePtr(),{},false);
    const auto found=std::find_if(choices.begin(),choices.end(),[=](const auto& c) { return c.scriptFn==scriptFn; });
    return found==choices.end() ? std::nullopt : std::optional<GameScript::DlgChoice>(*found);
    };
  const auto run = [](const GameScript::DlgChoice& choice, Npc& hero, Npc& bennet, const char* label) {
    auto& script=hero.world().script();
    const auto scopes=Contract::bennetScopes;
    script.exec(choice,hero,bennet);
    require(Contract::bennetScopes>scopes,"Bennet must use its staged native dialogue route");
    std::printf("Bennet %s\n",label);
    };
  const auto setOre = [](Npc& npc, size_t count) {
    npc.delItem(ore,uint32_t(npc.itemCount(ore)));
    if(count!=0)
      require(npc.addItem(ore,count)!=nullptr,"Bennet contract could not prepare an ore stack");
    };
  const auto clearQueues = [](Npc& hero, Npc& bennet) {
    Contract::queue(hero).clear();
    Contract::queue(bennet).clear();
    Contract::outputOrder(hero.world().script())=0;
    };
  const auto holdings = [](Npc& hero, Npc& bennet) -> Counts {
    return {hero.itemCount(ore),bennet.itemCount(ore)};
    };
  const auto requireReplies = [](Npc& hero, Npc& bennet, std::string_view prefix, const auto& suffixes) {
    std::array<bool,std::tuple_size_v<std::decay_t<decltype(suffixes)>>> seen={};
    for(auto* speaker : {&hero,&bennet}) {
      auto queue=Contract::queue(*speaker);
      while(queue.size()>0) {
        const auto reply=queue.pop();
        require(reply.act==AI_Output && reply.i0>=0 && size_t(reply.i0)<seen.size(),
                "Bennet reply must use the stock output order");
        const size_t index=size_t(reply.i0);
        const std::string_view suffix=suffixes[index];
        require(!seen[index] && reply.s0==std::string(prefix)+std::string(suffix) &&
                (speaker==&hero)==suffix.starts_with("15_") && reply.target==(speaker==&hero ? &bennet : &hero),
                "Bennet reply must preserve its text key, speaker and listener");
        seen[index]=true;
        }
      }
    require(std::all_of(seen.begin(),seen.end(),[](bool value) { return value; }),"every Bennet reply must be queued");
    };
  const auto queueTrace = [](Npc& hero, Npc& bennet) {
    return std::pair{activityQueue(hero),activityQueue(bennet)};
    };

  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  auto [hero,bennet]=actors();
  auto& original=hero->world().script();
  Contract::watchBennetRoute(original);
  require(original.globalIntSymbol(mission)==0 && original.globalIntSymbol(trained)==0 &&
          !offer(*hero,*bennet,answerFn) && !offer(*hero,*bennet,leadFn),
          "new game must have neither accepted the quest nor learned ore smithing");
  hero->handle().guild=int32_t(GIL_SLD);
  hero->setTalentSkill(TALENT_SMITH,1);
  require(original.setGlobalIntSymbol("PLAYER_TALENT_SMITH",1),"Bennet fixture needs the normal smithing prerequisite");
  setOre(*hero,5);
  setOre(*bennet,0);

  std::string error;
  error=original.addStoryVariable(directorVar,37);
  require(error.empty(),error.c_str());
  const auto ownNpc=original.addDirectorNpcSymbol(directorNpc,"NONE_100_XARDAS",error);
  require(ownNpc.has_value(),error.c_str());
  const auto directorId=*ownNpc;
  require(hero->world().addNpc(directorId,hero->position())!=nullptr,"Bennet fixture must add the director NPC to the world");

  clearQueues(*hero,*bennet);
  const auto starter=offer(*hero,*bennet,startFn);
  require(starter && starter->sort==7 && !starter->title.empty(),"fresh Bennet must offer the normal ore smithing topic");
  run(*starter,*hero,*bennet,"offer accepted");
  requireReplies(*hero,*bennet,"DIA_Bennet_WannaSmithORE_",starterLines);
  require(original.globalIntSymbol(mission)==1 && original.globalIntSymbol(trained)==0 &&
          original.doesNpcKnowInfo(hero->handle(),startId) && holdings(*hero,*bennet)==Counts{5,0},
          "Bennet's starter must set LOG_RUNNING through its normal dialogue without taking ore");

  clearQueues(*hero,*bennet);
  const auto lead=offer(*hero,*bennet,leadFn);
  require(lead && lead->sort==8,"the active quest must offer the ore location");
  run(*lead,*hero,*bennet,"ore location");
  requireReplies(*hero,*bennet,"DIA_Bennet_WhereOre_",leadLines);
  require(original.doesNpcKnowInfo(hero->handle(),leadId) && !offer(*hero,*bennet,leadFn),
          "Bennet's ore location reply must only be offered once");

  setOre(*hero,4);
  require(!offer(*hero,*bennet,answerFn),"Bennet must decline delivery with four ore");
  setOre(*hero,5);
  clearQueues(*hero,*bennet);
  const auto ready=caseDir/"bennet-native-running.sav";
  save("Bennet LOG_RUNNING",ready);

  for(const bool merge:{false,true}) {
    std::tie(hero,bennet)=load(ready);
    auto& script=hero->world().script();
    Contract::watchBennetRoute(script);
    require(script.globalIntSymbol(mission)==1 && holdings(*hero,*bennet)==Counts{5,0} &&
            script.doesNpcKnowInfo(hero->handle(),startId) && script.doesNpcKnowInfo(hero->handle(),leadId) &&
            !script.doesNpcKnowInfo(hero->handle(),infoId),
            "reloading the active archive must restore the started quest and its known topics");
    if(merge) {
      setOre(*hero,6);
      setOre(*bennet,1);
      hero->getItem(ore)->handle().value=217;
      bennet->getItem(ore)->handle().value=219;
      }

    gothic.dialogScriptFn=[](uint32_t fn,int32_t& result) {
      if(fn!=conditionFn)
        return false;
      result=0;
      return true;
      };
    require(!offer(*hero,*bennet,answerFn),"the director's condition override must have priority");
    gothic.dialogScriptFn={};
    const auto choice=offer(*hero,*bennet,answerFn);
    require(choice && choice->sort==9 && !choice->title.empty(),"Bennet must offer to take five ore");
    clearQueues(*hero,*bennet);
    if(!merge) {
      DialogInstance wrongId;
      wrongId.npc=choice->handle->npc;
      wrongId.condition=choice->handle->condition;
      wrongId.information=choice->handle->information;
      require(Contract::isBennetOreDialog(script,*choice->handle) && !Contract::isBennetOreDialog(script,wrongId),
              "Bennet's route must require the prepared dialogue ID");
      auto& binding=Contract::bennetAnswer(script);
      const auto* answer=binding;
      auto incomplete=*answer;
      storygraph::Step missing;
      missing.kind=storygraph::Step::Call;
      missing.call=storygraph::ex::call("BENNET_MISSING_DEPENDENCY",{});
      incomplete.steps.push_back(std::move(missing));
      const auto before=queueTrace(*hero,*bennet);
      binding=&incomplete;
      run(*choice,*hero,*bennet,"failed preflight");
      binding=answer;
      require(holdings(*hero,*bennet)==Counts{5,0} && script.globalIntSymbol(mission)==1 &&
              !script.doesNpcKnowInfo(hero->handle(),infoId) && queueTrace(*hero,*bennet)==before,
              "Bennet dependency failure must preserve counts, mission, known dialogue and queued effects");
      }
    const int32_t heroXp=hero->experience();
    const auto journal=script.questLog().questCount();
    std::vector<Counts> counts;
    Contract::bennetTransferObserver(script)=[&](Npc& player,Npc& smith) { counts.push_back(holdings(player,smith)); };
    run(*choice,*hero,*bennet,merge ? "merge hand-in" : "cold hand-in");
    Contract::bennetTransferObserver(script)={};
    require(counts==std::vector<Counts>{merge ? Counts{1,6} : Counts{0,5},merge ? Counts{3,4} : Counts{2,3}},
            "Bennet must receive five ore before returning two");
    require(script.globalIntSymbol(mission)==2 && script.doesNpcKnowInfo(hero->handle(),infoId) &&
            hero->experience()==heroXp && script.questLog().questCount()==journal,
            "Bennet must finish the quest without changing experience or journal");
    if(merge)
      require(hero->getItem(ore)->handle().value==217 && bennet->getItem(ore)->handle().value==219,
              "both original ore stacks must retain their properties");

    auto heroQueue=Contract::queue(*hero);
    auto bennetQueue=Contract::queue(*bennet);
    require(heroQueue.size()==1 && bennetQueue.size()==5 && heroQueue.front()->act==AI_Output &&
            heroQueue.front()->s0==handInLines[0] && heroQueue.front()->i0==0,
            "Bennet hand-in must open with the hero's stock reply");
    for(int i=1;i<=3;++i) {
      const auto row=bennetQueue.pop();
      require(row.act==AI_Output && row.s0==handInLines[size_t(i)] && row.i0==i,"Bennet's reply order must stay stock");
      if(i==1 || i==3)
        require(bennetQueue.pop().act==AI_PrintScreen,"Bennet's two transfers must each queue a notice in order");
      }
    const auto queue=queueTrace(*hero,*bennet);

    const auto assertTransfer = [](Npc& player, Npc& smith) {
      auto& script=player.world().script();
      const auto& vars=Contract::transferVariables(script);
      require(vars.amount==2 && vars.item==int32_t(ore) && !vars.itemName.empty() && !vars.notice.empty() &&
              Contract::alias(script,"B_GIVEINVITEMS.GIVER").get()==static_cast<NpcInstance*>(smith.handlePtr().get()) &&
              Contract::alias(script,"B_GIVEINVITEMS.TAKER").get()==static_cast<NpcInstance*>(player.handlePtr().get()) &&
              Contract::alias(script,"ITEM").get()==static_cast<ItemInstance*>(player.getItem(ore)->handlePtr().get()),
              "Bennet transfer must preserve its variables, actors and the hero's exact ITEM reference");
      };
    assertTransfer(*hero,*bennet);
    // Two references to different inventory objects of the same item class.
    Contract::alias(script,"ITMI_NUGGET") = std::static_pointer_cast<ItemInstance>(bennet->getItem(ore)->handlePtr());
    const auto finished=caseDir/(merge ? "bennet-native-merged.sav" : "bennet-native-finished.sav");
    save("Bennet LOG_SUCCESS",finished);
    std::tie(hero,bennet)=load(finished);
    auto& reloaded=hero->world().script();
    Contract::watchBennetRoute(reloaded);
    require(reloaded.globalIntSymbol(mission)==2 && holdings(*hero,*bennet)==counts.back() &&
            reloaded.doesNpcKnowInfo(hero->handle(),infoId) && !offer(*hero,*bennet,answerFn) &&
            !offer(*hero,*bennet,leadFn) && queueTrace(*hero,*bennet)==queue,
            "ordinary reload must restore Bennet's result, exact queue and one-shot topics");
    assertTransfer(*hero,*bennet);
    require(Contract::alias(reloaded,"ITMI_NUGGET").get()==
              static_cast<ItemInstance*>(bennet->getItem(ore)->handlePtr().get()),
            "the other ore reference must retain Bennet as its owner after reload");
    const auto* witness=hero->world().findNpcByInstance(directorId);
    require(reloaded.globalIntSymbol(directorVar)==37 && witness && reloaded.storyNpcName(directorId)==directorNpc,
            "ordinary reload must preserve director variables and the spawned NPC identity");
    std::printf("Bennet %s: %zu/%zu then %zu/%zu, LOG_SUCCESS restored in v%u\n",
                merge ? "merging" : "cold",counts[0].first,counts[0].second,counts[1].first,counts[1].second,unsigned(Serialize::Current));

    if(!merge) {
      // The permanent smithing topic teaches ore smithing after the hand-in.
      clearQueues(*hero,*bennet);
      const auto training=offer(*hero,*bennet,startFn);
      require(bool(training),"the ore smithing topic must remain available after successful delivery");
      bool overridden=false;
      gothic.dialogScriptFn=[&](uint32_t fn,int32_t&) {
        overridden=fn==startFn;
        return overridden;
        };
      reloaded.exec(*training,*hero,*bennet);
      require(overridden && reloaded.globalIntSymbol(trained)==0 &&
              Contract::queue(*hero).size()==0 && Contract::queue(*bennet).size()==0,
              "the director's starter-topic override must have priority");
      gothic.dialogScriptFn={};
      run(*training,*hero,*bennet,"ore smithing unlocked");
      requireReplies(*hero,*bennet,"DIA_Bennet_WannaSmithORE_",trainingLines);
      require(reloaded.globalIntSymbol(trained)==1 && !offer(*hero,*bennet,startFn),
              "Bennet must unlock ore smithing through the stock follow-up and then hide the topic");
      }
    }
  gothic.clearGame().reset();
  }

}

int main(int argc, const char** argv) {
  if(argc!=2) {
    std::fputs("Usage: gothic-bennet-ore-contract /absolute/case-directory\n",stderr);
    return EXIT_FAILURE;
    }
  try {
    bennetOreContract(argv[1]);
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: Bennet contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
