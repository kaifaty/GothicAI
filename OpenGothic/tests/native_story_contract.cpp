#include "commandline.h"
#include "dialoggraph.h"
#include "npccontroller.h"
#include "storygraph/exprjson.h"
#include "world/npcactivityprobe.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/menurecord.h"
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
#include "world/records/focusrecord.h"
#include "world/waypoint.h"
#include "world/world.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
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
  static const auto& aiState(const Npc& npc) { return npc.aiState; }
  static void tickRoutine(Npc& npc) { npc.aiState.loopNextTime=npc.world().tickCount(); npc.tickRoutine(); }
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

Npc* liveNpc(World& world, std::string_view name) {
  const auto id=world.script().storyNpcId(name);
  return id ? world.findNpcByInstance(*id) : nullptr;
  }

class ActivityCalls final : public NpcActivityProbe {
  public:
    ActivityCalls() : previous(instance) { instance=this; }
    ~ActivityCalls() override { instance=previous; }
    size_t phase(const Npc& npc, std::string_view name) const {
      const auto it=phases.find({&npc,std::string(name)});
      return it!=phases.end() ? it->second : 0;
      }

  private:
    void phaseBegin(const Npc* npc, std::string_view name) override { ++phases[{npc,std::string(name)}]; }
    void phaseEnd(int) override {}
    void random(const Npc*,uint32_t,uint32_t,bool) override {}
    void queued(const Npc*,const AiQueue::AiAction&) override {}
    void point(const Npc&,const WayPoint*,const WayPoint*) override {}
    void mob(const Npc&,const Interactive&,bool) override {}

    NpcActivityProbe* previous;
    std::map<std::pair<const Npc*,std::string>,size_t> phases;
  };

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
  const char* args[] = {"gothic-native-story-contract","-g",game};
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

void nativeStoryGraphContract(const std::filesystem::path& caseDir) {
  using Contract = NpcDefinitionContract;
  constexpr auto stock = "DIA_ADDON_CAVALORN_LETSKILLBANDITS";
  constexpr auto actorName = "BAU_4300_ADDON_CAVALORN";
  constexpr auto directorVar = "DIR_NATIVE_STORY_RESULT";
  constexpr auto directorNpc = "DIR_NATIVE_STORY_ACTOR";
  constexpr auto authored = "DIR_NATIVE_STORY";
  constexpr auto continuation = "DIR_NATIVE_STORY_CONTINUE";
  constexpr auto topic = "Native StoryGraph contract";
  constexpr std::array banditNames={"BDT_1013_BANDIT_L","BDT_1014_BANDIT_L","BDT_1015_BANDIT_L"};
  require(caseDir.is_absolute(),"native StoryGraph contract case directory must be absolute");
  const auto* game=std::getenv("GOTHIC_DIR");
  require(game && *game,"GOTHIC_DIR must point to prepared Gothic II settings");
  setenv("GOTHIC_AI","0",1);
  const char* args[]={"gothic-native-story-contract","-g",game};
  CommandLine commandLine(3,args);
  Resources resources;
  Gothic gothic;
  requireNativeQuestBase();
  gothic.isNpcInDialogFn=[](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  const auto journalBefore = gothic.world()->script().questLog().questCount();
  const auto stockId = gothic.world()->script().storySymbolIndex(stock);
  const auto stateInit = gothic.world()->script().findFunction("ZS_SHORTZAPPED");
  const auto stateLoop = gothic.world()->script().findFunction("ZS_SHORTZAPPED_LOOP");
  const auto stateEnd = gothic.world()->script().findFunction("ZS_SHORTZAPPED_END");
  require(stateInit.isValid() && stateLoop.isValid() && stateEnd.isValid(),"the state fixture must have all three phases");
  std::optional<uint32_t> directorNpcId;
  {
    auto& world=*gothic.world();
    auto& script=world.script();
    auto* hero=world.player();
    auto* actor=liveNpc(world,actorName);
    require(hero && actor && stockId,"native StoryGraph contract needs stock Cavalorn and his dialogue");

    constexpr std::array aliases={"AMBUSHER_1013","AMBUSHER_1014","AMBUSHER_1015"};
    std::array<Npc*,banditNames.size()> bandits={};
    std::array<ScriptVars::Object*,aliases.size()> aliasSlots={};
    for(size_t i=0;i<bandits.size();++i) {
      bandits[i]=liveNpc(world,banditNames[i]);
      if(bandits[i]==nullptr) {
        const auto id=script.storyNpcId(banditNames[i]);
        require(id.has_value(),"the ambush actor must have a stock NPC archetype");
        bandits[i]=world.addNpc(*id,actor->position());
        }
      aliasSlots[i]=&Contract::alias(script,aliases[i]);
      require(bandits[i] && aliasSlots[i] && !bandits[i]->isDead(),"the ambush fixture needs three live actors");
      aliasSlots[i]->reset();
      bandits[i]->handle().aivar[61]=1;
      }
    require(script.setGlobalIntSymbol("MIS_ADDON_CAVALORN_KILLBRAGO",1) &&
            script.setGlobalIntSymbol("MIS_ADDON_CAVALORN_LETTER2VATRAS",0) &&
            script.setGlobalIntSymbol("MIS_ADDON_NEFARIUS_BRINGMISSINGORNAMENTS",0) &&
            script.setGlobalIntSymbol("BDT_1013_AWAY",0) &&
            script.setGlobalIntSymbol("CAVALORN_ADDON_TEACHPLAYER",1),"Cavalorn contract needs the stock quest flags");
    hero->handle().aivar[86]=35; // REAL_TALENT_BOW, the two-point price tier.
    hero->handle().hitchance[TALENT_BOW]=35;
    hero->handle().lp=2;
    const auto teachFn=script.storyScriptFunctionId("DIA_ADDON_CAVALORN_TEACH_INFO");
    const auto bowFn=script.storyScriptFunctionId("DIA_ADDON_CAVALORN_TEACH_BOW_1");
    require(teachFn && bowFn,"Cavalorn's lesson callbacks must have prepared IDs");
    Contract::queue(*hero).clear();
    Contract::queue(*actor).clear();
    bandits.front()->clearState(true);

    std::vector<std::string> chosen;
    StoryRuntime runtime(nullptr);
    DialogGraph graph([](std::string&&) {},nullptr,runtime);
    graph.chosen=[&](Npc& speaker,const std::string& node) {
      require(&speaker==actor,"the StoryGraph event must name Cavalorn");
      chosen.push_back(node);
      };
    const auto apply=[&](const char* operation) {
      rapidjson::Document json;
      require(!json.Parse(operation).HasParseError(),"invalid StoryGraph contract patch");
      const auto error=graph.applyOp(json);
      require(error.empty(),error.c_str());
      };
    const auto offered=[&](std::string_view name) {
      const auto nodes=graph.offered(world,*actor);
      return std::ranges::find(nodes,name)!=nodes.end();
      };
    const auto choose=[&](std::string_view name) {
      std::string error;
      require(graph.choose(world,*actor,name,error),error.c_str());
      };

    {
      auto& itemAlias=Contract::alias(script,"ITEM");
      const auto previousItem=itemAlias;
      const auto previousName=actor->handle().name[4];
      auto item=std::make_shared<ItemInstance>();
      item->text[5]="Story field test";
      itemAlias=item;
      rapidjson::Document fields;
      fields.Parse(R"([
        {"do":"set","target":["field",["inst","ITEM"],"FLAGS"],"value":["int",-2147483648]},
        {"do":"set","target":["field",["inst","SELF"],"NAME",4],"value":["field",["inst","ITEM"],"TEXT",5]},
        {"do":"return","value":["field",["inst","SELF"],"NAME",4]}
      ])");
      const auto updated=script.runStoryProgram(storygraph::readProgram(fields,true),actor,hero,true);
      require(updated.ok && updated.returned.s==item->text[5] && actor->handle().name[4]==item->text[5] &&
              uint32_t(item->flags)==0x80000000u,"StoryGraph must read and write NPC and item fields without a VM member binding");
      fields.Parse(R"([{"do":"return","value":["field",["inst","ITEM"],"NAME",1]}])");
      const auto wrongIndex=script.runStoryProgram(storygraph::readProgram(fields,true),actor,hero,true);
      require(!wrongIndex.ok && wrongIndex.started,"the NPC name array must not extend an item's scalar name");
      actor->handle().name[4]=previousName;

      const auto runFields=[&](const ScriptVars::Object& record, const char* program) {
        itemAlias=record;
        fields.Parse(program);
        return script.runStoryProgram(storygraph::readProgram(fields,true),actor,hero,true);
        };
      auto info=std::make_shared<DialogInstance>();
      const auto infoResult=runFields(info,R"([
        {"do":"set","target":["field",["inst","ITEM"],"DESCRIPTION"],"value":["str","Story topic"]},
        {"do":"return","value":["field",["inst","ITEM"],"DESCRIPTION"]}])");
      require(infoResult.ok && infoResult.returned.s=="Story topic" && info->description=="Story topic",
              "StoryGraph must access dialog fields on the dialog record");
      struct StoryFocus : RecordInstance, FocusRecord {};
      auto focus=std::make_shared<StoryFocus>();
      const auto focusResult=runFields(focus,R"([
        {"do":"set","target":["field",["inst","ITEM"],"NPC_RANGE1"],"value":["int",1069547520]},
        {"do":"return","value":["field",["inst","ITEM"],"NPC_RANGE1"]}])");
      require(focusResult.ok && focusResult.returned.type==storygraph::NativeValue::Float &&
              focusResult.returned.f==1.5f && focus->npc_range1==1.5f,
              "StoryGraph must preserve the float bit pattern in a focus field");
      struct StoryMenuItem : RecordInstance, MenuDefinitions::MenuItem {};
      auto menuRecord=std::make_shared<StoryMenuItem>();
      const auto menuResult=runFields(menuRecord,R"([
        {"do":"set","target":["field",["inst","ITEM"],"TEXT",9],"value":["str","Story menu"]},
        {"do":"return","value":["field",["inst","ITEM"],"TEXT",9]}])");
      require(menuResult.ok && menuResult.returned.s=="Story menu" && menuRecord->text[9]=="Story menu",
              "StoryGraph must use the menu's own text extent when both object classes have TEXT");
      const auto nullResult=runFields(nullptr,R"([{"do":"return","value":["field",["inst","ITEM"],"NPC_RANGE1"]}])");
      require(nullResult.ok && nullResult.returned.type==storygraph::NativeValue::Int && nullResult.returned.i==0,
              "a missing focus instance must still read as integer zero");
      itemAlias=previousItem;

      std::string symbolError;
      directorNpcId=script.addDirectorNpcSymbol(directorNpc,actorName,symbolError);
      require(directorNpcId.has_value(),symbolError.c_str());
      require(script.storyNpcId(directorNpc)==directorNpcId,"native story lookup must resolve the new director NPC id");
      require(!offered(stock),"Cavalorn must hide the topic when the ambush actors are absent");
      for(size_t i=0;i<bandits.size();++i)
        *aliasSlots[i]=std::static_pointer_cast<NpcInstance>(bandits[i]->handlePtr());
      require(offered(stock),"Cavalorn must offer the stock topic after its nested ambush check");
      apply(R"({"op":"program","node":"DIA_ADDON_CAVALORN_LETSKILLBANDITS","part":"condition",
                "steps":[{"do":"return","value":["int",0]}]})");
      require(!offered(stock),"the director's condition must replace the stock condition");
      apply(R"({"op":"program","node":"DIA_ADDON_CAVALORN_LETSKILLBANDITS","part":"condition","steps":null})");
      choose(stock);
      const auto* waypoint=actor->currentTaPoint();
      require(script.doesNpcKnowInfo(hero->handle(),size_t(*stockId)) &&
              std::ranges::all_of(bandits,[](Npc* bandit) { return bandit->handle().aivar[61]==0; }) &&
              hero->aiQueueFront() && hero->aiQueueFront()->s0=="DIA_Addon_Cavalorn_LETSKILLBANDITS_15_00" &&
              waypoint && waypoint->name=="NW_XARDAS_BANDITS_LEFT",
              "Cavalorn must mark the topic heard, update the ambush, queue the stock line and change routine");

      apply(R"({"op":"vars","vars":{"DIR_NATIVE_STORY_RESULT":0}})");
      apply(R"({"op":"add_node","node":{"id":"DIR_NATIVE_STORY","npc":"BAU_4300_ADDON_CAVALORN",
        "description":"Discuss the ambush","condition":["!",["fn","C_BRAGOBANDITSDEAD"]],"program":[
        {"do":"set","target":["var","DIR_NATIVE_STORY_RESULT"],"value":["int",10]},
        {"do":"choice","id":"DIR_NATIVE_STORY_CONTINUE","text":"Record the ambush","steps":[
          {"do":"set","target":["var","DIR_NATIVE_STORY_RESULT"],"value":["int",42]},
          {"do":"call","call":["call","LOG_CREATETOPIC",["str","Native StoryGraph contract"],["int",0]]},
          {"do":"call","call":["call","LOG_ADDENTRY",["str","Native StoryGraph contract"],["str","Ambush recorded"]]}
        ]}]}})");
      require(offered(authored),"the director's new topic must use the same stock helper");
      choose(authored);
      const auto choices=graph.choices(world,*actor,authored);
      require(choices==std::vector<std::pair<std::string,std::string>>{{continuation,"Record the ambush"}},
              "the authored program must expose its continuation in the normal menu");
      choose(continuation);
      require(chosen==std::vector<std::string>{stock,authored,continuation},"the director must see all three choices");

      const auto menu=actor->dialogChoices(*hero,{},false);
      const auto teach=std::ranges::find(menu,*teachFn,&GameScript::DlgChoice::scriptFn);
      require(teach!=menu.end(),"Cavalorn must offer the stock training dialogue");
      script.exec(*teach,*hero,*actor);
      const auto lessons=script.updateDialog(*teach,*hero,*actor);
      const auto bow=std::ranges::find(lessons,*bowFn,&GameScript::DlgChoice::scriptFn);
      require(bow!=lessons.end() && bow->title.ends_with("2 LP"),
              "Cavalorn's bow lesson must display the learner's two-point price");
      script.exec(*bow,*hero,*actor);
      require(hero->handle().lp==0 && hero->handle().hitchance[TALENT_BOW]==36 && hero->handle().aivar[86]==36,
              "Cavalorn must debit two LP and update current and trained bow skill");
      const auto afterPurchase=script.callStoryFunction("B_TEACHFIGHTTALENTPERCENT",actor,hero,
                                                       {actor,hero,int32_t(TALENT_BOW),1,90});
      require(afterPurchase.ok && afterPurchase.returned.i==0 && hero->handle().lp==0 &&
              hero->handle().hitchance[TALENT_BOW]==36 && hero->handle().aivar[86]==36,
              "a repeat lesson without LP must refuse without changing the learner");
      require(script.globalIntSymbol("B_TEACHFIGHTTALENTPERCENT.KOSTEN")==2 &&
              script.globalIntSymbol("B_TEACHFIGHTTALENTPERCENT.REALHITCHANCE")==36,
              "the training result must update the exact stock named values");

      rapidjson::Document invalid;
      invalid.Parse(R"([{"do":"set","target":["var","DIR_NATIVE_STORY_RESULT"],"value":["int",99]},
                         {"do":"call","call":["fn","MISSING_STORY_CONTRACT_HELPER"]}])");
      const auto rejected=runtime.run(storygraph::readProgram(invalid,false),actor,hero);
      require(!rejected.ok && !rejected.started,"an unresolved dependency must be rejected before executing the first effect");

      require(bandits.front()->startState(stateInit,{}),"the bandit must enter a new state without a VM lookup");
      const auto& state=Contract::aiState(*bandits.front());
      require(state.funcIni==stateInit && state.funcLoop==stateLoop && state.funcEnd==stateEnd &&
              std::string_view(state.hint)=="ZS_SHORTZAPPED",
              "a first state transition must resolve the stock Init, Loop and End records");
    }
    ActivityCalls activityCalls;
    Contract::tickRoutine(*bandits.front());
    require(Contract::aiState(*bandits.front()).started && activityCalls.phase(*bandits.front(),"ZS_SHORTZAPPED")==1,
            "the game must run the state Init before its mid-state save");
  }

  std::filesystem::create_directories(caseDir);
  const auto savePath=caseDir/"native-storygraph.sav";
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"Native StoryGraph contract",preview);
  }
  std::ofstream file(savePath,std::ios::binary|std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  file.close();
  require(bool(file),"could not write the native StoryGraph save");
  gothic.clearGame().reset();
  std::ifstream saved(savePath,std::ios::binary);
  require(bool(saved),"could not reopen the native StoryGraph save");
  const std::string savedBytes{std::istreambuf_iterator<char>{saved},std::istreambuf_iterator<char>{}};
  {
    Tempest::MemReader reader(savedBytes.data(),savedBytes.size());
    Serialize in(reader);
    SaveGameHeader header;
    require(in.setEntry("header"),"the native StoryGraph save must have the normal save header");
    in.read(header);
    require(header.version==Serialize::Current,"the StoryGraph contract must reload a newly written current save");
  }
  Tempest::MemReader reader(savedBytes.data(),savedBytes.size());
  Serialize in(reader);
  gothic.setGame(std::make_unique<GameSession>(in));
  auto& world=*gothic.world();
  auto& script=world.script();
  const auto* cavalorn=liveNpc(world,actorName);
  require(world.player()->handle().lp==0 && world.player()->handle().hitchance[TALENT_BOW]==36 &&
          world.player()->handle().aivar[86]==36 && script.globalIntSymbol("B_TEACHFIGHTTALENTPERCENT.KOSTEN")==2 &&
          script.globalIntSymbol("B_TEACHFIGHTTALENTPERCENT.REALHITCHANCE")==36,
          "ordinary reload must restore the purchased skill, LP and saved training values");
  const auto* waypoint=cavalorn ? cavalorn->currentTaPoint() : nullptr;
  require(waypoint && waypoint->name=="NW_XARDAS_BANDITS_LEFT" &&
          std::ranges::all_of(banditNames,[&](const char* name) {
            auto* bandit=liveNpc(world,name);
            return bandit!=nullptr && bandit->handle().aivar[61]==0;
            }),"ordinary reload must restore Cavalorn's changed waypoint and the ambush actors' flags");
  const auto& journal=script.questLog();
  const auto* directorEntry=script.ownSymbols().find(directorNpc);
  require(directorEntry && directorEntry->like==actorName && script.storyNpcId(directorNpc)==directorNpcId,
          "ordinary reload must restore the director NPC definition and its id");
  require(script.globalIntSymbol(directorVar)==42 && script.doesNpcKnowInfo(gothic.world()->player()->handle(),size_t(*stockId)) &&
          journal.questCount()==journalBefore+1 && journal.quest(journalBefore).name==topic &&
          journal.quest(journalBefore).entry==std::vector<std::string>{"Ambush recorded"},
          "ordinary reload must restore the stock dialogue and the authored variable and journal effects");
  auto* bandit=liveNpc(world,banditNames.front());
  require(bandit && bandit->isInState(stateInit),"ordinary reload must restore the active state");
  const auto& state=Contract::aiState(*bandit);
  require(state.started && state.funcLoop==stateLoop && state.funcEnd==stateEnd &&
          std::string_view(state.hint)=="ZS_SHORTZAPPED","ordinary reload must keep the state phases and name");
  ActivityCalls activityCalls;
  script.invokeState(bandit,nullptr,nullptr,state.funcLoop);
  bandit->clearState(false);
  require(activityCalls.phase(*bandit,"ZS_SHORTZAPPED")==0 && activityCalls.phase(*bandit,"ZS_SHORTZAPPED_LOOP")==1 &&
          activityCalls.phase(*bandit,"ZS_SHORTZAPPED_END")==1,
          "a restored state must continue through Loop and End without replaying Init");

  const auto ore=script.findItemInstance("ITMI_NUGGET");
  require(ore && bandit->addItem(*ore,1),"the bridge snapshot needs an ore stack owned by the bandit");
  std::string snapshot;
  NpcController controller([&](std::string&& message) { snapshot=std::move(message); });
  std::string error;
  require(!controller.attach(world,"itmi_nugget",error) && error=="npc not found in current world: ITMI_NUGGET",
          "the bridge must reject an item ID when resolving an NPC");
  require(controller.requestSnapshot(world,"bdt_1013_bandit_l"),"the bridge must resolve the live NPC by its authored ID");
  rapidjson::Document message;
  require(!message.Parse(snapshot.c_str()).HasParseError() && message.HasMember("npc"),
          "the bridge must emit an NPC snapshot");
  const auto& actor=message["npc"];
  require(std::string_view(actor["id"].GetString())==banditNames.front() &&
          std::ranges::any_of(actor["inventory"].GetArray(),[](const auto& item) {
            return std::string_view(item["item"].GetString())=="ITMI_NUGGET" && item["count"].GetInt()==1;
            }),"the snapshot must name the intended NPC and its ore stack");

  std::puts("Cavalorn and authored StoryGraph effects survived save and reload");
  gothic.clearGame().reset();
  }

}

int main(int argc, const char** argv) {
  const bool cavalorn=argc==3 && std::string_view(argv[1])=="--cavalorn";
  if(argc!=2 && !cavalorn) {
    std::fputs("Usage: gothic-native-story-contract [--cavalorn] /absolute/case-directory\n",stderr);
    return EXIT_FAILURE;
    }
  try {
    if(cavalorn)
      nativeStoryGraphContract(argv[2]);
    else
      bennetOreContract(argv[1]);
    return EXIT_SUCCESS;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: native story contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
