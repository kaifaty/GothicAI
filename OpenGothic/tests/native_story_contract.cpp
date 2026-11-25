#include "camera.h"
#include "commandline.h"
#include "dialoggraph.h"
#include "npccontroller.h"
#include "storygraph/exprjson.h"
#include "world/npcactivityprobe.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/menurecord.h"
#include "game/definitions/nativebasecatalog.h"
#include "game/definitions/nativeworldplacement.h"
#include "game/definitions/stockstoryprograms.h"
#include "game/gamescript_programs.h"
#include "game/gamesession.h"
#include "game/savegameheader.h"
#include "game/serialize.h"
#include "gamemusic.h"
#include "gothic.h"
#include "physics/dynamicworld.h"
#include "physics/physicsnoise.h"
#include "physics/ragdoll.h"
#include "resources.h"
#include "world/fplock.h"
#include "world/objects/interactive.h"
#include "world/objects/item.h"
#include "world/objects/npc.h"
#include "world/records/focusrecord.h"
#include "world/records/liveinstances.h"
#include "world/npcsimtick.h"
#include "world/waypoint.h"
#include "world/world.h"

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

#include <rapidjson/document.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
  static auto& aiState(Npc& npc) { return npc.aiState; }
  static bool monsterRestSleeping(const Npc& npc) { return npc.monsterRestSleeping; }
  static void tickRoutine(Npc& npc) { npc.aiState.loopNextTime=npc.world().tickCount(); npc.tickRoutine(); }
  static void nextAiAction(Npc& npc) { npc.nextAiAction(npc.aiQueue,0); }
  static ScriptFn perception(const Npc& npc, PercType type) { return npc.perception[type].func; }
  static int32_t& lastHitSpell(Npc& npc) { return npc.lastHitSpell; }
  static int32_t activeSpellInfo(const Npc& npc) { return npc.spellInfo; }
  static void spawnNpcRange(GameScript& script, Npc& caster, int creature, int count, float lifetime) {
    script.wld_spawnnpcrange(std::static_pointer_cast<NpcInstance>(caster.handlePtr()),creature,count,lifetime);
    }
  static auto& passivePerceptions(World& world) { return world.wobj.sndPerc; }
  static bool refreshNearbyNpc(Npc& npc) {
    auto& world = npc.world();
    world.wobj.updateNpcPolicies(world.player(),world.player()->position());
    const auto& near = world.wobj.npcNear;
    return std::find(near.begin(),near.end(),&npc)!=near.end();
    }
  static bool movePhysicalNpc(Npc& npc, const Tempest::Vec3& position) {
    if(!npc.createRagdoll())
      return false;
    auto state = npc.ragdoll->state();
    if(!Ragdoll::relocateState(*npc.visual.visualSkeleton(),state,position) || !npc.ragdoll->restore(state))
      return false;
    npc.syncRagdoll();
    return true;
    }
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

void loadNativeWorld(Gothic& gothic, const std::filesystem::path& savePath) {
  std::ifstream file(savePath,std::ios::binary);
  require(bool(file),"could not reopen the native world contract save");
  const std::string bytes{std::istreambuf_iterator<char>{file},std::istreambuf_iterator<char>{}};
  Tempest::MemReader reader(bytes.data(),bytes.size());
  Serialize in(reader);
  SaveGameHeader header;
  require(in.setEntry("header"),"native world contract save must have the normal save header");
  in.read(header);
  require(header.version==Serialize::Current && in.setEntry(ScriptVars::StateEntry) &&
          !in.setEntry("game/daedalus") && !in.setEntry("game/symbols"),
          "native world contract save must use the current typed session layout");
  gothic.clearGame().reset();
  gothic.setGame(std::make_unique<GameSession>(in));
  }

void reloadNativeWorld(Gothic& gothic, const std::filesystem::path& savePath) {
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    Tempest::Pixmap preview(1,1,Tempest::TextureFormat::RGBA8);
    std::memset(preview.data(),0,4);
    gothic.gameSession()->save(out,"Native world contract",preview);
  }
  std::ofstream out(savePath,std::ios::binary|std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
  out.close();
  require(bool(out),"could not write the native world contract save");
  loadNativeWorld(gothic,savePath);
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

using WorldContract = void (*)(Gothic&, const std::filesystem::path&);

void runNativeWorldContract(const std::filesystem::path& caseDir, WorldContract contract) {
  require(caseDir.is_absolute(),"native world contract case directory must be absolute");
  const auto* game=std::getenv("GOTHIC_DIR");
  require(game && *game,"GOTHIC_DIR must point to a game settings directory");
  setenv("GOTHIC_AI","0",1);
  const char* args[]={"gothic-native-story-contract","-g",game};
  CommandLine commandLine(3,args);
  Resources resources;
  Gothic gothic;
  requireNativeQuestBase();
  gothic.isNpcInDialogFn=[](const Npc*) { return false; };
  gothic.setupGlobalScripts();
  GameMusic music;
  std::filesystem::create_directories(caseDir);
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  contract(gothic,caseDir);
  gothic.clearGame().reset();
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

void nativeNpcInventoryContract(Gothic& gothic, const std::filesystem::path& caseDir) {
  using Contract = NpcDefinitionContract;
  constexpr size_t ransacked = 10, trophyGiven = 11, victoryXpGiven = 16, species = 43, inventoryChapter = 49;
  constexpr auto matteoName = "VLK_416_MATTEO", kardifName = "VLK_431_KARDIF", wolfName = "WOLF";
  constexpr auto hildaName = "BAU_951_HILDA", mikaName = "MIL_337_MIKA", joraName = "VLK_408_JORA";
  constexpr auto matteoStock = "MATTEO_ITEMSGIVEN_CHAPTER_1", kardifStock = "KARDIF_ITEMSGIVEN_CHAPTER_2";
  constexpr auto trophyTalent = "PLAYER_TALENT_TAKEANIMALTROPHY";
  constexpr std::array trophyRolls = {"GOBLINGREEN_RANDOMIZER", "GOBLINBLACK_RANDOMIZER", "ORC_RANDOMIZER"};
  const auto actor = [](World& world, const char* name) -> Npc& {
    auto* npc = liveNpc(world,name);
    if(npc==nullptr)
      throw std::runtime_error(std::string("NPC inventory: missing stock actor ")+name);
    return *npc;
    };
  const auto setInt = [](GameScript& script, const char* name, int32_t value) {
    if(!script.setGlobalIntSymbol(name,value))
      throw std::runtime_error(std::string("NPC inventory: missing stock state ")+name);
    };
  const auto npcAlias = [](GameScript& script, const char* name, Npc& npc) {
    return Contract::alias(script,name)==std::static_pointer_cast<NpcInstance>(npc.handlePtr());
    };
  const auto itemAlias = [](GameScript& script, const char* name, Item* item) {
    return item && Contract::alias(script,name)==std::static_pointer_cast<ItemInstance>(item->handlePtr());
    };
  const auto call = [](GameScript& script, const char* name, Npc& self, Npc& recipient) {
    const auto caller = script.storyActors();
    const auto result = script.callStoryFunction(name,&self,nullptr,{&recipient});
    if(!result.ok)
      throw std::runtime_error(std::string(name)+": "+result.error);
    require(result.started && script.storyActors()==caller,"NPC inventory: stock call restores the caller's actors");
    };

  auto& world = *gothic.world();
  auto& script = world.script();
  auto& hero = *world.player();
  auto& matteo = actor(world,matteoName);
  auto& kardif = actor(world,kardifName);
  auto& wolf = actor(world,wolfName);
  auto& hilda = actor(world,hildaName);
  auto& mika = actor(world,mikaName);
  auto& jora = actor(world,joraName);

  const auto gold = script.findItemInstance("ITMI_GOLD").value();
  const auto fur = script.findItemInstance("ITAT_WOLFFUR").value();
  const auto teeth = script.findItemInstance("ITAT_TEETH").value();
  const auto lockpick = script.findItemInstance("ITKE_LOCKPICK").value();
  const auto light = script.findItemInstance("ITRU_LIGHT").value();
  const auto paladinLight = script.findItemInstance("ITRU_PALLIGHT").value();
  const auto armor = script.findItemInstance("ITAR_MIL_L").value();
  const auto belt = script.findItemInstance("ITBE_ADDON_MIL_01").value();
  const auto sword = script.findItemInstance("ITMW_1H_VLK_SWORD").value();
  const auto alrik = script.findItemInstance("ITMW_ALRIKSSWORD_MIS").value();

  require(!matteo.isDown() && matteo.addItem(fur,2),"trade inventory: start with Matteo awake and carrying junk");
  const auto matteoGold = matteo.itemCount(gold);
  setInt(script,"KAPITEL",1);
  setInt(script,matteoStock,0);
  matteo.handle().aivar[inventoryChapter] = 1;
  matteo.handle().aivar[victoryXpGiven] = 1;
  call(script,"B_GIVETRADEINV",matteo,matteo);
  require(matteo.itemCount(fur)==0 && matteo.itemCount(gold)==matteoGold+100 &&
          matteo.handle().aivar[inventoryChapter]==2 && script.globalIntSymbol(matteoStock)==1 &&
          npcAlias(script,"B_GIVETRADEINV.TRD_MATTEO",matteo) && npcAlias(script,"B_GIVETRADEINV_MATTEO.SLF",matteo),
          "trade inventory: remove junk and give Matteo the first chapter's stock with the two live actor aliases");
  call(script,"B_GIVETRADEINV",matteo,matteo);
  require(matteo.itemCount(gold)==matteoGold+100,"trade inventory: repeat visit cannot grant the same gold twice");
  matteo.setSimTier(NpcSimTier::Full);
  matteo.setProcessPolicy(NpcProcessPolicy::AiNormal);
  matteo.setAttitude(ATT_NEUTRAL);
  matteo.setOther(&hero);
  const auto xpBeforeKnockout = hero.experience();
  matteo.changeAttribute(ATR_HITPOINTS,-matteo.attribute(ATR_HITPOINTS),true);
  matteo.setOther(&hero);
  require(matteo.isUnconscious() && !matteo.isDead() && matteo.attribute(ATR_HITPOINTS)==1,
          "trade inventory: the stock helper must see the nonlethally defeated merchant");
  call(script,"B_GIVETRADEINV",matteo,matteo);
  require(matteo.itemCount(gold)==0 && matteo.handle().aivar[inventoryChapter]==0 &&
          script.globalIntSymbol(matteoStock)==1 && npcAlias(script,"B_CLEARDEADTRADER.TRADER",matteo),
          "trade inventory: defeat clears stock and resets the chapter counter without resetting the grant flag");
  const auto wakeAt = int64_t(script.globalIntSymbol("HAI_TIME_UNCONSCIOUS").value())*1000;
  require(wakeAt>0,"trade inventory: need the verified recovery deadline");
  {
    ActivityCalls phases;
    Contract::tickRoutine(matteo);
    require(phases.phase(matteo,"ZS_UNCONSCIOUS")==1 && matteo.hasPerc(PERC_ASSESSMAGIC) &&
            matteo.stateOther()==&hero && hero.experience()==xpBeforeKnockout &&
            script.globalIntSymbol("C_BODYSTATECONTAINS.BODYSTATE")==BS_SWIM &&
            npcAlias(script,"C_BODYSTATECONTAINS.SLF",matteo),
            "trade inventory: knockout Init records the body check and cause and preserves an already awarded victory");
    matteo.clearAiQueue();
    matteo.handle().aivar[ransacked] = 1;
    matteo.setStateTime(wakeAt-1);
    Contract::tickRoutine(matteo);
    require(phases.phase(matteo,"ZS_UNCONSCIOUS_LOOP")==1 && matteo.isUnconscious() && matteo.isAiQueueEmpty() &&
            matteo.handle().aivar[ransacked]==1,"trade inventory: the last millisecond before recovery still waits");
    }

  setInt(script,"KAPITEL",2);
  setInt(script,"KARDIF_ITEMSGIVEN_CHAPTER_1",1);
  const auto heroLockpicks = hero.itemCount(lockpick);
  const auto kardifLockpicks = kardif.itemCount(lockpick);
  for(const bool knowsSign : {false,true}) {
    setInt(script,kardifStock,0);
    setInt(script,"KNOWS_SECRETSIGN",int32_t(knowsSign));
    call(script,"B_GIVETRADEINV_KARDIF",hero,kardif);
    require(script.globalIntSymbol(kardifStock)==1 && kardif.itemCount(lockpick)==kardifLockpicks &&
            hero.itemCount(lockpick)==heroLockpicks+(knowsSign ? 20u : 0u),
            "trade inventory: Kardif grants the sign bonus only to SELF, without stocking lockpicks on the trader");
    }
  setInt(script,"KAPITEL",1);

  require(!hilda.isDown() && hilda.attribute(ATR_HITPOINTS)>0 && hilda.addItem(light,5) && hilda.addItem(paladinLight,2),
          "death inventory: Hilda starts alive with both stock light runes");
  hilda.setSimTier(NpcSimTier::Full);
  hilda.setProcessPolicy(NpcProcessPolicy::AiNormal);
  hilda.handle().aivar[victoryXpGiven] = 0;
  const auto beforeXp = hero.experience();
  const auto deathXp = hilda.handle().level*script.globalIntSymbol("XP_PER_VICTORY").value();
  hilda.changeAttribute(ATR_HITPOINTS,-hilda.attribute(ATR_HITPOINTS),false);
  hilda.setOther(&hero);
  require(hilda.isDead() && !Contract::aiState(hilda).started,"death inventory: HP loss enters the stock death state");
  {
    ActivityCalls phases;
    Contract::tickRoutine(hilda);
    Contract::tickRoutine(hilda);
    Contract::tickRoutine(hilda);
    require(phases.phase(hilda,"ZS_DEAD")==1 && phases.phase(hilda,"ZS_DEAD_LOOP")==2 &&
            hilda.itemCount(light)==3 && hilda.itemCount(paladinLight)==0 && hilda.stateOther()==&hero &&
            hilda.handle().aivar[victoryXpGiven]==1 && hero.experience()==beforeXp+deathXp,
            "death inventory: two light runes keep the stock cross-count and only Init grants victory XP");
    }
  hilda.setStateTime(750);
  const auto corpseQueue = activityQueue(hilda);

  require(mika.currentArmor() && mika.currentArmor()->clsId()==armor,"equipment: Mika starts in his stock militia armor");
  require(!hero.currentArmor(),"equipment: the starting hero must not already wear the militia set");
  require(jora.currentMeleeWeapon() && jora.currentMeleeWeapon()->clsId()==alrik,
          "equipment: Jora starts with Alrik's stock sword equipped");
  require(jora.getItem(sword) || jora.addItem(sword,1),"equipment: give Jora the plain comparison sword");
  // The comparison needs a fighter exactly ten points below the master threshold.
  jora.unequipItem(alrik);
  jora.handle().hitchance[TALENT_1H] = 50;
  jora.setTalentSkill(TALENT_1H,1);
  jora.useItem(alrik,Item::NSLOT,true);
  require(jora.currentMeleeWeapon()==jora.getItem(alrik) && jora.hitChance(TALENT_1H)==60 && jora.talentSkill(TALENT_1H)==2,
          "equipment: Alrik's stock sword raises the prepared fighter to master rank");
  const auto guardProtection = std::to_array(mika.handle().protection);
  require(hero.addItem(belt,1) && hero.addItem(armor,1) && hero.addItem(sword,1),
          "equipment: add the three stock pieces to the hero's real inventory");
  hero.useItem(belt);
  const auto beltProtection = std::to_array(hero.handle().protection);
  require(hero.getItem(belt)->isEquipped() && script.globalIntSymbol("MIL01_EQUIPPED")==1 &&
          script.globalIntSymbol("MILARMOR_EQUIPPED")==0,"equipment: the worn belt sets only its own flag");
  mika.unequipItem(armor);
  mika.useItem(armor);
  require(mika.currentArmor()==mika.getItem(armor) && mika.currentArmor()->isEquipped() &&
          std::to_array(mika.handle().protection)==guardProtection && script.globalIntSymbol("MILARMOR_EQUIPPED")==0,
          "equipment: Mika keeps ordinary armor protection and does not set the hero's flag");
  hero.useItem(armor);
  require(hero.currentArmor() && hero.currentArmor()->clsId()==armor && script.globalIntSymbol("MILARMOR_EQUIPPED")==1,
          "equipment: the hero equips the second set piece through inventory");
  const auto setProtection = std::to_array(hero.handle().protection);
  const auto setBonus = script.globalIntSymbol("BA_BONUS01").value();
  require(setBonus>0,"equipment: need the verified militia set bonus");
  for(size_t i=0; i<setProtection.size(); ++i) {
    const bool affected = i==PROT_EDGE || i==PROT_BLUNT || i==PROT_POINT || i==PROT_MAGIC || i==PROT_FIRE;
    require(setProtection[i]==beltProtection[i]+hero.currentArmor()->handle().protection[i]+(affected ? setBonus : 0),
            "equipment: the militia set adds exactly five stock protection bonuses");
    }
  if(const auto* oldSword = hero.currentMeleeWeapon())
    hero.unequipItem(oldSword->clsId());
  hero.handle().hitchance[TALENT_1H] = 25;
  hero.setTalentSkill(TALENT_1H,0);
  hero.useItem(sword,Item::NSLOT,true);
  require(hero.currentMeleeWeapon() && hero.currentMeleeWeapon()->clsId()==sword &&
          hero.hitChance(TALENT_1H)==30 && hero.talentSkill(TALENT_1H)==1,
          "equipment: the hero's stock sword raises 25 percent to fighter rank at 30");
  jora.unequipItem(alrik);
  require(!jora.getItem(alrik)->isEquipped() && jora.hitChance(TALENT_1H)==50 && jora.talentSkill(TALENT_1H)==1 &&
          script.globalIntSymbol("B_ADDFIGHTSKILL.PERCENT")==-10,"equipment: Alrik's sword removal lowers Jora's rank");
  jora.useItem(sword,Item::NSLOT,true);
  require(jora.currentMeleeWeapon()==jora.getItem(sword) && jora.hitChance(TALENT_1H)==50 &&
          jora.talentSkill(TALENT_1H)==1 && script.globalIntSymbol("B_ADDFIGHTSKILL.PERCENT")==-10,
          "equipment: the generic sword leaves NPC skill and helper arguments alone");
  jora.unequipItem(sword);
  jora.useItem(alrik,Item::NSLOT,true);
  require(jora.currentMeleeWeapon()==jora.getItem(alrik) && jora.hitChance(TALENT_1H)==60 &&
          jora.talentSkill(TALENT_1H)==2,"equipment: Alrik's sword returns Jora to master rank");

  require(wolf.handle().aivar[species]==script.globalIntSymbol("ID_WOLF").value(),"death trophies: need the stock wolf");
  const auto playerFur = hero.itemCount(fur)+1;
  require(hero.addItem(fur,1),"death trophies: create another owner's item of the same class");
  {
    auto host = script.makeStoryHost(nullptr,nullptr,true);
    std::string error;
    for(uint16_t i=0; i<14; ++i) {
      const storygraph::NativeSlot slot{storygraph::NativeSlot::Variable,trophyTalent,i};
      require(host->write(slot,storygraph::NativeValue::integer(i==0 || i==2 ? 1 : 0),error),
              "death trophies: set exactly the stock teeth and fur talents");
      }
    }
  wolf.handle().aivar[trophyGiven] = 0;
  script.setSeed(73106);
  std::mt19937 expectedRandom(73106);
  const std::array expectedRolls = {int32_t(expectedRandom()%100u), int32_t(expectedRandom()%100u), int32_t(expectedRandom()%10u)};
  std::ostringstream randomState;
  randomState << expectedRandom;
  const auto earnedTeeth = wolf.itemCount(teeth)+2;
  const auto earnedFur = wolf.itemCount(fur)+1;
  call(script,"B_GIVEDEATHINV",wolf,wolf);
  require(wolf.itemCount(teeth)==earnedTeeth && wolf.itemCount(fur)==earnedFur && wolf.handle().aivar[trophyGiven]==1 &&
          npcAlias(script,"B_GIVEDEATHINV.SLF",wolf) && itemAlias(script,"ITEM",wolf.getItem(fur)),
          "death trophies: the learned teeth and fur grants publish the exact last item and record the recipient");
  for(size_t i=0; i<trophyRolls.size(); ++i)
    require(script.globalIntSymbol(trophyRolls[i])==expectedRolls[i],"death trophies: each stock draw writes to its own global");
  require(script.randomState(nullptr)==randomState.str(),"death trophies: consume exactly three draws even for an ordinary wolf");
  call(script,"B_GIVEDEATHINV",wolf,wolf);
  require(wolf.itemCount(teeth)==earnedTeeth && wolf.itemCount(fur)==earnedFur &&
          script.randomState(nullptr)==randomState.str() && itemAlias(script,"ITEM",wolf.getItem(fur)),
          "death trophies: the repeat guard suppresses both grants and draws");

  reloadNativeWorld(gothic,caseDir/"npc-inventory.sav");
  auto& restored = *gothic.world();
  auto& saved = restored.script();
  auto& player = *restored.player();
  auto& trader = actor(restored,matteoName);
  auto& tavernKeeper = actor(restored,kardifName);
  auto& animal = actor(restored,wolfName);
  auto& corpse = actor(restored,hildaName);
  auto& guard = actor(restored,mikaName);
  auto& swordsman = actor(restored,joraName);

  require(animal.itemCount(teeth)==earnedTeeth && animal.itemCount(fur)==earnedFur && animal.handle().aivar[trophyGiven]==1 &&
          player.itemCount(fur)==playerFur && itemAlias(saved,"ITEM",animal.getItem(fur)) &&
          !itemAlias(saved,"ITEM",player.getItem(fur)) && npcAlias(saved,"B_GIVEDEATHINV.SLF",animal),
          "death trophies: v57 restores the latch, exact inventory instance and live recipient");
  for(size_t i=0; i<trophyRolls.size(); ++i)
    require(saved.globalIntSymbol(trophyRolls[i])==expectedRolls[i],"death trophies: v57 restores each exact stock draw");
  const auto beforeRepeat = saved.randomState(nullptr);
  call(saved,"B_GIVEDEATHINV",animal,animal);
  require(animal.itemCount(teeth)==earnedTeeth && animal.itemCount(fur)==earnedFur &&
          saved.randomState(nullptr)==beforeRepeat,"death trophies: the restored latch still forbids a second award");

  require(corpse.isDead() && Contract::aiState(corpse).started && corpse.stateOther()==&player &&
          corpse.stateTime()==750 && activityQueue(corpse)==corpseQueue &&
          corpse.itemCount(light)==3 && corpse.itemCount(paladinLight)==0 && player.experience()==beforeXp+deathXp,
          "death inventory: v57 restores Hilda's corpse, loot, credited player, queue and state time");
  {
    ActivityCalls phases;
    Contract::tickRoutine(corpse);
    require(phases.phase(corpse,"ZS_DEAD")==0 && phases.phase(corpse,"ZS_DEAD_LOOP")==1 &&
            corpse.itemCount(light)==3 && corpse.itemCount(paladinLight)==0 && player.experience()==beforeXp+deathXp,
            "death inventory: the loaded corpse continues Loop without replaying cleanup or victory XP");
    }

  require(trader.isUnconscious() && Contract::aiState(trader).started && trader.stateOther()==&player &&
          trader.stateTime()==uint64_t(wakeAt-1) && trader.handle().aivar[ransacked]==1 &&
          trader.handle().aivar[victoryXpGiven]==1 && trader.isAiQueueEmpty() &&
          trader.itemCount(gold)==0 && trader.handle().aivar[inventoryChapter]==0 && saved.globalIntSymbol(matteoStock)==1 &&
          npcAlias(saved,"B_CLEARDEADTRADER.TRADER",trader) && npcAlias(saved,"B_GIVETRADEINV_MATTEO.SLF",trader) &&
          saved.globalIntSymbol(kardifStock)==1 && player.itemCount(lockpick)==heroLockpicks+20 &&
          tavernKeeper.itemCount(lockpick)==kardifLockpicks,
          "trade inventory: v57 restores the knockdown deadline, defeated stock, chapter latches and the sign recipient");
  trader.handle().aivar[victoryXpGiven] = 0;
  saved.setSeed(5489); // The first stock Hlp_Random(7) draw is 1.
  call(saved,"B_GIVETRADEINV",trader,trader);
  require(trader.itemCount(gold)==5 && trader.handle().aivar[inventoryChapter]==0 && saved.globalIntSymbol(matteoStock)==1 &&
          trader.itemCount(saved.findItemInstance("ITMI_SILVERRING").value())==1 &&
          saved.globalIntSymbol("B_CREATEAMBIENTINV.ZUFALL")==1 &&
          saved.globalIntSymbol("B_CREATEAMBIENTINV_VLK.INVENTORYSET")==1 && npcAlias(saved,"B_CREATEAMBIENTINV.SLF",trader),
          "trade inventory: an unrewarded defeated trader gets the seeded ambient batch without replaying the chapter grant");
  trader.handle().aivar[victoryXpGiven] = 1;
  {
    ActivityCalls phases;
    trader.setSimTier(NpcSimTier::Full);
    trader.setStateTime(wakeAt);
    Contract::tickRoutine(trader);
    require(!trader.isUnconscious() && phases.phase(trader,"ZS_UNCONSCIOUS")==0 &&
            phases.phase(trader,"ZS_UNCONSCIOUS_LOOP")==1 && phases.phase(trader,"ZS_UNCONSCIOUS_END")==1 &&
            trader.handle().aivar[ransacked]==0 && player.experience()==beforeXp+deathXp,
            "trade inventory: the loaded merchant wakes at the deadline without replaying Init or victory XP");
    auto recovery = Contract::queue(trader);
    require(recovery.size()>=4 && recovery.pop().act==AI_StandUp,
            "trade inventory: recovery first queues standing up");
    auto action = recovery.pop();
    if(action.act==AI_TakeItem)
      action = recovery.pop();
    require(action.act==AI_EquipMelee && recovery.pop().act==AI_EquipRange && recovery.size()==1,
            "trade inventory: recovery retrieves any weapon before equipping melee and ranged");
    const auto heal = recovery.pop();
    require(heal.act==AI_StartState && heal.func==saved.findFunction("ZS_HEALSELF") && heal.target==&player,
            "trade inventory: recovery queues self-healing with the original other");
    }

  require(player.currentArmor()==player.getItem(armor) && player.currentArmor() &&
          player.getItem(belt) && player.getItem(belt)->isEquipped() && saved.globalIntSymbol("MILARMOR_EQUIPPED")==1 &&
          saved.globalIntSymbol("MIL01_EQUIPPED")==1 && std::to_array(player.handle().protection)==setProtection &&
          guard.currentArmor()==guard.getItem(armor) && guard.currentArmor() &&
          std::to_array(guard.handle().protection)==guardProtection,
          "equipment: v57 restores the armor slots, both set flags and exact protection without replaying the bonuses");
  player.unequipItem(armor);
  require(!player.currentArmor() && saved.globalIntSymbol("MILARMOR_EQUIPPED")==0 &&
          std::to_array(player.handle().protection)==beltProtection,
          "equipment: removing the restored armor subtracts base protection and the set bonus exactly once");
  require(player.currentMeleeWeapon()==player.getItem(sword) && player.currentMeleeWeapon() &&
          player.hitChance(TALENT_1H)==30 && player.talentSkill(TALENT_1H)==1 &&
          swordsman.currentMeleeWeapon()==swordsman.getItem(alrik) && swordsman.currentMeleeWeapon() &&
          swordsman.hitChance(TALENT_1H)==60 && swordsman.talentSkill(TALENT_1H)==2 &&
          npcAlias(saved,"B_ADDFIGHTSKILL.SLF",swordsman) &&
          saved.globalIntSymbol("B_ADDFIGHTSKILL.PERCENT")==10 && saved.globalIntSymbol("B_ADDFIGHTSKILL.TALENT")==TALENT_1H,
          "equipment: v57 restores both weapon instances, their owners' ranks and the last helper arguments");
  swordsman.unequipItem(alrik);
  player.unequipItem(sword);
  require(!swordsman.currentMeleeWeapon() && swordsman.hitChance(TALENT_1H)==50 && swordsman.talentSkill(TALENT_1H)==1 &&
          !player.currentMeleeWeapon() && player.hitChance(TALENT_1H)==25 && player.talentSkill(TALENT_1H)==0,
          "equipment: removing the restored swords reverses their own bonuses exactly once");
  std::puts("Native NPC inventory: corpse loot, trophies, trade and equipment survived v57 reload");
  }

#include "npc_murder_world_fixture.h"

void onlyBehaviorActors(World& world, const Npc* first, const Npc* second=nullptr) {
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* actor=world.npcById(id);
    if(actor)
      actor->setExternalControl(actor!=first && actor!=second);
    }
  }

void moveBehaviorHero(Gothic& gothic, const Tempest::Vec3& at) {
  auto& world=*gothic.world();
  auto& hero=*world.player();
  hero.setPosition(at);
  hero.updateTransform();
  auto& camera=*gothic.camera();
  camera.setViewport(640,480);
  camera.reset(&hero);
  world.updateLandscape(camera);
  world.updateNpcSimTiers();
  }

void stepBehaviorUntil(Gothic& gothic, size_t ticks, const auto& done, const char* message) {
  for(size_t i=0;i<ticks && !done();++i) {
    gothic.gameSession()->tick(33);
    gothic.gameSession()->updateAnimation(33);
    }
  require(done(),message);
  }

auto savedBehaviorQueue(const World& world, Npc& npc) {
  using Command=std::tuple<Action,uint32_t,uint32_t,std::string,size_t,int,int,std::string,int,std::string,bool>;
  std::vector<Command> result;
  auto queue=NpcDefinitionContract::queue(npc);
  while(queue.size()!=0) {
    const auto a=queue.pop();
    result.emplace_back(a.act,world.npcId(a.target),world.npcId(a.victim),a.point ? a.point->name : "",
                        a.func.ptr,a.i0,a.i1,a.s0,a.i2,a.s1,a.activity);
    }
  return result;
  }

void savedHumanRoutines(Gothic& gothic, const std::filesystem::path& savePath) {
  using Contract=NpcDefinitionContract;
  constexpr std::string_view banditName="BDT_1009_BANDIT_L", guardName="KDF_507_TALAMON";
  constexpr std::string_view banditState="ZS_BANDIT", guardState="ZS_GUARD_PASSAGE";
  constexpr std::string_view banditRoll="ZS_BANDIT_LOOP.RANDOM", waypointRoll="ZS_STAND_WP_LOOP.RANDOM";
  constexpr std::string_view ownerName="SLD_823_KHALED", contenderName="BAU_950_LOBART";
  constexpr std::string_view pointState="ZS_STAND_GUARDING";
  auto& world=*gothic.world();
  auto& script=world.script();
  auto* bandit=liveNpc(world,banditName);
  auto* guard=liveNpc(world,guardName);
  auto* owner=liveNpc(world,ownerName);
  auto* contender=liveNpc(world,contenderName);
  const auto* camp=world.findWayPoint("NW_FOREST_CAVE1_IN_04");
  const auto* post=world.findWayPoint("NW_MONASTERY_CELLAR_15");
  const auto banditId=script.findFunction(banditState);
  const auto guardId=script.findFunction(guardState);
  const auto pointStateId=script.findFunction(pointState);
  require(bandit && guard && !bandit->isDead() && !guard->isDead() && camp && post &&
          banditId.isValid() && guardId.isValid(),
          "routine save: the stock bandit, guard and waypoints must exist");
  require(owner && contender && !owner->isDead() && !contender->isDead() && pointStateId.isValid(),
          "guard post save: Khaled and Lobart must be present in the stock world");

  const auto farmPosts=[](World& active) {
    const auto* row=active.findWayPoint("NW_BIGFARM_HOUSE_12");
    const auto* upper=active.findPoint("FP_STAND_NW_BIGFARM_HOUSE_UP2_01",false);
    constexpr std::string_view name="FP_STAND_NW_BIGFARM_HOUSE_LEE_GUARDING";
    std::vector<const WayPoint*> posts;
    active.forEachWayPoint([&](const WayPoint& point) {
      if(point.name==name && point.isFreePoint())
        posts.push_back(&point);
      });
    require(row && upper && posts.size()==2,"guard post save: the farm row needs both identically named posts");
    if(posts[0]->qDistTo(row->position())>posts[1]->qDistTo(row->position()))
      std::swap(posts[0],posts[1]);
    return std::tuple{row,posts[0],posts[1],upper};
    };

  onlyBehaviorActors(world,nullptr);
  moveBehaviorHero(gothic,camp->position()+Tempest::Vec3(650,0,0));
  for(const auto [npc,point,state]:{std::tuple{bandit,camp,banditId},std::tuple{guard,post,guardId}}) {
    // Manual routine ticks do not run the world's distance-based policy update.
    npc->setProcessPolicy(NpcProcessPolicy::AiNormal);
    npc->setInteraction(nullptr,true);
    npc->clearState(true);
    npc->clearAiQueue();
    npc->setTarget(nullptr);
    npc->clearGoTo();
    npc->setPosition(point->position());
    npc->updateTransform();
    require(npc->startState(state,point->name),"routine save: actor must enter its stock state");
    Contract::tickRoutine(*npc);
    require(npc->isInState(state) && npc->isStateStarted(),"routine save: the stock Init must run");
    }
  bandit->clearAiQueue();
  const auto banditPose=bandit->bodyStateMasked();
  {
    ActivityCalls phases;
    const auto stage=bandit->handle().aivar[19];
    Contract::tickRoutine(*bandit);
    const auto calls=phases.phase(*bandit,"ZS_BANDIT_LOOP");
    require(stage==1 && bandit->handle().aivar[19]==0 && calls==1 && !bandit->isAiQueueEmpty(),
            ("routine save: bandit first Loop must choose the pose once, stage "+std::to_string(stage)+" -> "+
             std::to_string(bandit->handle().aivar[19])+", Loop calls "+std::to_string(calls)+", queue "+
             std::to_string(Contract::queue(*bandit).size())).c_str());
  }
  const auto pending=savedBehaviorQueue(world,*bandit);

  const auto [row,nearPost,farPost,upperPost]=farmPosts(world);
  moveBehaviorHero(gothic,row->position()+Tempest::Vec3(650,0,0));
  for(uint32_t id=0;id<world.npcCount();++id) {
    auto* actor=world.npcById(id);
    if(actor && (actor->currentWayPoint()==nearPost || actor->currentWayPoint()==farPost))
      actor->attachToPoint(nullptr);
    }
  require(nearPost->useCounter()==0 && farPost->useCounter()==0,
          "guard post save: both competing points must start free");
  {
    const FpLock nearBlocked(nearPost),upperBlocked(upperPost);
    for(auto* actor:{owner,contender}) {
      actor->setProcessPolicy(NpcProcessPolicy::AiNormal);
      actor->setSimTier(NpcSimTier::Full);
      actor->setInteraction(nullptr,true);
      actor->clearState(true);
      actor->clearAiQueue();
      actor->clearGoTo();
      actor->setTarget(nullptr);
      actor->setPosition(row->position());
      actor->attachToPoint(row);
      actor->updateTransform();
      require(actor->startState(pointStateId,row->name),"guard post save: begin the native guard activity");
      Contract::tickRoutine(*actor);
      for(size_t i=0;i<16 && !actor->isAiQueueEmpty();++i)
        Contract::nextAiAction(*actor);
      require(actor->isAiQueueEmpty(),"guard post save: the guard's Init commands must finish at the row");
      Contract::tickRoutine(*actor);
      const auto* walk=actor->aiQueueFront();
      require(walk && walk->act==AI_GoToPoint && walk->activity && walk->hasExactPoint() && walk->point==farPost,
              "guard post save: the native guard must queue the exact free duplicate");
      }
    }
  const auto farPosition=farPost->position();
  Contract::nextAiAction(*owner);
  contender->aiPush(AiQueue::aiLookAtNpc(owner));
  require(owner->currentWayPoint()==farPost && owner->currentGoToPoint()==farPost && farPost->isGuarded() &&
          farPost->useCounter()==1 && nearPost->useCounter()==0 && contender->currentWayPoint()==row,
          "guard post save: the active walk holds the exact post before the competing command runs");
  const auto contenderQueue=savedBehaviorQueue(world,*contender);

  require(script.setGlobalIntSymbol(banditRoll,1) && script.setGlobalIntSymbol(waypointRoll,7),
          "routine save: both typed random slots must be writable");
  bandit->setStateTime(5999);
  guard->setStateTime(2999);
  const auto banditPosition=bandit->position(), guardPosition=guard->position();
  const auto guardPose=guard->bodyStateMasked();
  const auto guardQueue=savedBehaviorQueue(world,*guard);
  reloadNativeWorld(gothic,savePath);

  auto& restored=*gothic.world();
  auto& resumedScript=restored.script();
  bandit=liveNpc(restored,banditName);
  guard=liveNpc(restored,guardName);
  owner=liveNpc(restored,ownerName);
  contender=liveNpc(restored,contenderName);
  require(bandit && bandit->isInState(banditId) && bandit->isStateStarted() && bandit->stateTime()==5999 &&
          bandit->bodyStateMasked()==banditPose && (bandit->position()-banditPosition).length()<1.f &&
          bandit->handle().aivar[19]==0 && savedBehaviorQueue(restored,*bandit)==pending &&
          resumedScript.globalIntSymbol(banditRoll)==1 && resumedScript.globalIntSymbol(waypointRoll)==7,
          "routine save: load restores the bandit's pose, timer, queued commands and exact random slots");
  require(guard && guard->isInState(guardId) && guard->isStateStarted() && guard->stateTime()==2999 &&
          guard->bodyStateMasked()==guardPose && (guard->position()-guardPosition).length()<1.f &&
          savedBehaviorQueue(restored,*guard)==guardQueue,
          "routine save: load restores the guard's pose, position, timer and queued commands");

  ActivityCalls phases;
  const auto [savedRow,savedNear,savedFar,savedUpper]=farmPosts(restored);
  require(owner && contender && savedFar->position()==farPosition &&
          owner->isInState(pointStateId) && owner->isStateStarted() && owner->currentWayPoint()==savedFar &&
          owner->currentGoToPoint()==savedFar && savedFar->isGuarded() && savedFar->useCounter()==1 &&
          savedNear->useCounter()==0 && contender->isInState(pointStateId) && contender->isStateStarted() &&
          contender->currentWayPoint()==savedRow && savedBehaviorQueue(restored,*contender)==contenderQueue,
          "guard post save: load preserves the owner, lock and competing action order");
  const auto* restoredWalk=contender->aiQueueFront();
  require(restoredWalk && restoredWalk->act==AI_GoToPoint && restoredWalk->activity &&
          restoredWalk->hasExactPoint() && restoredWalk->point==savedFar,
          "guard post save: the queued walk still targets that duplicate rather than its closer namesake");
  contender->setProcessPolicy(NpcProcessPolicy::AiNormal);
  Contract::nextAiAction(*contender);
  const auto* retained=contender->aiQueueFront();
  require(retained && retained->act==AI_LookAtNpc && retained->target==owner && !retained->activity &&
          Contract::queue(*contender).size()==1 && contender->currentWayPoint()==savedRow &&
          contender->currentGoToPoint()==nullptr && owner->currentWayPoint()==savedFar &&
          savedFar->useCounter()==1 && savedNear->useCounter()==0,
          "guard post save: the rejected native walk cannot take either post or discard the external look");
  Contract::nextAiAction(*contender);
  require(contender->lookAtTarget()==owner && contender->isAiQueueEmpty() &&
          phases.phase(*contender,pointState)==0 && phases.phase(*contender,"ZS_STAND_GUARDING_LOOP")==0,
          "guard post save: the exact external command runs before another activity step");
  std::puts("Native guard post save: occupied duplicate, competing native walk and external look survived v57");

  bandit->setProcessPolicy(NpcProcessPolicy::AiNormal);
  bandit->clearAiQueue();
  require(resumedScript.setGlobalIntSymbol(banditRoll,-1),"routine save: reset the saved draw before the threshold");
  resumedScript.setSeed(73106);
  Contract::tickRoutine(*bandit);
  require(bandit->stateTime()==5999 && bandit->isAiQueueEmpty() && resumedScript.globalIntSymbol(banditRoll)==-1,
          "routine save: the bandit leaves its draw untouched before six whole seconds");
  bandit->setStateTime(6000);
  Contract::tickRoutine(*bandit);
  require(bandit->stateTime()==0 && bandit->isAiQueueEmpty() && resumedScript.globalIntSymbol(banditRoll)==1 &&
          resumedScript.globalIntSymbol(waypointRoll)==7 &&
          phases.phase(*bandit,banditState)==0 && phases.phase(*bandit,"ZS_BANDIT_LOOP")==2,
          "routine save: seed 73106 consumes one silent bandit draw at six seconds without rerunning Init");

  guard->setProcessPolicy(NpcProcessPolicy::AiNormal);
  for(const auto [distance,time,align]:{std::tuple{650.f,2999,false},std::tuple{450.f,3000,false},
                                        std::tuple{650.f,3000,true}}) {
    moveBehaviorHero(gothic,guardPosition+Tempest::Vec3(distance,0,0));
    guard->clearAiQueue();
    guard->setStateTime(time);
    Contract::tickRoutine(*guard);
    const auto* pendingAction=guard->aiQueueFront();
    require(guard->stateTime()==(align ? 0u : uint64_t(time)) &&
            (align ? Contract::queue(*guard).size()==1 && pendingAction && pendingAction->act==AI_AlignToWp
                   : guard->isAiQueueEmpty()),
            "routine save: the restored guard aligns only after three seconds with the hero outside 500 cm");
    }
  require(phases.phase(*guard,guardState)==0 && phases.phase(*guard,"ZS_GUARD_PASSAGE_LOOP")>=1,
          "routine save: the guard checks the threshold in Loop without repeating Init");
  std::puts("Native routine saves: bandit pose, named random slots and guard thresholds after v57 reload");
  }

void farIdleMonster(Gothic& gothic) {
  gothic.clearGame().reset();
  gothic.setGame(std::make_unique<GameSession>("NEWWORLD.ZEN"));
  auto& session=*gothic.gameSession();
  auto& world=*session.world();
  const auto restore=NpcSimTick::config();
  NpcSimTick::setConfig({33,NpcSimTick::Scope::Graph});
  const auto* field=world.findWayPoint("NW_BIGFARM_VORPOSTEN1_01_RIGHTFIELD_02");
  require(field!=nullptr,"far monster fixture needs the field");
  const auto groundAt=[&](const Tempest::Vec3& position) {
    const auto ground=world.physic()->landRay(position+Tempest::Vec3(0,300,0),800);
    require(ground.hasCol,"far monster fixture needs solid ground");
    return ground.v;
    };
  const auto spawn=groundAt(field->groundPos+Tempest::Vec3(0,0,900));
  const auto preyPoint=groundAt(spawn+Tempest::Vec3(450,0,0));
  moveBehaviorHero(gothic,field->groundPos+Tempest::Vec3(0,0,9000));
  session.setTimeMultiplyer(1.f);
  const auto tick=[&] {
    session.tick(8);
    session.updateAnimation(8);
    };
  const auto rayHits=[&](Npc& npc) {
    const auto center=npc.collosionCenter();
    const Tempest::Vec3 delta(0,0,100);
    return world.physic()->rayNpc(center-delta,center+delta,nullptr).npcHit==&npc;
    };

  const auto wolfType=world.script().findSymbolIndex("WOLF");
  const auto preyType=world.script().findSymbolIndex("BDT_1003_BANDIT_M");
  for(const bool damage : {false,true}) {
    auto* wolf=world.addNpc(wolfType,spawn);
    auto* prey=world.addNpc(preyType,preyPoint);
    require(wolf!=nullptr && prey!=nullptr,"far monster fixture spawns a wolf and prey");
    onlyBehaviorActors(world,wolf);
    prey->clearAiQueue();
    prey->clearState(true);
    prey->clearGoTo();
    prey->handle().attribute[ATR_HITPOINTSMAX]=2000;
    prey->handle().attribute[ATR_HITPOINTS]=2000;
    prey->handle().attribute[ATR_REGENERATEHP]=0;

    wolf->clearRoutine();
    wolf->clearState(true);
    wolf->clearAiQueue();
    wolf->clearGoTo();
    wolf->setTarget(nullptr);
    wolf->handle().start_aistate=0;
    wolf->setToFistMode();
    wolf->setAnim(Npc::Anim::Idle);
    wolf->setSimTierOverride(NpcSimTier::Simplified);
    wolf->setDirection(prey->position()-wolf->position());
    wolf->updateTransform();
    wolf->setPerceptionEnable(PERC_ASSESSENEMY,world.script().findFunction("B_MM_ASSESSENEMY").ptr);
    wolf->setPerceptionEnable(PERC_ASSESSDAMAGE,world.script().findFunction("B_MM_ASSESSDAMAGE").ptr);

    for(unsigned t=0;t<8;++t)
      tick();
    if(!damage) {
      // Regeneration makes every dispatched dt observable even when the
      // wolf does not move. Compare the wheel with the original cadence.
      auto& attr = wolf->handle().attribute;
      const auto oldHp = attr[ATR_HITPOINTS], oldMax = attr[ATR_HITPOINTSMAX], oldRegen = attr[ATR_REGENERATEHP];
      attr[ATR_HITPOINTSMAX] = 10000;
      attr[ATR_REGENERATEHP] = 1000;
      auto pending = wolf->pendingSimTick();
      const auto phase = NpcSimTick::phase(*wolf,33);
      for(const uint64_t dt:{7,8,0,16,7,8,24,8,25,30,33,34,50,7,8,8,7,8,8,7,8}) {
        const auto clock = world.npcCadenceClock();
        const auto hp = attr[ATR_HITPOINTS];
        require(NpcSimTick::qualifies(*wolf,NpcSimTick::Scope::Graph),"the cadence fixture stays eligible");
        session.tick(dt);
        session.updateAnimation(dt);
        require(world.npcCadenceClock()==clock+dt,"the fixture uses the requested step");
        pending += dt;
        const bool due = NpcSimTick::due(clock+dt,dt,pending,phase,33);
        require(wolf->pendingSimTick()==(due ? 0 : pending) &&
                attr[ATR_HITPOINTS]==hp+(due ? int32_t(pending) : 0),
                "the idle wheel dispatches on the original step with the original elapsed time");
        if(due)
          pending=0;
        }
      attr[ATR_HITPOINTS] = oldHp;
      attr[ATR_HITPOINTSMAX] = oldMax;
      attr[ATR_REGENERATEHP] = oldRegen;
      std::puts("Idle wolf cadence: zero, short, age-limited and long steps match native dispatch and elapsed time");
      }
    for(unsigned t=0;t<8 && wolf->pendingSimTick()!=8;++t)
      tick();
    require(wolf->processPolicy()==NpcProcessPolicy::AiFar2 && wolf->simTier()==NpcSimTier::Simplified &&
           wolf->weaponState()==WeaponState::Fist && !wolf->usesSimplifiedMovement() &&
           wolf->pendingSimTick()==8 && rayHits(*wolf),
           "an idle distant wolf defers its native tick and keeps its collision");
    const auto alertedAt=world.tickCount();
    const auto preyHp=prey->attribute(ATR_HITPOINTS);
    const auto wolfHp=wolf->attribute(ATR_HITPOINTS);
    const auto startDistance=wolf->qDistTo(*prey);
    if(damage) {
      wolf->takeDamage(*prey,nullptr,true);
      require(wolf->attribute(ATR_HITPOINTS)<wolfHp,"native damage reaches the waiting wolf");
      } else {
      require(wolf->perceptionProcess(*prey,prey,wolf->qDistTo(*prey),PERC_ASSESSENEMY),
             "the enemy perception reaches the wolf's native callback");
      }
    require((wolf->target()==prey || wolf->aiQueueSize()>0) && !NpcSimTick::qualifies(*wolf,NpcSimTick::Scope::Graph),
           "the wolf leaves the deferred cadence as soon as it acquires a target or queues a warning");
    tick();
    require(world.tickCount()==alertedAt+8 && wolf->pendingSimTick()==0 &&
            wolf->logicalBodyState().worldStep==16 && rayHits(*wolf),
           "the first native step after the alert consumes the time owed and keeps collision");

    uint64_t pursuit=0;
    bool attack=false;
    for(unsigned t=0;t<1500 && prey->attribute(ATR_HITPOINTS)==preyHp;++t) {
      tick();
      require(wolf->pendingSimTick()==0,"the engaged wolf ticks every frame");
      if(pursuit==0 && wolf->qDistTo(*prey)<startDistance*0.65f)
        pursuit=world.tickCount()-alertedAt;
      attack|=wolf->isAttackAnim();
      }
    require(pursuit>0 && attack && prey->attribute(ATR_HITPOINTS)<preyHp,
           "the awakened wolf pursues and lands a native first hit");
    std::printf("Idle wolf %s: native tick 8 ms, pursuit %llu ms, first hit %llu ms, damage %d, collision present\n",
                damage ? "damage" : "aggro", (unsigned long long)pursuit,
                (unsigned long long)(world.tickCount()-alertedAt), int(preyHp-prey->attribute(ATR_HITPOINTS)));
    world.removeNpc(*wolf);
    world.removeNpc(*prey);
    }

  const auto skeletonType = world.script().findSymbolIndex("SKELETON");
  const auto sword = world.script().findItemInstance("ITMW_1H_VLK_SWORD");
  const auto rest = world.script().findFunction("ZS_MM_RTN_REST");
  auto* skeleton = world.addNpc(skeletonType,field->groundPos);
  require(skeleton && sword && rest.isValid(),"armed rest fixture needs a skeleton, a sword and its native state");
  onlyBehaviorActors(world,skeleton);
  skeleton->clearRoutine();
  skeleton->clearState(true);
  skeleton->clearAiQueue();
  skeleton->clearGoTo();
  skeleton->setTarget(nullptr);
  skeleton->handle().start_aistate=0;
  skeleton->attachToPoint(field);
  skeleton->setToFightMode(*sword);
  skeleton->setAnim(Npc::Anim::Idle);
  skeleton->setSimTierOverride(NpcSimTier::Simplified);
  require(skeleton->startState(rest,field->name),"the skeleton enters stock rest");
  for(unsigned t=0;t<220 && !NpcDefinitionContract::monsterRestSleeping(*skeleton);++t)
    tick();
  require(skeleton->weaponState()==WeaponState::W1H && NpcDefinitionContract::monsterRestSleeping(*skeleton) &&
          skeleton->pendingSimTick()==0 && rayHits(*skeleton) &&
          skeleton->lockedWayPoint()==field,
          "armed rest sleeps at its original point with native collision");
  const auto nextLoop = NpcDefinitionContract::aiState(*skeleton).loopNextTime;
  for(unsigned t=0;t<4 && world.tickCount()+8<nextLoop;++t) {
    tick();
    require(NpcDefinitionContract::monsterRestSleeping(*skeleton) && skeleton->pendingSimTick()==0 &&
            NpcDefinitionContract::aiState(*skeleton).loopNextTime==nextLoop,
            "armed rest waits for its native routine deadline while the body ticks");
    }
  for(unsigned t=0;t<220 && NpcDefinitionContract::aiState(*skeleton).loopNextTime==nextLoop;++t)
    tick();
  require(NpcDefinitionContract::aiState(*skeleton).loopNextTime>nextLoop && skeleton->isInState(rest) &&
          skeleton->lockedWayPoint()==field && rayHits(*skeleton),
          "stock rest resumes on its native perception beat without losing the post or body");
  moveBehaviorHero(gothic,field->groundPos+Tempest::Vec3(200,0,0));
  tick();
  require(!NpcDefinitionContract::monsterRestSleeping(*skeleton) && rayHits(*skeleton) &&
          skeleton->lockedWayPoint()==field && skeleton->isStanding(),
          "the approaching hero wakes armed rest with the same standing body and point");
  std::puts("Armed monster rest: native deadline, point reservation, approaching hero and collision");
  world.removeNpc(*skeleton);

  auto* body = world.addNpc(preyType,spawn);
  require(body!=nullptr,"pose and ragdoll fixture needs a human skeleton");
  body->setSimTier(NpcSimTier::Full);
  body->updateTransform();
  Tempest::Vec3 head;
  require(body->hitZoneCenter(HitZones::Head,head),"the actor needs a native head hit zone");
  const Tempest::Vec3 offset(4000,0,0), ray(0,0,150);
  float hitAt = 0;
  require(body->rayHitZone(head-ray,head+ray,hitAt)!=HitZones::None,"the reference ray hits the original actor");
  body->setPosition(body->position()+offset);
  body->deferAnimation(8);
  const auto hit = body->rayHitZone(head+offset-ray,head+offset+ray,hitAt);
  require(!body->hasDeferredAnimation(),"a stray arrow catches up the actor's ordinary pose debt");
  require(hit!=HitZones::None,"a stray arrow reads the actor's pose after a same-tick relocation");

  require(!NpcDefinitionContract::refreshNearbyNpc(*body),"the actor starts outside the hero's near list");
  const auto close = world.player()->position()+Tempest::Vec3(500,0,0);
  require(NpcDefinitionContract::movePhysicalNpc(*body,close) &&
          NpcDefinitionContract::refreshNearbyNpc(*body) && body->processPolicy()==NpcProcessPolicy::AiNormal,
          "a ragdoll crossing the near boundary updates the index while the hero stands still");
  std::puts("NPC spatial updates: arrows read the recovered pose and ragdolls enter the near list");
  world.removeNpc(*body);
  NpcSimTick::setConfig(restore);
  }

void nativeNpcBehaviorContract(Gothic& gothic, const std::filesystem::path& caseDir) {
  using Contract=NpcDefinitionContract;
  constexpr std::string_view hilda="BAU_951_HILDA";
  constexpr std::string_view portal="S:FARM01_NW_CITY_BODENDIELEN_01";
  constexpr std::string_view farm="FARM01";
  constexpr std::string_view comment="PLAYER_LEFTROOMCOMMENT";
  constexpr std::string_view checkedGuild="C_NPCISBOTHEREDBYPLAYERROOMGUILD.PORTALGUILD";
  constexpr std::string_view fakeBandit="C_PLAYERISFAKEBANDIT.SLF";
  const auto& initial=*gothic.world();
  const auto* post=initial.findWayPoint("NW_FARM1_INHOUSE_02");
  const auto* otherRoom=initial.findWayPoint("NW_FARM1_INHOUSE_01");
  const auto* beyond=initial.findWayPoint("NW_FARM1_ENTRANCE_01");
  require(post && otherRoom && beyond,"quiet sound: Lobart's house needs two rooms and an exit");
  const auto postName=post->name;
  const auto at=post->groundPos, separate=otherRoom->groundPos, exit=beyond->groundPos;
  std::optional<Tempest::Vec3> inside;
  for(const auto offset:{Tempest::Vec3(-180,0,0),Tempest::Vec3(180,0,0),Tempest::Vec3(0,0,180),Tempest::Vec3(0,0,-180),
                         Tempest::Vec3(-180,0,180),Tempest::Vec3(-180,0,-180),Tempest::Vec3(180,0,180),Tempest::Vec3(180,0,-180)}) {
    const auto floor=initial.physic()->landRay(at+offset+Tempest::Vec3(0,40,0),120);
    const Tempest::Vec3 eyes(0,160,0);
    if(floor.sector && std::string_view(floor.sector)==portal && !initial.physic()->ray(at+eyes,floor.v+eyes).hasCol) {
      inside=floor.v;
      break;
      }
    }
  require(inside.has_value(),"quiet sound: the guest needs a line of sight and the same tagged floor");
  auto& script=gothic.world()->script();
  const auto stand=script.findFunction("ZS_STAND_WP");
  const auto observe=script.findFunction("ZS_OBSERVEPLAYER");
  const auto clear=script.findFunction("ZS_CLEARROOM");
  const auto portalCallback=script.findFunction("B_ASSESSPORTALCOLLISION");
  require(stand.isValid() && observe.isValid() && clear.isValid() && portalCallback.isValid(),
          "quiet sound: the stock stand, watch, clear and portal callback must exist");

  const auto prepare=[&](bool sameRoom) {
    auto& world=*gothic.world();
    auto* listener=liveNpc(world,hilda);
    auto& hero=*world.player();
    require(listener!=nullptr,"quiet sound: the listener must exist in the live world");
    onlyBehaviorActors(world,nullptr);
    listener->setInteraction(nullptr,true);
    listener->clearState(true);
    listener->clearAiQueue();
    listener->clearGoTo();
    listener->setTarget(nullptr);
    listener->clearRoutine();
    listener->addRoutine(gtime(0,0),gtime(23,59),uint32_t(stand.ptr),postName);
    listener->setAttitude(ATT_NEUTRAL);
    world.assignRoomToGuild(farm,int32_t(GIL_PUBLIC));
    moveBehaviorHero(gothic,sameRoom ? *inside : separate);
    listener->setPosition(at);
    listener->updateTransform();
    require(listener->startState(stand,postName),"quiet sound: listener must enter its normal activity");
    Contract::tickRoutine(*listener);
    for(size_t i=0;i<PERC_Count;++i)
      if(i!=PERC_ASSESSQUIETSOUND)
        listener->setPerceptionDisable(PercType(i));
    onlyBehaviorActors(world,listener,&hero);
    stepBehaviorUntil(gothic,120,[&] {
      return world.guildOfRoom(listener->portalName())==int32_t(GIL_PUBLIC) &&
             ((listener->portalName()==hero.portalName())==sameRoom) && listener->isAiQueueEmpty();
      },"quiet sound: the listener and source must settle in the specified rooms");
    listener->setDirection(hero.position()-listener->position());
    listener->updateTransform();
    listener->setStateTime(0);
    require(listener->isInState(stand) && listener->hasPerc(PERC_ASSESSQUIETSOUND) && listener->qDistTo(hero)<1000.f*1000.f,
            "quiet sound: the standing listener needs the callback and source in range");
    if(sameRoom)
      require(listener->canSeeNpc(hero,false),"quiet sound: the listener must see the source in the same room");
    return listener;
    };

  struct Site { bool sameRoom, privateRoom, friendly; int height; };
  for(const auto site:{Site{true,false,false,0},Site{false,false,false,0},Site{true,false,false,251},
                       Site{true,true,false,0},Site{true,true,true,0}}) {
    auto& world=*gothic.world();
    auto& hero=*world.player();
    auto& listener=*prepare(site.sameRoom);
    if(site.privateRoom) {
      world.assignRoomToGuild(farm,int32_t(listener.handle().guild));
      require(script.setGlobalIntSymbol(comment,1),"quiet sound: the private-room comment must be writable");
      }
    require(script.setGlobalIntSymbol(checkedGuild,-123),"quiet sound: the owner scratch field must be writable");
    Contract::alias(script,fakeBandit).reset();
    if(site.friendly) {
      listener.setAttitude(ATT_FRIENDLY);
      listener.aiPush(AiQueue::aiLookAtNpc(&hero));
      Contract::nextAiAction(listener);
      require(listener.canSeeSource(),"quiet sound: the friendly witness needs the actual source in view");
      listener.aiPush(AiQueue::aiWait(173));
      }
    if(!site.sameRoom) {
      listener.setAttitude(ATT_HOSTILE);
      listener.aiPush(AiQueue::aiStopLookAt());
      Contract::nextAiAction(listener);
      require(!listener.canSeeSource(),"quiet sound: the hostile listener cannot see the source in the other room");
      listener.aiPush(AiQueue::aiWait(173));
      }
    if(site.height!=0) {
      auto raised=hero.position();
      raised.y=std::nextafter(listener.position().y+float(site.height),std::numeric_limits<float>::infinity());
      moveBehaviorHero(gothic,raised);
      require(int32_t(std::abs(hero.position().y-listener.position().y))==site.height &&
              world.guildOfRoom(hero.portalName())==int32_t(GIL_PUBLIC),
              "quiet sound: the elevated source retains the indoor portal for the height gate");
      }

    ActivityCalls phases;
    require(listener.perceptionProcess(hero,&hero,listener.qDistTo(hero),PERC_ASSESSQUIETSOUND) &&
            phases.phase(listener,"B_ASSESSQUIETSOUND")==1,
            "quiet sound: the engine must dispatch exactly one quiet perception");
    const auto* queued=listener.aiQueueFront();
    if(site.height!=0) {
      require(queued==nullptr && listener.hasPerc(PERC_ASSESSQUIETSOUND) &&
              script.globalIntSymbol(checkedGuild)==-123 && !Contract::alias(script,fakeBandit),
              "quiet sound: height rejection leaves the activity and scratch fields untouched");
      } else if(site.friendly) {
      require(queued && queued->act==AI_Wait && queued->i0==173 && Contract::queue(listener).size()==1 &&
              listener.hasPerc(PERC_ASSESSQUIETSOUND) && script.globalIntSymbol(comment)==0,
              "quiet sound: the friendly private owner still clears the room comment and keeps waiting");
      } else {
      require(queued && queued->act==AI_StartState && queued->func==(site.privateRoom ? clear : observe) &&
              queued->target==&hero && queued->victim==&hero && queued->i0==1 &&
              Contract::queue(listener).size()==1 && !listener.hasPerc(PERC_ASSESSQUIETSOUND),
              "quiet sound: the room reaction must replace the queue with the exact guest transition");
      if(site.privateRoom)
        require(script.globalIntSymbol(checkedGuild)==int32_t(listener.handle().guild) &&
                script.globalIntSymbol(comment)==0,"quiet sound: the room check retains its owner and clears the comment");
      if(!site.sameRoom)
        require(Contract::alias(script,fakeBandit)==std::static_pointer_cast<NpcInstance>(listener.handlePtr()),
                "quiet sound: the hostile-source check writes the actual listener to its scratch slot");
      }
    }

  for(bool privateRoom:{false,true}) {
    auto& world=*gothic.world();
    auto& hero=*world.player();
    auto& listener=*prepare(true);
    if(privateRoom)
      world.assignRoomToGuild(farm,int32_t(listener.handle().guild));
    require(script.setGlobalIntSymbol(comment,1),"enter room: initialize the original comment field");
    listener.setPerceptionEnable(PERC_ASSESSENTERROOM,portalCallback.ptr);
    listener.aiPush(AiQueue::aiWait(173));
    const auto delivered=listener.perceptionProcess(hero,&hero,listener.qDistTo(hero),PERC_ASSESSENTERROOM);
    const auto* next=listener.aiQueueFront();
    require(delivered && next && next->act==AI_StartState && next->func==(privateRoom ? clear : observe) &&
            next->target==&hero && next->i0==1 && Contract::queue(listener).size()==1 &&
            !listener.hasPerc(PERC_ASSESSENTERROOM) && !listener.hasPerc(PERC_ASSESSQUIETSOUND) &&
            script.globalIntSymbol(comment)==(privateRoom ? 0 : 1),
            "enter room: the collision callback keeps the original public and private policies");
    }
  std::puts("Native quiet sound: same room, separate room, height, private, friendly and portal branches");

  {
    auto* listener=prepare(true);
    auto& world=*gothic.world();
    propertyCrimePerceptions(world,*listener,*world.player());
    listener=prepare(true);
    murderPerceptions(world,*listener,*world.player());
    crimeMemoryCases(world,*listener,*world.player());
    }

  auto* listener=prepare(true);
  auto& world=*gothic.world();
  auto& hero=*world.player();
  world.script().setSeed(73106);
  PhysicsNoise impact;
  impact.culprit=&hero;
  impact.position=hero.position();
  require(listener->hearPhysicsNoise(impact) && listener->isInState(observe) && listener->isStateStarted() &&
          listener->stateOther()==&hero && listener->hasPerc(PERC_ASSESSTHEFT) && listener->handle().aivar[68]==2,
          "physical sound: the real impact callback starts observation immediately with the stock seeded delay");
  listener->setStateTime(750);
  const auto pending=savedBehaviorQueue(world,*listener);
  require(!pending.empty(),"physical sound: observer must still have pending commands at save time");
  auto crimeFields=crimeCounterNames();
  for(std::string_view region:crimeRegions)
    crimeFields.emplace_back("ABSOLUTIONLEVEL_"+std::string(region));
  const auto fixtureCrimeValue=[](size_t index) { return int32_t(index)-(index<12 ? 12 : 11); };
  for(size_t i=0;i<crimeFields.size();++i)
    require(world.script().setGlobalIntSymbol(crimeFields[i],fixtureCrimeValue(i)),
            "crime save: all signed counters and absolution levels must be writable");
  Contract::alias(world.script(),"B_MEMORIZEPLAYERCRIME.SLF")=std::static_pointer_cast<NpcInstance>(listener->handlePtr());
  Contract::alias(world.script(),"B_MEMORIZEPLAYERCRIME.TAETER")=std::static_pointer_cast<NpcInstance>(hero.handlePtr());
  reloadNativeWorld(gothic,caseDir/"observing.sav");
  auto& restored=*gothic.world();
  listener=liveNpc(restored,hilda);
  auto& guest=*restored.player();
  require(listener && listener->isInState(observe) && listener->isStateStarted() && listener->stateOther()==&guest &&
          listener->stateTime()==750 && listener->handle().aivar[68]==2 && listener->hasPerc(PERC_ASSESSTHEFT) &&
          savedBehaviorQueue(restored,*listener)==pending,
          "physical sound: v57 restores the target, elapsed time, exact draw, perceptions and pending commands");
  for(size_t i=0;i<crimeFields.size();++i)
    require(restored.script().globalIntSymbol(crimeFields[i])==fixtureCrimeValue(i),
            ("crime save: wrong restored value for "+crimeFields[i]).c_str());
  require(Contract::alias(restored.script(),"B_MEMORIZEPLAYERCRIME.SLF")==std::static_pointer_cast<NpcInstance>(listener->handlePtr()) &&
          Contract::alias(restored.script(),"B_MEMORIZEPLAYERCRIME.TAETER")==std::static_pointer_cast<NpcInstance>(guest.handlePtr()),
          "crime save: reload reconnects the exact live witness and culprit");
  std::puts("Native crime save: signed regional counters, absolution and live witness aliases restored by v57");
  {
    ActivityCalls phases;
    onlyBehaviorActors(restored,listener,&guest);
    moveBehaviorHero(gothic,exit);
    stepBehaviorUntil(gothic,240,[&] { return listener->isInState(stand) && listener->isStateStarted(); },
                      "physical sound: the loaded observer returns to its original routine when the source leaves");
    require(phases.phase(*listener,"ZS_OBSERVEPLAYER")==0 && phases.phase(*listener,"ZS_OBSERVEPLAYER_LOOP")>0 &&
            phases.phase(*listener,"ZS_OBSERVEPLAYER_END")==1,
            "physical sound: the loaded episode continues through Loop and End exactly once without repeating Init");
    }
  std::puts("Native physical sound: v57 restored observation and the original routine resumed");

  const auto hasWarning=[](Npc& owner,Npc& intruder,std::string_view line) {
    auto queue=Contract::queue(owner);
    while(queue.size()!=0) {
      const auto action=queue.pop();
      if(action.act==AI_OutputSvm && action.target==&intruder && action.s0==line)
        return true;
      }
    return false;
    };
  for(bool hostile:{true,false}) {
    listener=prepare(true);
    auto& roomWorld=*gothic.world();
    auto& roomScript=roomWorld.script();
    auto& intruder=*roomWorld.player();
    const auto oldGuild=listener->handle().guild;
    if(hostile)
      listener->handle().guild=GIL_MIL;
    roomWorld.assignRoomToGuild(farm,int32_t(listener->handle().guild));
    listener->handle().aivar[1]=roomScript.intConstant("CRIME_NONE").value();
    listener->setPerceptionEnable(PERC_ASSESSENTERROOM,portalCallback.ptr);
    require(listener->perceptionProcess(intruder,&intruder,listener->qDistTo(intruder),PERC_ASSESSENTERROOM) &&
            listener->aiQueueFront() && listener->aiQueueFront()->func==clear,
            "clear room: a matching private portal must start the reaction");
    Contract::nextAiAction(*listener);
    Contract::tickRoutine(*listener);
    require(listener->isInState(clear) && listener->stateOther()==&intruder && listener->isStateStarted() &&
            listener->handle().aivar[19]==1 && listener->hasPerc(PERC_ASSESSENTERROOM) &&
            listener->hasPerc(PERC_MOVEMOB) && listener->hasPerc(PERC_ASSESSTALK),
            "clear room: Init preserves the guest and installs the stock callbacks");
    const auto clearLoop=roomScript.findFunction("ZS_CLEARROOM_LOOP");
    const auto loop=[&] { return roomScript.invokeState(listener,&intruder,&intruder,clearLoop); };
    listener->clearAiQueue();
    listener->setStateTime(2600);
    require(loop()==LOOP_CONTINUE && listener->handle().aivar[19]==0 && listener->stateTime()==0 &&
            hasWarning(*listener,intruder,hostile ? "$GETOUTOFHERE" : "$WHYAREYOUINHERE"),
            "clear room: the first Loop warns the actual guest and resets the timer");
    if(hostile) {
      listener->clearAiQueue();
      listener->setStateTime(5999);
      require(loop()==LOOP_CONTINUE && listener->isAiQueueEmpty() && listener->stateTime()==5999,
              "clear room: the guest still has time to leave at 5999 ms");
      listener->setStateTime(6000);
      require(loop()==LOOP_END && listener->aiQueueFront() && listener->aiQueueFront()->act==AI_StartState &&
              listener->aiQueueFront()->func==roomScript.findFunction("ZS_ATTACK") &&
              listener->aiQueueFront()->target==&intruder &&
              listener->handle().aivar[9]==roomScript.intConstant("AR_CLEARROOM").value(),
              "clear room: at 6000 ms the exact escalation targets the guest");
      listener->handle().guild=oldGuild;
      } else {
      listener->setStateTime(1250);
      const auto roomQueue=savedBehaviorQueue(roomWorld,*listener);
      reloadNativeWorld(gothic,caseDir/"clear-room.sav");
      auto& loaded=*gothic.world();
      listener=liveNpc(loaded,hilda);
      auto& guest=*loaded.player();
      require(listener && listener->isInState(clear) && listener->isStateStarted() && listener->stateOther()==&guest &&
              listener->stateTime()==1250 && listener->handle().aivar[19]==0 && listener->hasPerc(PERC_ASSESSENTERROOM) &&
              loaded.guildOfRoom(guest.portalName())==int32_t(listener->handle().guild) &&
              savedBehaviorQueue(loaded,*listener)==roomQueue,
              "clear room: v57 restores the private portal, guest, clock and pending warning");
      ActivityCalls phases;
      onlyBehaviorActors(loaded,&guest);
      moveBehaviorHero(gothic,separate);
      stepBehaviorUntil(gothic,120,[&] { return loaded.guildOfRoom(guest.portalName())!=int32_t(listener->handle().guild); },
                        "clear room: the loaded guest must make a real departure from the private floor");
      onlyBehaviorActors(loaded,listener,&guest);
      require(listener->perceptionProcess(guest,&guest,listener->qDistTo(guest),PERC_ASSESSENTERROOM) &&
              hasWarning(*listener,guest,"$WISEMOVE") && !hasWarning(*listener,guest,"$WHYAREYOUINHERE"),
              "clear room: the restored callback replaces the warning after departure");
      auto commands=Contract::queue(*listener);
      require(commands.size()==3 && commands.pop().act==AI_StandUp && commands.pop().act==AI_OutputSvm &&
              commands.pop().act==AI_ContinueRoutine,
              "clear room: departure preserves the stand, speech and routine-resume order");
      stepBehaviorUntil(gothic,360,[&] { return listener->isInState(stand) && listener->isStateStarted(); },
                        "clear room: after departure the loaded owner returns to its original standing activity");
      require(phases.phase(*listener,"ZS_CLEARROOM")==0 && phases.phase(*listener,"ZS_CLEARROOM_END")==1,
              "clear room: the loaded reaction ends exactly once without replaying Init");
      }
    }
  std::puts("Native private-room reaction: exact escalation timer and v57 continuation on departure");
  savedHumanRoutines(gothic,caseDir/"routines.sav");
  farIdleMonster(gothic);
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

Npc& addSpellActor(World& world, std::string_view archetype, std::string_view point) {
  const auto id=world.script().storyNpcId(archetype);
  const auto* at=world.findWayPoint(point);
  require(id.has_value() && at!=nullptr,"spell contract needs a stock archetype and waypoint");
  auto* actor=world.addNpc(*id,at->position());
  require(actor!=nullptr,"spell contract could not add its NPC");
  return *actor;
  }

void spellRequire(bool condition, std::string_view name, std::string_view message) {
  if(!condition)
    throw std::runtime_error(std::string(name)+": "+std::string(message));
  }

Item& readyContractSpell(Npc& caster, std::string_view name) {
  const auto id=caster.world().script().findSymbolIndex(name);
  spellRequire(id!=size_t(-1),name,"missing spell item archetype");
  auto* item=caster.addItem(id,1);
  spellRequire(item && caster.drawSpell(item->spellId()) && caster.activeWeapon()==item,
               name,"the actual rune or scroll must be active");
  return *item;
  }

int32_t spellValue(const GameScript& script, std::string_view slot) {
  const auto value=script.globalIntSymbol(slot);
  spellRequire(value.has_value(),slot,"missing integer in the native catalog");
  return *value;
  }

void spellValue(GameScript& script, std::string_view slot, int32_t value) {
  spellRequire(script.setGlobalIntSymbol(slot,value),slot,"cannot set the typed spell variable");
  }

auto spellActorScope(GameScript& script) {
  return std::array{NpcDefinitionContract::alias(script,"SELF").get(),
                    NpcDefinitionContract::alias(script,"OTHER").get(),
                    NpcDefinitionContract::alias(script,"VICTIM").get(),
                    NpcDefinitionContract::alias(script,"ITEM").get()};
  }

int verifyManaDispatch(GameScript& script, Npc& caster, Npc* target, std::string_view itemName, Item& item,
                       int32_t invested, std::string_view local={}, bool release=false) {
  const auto amount=release ? "SPELL_PROCESSMANA_RELEASE.MANAINVESTED" : "SPELL_PROCESSMANA.MANAINVESTED";
  const auto active=release ? "SPELL_PROCESSMANA_RELEASE.ACTIVESPELL" : "SPELL_PROCESSMANA.ACTIVESPELL";
  spellValue(script,amount,-1);
  spellValue(script,active,-1);
  if(!local.empty())
    spellValue(script,local,-1);
  NpcDefinitionContract::alias(script,itemName).reset();
  const auto status=release ? script.invokeManaRelease(caster,target,invested) : script.invokeMana(caster,target,invested);
  spellRequire(spellValue(script,amount)==invested && spellValue(script,active)==item.spellId() &&
               (local.empty() || spellValue(script,local)==invested) &&
               NpcDefinitionContract::alias(script,itemName).get()==static_cast<ItemInstance*>(item.handlePtr().get()),
               itemName,"mana dispatch must publish the exact item and save the selected spell and amount");
  return status;
  }

void stockSpellAccountingNative(World& world) {
  auto& script=world.script();
  auto& target=*world.player();
  constexpr auto point="NW_FARM4_BALTHASAR";
  static constexpr struct Cast {
    std::string_view item, local, level;
    int mana, remaining, healing=0, step=0;
    bool fear=false;
    } cases[] = {
    {"ITSC_ICEBOLT","SPELL_LOGIC_ICEBOLT.MANAINVESTED",{},3,-2},
    {"ITRU_ICEBOLT","SPELL_LOGIC_ICEBOLT.MANAINVESTED",{},3,-7},
    {"ITSC_ZAP","SPELL_LOGIC_ZAP.MANAINVESTED","SPELL_CAST_ZAP.SPELLLEVEL",3,-2},
    {"ITSC_SLEEP",{},{},3,3},
    {"ITSC_CHARGEFIREBALL","SPELL_LOGIC_CHARGEFIREBALL.MANAINVESTED","SPELL_CAST_CHARGEFIREBALL.SPELLLEVEL",3,0,0,40},
    {"ITRU_WINDFIST","SPELL_LOGIC_WINDFIST.MANAINVESTED","SPELL_CAST_WINDFIST.SPELLLEVEL",80,60,0,20},
    {"ITSC_FEAR",{},{},3,-2,0,0,true},
    {"ITRU_SKULL",{},{},70,0},
    {"ITSC_MEDIUMHEAL",{},{},3,-2,400},
    {"ITRU_PALFULLHEAL",{},{},3,-47,800},
    };
  for(const auto& row:cases) {
    auto& caster=addSpellActor(world,"PC_HERO",point);
    auto& item=readyContractSpell(caster,row.item);
    auto& mana=caster.handle().attribute[ATR_MANA];
    auto& level=caster.handle().aivar[88];
    const auto scope=spellActorScope(script);
    if(row.step!=0) {
      const int step=row.step;
      const struct Charge { int mana, invested, level, manaAfter, levelAfter; SpellCode status; } charges[] = {
        {step-1,step+1,2,step-1,2,SPL_DONTINVEST},
        {step,step,4,step,1,SPL_STATUS_CANINVEST_NO_MANADEC},
        {step,step+1,1,0,2,SPL_NEXTLEVEL},
        {step,3*step,3,step,3,SPL_STATUS_CANINVEST_NO_MANADEC},
        {step,3*step+1,3,0,4,SPL_NEXTLEVEL},
        {step,3*step+1,4,step,4,SPL_DONTINVEST},
        };
      for(const auto& charge:charges) {
        mana=charge.mana;
        level=charge.level;
        const auto status=verifyManaDispatch(script,caster,&target,row.item,item,charge.invested,row.local);
        spellRequire(status==charge.status && mana==charge.manaAfter && level==charge.levelAfter,
                     row.item,"charge has the wrong status, mana debit or level at a threshold");
        }
      spellRequire(verifyManaDispatch(script,caster,&target,row.item,item,27,{},true)==SPL_SENDCAST,row.item,
                   "releasing a charged spell must permit the cast");
      }
    else if(!row.local.empty()) {
      const int cost=row.mana-row.remaining;
      for(const int available:{cost-1,cost}) {
        mana=available;
        const auto status=verifyManaDispatch(script,caster,&target,row.item,item,17,row.local);
        spellRequire(status==(available==cost ? SPL_SENDCAST : SPL_SENDSTOP) && mana==available,
                     row.item,"admission must check mana without spending it");
        }
      }
    mana=row.mana;
    caster.handle().attribute[ATR_HITPOINTSMAX]=1000;
    caster.handle().attribute[ATR_HITPOINTS]=100;
    caster.handle().aivar[20]=9;
    caster.clearAiQueue();
    if(!row.level.empty())
      spellValue(script,row.level,71);
    const auto targetHp=target.attribute(ATR_HITPOINTS);
    const auto targetQueue=activityQueue(target);
    if(row.healing!=0)
      NpcDefinitionContract::alias(script,row.item).reset();
    script.invokeSpell(caster,&target,item);
    spellRequire(mana==row.remaining && caster.attribute(ATR_HITPOINTS)==100+row.healing &&
                 caster.handle().aivar[20]==(row.healing!=0 ? 9 : 10) &&
                 (row.level.empty() || spellValue(script,row.level)==0),
                 row.item,"cast must preserve the stock mana, health, selection and level accounting");
    if(row.healing!=0)
      spellRequire(NpcDefinitionContract::alias(script,row.item).get()==static_cast<ItemInstance*>(item.handlePtr().get()),
                   row.item,"healing must publish the spell item");
    spellRequire(target.attribute(ATR_HITPOINTS)==targetHp && activityQueue(target)==targetQueue &&
                 spellActorScope(script)==scope,row.item,"cast must preserve the target and caller context");
    if(row.fear) {
      const auto* action=caster.aiQueueFront();
      require(action && action->act==AI_SetNpcsToState && action->func==script.findFunction("ZS_MAGICFLEE") &&
              action->i0==1000 && NpcDefinitionContract::queue(caster).size()==1,
              "Fear must queue exactly one area state with the stock radius");
      }
    else
      spellRequire(caster.isAiQueueEmpty(),row.item,"numerical casts should leave the queue empty");
    world.removeNpc(caster);
    }
  std::puts("Native spells: charge, release, mana exhaustion, healing and cast accounting");
  }

void stockSpecialManaNative(World& world) {
  auto& script=world.script();
  constexpr auto point="NW_FARM4_BALTHASAR";
  auto& target=addSpellActor(world,"BAU_951_HILDA",point);
  target.handle().guild=GIL_OUT;
  target.handle().attribute[ATR_HITPOINTS]=100;
  static constexpr struct {
    std::string_view item, local;
    int mana, invested, remaining;
    SpellCode status;
    } cases[] = {
    {"ITSC_SLEEP","SPELL_LOGIC_SLEEP.MANAINVESTED",5,17,0,SPL_SENDCAST},
    {"ITSC_CHARM","SPELL_LOGIC_CHARM.MANAINVESTED",5,17,5,SPL_SENDCAST},
    {"ITRU_SWARM","SPELL_LOGIC_SWARM.MANAINVESTED",20,17,0,SPL_SENDCAST},
    {"ITRU_SUCKENERGY","SPELL_LOGIC_SUCKENERGY.MANAINVESTED",0,0,0,SPL_RECEIVEINVEST},
    {"ITRU_SUCKENERGY","SPELL_LOGIC_SUCKENERGY.MANAINVESTED",29,1,29,SPL_SENDSTOP},
    {"ITRU_SUCKENERGY","SPELL_LOGIC_SUCKENERGY.MANAINVESTED",30,1,30,SPL_SENDCAST},
    };
  for(const auto& row:cases) {
    auto& caster=addSpellActor(world,"PC_HERO",point);
    auto& item=readyContractSpell(caster,row.item);
    target.clearAiQueue();
    if(row.item=="ITSC_SLEEP")
      target.aiPush(AiQueue::aiWait(1));
    target.handle().aivar[1]=spellValue(script,"CRIME_ATTACK");
    target.handle().aivar[0]=19;
    spellValue(script,"MIS_IGNAZ_CHARM",spellValue(script,"LOG_RUNNING"));
    spellValue(script,"CHARM_TEST",0);
    caster.handle().attribute[ATR_MANA]=row.mana;
    const auto scope=spellActorScope(script);
    const auto result=verifyManaDispatch(script,caster,&target,row.item,item,row.invested,row.local);
    spellRequire(result==row.status && caster.attribute(ATR_MANA)==row.remaining &&
                 spellActorScope(script)==scope,row.item,"special mana stage returned the wrong result or cost");
    if(row.item=="ITSC_SLEEP") {
      const auto* action=target.aiQueueFront();
      require(action && action->act==AI_StartState && action->func==script.findFunction("ZS_MAGICSLEEP") &&
              action->target==&target && action->i0==0 && action->s0.empty() &&
              NpcDefinitionContract::queue(target).size()==1,
              "Sleep must replace the previous action with its exact stock state");
      }
    if(row.item=="ITSC_CHARM")
      require(target.handle().aivar[1]==0 && target.handle().aivar[0]==0 &&
              spellValue(script,"CHARM_TEST")==1 && target.isAiQueueEmpty(),
              "Charm must clear the crime and fight while updating Ignaz's quest");
    world.removeNpc(caster);
    }
  world.removeNpc(target);

  for(const auto& [itemName,local,form]:{std::array{"ITSC_TRFSHEEP","SPELL_LOGIC_TRFSHEEP.MANAINVESTED","SHEEP"},
                                        std::array{"ITSC_TRFDRAGONSNAPPER","SPELL_LOGIC_TRFDRAGONSNAPPER.MANAINVESTED","DRAGONSNAPPER"}}) {
    auto& caster=addSpellActor(world,"PC_HERO",point);
    auto& item=readyContractSpell(caster,itemName);
    const auto formId=script.storyNpcId(form);
    require(formId.has_value(),"Transform needs the original NPC form");
    for(const int mana:{4,5,10}) {
      caster.handle().attribute[ATR_MANA]=mana;
      caster.setActiveSpellInfo(71);
      const auto status=verifyManaDispatch(script,caster,nullptr,itemName,item,17,local);
      const auto allowed=mana>=5;
      spellRequire(status==(allowed ? SPL_SENDCAST : SPL_SENDSTOP) &&
                   caster.attribute(ATR_MANA)==(allowed ? mana-10 : mana) &&
                   NpcDefinitionContract::activeSpellInfo(caster)==(allowed ? int32_t(*formId) : 71),
                   itemName,"Transform must retain the scroll threshold, full debit and form");
      }
    world.removeNpc(caster);
    }
  std::puts("Native spells: Sleep, Charm, Swarm, Suck Energy and transformation mana stages");
  }

void stockSpellSummonsNative(World& world) {
  auto& script=world.script();
  auto& hero=*world.player();
  constexpr auto point="NW_FARM4_BALTHASAR";
  static constexpr struct {
    std::string_view item, creature;
    int cost, selection;
    uint32_t count;
    float lifetime;
    bool player;
    } cases[] = {
    {"ITSC_SUMWOLF","SUMMONED_WOLF",5,1,1,500.f,true},
    {"ITRU_SUMSKEL","SKELETON",60,1,1,500.f,false},
    {"ITRU_SUMMONGUARDIAN","STONEGUARDIAN",60,2,1,500.f,false},
    {"ITSC_ARMYOFDARKNESS","SUMMONED_SKELETON",5,1,6,800.f,true},
    };
  for(const auto& row:cases) {
    auto& caster=addSpellActor(world,"PC_HERO",point);
    caster.setProcessPolicy(row.player ? NpcProcessPolicy::Player : NpcProcessPolicy::AiNormal);
    spellRequire(caster.isPlayer()==row.player,row.item,"the caster has the wrong engine role");
    auto& item=readyContractSpell(caster,row.item);
    const auto base=world.npcCount();
    const auto creatureId=script.storyNpcId(row.creature);
    require(creatureId.has_value(),"the original creature must resolve in the native catalog");
    auto& messages=NpcDefinitionContract::passivePerceptions(world);
    const auto messageCount=messages.size();
    const auto takeSpawns=[&] {
      spellRequire(world.npcCount()==base+row.count,row.item,"wrong number of live summons");
      std::vector<Tempest::Vec3> positions;
      for(uint32_t i=0;i<row.count;++i) {
        auto* npc=world.npcById(base);
        spellRequire(npc && npc->instanceSymbol()==*creatureId,row.item,"wrong creature in the stock spawn order");
        positions.push_back(npc->position());
        world.removeNpc(*npc);
        }
      messages.resize(messageCount);
      return positions;
      };
    NpcDefinitionContract::spawnNpcRange(script,caster,int(*creatureId),int(row.count),row.lifetime);
    const auto expected=takeSpawns();
    caster.handle().attribute[ATR_MANA]=3;
    caster.handle().aivar[20]=9;
    spellValue(script,"SPELL_CAST_ARMYOFDARKNESS.SPELLLEVEL",71);
    const auto scope=spellActorScope(script);
    script.invokeSpell(caster,row.player ? nullptr : &hero,item);
    spellRequire(world.npcCount()==base+row.count,row.item,
                 "summon count is "+std::to_string(int64_t(world.npcCount())-base)+", expected "+std::to_string(row.count));
    spellRequire(caster.attribute(ATR_MANA)==3-row.cost,row.item,
                 "summon leaves "+std::to_string(caster.attribute(ATR_MANA))+" mana, expected "+std::to_string(3-row.cost));
    spellRequire(caster.handle().aivar[20]==9+row.selection,row.item,
                 "summon selection is "+std::to_string(caster.handle().aivar[20])+", expected "+std::to_string(9+row.selection));
    spellRequire(spellValue(script,"SPELL_CAST_ARMYOFDARKNESS.SPELLLEVEL")== (row.count==6 ? 0 : 71),row.item,
                 "summon changed the saved Army of Darkness level unexpectedly");
    const auto after=spellActorScope(script);
    spellRequire(after[0]==scope[0] && after[1]==scope[1],row.item,"summon did not restore its caster and target aliases");
    spellRequire(takeSpawns()==expected,row.item,"the cast did not place each creature in stock spawn order");
    world.removeNpc(caster);
    }
  std::puts("Native spells: hero and NPC summons, including six ordered Army of Darkness spawns");
  }

void stockShrinkCastNative(World& world) {
  auto& script=world.script();
  constexpr auto point="NW_FARM4_BALTHASAR";
  auto& target=addSpellActor(world,"SHEEP",point);
  const auto state=script.findFunction("ZS_MAGICSHRINK");
  require(state.isValid(),"missing the shrink state");
  const struct {
    int32_t guild, stage, species;
    NpcFlag flags;
    bool eligible;
    } cases[] = {
    {GIL_SHEEP,0,0,NpcFlag::NONE,true},
    {GIL_SHEEP,0,0,NpcFlag::IMMORTAL|NpcFlag::FRIENDS,true},
    {GIL_SHEEP,0,0,NpcFlag::IMMORTAL,false},
    {GIL_SHEEP,1,0,NpcFlag::NONE,false},
    {GIL_SHEEP,0,42,NpcFlag::NONE,false},
    {GIL_SKELETON,0,0,NpcFlag::NONE,false},
    {GIL_BAU,0,0,NpcFlag::NONE,false},
    };
  for(const auto [itemName,cost]:{std::pair("ITSC_SHRINK",5),std::pair("ITRU_SHRINK",300)}) {
    auto& caster=addSpellActor(world,"PC_HERO",point);
    auto& item=readyContractSpell(caster,itemName);
    auto& mana=caster.handle().attribute[ATR_MANA];
    const auto scope=spellActorScope(script);
    mana=cost-1;
    require(verifyManaDispatch(script,caster,&target,itemName,item,17,"SPELL_LOGIC_SHRINK.MANAINVESTED")==SPL_SENDSTOP && mana==cost-1,
            "Shrink must deny one mana below its cost");
    mana=cost;
    require(verifyManaDispatch(script,caster,&target,itemName,item,18,"SPELL_LOGIC_SHRINK.MANAINVESTED")==SPL_SENDCAST && mana==cost,
            "Shrink must admit the exact cost without spending it");
    require(verifyManaDispatch(script,caster,&target,itemName,item,19,{},true)==SPL_SENDSTOP,"Shrink must stop on release");
    for(const auto& row:cases) {
      target.handle().guild=row.guild;
      target.handle().aivar[42]=row.stage;
      target.handle().aivar[43]=row.species;
      target.handle().flags=row.flags;
      target.clearAiQueue();
      target.aiPush(AiQueue::aiWait(912));
      for(int i=1;i<PERC_Count;++i)
        target.setPerceptionEnable(PercType(i),state.ptr);
      NpcDefinitionContract::alias(script,"C_NPCISUNDEAD.SLF").reset();
      NpcDefinitionContract::alias(script,"B_CLEARPERCEPTIONS.SLF").reset();
      mana=cost+1;
      const auto selection=caster.handle().aivar[20];
      script.invokeSpell(caster,&target,item);
      const auto* action=target.aiQueueFront();
      spellRequire(action && NpcDefinitionContract::queue(target).size()==1 &&
                   (row.eligible ? action->act==AI_StartState && action->func==state &&
                                   action->i0==0 && action->target==&target :
                                   action->act==AI_Wait && action->i0==912),
                   itemName,"Shrink must replace the queue only for an eligible target");
      spellRequire(mana==1 && caster.handle().aivar[20]==selection+1 &&
                   NpcDefinitionContract::alias(script,"C_NPCISUNDEAD.SLF").get()==static_cast<NpcInstance*>(target.handlePtr().get()) &&
                   NpcDefinitionContract::alias(script,"B_CLEARPERCEPTIONS.SLF").get()==
                       (row.eligible ? static_cast<NpcInstance*>(target.handlePtr().get()) : nullptr) &&
                   spellActorScope(script)==scope,
                   itemName,"Shrink must debit mana, select once and keep the exact helper actor slots");
      require(target.hasPerc(PERC_ASSESSITEM) && target.hasPerc(PERC_ASSESSMAGIC)!=row.eligible,
              "Shrink must clear combat subscriptions only on an eligible target");
      }
    world.removeNpc(caster);
    }
  world.removeNpc(target);
  std::puts("Native spells: Shrink eligibility, scroll/rune cost, actor slots and queued state");
  }

void stockSpellCollisionNative(World& world) {
  auto& script=world.script();
  auto& hero=*world.player();
  auto& target=addSpellActor(world,"SHEEP","NW_FARM4_BALTHASAR");
  require(!target.isDown(),"the spell collision fixture needs a living target");
  const auto originalHeroGuild=hero.handle().guild;
  const auto check=[&](Npc& victim, Npc& shooter, std::string_view name, CollideMask expected) {
    const auto saved=spellActorScope(script);
    const auto spell=spellValue(script,name);
    spellRequire(script.canNpcCollideWithSpell(victim,&shooter,spell)==expected &&
                 spellValue(script,"C_CANNPCCOLLIDEWITHSPELL.SPELLTYPE")==spell &&
                 spellActorScope(script)==saved,name,"collision mask, saved spell or scope changed");
    };
  target.handle().guild=GIL_FIREGOLEM;
  target.handle().aivar[43]=0;
  check(target,hero,"SPL_ICELANCE",COLL_APPLYDOUBLEDAMAGE);
  require(NpcDefinitionContract::alias(script,"C_NPCISDOWN.SLF").get()==static_cast<NpcInstance*>(target.handlePtr().get()) &&
          NpcDefinitionContract::alias(script,"C_BODYSTATECONTAINS.SLF").get()==static_cast<NpcInstance*>(target.handlePtr().get()) &&
          spellValue(script,"C_BODYSTATECONTAINS.BODYSTATE")==BS_SWIM,
          "spell collision must keep the exact actor and swimming predicate arguments");
  check(target,hero,"SPL_FIREBOLT",COLL_APPLYHALVEDAMAGE);
  check(target,hero,"SPL_ZAP",CollideMask(COLL_APPLYDAMAGE | COLL_DONTKILL));
  target.handle().guild=GIL_SKELETON;
  target.handle().attribute[ATR_HITPOINTSMAX]=1000;
  check(target,hero,"SPL_DESTROYUNDEAD",COLL_DOEVERYTHING);
  target.handle().attribute[ATR_HITPOINTSMAX]=1001;
  check(target,hero,"SPL_DESTROYUNDEAD",COLL_DONOTHING);
  require(NpcDefinitionContract::alias(script,"C_NPCISUNDEAD.SLF").get()==static_cast<NpcInstance*>(target.handlePtr().get()),
          "spell collision must save the rejected undead target's identity");
  target.handle().guild=GIL_DMT;
  hero.handle().guild=GIL_DMT;
  check(target,hero,"SPL_FIRESTORM",COLL_DONOTHING);
  hero.handle().guild=GIL_NONE;
  check(hero,target,"SPL_LIGHTNINGFLASH",COLL_APPLYHALVEDAMAGE);
  hero.handle().guild=originalHeroGuild;
  world.removeNpc(target);
  std::puts("Native spells: collision masks and exact saved predicate arguments");
  }

void stockMagicPerceptionNative(World& world) {
  auto& script=world.script();
  auto& caster=*world.player();
  auto& victim=addSpellActor(world,"VLK_408_JORA","NW_CITY_PATH_HABOUR_03");
  const auto callback=script.findFunction("B_ASSESSMAGIC");
  require(callback.isValid(),"magic perception must resolve the stock callback");
  static constexpr struct {
    std::string_view spell, state;
    int32_t guild;
    bool clear, fear=false;
    } cases[] = {
    {"SPL_WHIRLWIND","ZS_WHIRLWIND",GIL_VLK,true},
    {"SPL_SUCKENERGY","ZS_SUCKENERGY",GIL_VLK,true},
    {"SPL_GREENTENTACLE","ZS_GREENTENTACLE",GIL_VLK,true},
    {"SPL_SWARM","ZS_SWARM",GIL_VLK,true},
    {"SPL_ICEWAVE","ZS_MAGICFREEZE",GIL_VLK,true},
    {"SPL_ICECUBE","ZS_MAGICFREEZE",GIL_VLK,true},
    {"SPL_CHARGEZAP","ZS_SHORTZAPPED",GIL_VLK,true},
    {"SPL_FEAR","ZS_MAGICFLEE",GIL_VLK,true,true},
    {"SPL_FEAR","ZS_MM_FLEE",GIL_WOLF,true,true},
    {"SPL_FEAR",{},GIL_PAL,true,true},
    {"SPL_FEAR",{},GIL_KDF,true,true},
    {"SPL_FEAR",{},GIL_TROLL,true,true},
    {"SPL_FEAR",{},GIL_SEPERATOR_HUM,true,true},
    {"SPL_FIRERAIN","ZS_MAGICBURNSHORT",GIL_VLK,false},
    {"SPL_FIREBOLT",{},GIL_VLK,false},
    {"SPL_PALLIGHT",{},GIL_VLK,false},
    };
  constexpr std::array cleared={
    PERC_ASSESSPLAYER,PERC_ASSESSENEMY,PERC_ASSESSBODY,PERC_ASSESSMAGIC,PERC_ASSESSDAMAGE,PERC_ASSESSMURDER,
    PERC_ASSESSTHEFT,PERC_ASSESSUSEMOB,PERC_ASSESSENTERROOM,PERC_ASSESSTHREAT,PERC_DRAWWEAPON,PERC_ASSESSFIGHTSOUND,
    PERC_ASSESSQUIETSOUND,PERC_ASSESSWARN,PERC_ASSESSTALK,PERC_MOVEMOB,PERC_ASSESSOTHERSDAMAGE,
    PERC_ASSESSSTOPMAGIC,PERC_ASSESSSURPRISE};
  for(const auto& row:cases) {
    const auto spell=spellValue(script,row.spell);
    victim.handle().guild=row.guild;
    NpcDefinitionContract::lastHitSpell(victim)=spell;
    victim.setTarget(nullptr);
    victim.clearAiQueue();
    victim.aiPush(AiQueue::aiWait(713));
    for(int i=1;i<PERC_Count;++i)
      victim.setPerceptionEnable(PercType(i),callback.ptr);
    NpcDefinitionContract::alias(script,"B_CLEARPERCEPTIONS.SLF").reset();
    const auto scope=spellActorScope(script);
    auto& messages=NpcDefinitionContract::passivePerceptions(world);
    const auto count=messages.size();
    require(victim.perceptionProcess(caster,nullptr,0,PERC_ASSESSMAGIC),
            "magic perception must dispatch through the NPC");
    const bool hostile=script.spellDesc(spell).spell_type==SPELL_BAD;
    spellRequire(messages.size()==count+size_t(hostile) && (!hostile ||
                 (messages.back().what==PERC_ASSESSFIGHTSOUND && messages.back().self==&victim &&
                  messages.back().victim==&victim && messages.back().other==&caster)),
                 row.spell,"magic perception must emit one fight sound with the actual actors for a hostile spell");
    messages.resize(count);
    spellRequire(victim.target()==(row.fear ? &caster : nullptr) &&
                 NpcDefinitionContract::alias(script,"B_CLEARPERCEPTIONS.SLF").get()==(row.clear ? static_cast<NpcInstance*>(victim.handlePtr().get()) : nullptr) &&
                 spellActorScope(script)==scope,row.spell,"magic perception must preserve the fear target and actor context");
    const auto* action=victim.aiQueueFront();
    if(!row.state.empty())
      spellRequire(action && action->act==AI_StartState && action->func==script.findFunction(row.state) &&
                   action->i0==0 && action->target==&caster && action->s0.empty() &&
                   NpcDefinitionContract::queue(victim).size()==1,row.spell,"wrong queued reaction");
    else if(row.fear)
      spellRequire(action==nullptr,row.spell,"an immune target must discard its queue without starting a state");
    else
      spellRequire(action && action->act==AI_Wait && action->i0==713 &&
                   NpcDefinitionContract::queue(victim).size()==1,row.spell,"an unrelated spell must preserve the queue");
    for(int i=1;i<PERC_Count;++i) {
      const auto expected=!row.clear || std::ranges::find(cleared,PercType(i))==cleared.end();
      spellRequire(victim.hasPerc(PercType(i))==expected,row.spell,"the wrong perception subscription was cleared");
      }
    }
  world.removeNpc(victim);
  std::puts("Native spells: real magic perception, fight sound, immune guilds and all subscriptions");
  }

void resumeShrinkNative(Gothic& gothic, const std::filesystem::path& caseDir) {
  using Contract=NpcDefinitionContract;
  auto& world=*gothic.world();
  auto& original=addSpellActor(world,"SHEEP","NW_FARM4_BALTHASAR");
  const auto npcId=world.npcId(&original);
  const auto archetype=original.instanceSymbol();
  const auto shrinking=world.script().findFunction("ZS_MAGICSHRINK");
  const auto shrunk=world.script().findFunction("ZS_MAGICSHRUNK");
  original.setScale(1.1f,1.2f,1.3f);
  original.handle().attribute[ATR_HITPOINTS]=319;
  original.handle().attribute[ATR_HITPOINTSMAX]=493;
  original.handle().attribute[ATR_STRENGTH]=167;
  for(int i=0;i<PROT_MAX;++i)
    original.handle().protection[i]=10*i+9;
  const auto scale=[&](Npc& npc) {
    npc.updateTransform();
    return npc.transform().at(1,1);
    };
  require(original.startState(shrinking,""),"could not begin Shrink on the prepared NPC");
  Contract::tickRoutine(original);
  require(std::abs(scale(original)-1.2f)<0.001f && original.handle().aivar[42]==0,
          "Shrink Init must preserve the scale and zero its stage");
  const auto advance=[&](Npc& npc, int first, int end) {
    constexpr float stages[]={0.90f,0.85f,0.80f,0.75f,0.70f,0.65f,0.60f,0.55f,0.50f,0.45f,0.40f,0.35f,0.30f};
    for(int i=first;i<end;++i) {
      npc.clearAiQueue();
      Contract::tickRoutine(npc);
      const auto* action=npc.aiQueueFront();
      require(std::abs(scale(npc)-stages[i])<0.001f && npc.handle().aivar[42]==i+1 &&
              npc.attribute(ATR_HITPOINTS)==319 && action && action->act==AI_Wait && action->i0==380,
              "Shrink must follow each exact intermediate scale before changing attributes");
      }
    };
  advance(original,0,5);
  const auto queue=activityQueue(original);
  reloadNativeWorld(gothic,caseDir/"mid-shrink-v57.sav");
  auto* npc=gothic.world()->npcById(npcId);
  require(npc && npc->instanceSymbol()==archetype && npc->isInState(shrinking) && Contract::aiState(*npc).started &&
          npc->handle().aivar[42]==5 && std::abs(scale(*npc)-0.70f)<0.001f && activityQueue(*npc)==queue,
          "v57 reload must restore the active Shrink phase, stage, scale and wait");
  const auto& loadedScript=gothic.world()->script();
  require(spellValue(loadedScript,"SPELL_PROCESSMANA.MANAINVESTED")==18 &&
          spellValue(loadedScript,"SPELL_LOGIC_SHRINK.MANAINVESTED")==18 &&
          spellValue(loadedScript,"SPELL_PROCESSMANA_RELEASE.MANAINVESTED")==19,
          "v57 reload must restore three separate mana parameters written by the spell");
  advance(*npc,5,13);
  npc->clearAiQueue();
  Contract::tickRoutine(*npc);
  require(npc->attribute(ATR_HITPOINTS)==32 && npc->attribute(ATR_HITPOINTSMAX)==50 &&
          npc->attribute(ATR_STRENGTH)==17 && npc->handle().aivar[42]==14,
          "Shrink must apply the original integer attribute reductions at stage 13");
  constexpr std::array<int,PROT_MAX> protection={9,1,2,3,0,5,6,79};
  require(std::equal(protection.begin(),protection.end(),npc->handle().protection),
          "Shrink must keep barrier/fall protection and reduce the six stock channels");
  npc->clearAiQueue();
  Contract::tickRoutine(*npc);
  const auto* action=npc->aiQueueFront();
  require(action && action->act==AI_StartState && action->func==shrunk && action->i0==1 &&
          npc->handle().start_aistate==int32_t(shrunk.ptr),"completed Shrink must queue the permanent stock state");
  Contract::nextAiAction(*npc);
  require(npc->isInState(shrunk),"the real queue failed to enter the permanent state");
  Contract::tickRoutine(*npc);
  require(npc->target()==gothic.world()->player(),"the permanent shrunken state must target the hero");
  Contract::tickRoutine(*npc);
  npc->clearState(false);
  require(std::abs(scale(*npc)-0.30f)<0.001f && npc->attribute(ATR_HITPOINTS)==32,
          "finishing Shrink must preserve permanent scale and attributes");
  gothic.world()->removeNpc(*npc);
  std::puts("Native spells: all Shrink phases and permanent state after an ordinary v57 reload");
  }

void roundTripSpellVictim(Npc& npc) {
  std::vector<uint8_t> bytes;
  {
    Tempest::MemWriter writer(bytes);
    Serialize out(writer);
    out.setContext(&npc.world());
    npc.save(out,0,"/npc/");
  }
  NpcDefinitionContract::aiState(npc)={};
  npc.clearAiQueue();
  npc.aiPush(AiQueue::aiWait(4321));
  for(const auto perception:{PERC_ASSESSMAGIC,PERC_ASSESSSTOPMAGIC,PERC_ASSESSDAMAGE,PERC_ASSESSTALK})
    npc.setPerceptionDisable(perception);
  npc.setOther(nullptr);
  Tempest::MemReader reader(bytes);
  Serialize in(reader);
  in.setContext(&npc.world());
  npc.load(in,0,"/npc/");
  npc.postValidate();
  npc.setSimTier(NpcSimTier::Full);
  }

void periodicMagicNative(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& hero=*world.player();
  const auto oldXp=hero.handle().exp, oldNext=hero.handle().exp_next, oldLevel=hero.handle().level;
  constexpr struct {
    std::string_view state, stop, restart;
    int damage;
    bool exactSecond, shortBurn;
    } cases[] = {
    {"ZS_MAGICBURN","B_STOPMAGICBURN","B_RESTARTBURN",1,true,false},
    {"ZS_MAGICBURNSHORT","B_STOPMAGICBURNSHORT","B_RESTARTBURNSHORT",1,true,true},
    {"ZS_PYRO","B_STOPPYRO",{},20,false,false},
    {"ZS_ZAPPED","B_STOPZAPPED",{},2,false,false},
    };
  for(const auto& row:cases) {
    auto& npc=addSpellActor(world,"VLK_408_JORA","NW_CITY_PATH_HABOUR_03");
    const auto init=script.findFunction(row.state);
    const auto loop=script.findFunction(std::string(row.state)+"_LOOP");
    const auto end=script.findFunction(std::string(row.state)+"_END");
    const auto stop=script.findFunction(row.stop);
    require(init.isValid() && loop.isValid() && end.isValid() && stop.isValid(),"missing native damage phases");
    npc.handle().attribute[ATR_HITPOINTS]=100;
    npc.handle().attribute[ATR_HITPOINTSMAX]=100;
    npc.handle().flags=NpcFlag::NONE;
    npc.handle().aivar[16]=0;
    npc.handle().level=3;
    npc.setLastHitBy(nullptr);
    require(npc.startState(init,""),"could not begin periodic damage");
    npc.setOther(&hero);
    Contract::tickRoutine(npc);
    require(Contract::perception(npc,PERC_ASSESSSTOPMAGIC)==stop,"damage Init must bind the stop callback");
    npc.setStateTime(999);
    const auto queue=activityQueue(npc);
    roundTripSpellVictim(npc);
    require(npc.isInState(init) && npc.stateTime()==999 && Contract::aiState(npc).started && activityQueue(npc)==queue &&
            npc.stateOther()==&hero && Contract::perception(npc,PERC_ASSESSSTOPMAGIC)==stop,
            "the saved periodic status must restore the clock, phase, source, stop callback and queue");
    for(const int32_t time:{999,1000,1999,2000}) {
      npc.handle().attribute[ATR_HITPOINTS]=100;
      npc.setStateTime(time);
      const auto result=script.invokeState(&npc,&hero,nullptr,loop);
      const bool due=time>=1000 && (!row.exactSecond || time<2000);
      spellRequire(npc.attribute(ATR_HITPOINTS)==100-(due ? row.damage : 0) &&
                   npc.stateTime()==uint64_t(due ? 0 : time) && npc.lastHitBy()==nullptr &&
                   result==(row.shortBurn && due ? LOOP_END : LOOP_CONTINUE),
                   row.state,"damage must obey the original integer timing and attribution");
      if(due)
        spellRequire(NpcDefinitionContract::alias(script,"B_MAGICHURTNPC.SLF").get()==static_cast<NpcInstance*>(hero.handlePtr().get()) &&
                     NpcDefinitionContract::alias(script,"B_MAGICHURTNPC.OTH").get()==static_cast<NpcInstance*>(npc.handlePtr().get()) &&
                     spellValue(script,"B_MAGICHURTNPC.DAMAGE")==row.damage,row.state,"the damage argument slots changed");
      }
    script.invokeState(&npc,&hero,nullptr,end);
    if(row.state=="ZS_PYRO") {
      hero.handle().level=1;
      hero.handle().exp=100;
      hero.handle().exp_next=10000;
      npc.handle().attribute[ATR_HITPOINTS]=row.damage;
      npc.setStateTime(1000);
      script.invokeState(&npc,&hero,nullptr,loop);
      require(npc.isDead() && npc.handle().aivar[16]==1 && hero.handle().exp==130 && npc.lastHitBy()==nullptr,
              "lethal periodic damage must award the victim's level as XP without changing attribution");
      npc.setStateTime(1000);
      script.invokeState(&npc,&hero,nullptr,loop);
      require(hero.handle().exp==130,"periodic damage must credit victory only once");
      hero.handle().exp=oldXp;
      hero.handle().exp_next=oldNext;
      hero.handle().level=oldLevel;
      }
    if(!row.restart.empty()) {
      const auto restart=script.findFunction(row.restart);
      require(restart.isValid(),"missing the burn restart callback");
      for(const auto* spell:{"SPL_WHIRLWIND",row.shortBurn ? "SPL_FIRESTORM" : "SPL_FIREBOLT","SPL_ICEWAVE"}) {
        npc.clearAiQueue();
        npc.aiPush(AiQueue::aiWait(173));
        npc.setPerceptionEnable(PERC_ASSESSMAGIC,restart.ptr);
        npc.setStateTime(17000);
        Contract::lastHitSpell(npc)=spellValue(script,spell);
        script.invokeState(&npc,&hero,nullptr,restart);
        const auto* action=npc.aiQueueFront();
        const std::string_view incoming=spell;
        const bool ice=incoming=="SPL_ICEWAVE", unrelated=incoming=="SPL_WHIRLWIND";
        require(action && npc.stateTime()==(ice || unrelated ? 17000u : 0u) &&
                (ice ? action->act==AI_StartState && action->func==script.findFunction("ZS_MAGICFREEZE") &&
                        !npc.hasPerc(PERC_ASSESSMAGIC) : action->act==AI_Wait && action->i0==173),
                "burn restart must keep its clock, reset it, or replace the queue with Freeze");
        }
      }
    npc.handle().guild=GIL_WOLF;
    npc.setTempAttitude(ATT_NEUTRAL);
    npc.setPerceptionDisable(PERC_ASSESSMAGIC);
    script.invokeState(&npc,&hero,nullptr,stop);
    auto actions=Contract::queue(npc);
    require(actions.size()==2 && actions.pop().act==AI_StandUp && actions.pop().act==AI_ContinueRoutine &&
            Contract::perception(npc,PERC_ASSESSMAGIC)==script.findFunction("B_ASSESSMAGIC") &&
            npc.tempAttitude()==ATT_HOSTILE,"stopping damage must stand, continue the routine and restore perception in order");
    world.removeNpc(npc);
    }
  std::puts("Native spells: burn, pyro and zap timing, restored status, restart and one-time XP");
  }

void timedMagicNative(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& hero=*world.player();
  const auto heroPosition=hero.position();
  const auto heroHp=hero.attribute(ATR_HITPOINTS);
  auto& victim=addSpellActor(world,"VLK_408_JORA","NW_CITY_PATH_HABOUR_03");
  auto& caster=addSpellActor(world,"BAU_951_HILDA","NW_CITY_PATH_HABOUR_03");
  victim.setPosition({1000,2000,3000});
  caster.setPosition({1000,2000,3500});
  victim.setTrueGuild(GIL_WOLF);
  victim.handle().guild=GIL_WOLF;
  victim.setSimTier(NpcSimTier::Full);
  victim.setProcessPolicy(NpcProcessPolicy::AiNormal);
  victim.setAnim(Npc::Anim::Idle);
  static constexpr struct {
    std::string_view state, restart, spell;
    int damage, duration, first;
    size_t marker;
    } cases[] = {
    {"ZS_INFLATE","B_RESTARTINFLATE","SPL_INFLATE",5,19,5,77},
    {"ZS_SUCKENERGY","B_RESTARTSUCKENERGY","SPL_SUCKENERGY",100,9,0,79},
    {"ZS_SWARM","B_RESTARTSWARM","SPL_SWARM",80,6,2,78},
    };
  for(const auto& row:cases) {
    const auto init=script.findFunction(row.state);
    const auto restart=script.findFunction(row.restart);
    require(init.isValid() && restart.isValid(),"missing the timed magic state");
    const bool drain=row.state=="ZS_SUCKENERGY";
    victim.clearState(false);
    victim.clearAiQueue();
    victim.setFatness(0);
    victim.setTempAttitude(ATT_NEUTRAL);
    victim.handle().attribute[ATR_HITPOINTSMAX]=500;
    victim.handle().attribute[ATR_HITPOINTS]=500;
    caster.handle().attribute[ATR_HITPOINTSMAX]=500;
    caster.handle().attribute[ATR_HITPOINTS]=50;
    hero.setPosition(victim.position()+Tempest::Vec3(400,0,0));
    require(victim.startState(init,"",gtime::endOfTime(),true),"could not enter the real status state");
    victim.setOther(&caster);
    Contract::tickRoutine(victim);
    spellRequire(Contract::perception(victim,PERC_ASSESSMAGIC)==restart &&
                 victim.handle().aivar[row.marker]==int(drain) &&
                 (!drain || spellValue(script,"TEMP_SUCKENERGY_DISTTOPLAYER")==400),
                 row.state,"the Init phase must bind restart and seed the original tick marker");
    victim.clearAiQueue();
    victim.setStateTime(uint64_t(row.first)*1000);
    Contract::tickRoutine(victim);
    const auto casterHp=drain ? 150 : 50;
    spellRequire(victim.attribute(ATR_HITPOINTS)==500-row.damage && caster.attribute(ATR_HITPOINTS)==casterHp &&
                 hero.attribute(ATR_HITPOINTS)==heroHp && victim.handle().aivar[row.marker]==row.first &&
                 victim.fatness()==(row.first==5 ? 9.f : 0.f),
                 row.state,"first unseen second must damage, transfer life and deform as appropriate");
    const auto queue=activityQueue(victim);
    roundTripSpellVictim(victim);
    spellRequire(victim.isInState(init) && victim.stateOther()==&caster && victim.stateTime()==uint64_t(row.first)*1000 &&
                 Contract::perception(victim,PERC_ASSESSMAGIC)==restart && activityQueue(victim)==queue,
                 row.state,"restored status lost its phase, source, clock, restart callback or queued actions");
    Contract::tickRoutine(victim);
    spellRequire(victim.attribute(ATR_HITPOINTS)==500-row.damage && caster.attribute(ATR_HITPOINTS)==casterHp &&
                 activityQueue(victim)==queue,row.state,"a restored second must not replay damage or animation");

    victim.clearAiQueue();
    victim.setStateTime(4000);
    Contract::lastHitSpell(victim)=0;
    script.invokeState(&victim,&caster,nullptr,restart);
    require(victim.stateTime()==4000 && victim.isAiQueueEmpty(),"an unrelated spell must keep the active timer");
    Contract::lastHitSpell(victim)=spellValue(script,row.spell);
    script.invokeState(&victim,&caster,nullptr,restart);
    require(victim.stateTime()==0 && victim.isAiQueueEmpty()!=drain,
            "a repeated spell must restart its timer and queue the drain animation alone");
    victim.handle().attribute[ATR_HITPOINTS]=row.damage;
    if(drain)
      hero.setPosition(victim.position()+Tempest::Vec3(301,0,0));
    victim.setStateTime(uint64_t(row.duration)*1000);
    Contract::tickRoutine(victim);
    require(victim.isInState(init) && victim.attribute(ATR_HITPOINTS)==1 && caster.attribute(ATR_HITPOINTS)==casterHp,
            "the last second must apply partial damage without transferring life");
    if(drain)
      hero.setPosition(victim.position()+Tempest::Vec3(300,0,0));
    else
      victim.setStateTime(uint64_t(row.duration+1)*1000);
    Contract::tickRoutine(victim);
    auto actions=Contract::queue(victim);
    require(!victim.isInState(init) && victim.tempAttitude()==ATT_HOSTILE &&
            actions.size()==1 && actions.pop().act==AI_StandUp &&
            Contract::perception(victim,PERC_ASSESSMAGIC)==script.findFunction("B_ASSESSMAGIC"),
            "the status must end at the exact timeout or drain distance and restore action and perception");
    }
  hero.setPosition(heroPosition);
  world.removeNpc(victim);
  world.removeNpc(caster);
  std::puts("Native spells: Inflate, Suck Energy and Swarm stages, restart, no double tick and exact expiry");
  }

void reactiveMagicNative(World& world) {
  using Contract=NpcDefinitionContract;
  auto& script=world.script();
  auto& hero=*world.player();
  for(const bool fear:{false,true}) {
    const auto state=fear ? "ZS_MAGICFLEE" : "ZS_MAGICSLEEP";
    const auto init=script.findFunction(state);
    const auto loop=script.findFunction(std::string(state)+"_LOOP");
    const auto stop=script.findFunction(fear ? "B_STOPMAGICFLEE" : "B_STOPMAGICSLEEP");
    auto& actor=addSpellActor(world,"VLK_408_JORA","NW_CITY_PATH_HABOUR_03");
    actor.setProcessPolicy(NpcProcessPolicy::AiNormal);
    require(init.isValid() && loop.isValid() && stop.isValid() && actor.startState(init,""),
            "Fear and Sleep need live prepared states and callbacks");
    actor.setOther(&hero);
    if(fear) {
      script.setSeed(73106);
      spellValue(script,"ZS_MAGICFLEE.RANDY",-1);
      }
    Contract::tickRoutine(actor);
    auto initActions=Contract::queue(actor);
    require(initActions.size()>0,"the magic status Init must queue its victim pose");
    while(initActions.size()>1)
      initActions.pop();
    const auto pose=initActions.pop();
    spellRequire(pose.act==AI_PlayAnimBs && pose.i0==(fear ? BS_STAND : BS_LIE) &&
                 pose.s0==(fear ? "T_STAND_2_FEAR_VICTIM2" : "T_STAND_2_VICTIM_SLE") &&
                 (!fear || spellValue(script,"ZS_MAGICFLEE.RANDY")==1),state,
                 "Init must queue the exact victim pose and store the seeded fear roll in RANDY");
    roundTripSpellVictim(actor);
    spellRequire(actor.isInState(init) && Contract::aiState(actor).started &&
                 Contract::perception(actor,PERC_ASSESSDAMAGE)==stop,state,"reload must preserve the damage callback");
    actor.clearAiQueue();
    if(!fear) {
      require(actor.passPerception(hero,nullptr,PERC_ASSESSTALK),"Sleep must keep its talk perception");
      auto actions=Contract::queue(actor);
      require(actions.size()>0,"Sleep talk must queue player observation");
      while(actions.size()>1)
        actions.pop();
      const auto observe=actions.pop();
      require(observe.act==AI_StartState && observe.i0==1 && observe.func==script.findFunction("ZS_OBSERVEPLAYER"),
              "Sleep talk must interrupt the pose to observe the hero");
      actor.clearAiQueue();
      }
    const auto boundary=fear ? 6000u : 31000u;
    actor.aiPush(AiQueue::aiWait(1337));
    actor.setStateTime(boundary-1);
    spellRequire(script.invokeState(&actor,&hero,nullptr,loop)==LOOP_CONTINUE && actor.aiQueueFront() &&
                 actor.aiQueueFront()->act==AI_Wait && actor.hasPerc(PERC_ASSESSDAMAGE),
                 state,"the queue and damage callback must survive until the next whole second");
    spellRequire(actor.passPerception(hero,nullptr,PERC_ASSESSDAMAGE) && !actor.hasPerc(PERC_ASSESSDAMAGE) &&
                 Contract::queue(actor).size()>1 && actor.aiQueueFront()->act==AI_Wait,
                 state,"a hit must release the victim after the commands already queued");
    actor.setPerceptionEnable(PERC_ASSESSDAMAGE,stop.ptr);
    actor.setStateTime(boundary);
    spellRequire(script.invokeState(&actor,&hero,nullptr,loop)==(fear ? LOOP_CONTINUE : LOOP_END) &&
                 !actor.hasPerc(PERC_ASSESSDAMAGE),state,"expiry must clear the damage callback and return the original result");
    auto actions=Contract::queue(actor);
    require(actions.size()>0,"expiry must queue a release action");
    while(actions.size()>1)
      actions.pop();
    const auto exit=actions.pop();
    spellRequire(fear ? exit.act==AI_StartState && exit.func==script.findFunction("ZS_FLEE") && actor.target()==&hero :
                        exit.act==AI_PlayAnim && exit.s0=="T_VICTIM_SLE_2_STAND",
                 state,"expiry must queue the exact release action");
    world.removeNpc(actor);
    }

  for(const bool freeze:{false,true}) {
    const auto state=freeze ? "ZS_MAGICFREEZE" : "ZS_WHIRLWIND";
    const auto init=script.findFunction(state);
    const auto loop=script.findFunction(std::string(state)+"_LOOP");
    const auto restart=script.findFunction(freeze ? "B_RESTARTFREEZE" : "B_RESTARTWHIRLWIND");
    auto& actor=addSpellActor(world,"VLK_408_JORA","NW_CITY_PATH_HABOUR_03");
    actor.setProcessPolicy(NpcProcessPolicy::AiNormal);
    actor.handle().attribute[ATR_HITPOINTS]=500;
    actor.handle().attribute[ATR_HITPOINTSMAX]=500;
    actor.handle().flags=NpcFlag::NONE;
    require(init.isValid() && loop.isValid() && restart.isValid() && actor.startState(init,""),
            "Freeze and Whirlwind need live prepared states and callbacks");
    actor.setOther(&hero);
    Contract::tickRoutine(actor);
    roundTripSpellVictim(actor);
    spellRequire(actor.isInState(init) && Contract::aiState(actor).started &&
                 Contract::perception(actor,PERC_ASSESSMAGIC)==restart,state,"reload must preserve restart perception");
    actor.clearAiQueue();
    const auto tickSlot=freeze ? 53u : 76u;
    const auto pulse=[&](uint64_t time,int damage) {
      const auto hp=actor.attribute(ATR_HITPOINTS);
      actor.setStateTime(time);
      spellRequire(script.invokeState(&actor,&hero,nullptr,loop)==LOOP_CONTINUE &&
                   actor.handle().aivar[tickSlot]==int32_t(time/1000) &&
                   actor.attribute(ATR_HITPOINTS)==hp-damage,state,"wrong damage pulse or whole-second marker");
      };
    pulse(1000,freeze ? 2 : 0);
    pulse(1700,0);
    actor.handle().guild=GIL_FIREGOLEM;
    pulse(4000,freeze ? 4 : 0);
    actor.handle().guild=GIL_ICEGOLEM;
    pulse(5000,freeze ? 1 : 0);
    Contract::lastHitSpell(actor)=spellValue(script,freeze ? "SPL_WHIRLWIND" : "SPL_ICECUBE");
    spellRequire(actor.passPerception(hero,nullptr,PERC_ASSESSMAGIC) && actor.stateTime()==5000 &&
                 actor.handle().aivar[tickSlot]==5,state,"another spell must leave the status clock alone");
    Contract::lastHitSpell(actor)=spellValue(script,freeze ? "SPL_ICEWAVE" : "SPL_WHIRLWIND");
    spellRequire(actor.passPerception(hero,nullptr,PERC_ASSESSMAGIC) && actor.stateTime()==0 &&
                 actor.handle().aivar[tickSlot]==5,state,"reapplying the spell must reset only the clock");
    pulse(freeze ? 19999u : 10999u,freeze ? 1 : 0);
    actor.aiPush(AiQueue::aiWait(1337));
    actor.setTempAttitude(ATT_FRIENDLY);
    actor.setStateTime(freeze ? 20000u : 11000u);
    spellRequire(script.invokeState(&actor,&hero,nullptr,loop)==LOOP_END &&
                 actor.aiQueueFront() && actor.aiQueueFront()->act==AI_StandUp && Contract::queue(actor).size()==1 &&
                 actor.tempAttitude()==ATT_HOSTILE &&
                 Contract::perception(actor,PERC_ASSESSMAGIC)==script.findFunction("B_ASSESSMAGIC"),
                 state,"expiry must restore magic perception and queue a single stand-up");
    world.removeNpc(actor);
    }
  std::puts("Native spells: exact expiry and restored reactions of Fear, Sleep, Freeze and Whirlwind");
  }

void nativeSpellContract(Gothic& gothic, const std::filesystem::path& caseDir) {
  auto& world=*gothic.world();
  stockSpellAccountingNative(world);
  stockSpecialManaNative(world);
  stockSpellSummonsNative(world);
  stockShrinkCastNative(world);
  stockSpellCollisionNative(world);
  stockMagicPerceptionNative(world);
  resumeShrinkNative(gothic,caseDir);
  periodicMagicNative(*gothic.world());
  timedMagicNative(*gothic.world());
  reactiveMagicNative(*gothic.world());
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

int32_t chapterValue(const GameScript& script, std::string_view name) {
  const auto value=script.globalIntSymbol(name);
  if(!value)
    throw std::runtime_error("Missing stock integer "+std::string(name));
  return *value;
  }

void chapterSet(GameScript& script, std::string_view name, int32_t value) {
  if(!script.setGlobalIntSymbol(name,value))
    throw std::runtime_error("Missing writable stock integer "+std::string(name));
  }

const std::string& chapterText(std::string_view name) {
  const auto* constant=NativeBaseCatalog::findConstant(*baseNativeCatalog(),name);
  const auto* strings=constant ? std::get_if<std::vector<std::string>>(&constant->values) : nullptr;
  if(strings==nullptr || strings->size()!=1)
    throw std::runtime_error("Missing stock text "+std::string(name));
  return strings->front();
  }

const QuestLog::Quest& chapterQuest(const GameScript& script, std::string_view name) {
  const auto& log=script.questLog();
  const QuestLog::Quest* found=nullptr;
  for(size_t i=0;i<log.questCount();++i) {
    if(log.quest(i).name!=name)
      continue;
    require(found==nullptr,"chapter journal must not duplicate the topic");
    found=&log.quest(i);
    }
  require(found!=nullptr,"chapter journal is missing its topic");
  return *found;
  }

class ChapterShipEvents {
  public:
    explicit ChapterShipEvents(Gothic& gothic) : gothic(gothic) {
      gothic.onIntroChapter.bind(this,&ChapterShipEvents::show);
      gothic.onVideo.bind(this,&ChapterShipEvents::video);
      }
    ~ChapterShipEvents() {
      gothic.onIntroChapter.ubind(this,&ChapterShipEvents::show);
      gothic.onVideo.ubind(this,&ChapterShipEvents::video);
      }
    std::vector<ChapterScreen::Show> notices;
    std::vector<std::pair<std::string,bool>> videos;

  private:
    void show(const ChapterScreen::Show& notice) { notices.push_back(notice); }
    void video(std::string_view name,bool exit) { videos.emplace_back(name,exit); }
    Gothic& gothic;
  };

void chapterChangeWorld(Gothic& gothic, std::string_view world, std::string_view waypoint) {
  gothic.gameSession()->changeWorld(world,waypoint);
  gothic.gameSession()->tick(0);
  require(gothic.checkLoading()!=Gothic::LoadState::Idle,"the session must queue the ordinary world transition");
  while(gothic.checkLoading()==Gothic::LoadState::Loading)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const auto state=gothic.checkLoading();
  const bool finished=gothic.finishLoading();
  require(state==Gothic::LoadState::Finalize && finished,"ordinary chapter world transition failed");
  require(gothic.world()!=nullptr && storygraph::nativeOperationName(gothic.world()->name())==storygraph::nativeOperationName(world),
          "ordinary chapter transition opened the wrong world");
  }

void nativeChapterEffects(Gothic& gothic, ChapterShipEvents& events, const std::filesystem::path& savePath) {
  using Contract=NpcDefinitionContract;
  using storygraph::NativeSlot;
  using storygraph::NativeValue;
  auto& world=*gothic.world();
  auto& script=world.script();
  auto* hero=world.player();
  auto* pedro=liveNpc(world,"NOV_600_PEDRO_DI");
  const auto ore=script.findItemInstance("ITMI_NUGGET");
  require(hero && pedro && ore,"chapter effects need the hero, Pedro and the stock ore archetype");
  hero->delItem(*ore,uint32_t(hero->itemCount(*ore)));
  pedro->delItem(*ore,uint32_t(pedro->itemCount(*ore)));
  auto* heroOre=hero->addItem(*ore,2);
  auto* pedroOre=pedro->addItem(*ore,3);
  require(heroOre && pedroOre && heroOre!=pedroOre,"the chapter contract needs separate ore stacks with the same archetype");
  const auto heroRecord=std::static_pointer_cast<NpcInstance>(hero->handlePtr());
  const auto pedroRecord=std::static_pointer_cast<NpcInstance>(pedro->handlePtr());
  const auto pedroOreRecord=std::static_pointer_cast<ItemInstance>(pedroOre->handlePtr());
  Contract::alias(script,"ITEM")=pedroOreRecord;
  Contract::alias(script,"SELF")=heroRecord;
  Contract::alias(script,"OTHER")=pedroRecord;
  Contract::alias(script,"SEKOB")=heroRecord;
  Contract::alias(script,"BENGAR")=pedroRecord;

  constexpr NativeSlot selectedRecipe{NativeSlot::Variable,"PLAYER_TALENT_ALCHEMY",12};
  constexpr NativeSlot otherRecipe{NativeSlot::Variable,"PLAYER_TALENT_ALCHEMY",11};
  const auto& rescue=chapterText("TOPIC_RESCUEBENNET");
  const auto call=[&](std::string_view name, const std::vector<storygraph::StoryArgument>& args) {
    const auto result=script.callStoryFunction(name,pedro,hero,args);
    if(!result.ok)
      throw std::runtime_error("Chapter function "+std::string(name)+": "+result.error);
    };
  call("LOG_CREATETOPIC",{rescue,int32_t(QuestLog::Mission)});
  const auto island=chapterValue(script,"DRAGONISLAND_ZEN");
  for(int32_t chapter:{1,2,3,4,5,6,7}) {
    call("LOG_SETTOPICSTATUS",{rescue,int32_t(QuestLog::Status::Running)});
    chapterSet(script,"MIS_RESCUEBENNET",int32_t(QuestLog::Status::Failed));
    chapterSet(script,"XP_AMBIENT",-1);
    hero->handle().flags=NpcFlag::IMMORTAL;
    pedro->handle().flags=NpcFlag::IMMORTAL;
    std::string error;
    {
      auto host=script.makeStoryHost(pedro,hero,true);
      if(!host->write(selectedRecipe,NativeValue::integer(0),error) ||
         !host->write(otherRecipe,NativeValue::integer(9),error))
        throw std::runtime_error("Could not prepare the stock alchemy slots: "+error);
    }
    events.notices.clear();
    call("B_KAPITELWECHSEL",{chapter,island});
    require(chapterValue(script,"KAPITEL")==chapter &&
            chapterValue(script,"B_KAPITELWECHSEL.NEUES_KAPITEL")==chapter &&
            chapterValue(script,"B_KAPITELWECHSEL.AKTUELLES_LEVEL_ZEN")==island,
            "the native chapter switch must retain the chapter and its two saved arguments");
    require(chapterValue(script,"XP_AMBIENT")== (chapter<=6 ? 50*chapter : -1),
            "the native chapter switch must update ambient XP for chapters one through six");
    const auto expectedFlags=chapter==2 ? NpcFlag::NONE : NpcFlag::IMMORTAL;
    require(hero->handle().flags==expectedFlags && pedro->handle().flags==expectedFlags,
            "only chapter two must clear the two story actors' flags");
    require(chapterQuest(script,rescue).status==QuestLog::Status::Failed,
            "the native chapter switch must apply the live Bennet mission outcome to the journal");
    {
      auto host=script.makeStoryHost(pedro,hero,false);
      const auto selected=host->read(selectedRecipe,error);
      const auto untouched=host->read(otherRecipe,error);
      require(selected && untouched && selected->i==int32_t(chapter>=4 && chapter<=6) && untouched->i==9,
              "chapters four through six must unlock only the intended alchemy recipe");
    }
    require(Contract::alias(script,"SELF")==heroRecord && Contract::alias(script,"OTHER")==pedroRecord &&
            Contract::alias(script,"ITEM")==pedroOreRecord,
            "the chapter call must restore the caller's NPC context and leave the exact ITEM reference alone");
    gothic.tick(0);
    require(events.notices.size()==size_t(chapter<=2),"only the first two island-path chapter branches show a notice");
    if(chapter<=2) {
      const auto& notice=events.notices.front();
      const auto number=std::to_string(chapter);
      require(notice.time==6000 && notice.title==chapterText("KAPWECHSEL_"+number) &&
              notice.subtitle==chapterText("KAPWECHSEL_"+number+"_TEXT") &&
              notice.img=="chapter"+number+".tga" && notice.sound=="chapter_01.wav",
              "the chapter notice must dispatch the complete stock card arguments");
      }
    }

  reloadNativeWorld(gothic,savePath);
  auto& loaded=*gothic.world();
  auto& loadedScript=loaded.script();
  auto* loadedHero=loaded.player();
  auto* loadedPedro=liveNpc(loaded,"NOV_600_PEDRO_DI");
  require(loadedHero && loadedPedro && loadedHero->getItem(*ore) && loadedPedro->getItem(*ore) &&
          loadedHero->itemCount(*ore)==2 && loadedPedro->itemCount(*ore)==3 &&
          Contract::alias(loadedScript,"SELF").get()==static_cast<NpcInstance*>(loadedHero->handlePtr().get()) &&
          Contract::alias(loadedScript,"OTHER").get()==static_cast<NpcInstance*>(loadedPedro->handlePtr().get()) &&
          Contract::alias(loadedScript,"ITEM").get()==static_cast<ItemInstance*>(loadedPedro->getItem(*ore)->handlePtr().get()) &&
          Contract::alias(loadedScript,"ITEM").get()!=static_cast<ItemInstance*>(loadedHero->getItem(*ore)->handlePtr().get()),
          "v57 must resolve SELF and ITEM to their exact live owners after the chapter change");
  require(chapterValue(loadedScript,"KAPITEL")==7 && chapterValue(loadedScript,"XP_AMBIENT")==-1 &&
          chapterValue(loadedScript,"B_KAPITELWECHSEL.NEUES_KAPITEL")==7 &&
          chapterValue(loadedScript,"B_KAPITELWECHSEL.AKTUELLES_LEVEL_ZEN")==island &&
          chapterQuest(loadedScript,rescue).status==QuestLog::Status::Failed,
          "v57 must preserve the chapter result and journal change");
  std::puts("Native chapter change: seven branches, exact contextual references, journal and UI, v57 reload");
  }

void nativeChapterShipContract(Gothic& gothic, const std::filesystem::path& caseDir) {
  using Contract=NpcDefinitionContract;
  struct Passenger {
    std::string_view flag, name;
    uint8_t cases;
    std::string_view pairedPoint={}, singlePoint={};
  };
  constexpr Passenger passengers[]={
    {"JORGENISCAPTAIN","VLK_4250_JORGEN_DI",0b1010}, {"TORLOFISCAPTAIN","SLD_801_TORLOF_DI",0b1100},
    {"JACKISCAPTAIN","VLK_444_JACK_DI",0b1100}, {"LEE_ISONBOARD","SLD_800_LEE_DI",0b1000},
    {"MILTENNW_ISONBOARD","PC_MAGE_DI",0b1010,"SHIP_CREW_19","SHIP_CREW_19"},
    {"LESTER_ISONBOARD","PC_PSIONIC_DI",0b1100,"SHIP_CREW_19","SHIP_CREW_19"},
    {"MARIO_ISONBOARD","NONE_101_MARIO_DI",0b1000}, {"WOLF_ISONBOARD","SLD_811_WOLF_DI",0b1000},
    {"VATRAS_ISONBOARD","VLK_439_VATRAS_DI",0b1000}, {"BENNET_ISONBOARD","SLD_809_BENNET_DI",0b1000},
    {"DIEGO_ISONBOARD","PC_THIEF_DI",0b1010,"SHIP_CREW_05","SHIP_CREW_04"},
    {"GORN_ISONBOARD","PC_FIGHTER_DI",0b1000}, {"LARES_ISONBOARD","VLK_449_LARES_DI",0b1100,"SHIP_CREW_05","SHIP_CREW_04"},
    {"BIFF_ISONBOARD","DJG_713_BIFF_DI",0b1000}, {"ANGAR_ISONBOARD","DJG_705_ANGAR_DI",0b1000},
    {"GIRION_ISONBOARD","PAL_207_GIRION_DI",0b1000},
    };
  constexpr auto pedroName="NOV_600_PEDRO_DI";
  const auto& title=chapterText("TOPIC_HALLENVONIRDORATH");
  const auto placement=NativeWorldPlacement::Definitions::load(Resources::baseDir(),StockStoryPrograms::stockProgram,*baseNativeCatalog());
  ChapterShipEvents events(gothic);
  const auto baseline=caseDir/"ship-departure.sav";
  reloadNativeWorld(gothic,baseline);
  for(uint32_t index=0;index<4;++index) {
    if(index!=0)
      loadNativeWorld(gothic,baseline);
    auto& departure=gothic.world()->script();
    const auto key=departure.findItemInstance("ITKE_SHIP_LEVELCHANGE_MIS");
    const auto ore=departure.findItemInstance("ITMI_NUGGET");
    auto* hero=gothic.world()->player();
    require(key && ore && hero,"ship departure requires the original hero, key and ore");
    const auto cameAboard=[&](const Passenger& passenger) { return (passenger.cases & (1u<<index))!=0; };
    for(const auto& passenger:passengers) {
      const int32_t accepted=passenger.flag.ends_with("ISCAPTAIN") ? 1 : 2;
      chapterSet(departure,passenger.flag,cameAboard(passenger) ? accepted : 0);
      }
    if(index==0) {
      chapterSet(departure,"JORGENISCAPTAIN",2);
      chapterSet(departure,"GORN_ISONBOARD",1);
      }
    chapterSet(departure,"KAPITEL",5);
    hero->delItem(*key,uint32_t(hero->itemCount(*key)));
    const uint32_t keyCount=index==0 ? 0u : 2u;
    if(keyCount!=0)
      require(hero->addItem(*key,keyCount)!=nullptr,"ship departure must place two keys on the hero");
    hero->delItem(*ore,uint32_t(hero->itemCount(*ore)));
    auto* item=hero->addItem(*ore,1);
    require(item!=nullptr,"ship departure needs a current ITEM reference");
    const auto departureItem=std::static_pointer_cast<ItemInstance>(item->handlePtr());
    Contract::alias(departure,"ITEM")=departureItem;
    Contract::alias(departure,"SELF")=std::static_pointer_cast<NpcInstance>(hero->handlePtr());
    events.videos.clear();
    chapterChangeWorld(gothic,"DRAGONISLAND.ZEN","SHIP_DECK_01");

    const auto verify=[&] {
      auto& world=*gothic.world();
      auto& script=world.script();
      auto* player=world.player();
      auto* pedro=liveNpc(world,pedroName);
      require(player!=nullptr,"island arrival must restore the departing hero");
      require(pedro!=nullptr,"island arrival must place the island Pedro");
      require(Contract::alias(script,"PEDRO_DI").get()==static_cast<NpcInstance*>(pedro->handlePtr().get()),
              "island initialization must bind PEDRO_DI to the live Pedro");
      require(Contract::alias(script,"HERO").get()==static_cast<NpcInstance*>(player->handlePtr().get()),
              "island arrival must bind HERO to the restored player");
      require(!Contract::alias(script,"SELF"),"island arrival must clear the departing world's SELF context");
      const auto oreCount=player->itemCount(*ore);
      if(oreCount!=1)
        throw std::runtime_error("Hero ore after island arrival: expected 1, got "+std::to_string(oreCount));
      // The Master Dementor's constructor creates his chest key after the old world's ITEM is cleared.
      const auto& arrivalItem=Contract::alias(script,"ITEM");
      require(arrivalItem!=departureItem,"island arrival must clear the departing world's exact ITEM reference");
      auto* dem=liveNpc(world,"DMT_1299_OBERDEMENTOR_DI");
      const auto chestKey=script.findItemInstance("ITKE_CHESTMASTERDEMENTOR_MIS");
      auto* liveKey=dem && chestKey ? dem->getItem(*chestKey) : nullptr;
      require(liveKey!=nullptr,"the island Master Dementor must carry his created chest key");
      if(arrivalItem.get()!=static_cast<ItemInstance*>(liveKey->handlePtr().get())) {
        const auto name=arrivalItem ? script.storyItemName(arrivalItem->symbol_index()) : std::string_view("<null>");
        throw std::runtime_error("ITEM after island arrival: expected the live Master Dementor chest key, got "+std::string(name));
        }
      require(player->itemCount(*key)==(keyCount!=0 ? keyCount-1 : 0),
              "island initialization must remove exactly one ship key when present");
      require(chapterValue(script,"CURRENTLEVEL")==chapterValue(script,"DRAGONISLAND_ZEN") &&
              chapterValue(script,"KAPITEL")==6 && chapterValue(script,"XP_AMBIENT")==300,
              "ship arrival must advance the level, chapter and ambient XP");
      const auto& quest=chapterQuest(script,title);
      require(quest.section==QuestLog::Mission && quest.status==QuestLog::Status::Running &&
              quest.entry==std::vector<std::string>{placement.dragonArrival},
              "ship arrival must add the exact original journal entry once");
      uint32_t previous=world.npcId(player);
      for(const auto& passenger:passengers) {
        const auto id=script.storyNpcId(passenger.name);
        require(id.has_value(),"ship crew must have a native NPC archetype");
        auto* npc=world.findNpcByInstance(*id);
        if((npc!=nullptr)!=cameAboard(passenger) || world.findNpcByInstance(*id,1)!=nullptr)
          throw std::runtime_error("Wrong number of ship passengers for "+std::string(passenger.name));
        if(npc==nullptr)
          continue;
        require(world.npcId(npc)>previous,"ship captain and crew must keep their stock insertion order");
        previous=world.npcId(npc);
        if(passenger.pairedPoint.empty())
          continue;
        const bool paired=index==3;
        const auto callback=script.findFunction(paired ? "ZS_SMALLTALK" : "ZS_SIT_BENCH");
        const auto point=paired ? passenger.pairedPoint : passenger.singlePoint;
        require(callback.isValid(),"ship companion routine must have a native state");
        size_t rows=0;
        npc->forEachRoutine([&](const Npc::RoutineInfo& row) {
          require(row.state==callback && row.point==point && row.waypoint && row.waypoint->name==point &&
                  row.start==gtime(rows==0 ? 8 : 23,0) && row.end==gtime(rows==0 ? 23 : 8,0),
                  "paired companions must talk and unpaired companions must sit in their original schedule and point");
          ++rows;
          });
        require(rows==2,"ship companion must keep both prepared routine rows");
        }
      require(world.npcId(pedro)>previous,"Pedro must follow the optional captain and crew in placement order");
      const auto flask=script.findItemInstance("ITMI_FLASK");
      require(flask.has_value(),"ship deck placement requires the stock flask");
      const auto* firstPoint=world.findPoint("FP_ITEM_SHIP_03");
      const auto* secondPoint=world.findPoint("FP_ITEM_SHIP_07");
      require(firstPoint && secondPoint,"stock ship deck item points must exist");
      std::vector<const WayPoint*> points;
      for(size_t ordinal=0; auto* bottle=world.findItemByInstance(*flask,ordinal); ++ordinal) {
        if(bottle->position()==firstPoint->position())
          points.push_back(firstPoint);
        if(bottle->position()==secondPoint->position())
          points.push_back(secondPoint);
        }
      require(points==std::vector{firstPoint,secondPoint},"ship deck flasks must keep their stock placement order and points");
      };
    constexpr auto video="SHIP.BIK";
    require(events.videos==std::vector<std::pair<std::string,bool>>{{video,false}},"ship arrival must play its video exactly once");
    verify();
    reloadNativeWorld(gothic,caseDir/("ship-crew-"+std::to_string(index)+".sav"));
    verify();
    require(events.videos==std::vector<std::pair<std::string,bool>>{{video,false}},"reloading the island must not replay the arrival video");
    }
  std::puts("Native Irdorath crew: four captain and crew combinations, paired routines, ship arrival and v57 reload");
  nativeChapterEffects(gothic,events,caseDir/"chapter-aliases.sav");
  }

}

int main(int argc, const char** argv) {
  try {
    if(argc==2 && !std::string_view(argv[1]).starts_with("--")) {
      bennetOreContract(argv[1]);
      return EXIT_SUCCESS;
      }
    if(argc==3) {
      const std::string_view mode=argv[1];
      if(mode=="--cavalorn") {
        nativeStoryGraphContract(argv[2]);
        return EXIT_SUCCESS;
        }
      constexpr std::pair<std::string_view,WorldContract> cases[]={
        {"--chapter-ship",nativeChapterShipContract},
        {"--npc-behavior",nativeNpcBehaviorContract},
        {"--npc-inventory",nativeNpcInventoryContract},
        {"--spells",nativeSpellContract},
        };
      for(const auto& [name,contract]:cases) {
        if(mode==name) {
          runNativeWorldContract(argv[2],contract);
          return EXIT_SUCCESS;
          }
        }
      }
    std::fputs("Usage: gothic-native-story-contract [--cavalorn|--chapter-ship|--npc-behavior|--npc-inventory|--spells] /absolute/case-directory\n",stderr);
    return EXIT_FAILURE;
    }
  catch(const std::exception& error) {
    std::fprintf(stderr,"FAIL: native story contract: %s\n",error.what());
    return EXIT_FAILURE;
    }
  }
