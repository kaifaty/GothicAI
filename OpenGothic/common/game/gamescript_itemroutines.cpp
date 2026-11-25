#include "gamescript.h"

#include "gamescript_inventorytransfer.h"

#include "game/definitions/tradeinventorydata.h"
#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "gothic.h"
#include "game/definitions/deathtrophydefinitions.h"
#include "game/npcactivityhost.h"
#include "resources.h"
#include "world/objects/npc.h"
#include "world/world.h"

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct RestoreContext {
  daedalus::DaedalusVm& vm;
  std::shared_ptr<daedalus::DaedalusInstance> caller;
  ~RestoreContext() { vm.unsafe_set_gi(std::move(caller)); }
  };

} // namespace

void GameScript::loadNativeTradeInventory() {
  if(!nativeRoutineProgram)
    return;

  using Symbol = daedalus::DaedalusSymbol;
  using NpcRef = std::shared_ptr<daedalus::INpc>;
  const auto enabled = [this] {
    return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
    };
  const auto* ambient=findSymbol("B_CREATEAMBIENTINV");
  if(npcDefinitions.helpers.helpers.contains(ambient->index())) {
    vm.override_function("B_CREATEAMBIENTINV",std::function<void(NpcRef)>{[this,ambient](const NpcRef& actor) {
      using Value=NpcHelperDefinitions::Value;
      const std::array arguments={Value{.kind=Value::Kind::Self}};
      if(!actor || !invokeNpcHelper(actor,ambient->index(),arguments)) {
        vm.push_instance(actor);
        vm.unsafe_call(ambient);
        }
      }},[enabled] { return enabled() && NpcHelperRuntime::enabled(); });
    }

  auto record = TradeInventoryData::load(Resources::baseDir());
  if(!record)
    return;

  auto* lookup = findSymbol("HLP_GETNPC");
  auto* instanceId = findSymbol("HLP_GETINSTANCEID");
  auto* hasItems = findSymbol("NPC_HASITEMS");
  auto* removeItems = findSymbol("NPC_REMOVEINVITEMS");
  auto* validNpc = findSymbol("HLP_ISVALIDNPC");
  auto* inState = findSymbol("NPC_ISINSTATE");
  auto* runes = findSymbol("B_CLEARRUNEINV");
  auto* junk = findSymbol("B_CLEARJUNKTRADEINV");
  auto* dead = findSymbol("B_CLEARDEADTRADER");
  auto* runesActor = findSymbol("B_CLEARRUNEINV.SLF");
  auto* runeHero = findSymbol("B_CLEARRUNEINV.HER");
  auto* junkActor = findSymbol("B_CLEARJUNKTRADEINV.SLF");
  auto* tradeActor = findSymbol("B_GIVETRADEINV.SLF");
  const auto deadState = int32_t(findSymbolIndex("ZS_DEAD"));
  const auto unconsciousState = int32_t(findSymbolIndex("ZS_UNCONSCIOUS"));
  constexpr uint16_t stockChapter = 49; // AIV_INVCHAPTER.
  auto* create = findSymbol("CREATEINVITEMS");
  auto* yes = findSymbol("TRUE");
  auto* no = findSymbol("FALSE");
  auto* oldWorldFlag = findSymbol("ENTEROW_KAPITEL2");
  auto* secretSign = findSymbol("KNOWS_SECRETSIGN");
  auto* militia = findSymbol("GIL_MIL");
  auto* paladin = findSymbol("GIL_PAL");

  for(auto& stock : record->stock) {
    auto* actor = findSymbol(stock.actor);
    vm.override_function(findSymbol(stock.function)->name(),std::function<void(NpcRef)>{
      [this,actor,create,yes,no,oldWorldFlag,secretSign,militia,paladin,batches=std::move(stock.batches)](NpcRef npc) {
        const auto grantItems = [&](const std::vector<TradeInventoryData::Grant>& grants) {
          for(const auto& grant : grants) {
            vm.push_reference(findSymbol(grant.recipient));
            if(grant.itemSymbol)
              vm.push_reference(findSymbol(*grant.itemSymbol));
            else
              vm.push_int(grant.item);
            vm.push_int(grant.amount);
            vm.unsafe_call_external(create);
            }
          };
        const auto hasHeroGuild = [&](const Symbol* expected) {
          const auto value = expected->get_int();
          const auto hero = std::static_pointer_cast<daedalus::INpc>(vm.global_hero()->get_instance());
          vm.unsafe_set_gi(hero);
          return (hero ? hero->guild : 0)==value;
          };
        actor->set_instance(std::move(npc));
        for(const auto& batch : batches) {
          auto* granted = batch.granted ? findSymbol(*batch.granted) : nullptr;
          const bool pending = granted==nullptr || granted->get_int()==no->get_int();
          const bool enteredOldWorld = !batch.requiresOldWorld || oldWorldFlag->get_int()==yes->get_int();
          const bool chapterReached = !batch.minimumChapter || storyChapter>=*batch.minimumChapter;
          if(!pending || !enteredOldWorld || !chapterReached)
            continue;
          grantItems(batch.grants);
          if(batch.bonus) {
            bool eligible = false;
            if(batch.bonus->condition==TradeInventoryData::Bonus::Condition::HeroGuild) {
              const bool isMilitia = hasHeroGuild(militia);
              const bool isPaladin = hasHeroGuild(paladin);
              eligible = isMilitia || isPaladin;
              } else {
              eligible = secretSign->get_int()==yes->get_int();
              }
            if(eligible)
              grantItems(batch.bonus->grants);
            grantItems(batch.bonus->following);
            }
          if(granted!=nullptr)
            granted->set_int(yes->get_int());
          }
        }},enabled);
    }

  const auto getNpc = [this,lookup](int32_t source,Symbol* alias) {
    vm.push_int(source);
    vm.unsafe_call_external(lookup);
    alias->set_instance(vm.pop_instance());
    };
  const auto id = [this,instanceId](Symbol* actor) {
    vm.push_reference(actor);
    vm.unsafe_call_external(instanceId);
    return vm.pop_int();
    };
  const auto removePairs = [this,hasItems,removeItems](Symbol* actor,const auto& pairs) {
    for(const auto& [remove,count] : pairs) {
      vm.push_reference(actor);
      vm.push_int(remove);
      vm.push_reference(actor);
      vm.push_int(count);
      vm.unsafe_call_external(hasItems);
      // Keep the original external's INT on the void helper's call stack.
      vm.unsafe_call_external(removeItems);
      }
    };
  const auto call = [this](Symbol* actor,const Symbol* fn) {
    vm.push_reference(actor);
    vm.unsafe_call_bl(fn);
    };
  const auto member = [this](Symbol* actor) {
    auto npc = std::static_pointer_cast<daedalus::INpc>(actor->get_instance());
    vm.unsafe_set_gi(npc);
    return npc;
    };

  vm.override_function("B_CLEARRUNEINV",std::function<void(NpcRef)>{
    [this,runesActor,runeHero,heroIndex=record->hero,runePairs=std::move(record->runes),getNpc,id,removePairs](NpcRef actor) {
      runesActor->set_instance(std::move(actor));
      getNpc(heroIndex,runeHero);
      const auto hero = id(runeHero);
      if(hero!=id(vm.global_self()))
        removePairs(runesActor,runePairs);
    }},enabled);
  vm.override_function("B_CLEARJUNKTRADEINV",std::function<void(NpcRef)>{
    [this,junkActor,junkPairs=std::move(record->junk),validNpc,removePairs](NpcRef actor) {
      junkActor->set_instance(std::move(actor));
      vm.push_reference(junkActor);
      vm.unsafe_call_external(validNpc);
      if(vm.pop_int()!=0)
        removePairs(junkActor,junkPairs);
    }},enabled);
  vm.override_function("B_CLEARDEADTRADER",std::function<void(NpcRef)>{
    [this,call,member](NpcRef actor) {
      auto* trader = findSymbol("B_CLEARDEADTRADER.TRADER");
      trader->set_instance(std::move(actor));
      vm.push_reference(trader);
      vm.unsafe_call_external(findSymbol("NPC_CLEARINVENTORY"));
      const auto defeated = member(trader);
      if(!defeated || defeated->aivar[16]==0) // AIV_VICTORYXPGIVEN.
        call(trader,findSymbol("B_CREATEAMBIENTINV"));
      if(const auto current = member(trader))
        current->aivar[stockChapter] = std::bit_cast<int32_t>(uint32_t(storyChapter)-1u);
    }},enabled);
  vm.override_function("B_GIVETRADEINV",std::function<void(NpcRef)>{
    [this,traders=std::move(record->traders),restocks=std::move(record->restocks),
     defeatedTraders=std::move(record->defeated),tradeActor,runes,junk,dead,inState,unconsciousState,deadState,getNpc,id,call,member](NpcRef actor) {
      const auto sameTrader = [&](uint32_t alias) {
        const auto selected = id(findSymbol(alias));
        return selected==id(tradeActor);
        };
      const auto isInState = [&](int32_t state) {
        vm.push_reference(tradeActor);
        vm.push_int(state);
        vm.unsafe_call_external(inState);
        return vm.pop_int()!=0;
        };
      tradeActor->set_instance(std::move(actor));
      for(const auto& [source,alias] : traders)
        getNpc(source,findSymbol(alias));
      call(tradeActor,runes);
      const auto current = member(tradeActor);
      if((current ? current->aivar[stockChapter] : 0)<=storyChapter) {
        for(const auto& [alias,body] : restocks) {
          if(!sameTrader(alias))
            continue;
          call(tradeActor,junk);
          call(tradeActor,findSymbol(body));
          }
        if(const auto restocked = member(tradeActor))
          restocked->aivar[stockChapter] = std::bit_cast<int32_t>(uint32_t(storyChapter)+1u);
        }
      const bool unconscious = isInState(unconsciousState);
      const bool deceased = isInState(deadState);
      if(unconscious || deceased) {
        for(auto alias : defeatedTraders) {
          if(sameTrader(alias))
            call(tradeActor,dead);
          }
        }
    }},enabled);
  }

void GameScript::loadNativeDeathTrophies() {
  if(!nativeRoutineProgram)
    return;

  using namespace DeathTrophyDefinitions;
  using Symbol = daedalus::DaedalusSymbol;
  using NpcRef = std::shared_ptr<daedalus::INpc>;

  auto catalog = load(Resources::baseDir());
  if(!catalog)
    return;

  auto* actor = findSymbol("B_GIVEDEATHINV.SLF");
  auto* talent = findSymbol("PLAYER_TALENT_TAKEANIMALTROPHY");
  auto* yes = findSymbol("TRUE");
  auto* instanceId = findSymbol("HLP_GETINSTANCEID");
  auto* random = findSymbol("HLP_RANDOM");
  auto* create = findSymbol("CREATEINVITEMS");

  const auto check = [&](const Trophies& trophies) {
    using Type = daedalus::DaedalusDataType;
    for(const auto& trophy : trophies) {
      const auto* symbol = findSymbol(trophy.symbol);
      const auto type = trophy.match==Match::Instance ? Type::INSTANCE : Type::INT;
      if(symbol==nullptr || symbol->type()!=type)
        throw std::runtime_error("Invalid death-trophies.json match symbol "+std::to_string(trophy.symbol));
      }
    };
  for(const auto& learned : catalog->taught) {
    if(learned.index>=talent->count())
      throw std::runtime_error("Invalid death-trophies.json talent index "+std::to_string(learned.index));
    check(learned.trophies);
    }
  check(catalog->unconditional);
  for(const auto& roll : catalog->randomized) {
    const auto* result = findSymbol(roll.result);
    if(result==nullptr || result->type()!=daedalus::DaedalusDataType::INT || result->is_const() || result->count()==0)
      throw std::runtime_error("Invalid death-trophies.json RNG local "+std::to_string(roll.result));
    for(const auto& choices : roll.choices)
      check(choices);
    }

  vm.override_function("B_GIVEDEATHINV",std::function<void(NpcRef)>{
    [this,actor,talent,yes,instanceId,random,create,catalog=std::move(*catalog)](NpcRef npc) {
      const auto member = [&](Symbol* symbol) {
        auto current = std::static_pointer_cast<daedalus::INpc>(symbol->get_instance());
        vm.unsafe_set_gi(current);
        return current;
        };
      const auto id = [&](Symbol* symbol) {
        vm.push_reference(symbol);
        vm.unsafe_call_external(instanceId);
        return vm.pop_int();
        };
      const auto matches = [&](const Trophy& trophy) {
        auto* match = findSymbol(trophy.symbol);
        if(trophy.match==Match::Instance) {
          const auto expected = id(match);
          return expected==id(actor);
          }
        const auto expected = match->get_int();
        const auto current = member(trophy.match==Match::ActorSpecies ? actor : vm.global_self());
        return (current ? current->aivar[species] : 0)==expected;
        };
      const auto grant = [&](const Trophy& trophy) {
        for(const auto& item : trophy.grants) {
          vm.push_reference(actor);
          vm.push_int(item.item);
          vm.push_int(item.amount);
          vm.unsafe_call_external(create);
          }
        };
      const auto choose = [&](const Trophies& choices,const Symbol* randomizer) {
        for(const auto& trophy : choices) {
          bool chanceMatches = true;
          if(trophy.chance) {
            if(randomizer==nullptr)
              throw std::runtime_error("Invalid unconditional death trophy chance");
            const auto roll = randomizer->get_int();
            chanceMatches = trophy.chance->exact ? roll==trophy.chance->threshold : roll<=trophy.chance->threshold;
            }
          const bool targetMatches = matches(trophy);
          if(chanceMatches && targetMatches) {
            grant(trophy);
            return;
            }
          }
        };

      actor->set_instance(std::move(npc));
      if(const auto current = member(actor); current && current->aivar[given]==yes->get_int())
        return;
      for(const auto& learned : catalog.taught) {
        if(talent->get_int(learned.index)!=yes->get_int())
          continue;
        for(const auto& trophy : learned.trophies) {
          if(matches(trophy))
            grant(trophy);
          }
        }
      choose(catalog.unconditional,nullptr);
      // All three draws and their globals are observable even for non-monsters.
      for(const auto& roll : catalog.randomized) {
        vm.push_int(roll.bound);
        vm.unsafe_call_external(random);
        auto* result = findSymbol(roll.result);
        result->set_int(vm.pop_int());
        for(const auto& choices : roll.choices)
          choose(choices,result);
        }
      if(const auto current = member(actor))
        current->aivar[given] = yes->get_int();
    }},[this] {
    return activityEvents && !floorDefer.active() && ActivityDefinitionRuntime::mode()!=ActivityDefinitionRuntime::Mode::Off;
    });
  }

bool GameScript::applyArmorSetEffect(Npc& npc, uint32_t callback) {
  const auto it = itemDefinitions.armorSetEffects.find(callback);
  if(it==itemDefinitions.armorSetEffects.end())
    return false;
  if(npc.isPlayer()) {
    const auto& effect = it->second;
    auto* armor=std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(effect.armorFlag));
    const auto* belt=std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(effect.beltFlag));
    if(armor==nullptr || belt==nullptr)
      throw std::runtime_error("missing armor set runtime state");
    armor->values.front()=effect.equip ? 1 : 0;
    if(belt->values.front()==1) {
      const auto amount=itemRuntimeInt("BA_BONUS01");
      const auto delta = effect.equip ? amount : -amount;
      for(const auto protection : {PROT_EDGE,PROT_BLUNT,PROT_POINT,PROT_MAGIC,PROT_FIRE})
        npc.changeProtection(protection,npc.protection(protection)+delta);
      }
    }
  return true;
  }

bool GameScript::applyFightSkillItemEffect(Npc& npc, uint32_t callback) {
  const auto it = itemDefinitions.fightSkillEffects.find(callback);
  if(it==itemDefinitions.fightSkillEffects.end())
    return false;
  const auto& effect = it->second;
  if(effect.playerOnly && !npc.isPlayer())
    return true;
  using Value=NpcHelperDefinitions::Value;
  const std::array arguments={Value{.kind=Value::Kind::Self},Value{.integer=effect.talent},Value{.integer=effect.delta}};
  const auto id=storyScriptFunctionId("B_ADDFIGHTSKILL").value();
  return invokeNpcHelper(std::static_pointer_cast<daedalus::INpc>(npc.handlePtr()),id,arguments);
  }

bool GameScript::applyEquipmentStatEffect(Npc& npc, uint32_t callback) {
  const auto it = itemDefinitions.equipmentStatEffects.find(callback);
  if(it==itemDefinitions.equipmentStatEffects.end())
    return false;
  const auto actor = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto& effect = it->second;
  for(const auto& change : effect.changes) {
    const auto amount = uint32_t(change.amount);
    const auto delta = effect.equip ? amount : 0u-amount;
    if(const auto* index = std::get_if<Protection>(&change.stat)) {
      auto& stat=actor->protection[*index];
      stat=std::bit_cast<int32_t>(uint32_t(stat)+delta);
      }
    else if(const auto* write = std::get_if<ItemDefinitionRuntime::EquipmentAttributeWrite>(&change.stat)) {
      auto& stat=actor->attribute[write->attribute];
      const bool belowMinimum=!effect.equip && write->minimum && int64_t(stat)-int64_t(amount)<*write->minimum;
      stat=belowMinimum ? *write->minimum : std::bit_cast<int32_t>(uint32_t(stat)+delta);
      }
    else {
      npc_changeattribute(actor,std::get<Attribute>(change.stat),std::bit_cast<int32_t>(delta));
      }
    }
  return true;
  }

bool GameScript::applyQuestEquipmentEffect(std::string_view item, bool equip) {
  const bool chargedEye = item=="INNOSEYE";
  const bool depletedEye = equip && item=="ITMI_INNOSEYE_DISCHARGED_MIS";
  const bool deathAmulet = item=="ITAM_AMULETTOFDEATH_MIS";
  const bool ranger = item=="ITRI_RANGER_ADDON";
  const bool orcElite = item=="ORCELITERING";
  if(!chargedEye && !depletedEye && !deathAmulet && !ranger && !orcElite)
    return false;

  const auto self=std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("SELF"));
  const auto hero=std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("HERO"));
  const auto yes=itemRuntimeInt("TRUE"), no=itemRuntimeInt("FALSE");
  const auto play=[&](std::string_view visual) {
    wld_playeffect(itemRuntimeText(visual),self,self,0,0,0,no);
    };
  const auto cue=[&](std::string_view sound) { snd_play(itemRuntimeText(sound)); };
  const auto animate=[&](std::string_view animation) {
    ai_playani(self,itemRuntimeText(animation));
    ai_standup(self);
    };
  if(orcElite) {
    npc_changeattribute(self,ATR_STRENGTH,equip ? -20 : 20);
    if(equip)
      Gothic::inst().onPrint(itemRuntimeText("PRINT_ORCELITERINGEQUIP"));
    return true;
    }
  if(ranger) {
    if(hlp_getinstanceid(self)!=hlp_getinstanceid(hero))
      return true;
    if(equip) {
      if(itemRuntimeInt("ENTERED_ADDONWORLD")==no &&
         npc_hasitems(hero,int32_t(findItemInstance("ITRI_RANGER_ADDON").value()))==no)
        setItemRuntimeInt("RANGERRINGISLARESRING",yes);
      setItemRuntimeInt("SCISWEARINGRANGERRING",yes);
      Gothic::inst().onPrint(itemRuntimeText("PRINT_ADDON_SCISWEARINGRANGERRING"));
      } else {
      if(itemRuntimeInt("SC_ISRANGER")==no)
        setItemRuntimeInt("SCISWEARINGRANGERRING",no);
      setItemRuntimeInt("RANGERRINGISLARESRING",no);
      }
    return true;
    }
  if(depletedEye) {
    play("ITEM_EYE_DISCHARGED_VISUAL");
    cue("ITEM_EYE_DISCHARGED_SOUND");
    return true;
    }
  if(chargedEye && equip) {
    play("ITEM_EYE_CHARGED_VISUAL");
    play("ITEM_EYE_CHARGED_GLOW");
    play("ITEM_EYE_CHARGED_QUAKE");
    animate("ITEM_EYE_CHARGED_ANIMATION");
    cue("ITEM_EYE_CHARGED_SOUND");
    }
  static constexpr std::pair<Protection,const char*> fields[] = {
    {PROT_EDGE,"AM_EYEPROTEDGE"}, {PROT_BLUNT,"AM_EYEPROTEDGE"}, {PROT_POINT,"AM_EYEPROTPOINT"},
    {PROT_FIRE,"AM_EYEPROTFIRE"}, {PROT_MAGIC,"AM_EYEPROTMAGE"},
    };
  for(const auto& [index,eyeBonus] : fields) {
    const auto amount=chargedEye ? uint32_t(itemRuntimeInt(eyeBonus)) : 30u;
    auto& protection=self->protection[index];
    const auto current=uint32_t(protection);
    protection=std::bit_cast<int32_t>(equip ? current+amount : current-amount);
    }
  if(chargedEye && equip)
    ai_printscreen(itemRuntimeText("ITEM_EYE_CHARGED_MESSAGE"),-1,-1,itemRuntimeText("FONT_SCREEN"),0);
  if(deathAmulet && equip) {
    play("ITEM_DEATH_AMULET_LIGHT");
    play("ITEM_DEATH_AMULET_QUAKE");
    play("ITEM_DEATH_AMULET_VISUAL");
    animate("ITEM_DEATH_AMULET_ANIMATION");
    cue("ITEM_DEATH_AMULET_SOUND");
    }
  return true;
  }

void GameScript::applyWispEquipmentEffect(bool equip) {
  const auto name=equip ? "EQUIP_WISPDETECTOR.DETWSP" : "UNEQUIP_WISPDETECTOR.DETWSP";
  const auto id=storyNpcId("WISP_DETECTOR").value();
  const auto no=itemRuntimeInt("FALSE");
  if(equip && itemRuntimeInt("EQUIP_WISPDETECTOR_ONETIME")==no) {
    const auto yes=itemRuntimeInt("TRUE");
    setItemRuntimeInt("PLAYER_TALENT_WISPDETECTOR",yes);
    setItemRuntimeInt("WISPSEARCHING",itemRuntimeInt("WISPSEARCH_NF"));
    setItemRuntimeInt("EQUIP_WISPDETECTOR_ONETIME",yes);
    }
  const auto wisp=hlp_getnpc(int32_t(id));
  storyActorSlot(name)=wisp;
  if(equip) {
    ai_teleport(wisp,itemRuntimeText("ITEM_WISP_SPAWN_WAYPOINT"));
    const auto self=std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("SELF"));
    wld_spawnnpcrange(self,int32_t(id),1,500.f);
    const auto spawned=hlp_getnpc(int32_t(id));
    wld_playeffect(itemRuntimeText("ITEM_WISP_VISUAL"),spawned,spawned,0,0,0,no);
    snd_play(itemRuntimeText("ITEM_WISP_EQUIP_SOUND"));
    } else {
    if(!npc_isdead(wisp))
      snd_play(itemRuntimeText("ITEM_WISP_UNEQUIP_SOUND"));
    ai_teleport(wisp,itemRuntimeText("ITEM_WISP_REMOVE_WAYPOINT"));
    const auto [self,other]=storyActors();
    const auto localId=runtimeState.id(name).value();
    const auto removed=callStoryFunction("B_REMOVENPC",self,other,{int32_t(localId)});
    if(!removed.ok)
      throw std::runtime_error("cannot remove Wisp detector: "+removed.error);
    ai_teleport(wisp,itemRuntimeText("ITEM_WISP_RETURN_WAYPOINT"));
    }
  }

bool GameScript::applyStatefulEquipmentEffect(Npc& npc, std::string_view callback) {
  const bool equip = callback.starts_with("EQUIP_");
  if(!equip && !callback.starts_with("UNEQUIP_"))
    return false;
  const auto item = callback.substr(equip ? 6 : 8);
  if(equip && item=="ITMI_INNOSEYE_BROKEN_MIS") {
    const auto actor=std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
    wld_playeffect("spellFX_Fear",actor,actor,0,0,0,itemRuntimeInt("FALSE"));
    snd_play("MFX_FEAR_CAST");
    return true;
    }
  if(item=="WISPDETECTOR") {
    applyWispEquipmentEffect(equip);
    return true;
    }
  static constexpr struct Belt {
    std::string_view item;
    const char* equipped;
    const char* armor;
    } belts[] = {
      {"ITBE_ADDON_LEATHER_01", "LEATHER01_EQUIPPED", "LEATHERARMOR_EQUIPPED"},
      {"ITBE_ADDON_SLD_01",     "SLD01_EQUIPPED",     "SLDARMOR_EQUIPPED"},
      {"ITBE_ADDON_NOV_01",     "NOV01_EQUIPPED",     "NOVARMOR_EQUIPPED"},
      {"ITBE_ADDON_MIL_01",     "MIL01_EQUIPPED",     "MILARMOR_EQUIPPED"},
      {"ITBE_ADDON_KDF_01",     "KDF01_EQUIPPED",     "KDFARMOR_EQUIPPED"},
      {"ITBE_ADDON_MC",         "MC_EQUIPPED",        "MCARMOR_EQUIPPED"},
      };
  static constexpr struct ArtifactSet {
    std::array<std::string_view,3> items;
    const char* prefix;
    const char* visual;
    const char* sound;
    std::optional<Attribute> maximum;
    } sets[] = {
      {{"ITAM_ADDON_HEALTH", "ITRI_ADDON_HEALTH_01", "ITRI_ADDON_HEALTH_02"}, "HP", "ITEM_HEALTH_SET_VISUAL", "ITEM_HEALTH_SET_SOUND", ATR_HITPOINTSMAX},
      {{"ITAM_ADDON_MANA",   "ITRI_ADDON_MANA_01",   "ITRI_ADDON_MANA_02"  }, "MA", "ITEM_MANA_SET_VISUAL", "ITEM_MANA_SET_SOUND", ATR_MANAMAX},
      {{"ITAM_ADDON_STR",    "ITRI_ADDON_STR_01",    "ITRI_ADDON_STR_02"   }, "STR","ITEM_STRENGTH_SET_VISUAL", "ITEM_STRENGTH_SET_SOUND", std::nullopt},
      };
  const auto belt = std::ranges::find(belts,item,&Belt::item);
  const auto set = std::ranges::find_if(sets,[&](const ArtifactSet& entry) {
    return std::ranges::find(entry.items,item)!=entry.items.end();
    });
  const bool manaStaff = item=="ZAUBERSTAB";
  if(belt==std::end(belts) && set==std::end(sets) && !manaStaff)
    return applyQuestEquipmentEffect(item,equip);

  const auto actor = std::static_pointer_cast<daedalus::INpc>(npc.handlePtr());
  const auto read = [&](std::string_view name) { return itemRuntimeInt(name); };
  const auto yes = read("TRUE");
  const auto no = read("FALSE");
  const auto add = [](int32_t& field, uint32_t delta) {
    field=std::bit_cast<int32_t>(uint32_t(field)+delta);
    };
  if(manaStaff) {
    if(!npc_isplayer(actor))
      return true;
    npc_changeattribute(actor,ATR_MANAMAX,equip ? 20 : -20);
    if(equip) {
      npc_changeattribute(actor,ATR_MANA,20);
      } else {
      if(actor->attribute[ATR_MANA]>=20)
        npc_changeattribute(actor,ATR_MANA,-20);
      else
        actor->attribute[ATR_MANA]=0;
      }
    return true;
    }
  if(belt!=std::end(belts)) {
    const bool leather = belt==std::begin(belts);
    const auto flag=belt->equipped;
    if(!leather)
      setItemRuntimeInt(flag,equip ? yes : no);
    const auto protect = [&](Protection protection, const char* amount) {
      const auto delta = uint32_t(read(amount));
      add(actor->protection[protection],equip ? delta : 0u-delta);
      };
    for(const auto protection : {PROT_EDGE,PROT_BLUNT,PROT_POINT})
      protect(protection,"BELT_PROT_01");
    if(leather) {
      if(!npc_isplayer(actor))
        return true;
      setItemRuntimeInt(flag,equip ? yes : no);
      }
    if(read(belt->armor)==yes) {
      for(const auto protection : {PROT_EDGE,PROT_BLUNT,PROT_POINT})
        protect(protection,"BA_BONUS01");
      const bool monasteryEquip = equip && item=="ITBE_ADDON_MC";
      for(const auto protection : {PROT_MAGIC,PROT_FIRE})
        protect(protection,monasteryEquip ? "BA_BONUS01" : "BA_BONUS02");
      }
    return true;
    }

  const std::string prefix(set->prefix);
  const auto amulet = prefix+"_AMULETT_EQUIPPED";
  const auto firstRing = prefix+"_RING_1_EQUIPPED";
  const auto secondRing = prefix+"_RING_2_EQUIPPED";
  const auto value="C_"+prefix+"_ARTEFAKTVALUE."+prefix+"_ARTEFAKT_VALUE";
  const auto effectFlag = prefix+"_ARTEFAKT_EFFEKT";
  const auto calculateBonus = [&] {
    setItemRuntimeInt(value,0);
    const bool hasAmulet = read(amulet)==yes;
    const int rings = int(read(firstRing)==yes) + int(read(secondRing)==yes);
    if(hasAmulet && rings==2 && read(effectFlag)==no) {
      wld_playeffect(itemRuntimeText(set->visual),actor,actor,0,0,0,no);
      snd_play(itemRuntimeText(set->sound));
      setItemRuntimeInt(effectFlag,yes);
      }
    static constexpr const char* ringBonuses[] = {nullptr,"_RING_SOLO_BONUS","_RING_DOUBLE_BONUS"};
    static constexpr const char* amuletBonuses[] = {"_AMULETT_SOLO_BONUS","_AMULETT_EINRING_BONUS","_AMULETT_ARTEFAKT_BONUS"};
    const auto* bonus = hasAmulet ? amuletBonuses[rings] : ringBonuses[rings];
    if(bonus!=nullptr)
      setItemRuntimeInt(value,read(prefix+bonus));
    return read(value);
    };

  const auto previous=std::string(callback)+".OLDVALUE";
  const auto current=std::string(callback)+".NEWVALUE";
  setItemRuntimeInt(previous,calculateBonus());
  const auto& flag = item==set->items[0] ? amulet : (item==set->items[1] ? firstRing : secondRing);
  setItemRuntimeInt(flag,equip ? yes : no);
  setItemRuntimeInt(current,calculateBonus());
  const auto delta = uint32_t(read(current))-uint32_t(read(previous));
  if(set->maximum) {
    add(actor->attribute[*set->maximum],delta);
    } else {
    add(actor->protection[PROT_EDGE],delta);
    add(actor->protection[PROT_BLUNT],delta);
    }
  return true;
  }

namespace {
enum class Kind { HoldItem, Brush, Eat, Drink, Joint, Spit, Wash, Hammer };

struct ItemRoutine {
  std::string_view state, point, item;
  Kind             kind;
  int32_t          enterState;
  int32_t          timerSeconds = -1;
  };

constexpr ItemRoutine itemRoutines[] = {
  {"ZS_PLAY_LUTE",      "STAND", "ITMI_LUTE",       Kind::HoldItem, 1},
  {"ZS_RAKE_FP",        "PICK",  "ITMI_RAKE",       Kind::HoldItem, 1},
  {"ZS_SMOKE_JOINT",    "STAND", "ITMI_JOINT",      Kind::Joint,    0, 5},
  {"ZS_SPIT_FIRE",      "STAND", "ITLSTORCHFIRESPIT", Kind::Spit,   0, 10},
  {"ZS_STAND_DRINKING", "STAND", "ITFO_BOOZE",      Kind::Drink,    0, 7},
  {"ZS_STAND_EATING",   "STAND", {},                Kind::Eat,      0, 5},
  {"ZS_STAND_SWEEPING", "SWEEP", "ITMI_BROOM",      Kind::HoldItem, 1},
  {"ZS_SWEEP_FP",       "SWEEP", "ITMI_BRUSH",      Kind::Brush,    1},
  {"ZS_WASH_FP",        "WASH",  {},                Kind::Wash,     0},
  {"ZS_GUARD_HAMMER",   "STAND", {},                Kind::Hammer,   0, 5},
  };

constexpr std::array<std::string_view,4> food={"ITFO_APPLE","ITFO_CHEESE","ITFO_BACON","ITFOMUTTON"};
constexpr std::array<std::string_view,4> eatingGesture={
  "T_FOOD_RANDOM_1","T_FOODHUGE_RANDOM_1","T_MEAT_RANDOM_1","T_MEAT_RANDOM_1"
  };
}

int32_t GameScript::giveInventoryItems(const std::shared_ptr<daedalus::INpc>& giver,
                                       const std::shared_ptr<daedalus::INpc>& taker, int32_t item, int32_t amount) {
  inventoryTransferVariables.amount = amount;
  inventoryTransferVariables.item = item;
  storyActorSlot("B_GIVEINVITEMS.TAKER")=taker;
  storyActorSlot("B_GIVEINVITEMS.GIVER")=giver;
  const auto held=std::dynamic_pointer_cast<daedalus::IItem>(storyActorSlot("ITEM"));
  const auto transfer = InventoryTransfer::run(findNpc(giver),findNpc(taker),uint32_t(item),amount,
                                              held ? std::string_view(held->name) : std::string_view{},inventoryTransferText);
  if(transfer.itemChanged)
    storeItem(transfer.item);
  if(transfer.itemName)
    inventoryTransferVariables.itemName = *transfer.itemName;
  if(transfer.notice) {
    inventoryTransferVariables.notice = transfer.notice->text;
    ai_printscreen(transfer.notice->text,-1,transfer.notice->y,inventoryTransferFont,2);
    }
  return transfer.value;
  }

bool GameScript::runItemRoutine(Npc& npc, std::string_view name, int& result) {
  using Phase=ActivityDefinitions::Phase;
  auto phase=Phase::Init;
  if(name.ends_with("_LOOP")) {
    phase=Phase::Loop;
    name.remove_suffix(5);
    }
  else if(name.ends_with("_END")) {
    phase=Phase::End;
    name.remove_suffix(4);
    }
  const auto it=std::find_if(std::begin(itemRoutines),std::end(itemRoutines),[name](const auto& activity) {
    return activity.state==name;
    });
  if(it==std::end(itemRoutines))
    return false;
  const auto& a=*it;
  result=LOOP_CONTINUE;
  // The position and meal choice are already saved in the NPC record.
  auto& foodChoice=npc.handle().aivar[18];
  auto& stage=npc.handle().aivar[19];
  constexpr auto held=0, enter=1, walking=2;
  const auto itemName=[&]() -> std::string_view {
    if(a.kind!=Kind::Eat)
      return a.item;
    return foodChoice>=0 && size_t(foodChoice)<food.size() ? food[size_t(foodChoice)] : std::string_view{};
    };
  const auto roll=[&](uint32_t bound) {
    const auto value=rollRandom(bound,&npc,true);
    if(auto* local=findSymbol(std::string(name)+".RANDOM"))
      local->set_int(int32_t(value));
    return value;
    };
  const auto animate=[&](std::string_view animation) {
    npc.aiPush(AiQueue::aiPlayAnimBs(animation,BS_ITEMINTERACT));
    };
  const auto use=[&](std::string_view item, int32_t state) {
    npc.aiPush(AiQueue::aiUseItemToState(int32_t(findSymbolIndex(item)),state));
    };
  if(phase==Phase::Init) {
    const auto ensureItem=[&](std::string_view item) {
      const auto id=uint32_t(findSymbolIndex(item));
      if(npc.itemCount(id)==0)
        storeItem(npc.addItem(id,1));
      };
    NpcActivityHost host(world(),npc,[this](std::string_view) {
      setPerceptionPreset(true);
      });
    ActivityDefinitions::resetForActivity(host,"PERCEPTION_SET_NORMAL");
    host.setWalkMode(1);
    const auto& wp=npc.handle().wp;
    if(a.kind==Kind::Brush ? host.nearestWaypoint()!=wp : host.distanceToWaypoint(wp)>500)
      host.gotoWaypoint(wp);
    if(a.kind==Kind::Eat) {
      stage=enter;
      const auto choice=roll(uint32_t(food.size()));
      ensureItem(food[choice]);
      foodChoice=int32_t(choice);
      }
    else {
      if(!a.item.empty())
        ensureItem(a.item);
      if(a.kind!=Kind::Brush && a.kind!=Kind::Spit)
        stage=enter;
      }
    return true;
    }
  if(phase==Phase::End) {
    if(a.kind==Kind::Wash)
      npc.aiPush(AiQueue::aiPlayAnim("T_WASH_2_STAND"));
    else if(a.kind==Kind::Hammer)
      npc.aiPush(AiQueue::aiPlayAnim("T_HGUARD_2_STAND"));
    else {
      if(a.kind==Kind::Eat)
        animate("T_POTION_RANDOM_2");
      if(const auto item=itemName(); !item.empty())
        use(item,-1);
      }
    return true;
    }
  const bool onPoint=a.kind!=Kind::Spit && npc.isOnFreePoint(a.point);
  if(onPoint) {
    npc.aiPush(AiQueue::aiAlignToFp());
    if(stage==walking)
      stage=enter;
    }
  else if(const auto* point=world().findFreePoint(npc,a.point)) {
    npc.aiPush(AiQueue::aiGoToPoint(*point));
    if(a.kind!=Kind::Spit)
      npc.aiPush(AiQueue::aiStandup());
    npc.aiPush(AiQueue::aiAlignToFp());
    if(a.kind!=Kind::Spit)
      stage=walking;
    }
  else {
    npc.aiPush(AiQueue::aiAlignToWp());
    if(a.kind!=Kind::Spit && stage==walking)
      stage=enter;
    }

  if(a.kind!=Kind::Spit && stage==enter) {
    if(a.kind==Kind::Wash) {
      if(onPoint) {
        npc.aiPush(AiQueue::aiAlignToFp());
        npc.aiPush(AiQueue::aiStandup());
        npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_WASH"));
        stage=held;
        }
      }
    else if(a.kind==Kind::Hammer) {
      npc.aiPush(AiQueue::aiPlayAnim("T_STAND_2_HGUARD"));
      stage=held;
      }
    else if(const auto item=itemName(); !item.empty()) {
      use(item,a.enterState);
      if(a.kind==Kind::Eat && foodChoice==0)
        animate("T_FOOD_RANDOM_2");
      stage=held;
      }
    }
  if(a.kind==Kind::Hammer && NpcEngineCalls::distanceToWaypoint(world(),npc,"NW_MONASTERY_SANCTUM_01")<=500) {
    const auto hammer=int32_t(findSymbolIndex("HOLY_HAMMER_MIS"));
    if(npc.itemCount(uint32_t(hammer))==1) {
      npc.delItem(uint32_t(hammer),1);
      wld_insertitem(hammer,"FP_HAMMER");
      }
    }
  if(a.timerSeconds<0 || (a.kind!=Kind::Spit && stage!=held) || int32_t(npc.stateTime()/1000)<=a.timerSeconds)
    return true;

  switch(a.kind) {
    case Kind::Eat:
      animate(foodChoice>=0 && size_t(foodChoice)<eatingGesture.size() ? eatingGesture[size_t(foodChoice)] : eatingGesture[1]);
      break;
    case Kind::Drink: {
      const auto value=roll(10);
      if(value==0)
        animate("T_POTION_RANDOM_3");
      animate("T_POTION_RANDOM_1");
      if(value==1)
        animate("T_POTION_RANDOM_2");
      break;
      }
    case Kind::Joint:
      animate("T_JOINT_RANDOM_1");
      break;
    case Kind::Spit:
      use(a.item,5);
      use(a.item,0);
      break;
    case Kind::Hammer:
      if(roll(10)==0)
        npc.aiPush(AiQueue::aiPlayAnim("T_HGUARD_LOOKAROUND"));
      break;
    default:
      break;
    }
  npc.setStateTime(0);
  return true;
  }

namespace {
void refreshSmithWeaponDescriptions(daedalus::DaedalusVm& vm) {
  struct Recipe {
    std::string_view info, title, cost = {};
    };
  static constexpr Recipe recipes[] = {
    {"PC_ITMW_1H_COMMON",       "NAME_ITMW_1H_COMMON_01"},
    {"PC_ITMW_1H_SPECIAL_01",   "NAME_ITMW_1H_SPECIAL_01", "PRINT_SMITH_1H_SPECIAL_01"},
    {"PC_ITMW_2H_SPECIAL_01",   "NAME_ITMW_2H_SPECIAL_01", "PRINT_SMITH_2H_SPECIAL_01"},
    {"PC_ITMW_1H_SPECIAL_02",   "NAME_ITMW_1H_SPECIAL_02", "PRINT_SMITH_1H_SPECIAL_02"},
    {"PC_ITMW_2H_SPECIAL_02",   "NAME_ITMW_2H_SPECIAL_02", "PRINT_SMITH_2H_SPECIAL_02"},
    {"PC_ITMW_1H_SPECIAL_03",   "NAME_ITMW_1H_SPECIAL_03", "PRINT_SMITH_1H_SPECIAL_03"},
    {"PC_ITMW_2H_SPECIAL_03",   "NAME_ITMW_2H_SPECIAL_03", "PRINT_SMITH_2H_SPECIAL_03"},
    {"PC_ITMW_1H_SPECIAL_04",   "NAME_ITMW_1H_SPECIAL_04", "PRINT_SMITH_1H_SPECIAL_04"},
    {"PC_ITMW_2H_SPECIAL_04",   "NAME_ITMW_2H_SPECIAL_04", "PRINT_SMITH_2H_SPECIAL_04"},
    {"PC_WEAPON_1H_HARAD_01",   "NAME_ADDON_HARAD_01"},
    {"PC_WEAPON_1H_HARAD_02",   "NAME_ADDON_HARAD_02"},
    {"PC_WEAPON_1H_HARAD_03",   "NAME_ADDON_HARAD_03"},
    {"PC_WEAPON_1H_HARAD_04",   "NAME_ADDON_HARAD_04"},
    };
  auto* description=vm.find_symbol_by_name("C_INFO.DESCRIPTION");
  for(const auto& recipe:recipes) {
    const auto info=vm.find_symbol_by_name(recipe.info)->get_instance();
    const auto& title=vm.find_symbol_by_name(recipe.title)->get_string();
    if(recipe.cost.empty()) {
      vm.set_string(info,description,0,title);
      } else {
      std::string text=title;
      text+=vm.find_symbol_by_name(recipe.cost)->get_string();
      vm.set_string(info,description,0,text);
      }
    }
  }
}

void GameScript::runStockPickLock(int32_t success, int32_t brokenOrOpen) {
  setItemRuntimeInt("G_PICKLOCK.BBROKENOPEN",brokenOrOpen);
  setItemRuntimeInt("G_PICKLOCK.BSUCCESS",success);
  static constexpr std::pair<std::string_view,std::string_view> feedback[] = {
    {"HERO_LOCK_FAILURE_SOUND", "PRINT_PICKLOCK_FAILURE"},
    {"HERO_LOCK_BROKEN_SOUND", "PRINT_PICKLOCK_BROKEN"},
    {"HERO_LOCK_SUCCESS_SOUND", "PRINT_PICKLOCK_SUCCESS"},
    {"HERO_LOCK_UNLOCK_SOUND", "PRINT_PICKLOCK_UNLOCK"},
    };
  const auto& [sound,message]=feedback[2*size_t(success!=0)+size_t(brokenOrOpen!=0)];
  snd_play3d(std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("SELF")),itemRuntimeText(sound));
  Gothic::inst().onPrint(itemRuntimeText(message));
  if(!success && brokenOrOpen) {
    const auto roll=hlp_random(100);
    setItemRuntimeInt("G_PICKLOCK.RND",roll);
    if(roll<=25) {
      const auto hero=std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("HERO"));
      npc_sendpassiveperc(hero,PERC_ASSESSQUIETSOUND,hero,hero);
      }
    }
  return;
  }

namespace {

void giveMobXp(daedalus::DaedalusVm& vm, const char* amount) {
  vm.push_reference(vm.find_symbol_by_name(amount));
  vm.unsafe_call_bl(vm.find_symbol_by_name("B_GIVEPLAYERXP"));
  }

struct MayaBook {
  std::string_view callback;
  const char* studied;
  int32_t art;
  };

constexpr MayaBook mayaBooks[] = {
  {"USE_BOOKSTANDMAYAHIERCHARY_01_S1","BOOKSTANDMAYAHIERCHARY_1_PERMANENT",1},
  {"USE_BOOKSTANDMAYAHIERCHARY_02_S1","BOOKSTANDMAYAHIERCHARY_2_PERMANENT",2},
  {"USE_BOOKSTANDMAYAHIERCHARY_03_S1","BOOKSTANDMAYAHIERCHARY_3_PERMANENT",3},
  {"USE_BOOKSTANDMAYAHIERCHARY_04_S1","BOOKSTANDMAYAHIERCHARY_4_PERMANENT",4},
  {"USE_BOOKSTANDMAYAHIERCHARY_05_S1","BOOKSTANDMAYAHIERCHARY_5_PERMANENT",5},
  };

void readMayaBookForNpc(daedalus::DaedalusVm& vm, const MayaBook& book,
                        const std::shared_ptr<daedalus::INpc>& hero) {
  const auto context=vm.unsafe_get_gi();
  auto* art=vm.find_symbol_by_name("BOOKSTANDMAYAART");
  vm.set_int(context,art,0,book.art);
  vm.find_symbol_by_name("INITUSE_BOOKSTANDMAYA.HER")->set_instance(hero);
  vm.set_int(context,art,0,0);

  auto* studied=vm.find_symbol_by_name(book.studied);
  if(studied->get_int()!=vm.find_symbol_by_name("FALSE")->get_int())
    return;
  const auto* languages=vm.find_symbol_by_name("PLAYER_TALENT_FOREIGNLANGUAGE");
  const auto known=vm.find_symbol_by_name("TRUE")->get_int();
  for(uint16_t i:{uint16_t(0),uint16_t(1),uint16_t(2)}) {
    if(languages->get_int(i)==known) {
      giveMobXp(vm,"XP_AMBIENT");
      vm.set_int(context,studied,0,known);
      return;
      }
    }
  }

struct MobSpawn {
  const char* npc;
  const char* waypointLiteral;
  };

void spawnAtMob(daedalus::DaedalusVm& vm, const MobSpawn& spawn) {
  vm.push_int(int32_t(vm.find_symbol_by_name(spawn.npc)->index()));
  vm.push_reference(vm.find_symbol_by_name(spawn.waypointLiteral));
  vm.unsafe_call_external(vm.find_symbol_by_name("WLD_INSERTNPC"));
  }

void runOldCampGateAttack(daedalus::DaedalusVm& vm, daedalus::DaedalusSymbol* opened, int32_t truth) {
  static constexpr MobSpawn attackers[] = {
    // OC_TO_GUARD
    {"ORCELITE_ROAM","$52091"},
    {"ORCWARRIOR_ROAM","$52092"},
    {"ORCELITE_ROAM","$52093"},
    {"ORCWARRIOR_ROAM","$52094"},
    {"ORCSHAMAN_SIT","$52095"},
    {"ORCELITE_ROAM","$52096"},
    // OC_GUARD_ENTRANCE
    {"WARG","$52097"},
    {"ORCELITE_ROAM","$52098"},
    {"WARG","$52099"},
    {"ORCELITE_ROAM","$52100"},
    // OC_TRAIN_03
    {"ORCWARRIOR_ROAM","$52101"},
    {"ORCELITE_ROAM","$52102"},
    {"ORCWARRIOR_ROAM","$52103"},
    {"ORCELITE_ROAM","$52104"},
    // OC_TRAIN_04
    {"ORCWARRIOR_ROAM","$52105"},
    {"ORCELITE_ROAM","$52106"},
    // OC_CAMPFIRE_OUT_01
    {"ORCWARRIOR_ROAM","$52107"},
    {"WARG","$52108"},
    {"WARG","$52109"},
    {"ORCELITE_ROAM","$52110"},
    // OC_CENTER_02
    {"ORCWARRIOR_ROAM","$52111"},
    {"ORCELITE_ROAM","$52112"},
    // OC_CENTER_03
    {"ORCWARRIOR_ROAM","$52113"},
    {"ORCELITE_ROAM","$52114"},
    // OC_KITCHEN_ENTRANCE
    {"ORCWARRIOR_ROAM","$52115"},
    {"ORCELITE_ROAM","$52116"},
    {"ORCSHAMAN_SIT","$52117"},
    {"ORCELITE_ROAM","$52118"},
    // OC_EBR_HALL_THRONE
    {"ORCWARRIOR_ROAM","$52119"},
    {"ORCSHAMAN_SIT","$52120"},
    {"WARG","$52121"},
    };
  for(const auto& spawn:attackers)
    spawnAtMob(vm,spawn);

  static constexpr const char* victims[] = {
    "VLK_4143_HAUPTTORWACHE","VLK_4145_WAFFENKNECHT","VLK_4146_WAFFENKNECHT",
    "VLK_4147_WAFFENKNECHT","VLK_4101_WAFFENKNECHT","VLK_4102_WAFFENKNECHT",
    "VLK_4103_WAFFENKNECHT","VLK_4104_WAFFENKNECHT","VLK_4105_WAFFENKNECHT",
    "VLK_4108_ENGOR",
    };
  const auto symbol=[&](std::string_view name) { return vm.find_symbol_by_name(name); };
  for(const auto* name:victims) {
    vm.push_int(int32_t(symbol(name)->index()));
    vm.unsafe_call_bl(symbol("B_KILLNPC"));
    }

  RestoreContext restore{vm,vm.unsafe_get_gi()};
  auto* flags=symbol("C_NPC.FLAGS");
  const auto setFlags=[&](const char* name,int32_t value) {
    const auto npc=symbol(name)->get_instance();
    vm.unsafe_set_gi(npc);
    vm.set_int(npc,flags,0,value);
    };
  setFlags("GAROND",0);
  setFlags("BRUTUS",symbol("NPC_FLAG_IMMORTAL")->get_int());

  vm.push_reference(symbol("$52122")); // ORCATTACK.BIK
  const bool videoResult=vm.unsafe_call_external(symbol("PLAYVIDEO"));
  vm.set_int(vm.unsafe_get_gi(),opened,0,truth);
  if(videoResult)
    (void)vm.pop_int();
  }

bool runMobEvent(daedalus::DaedalusVm& vm, std::string_view name) {
  const auto symbol=[&](std::string_view name) { return vm.find_symbol_by_name(name); };
  const auto context=vm.unsafe_get_gi();
  const auto truth=symbol("TRUE")->get_int();
  const auto falsity=symbol("FALSE")->get_int();
  const auto mark=[&](daedalus::DaedalusSymbol* flag) { vm.set_int(context,flag,0,truth); };
  const auto sound=[&](const char* literal) {
    vm.push_reference(symbol(literal));
    vm.unsafe_call_external(symbol("SND_PLAY"));
    };

  // Stock waypoints are immutable literals in the verified program.
  static constexpr MobSpawn libraryDoor[] = {
    {"SKELETON","$52124"}, {"SKELETON","$52125"},
    {"SKELETON","$52126"}, {"DEMON","$52127"},
    {"DEMON","$52128"}, {"DEMONLORD","$52129"},
    };
  static constexpr MobSpawn chamberChest[] = {
    {"GOBBO_BLACK","$52176"}, {"GOBBO_BLACK","$52177"},
    {"GOBBO_BLACK","$52178"}, {"GOBBO_BLACK","$52179"},
    {"ORCWARRIOR_REST","$52180"},
    };
  static constexpr MobSpawn skeletonChest[] = {{"LESSER_SKELETON","$51976"}};
  static constexpr struct Trap {
    std::string_view callback;
    const char* flag;
    const char* sound;
    std::span<const MobSpawn> spawns;
    bool markBeforeSpawn;
    } traps[] = {
    {"EVT_OPEN_DOOR_LIBRARY_S1","EVT_OPEN_DOOR_LIBRARY_S1_ONETIME","$52123",libraryDoor,false},
    {"EVT_TRUHE_OW_01_S1","TRUHESFX","$52175",chamberChest,true},
    {"B_SPAWN_SKELETON_S1","SKELETT_SPAWN",nullptr,skeletonChest,false},
    };
  if(const auto* trap=std::ranges::find(traps,name,&Trap::callback); trap!=std::end(traps)) {
    auto* flag=symbol(trap->flag);
    if(flag->get_int()==falsity) {
      if(trap->sound)
        sound(trap->sound);
      if(trap->markBeforeSpawn)
        mark(flag);
      for(const auto& spawn:trap->spawns)
        spawnAtMob(vm,spawn);
      if(!trap->markBeforeSpawn)
        mark(flag);
      }
    return true;
    }

  if(name=="OPEN_ADANOSTEMPELCHEST_01_FUNC_S1" || name=="OPEN_ADANOSTEMPELCHEST_02_FUNC_S1") {
    const bool first=name=="OPEN_ADANOSTEMPELCHEST_01_FUNC_S1";
    auto* flag=symbol(first ? "OPEN_ADANOSTEMPELCHEST_01_FUNC_ONETIME" : "OPEN_ADANOSTEMPELCHEST_02_FUNC_ONETIME");
    if(flag->get_int()!=falsity)
      return true;
    static constexpr std::string_view firstGuardians[] = {"STONEGUARDIAN_TREASUREPITS_05C","STONEGUARDIAN_TREASUREPITS_05E"};
    static constexpr std::string_view secondGuardians[] = {
      "STONEGUARDIAN_TREASUREPITS_09A","STONEGUARDIAN_TREASUREPITS_09C","STONEGUARDIAN_TREASUREPITS_09E"
      };
    const auto guardians=first ? std::span<const std::string_view>(firstGuardians) : std::span<const std::string_view>(secondGuardians);
    for(auto guard:guardians) {
      vm.push_reference(symbol(guard));
      vm.unsafe_call_bl(symbol("B_AWAKE_STONEGUARDIAN"));
      }
    bool alive=false;
    for(auto guard=guardians.rbegin(); guard!=guardians.rend(); ++guard) {
      vm.push_reference(symbol(*guard));
      vm.unsafe_call_external(symbol("NPC_ISDEAD"));
      alive|=vm.pop_int()==falsity;
      }
    if(alive)
      sound(first ? "$52008" : "$52009");
    mark(flag);
    return true;
    }

  if(name=="EVT_OC_MAINGATE_FUNC_S1") {
    auto* flag=symbol("MIS_OCGATEOPEN");
    if(flag->get_int()==falsity)
      runOldCampGateAttack(vm,flag,truth);
    return true;
    }
  if(name=="EVT_ORKOBERST_SWITCH_S1") {
    mark(symbol("EVT_ORKOBERST_SWITCH_FOUND"));
    return true;
    }
  if(name=="EVT_VINOSKELLEREI_FUNC_S1") {
    auto* flag=symbol("FOUNDVINOSKELLEREI");
    if(flag->get_int()==falsity) {
      mark(flag);
      giveMobXp(vm,"XP_FOUNDVINOSKELLEREI");
      }
    return true;
    }

  if(name=="EVT_MONASTERY_SECRETLIBRARY_S1") {
    auto* flag=symbol("SECRETLIBRARYISOPEN");
    if(flag->get_int()==falsity) {
      giveMobXp(vm,"XP_OPENSECRETLIBRARY");
      mark(flag);
      for(const auto* point:{
          "$52074","$52075","$52076","$52077",
          "$52078","$52079","$52080","$52081",
          "$52082","$52083","$52084","$52085",
          "$52086","$52087","$52088"})
        spawnAtMob(vm,{"SKELETON",point});
      spawnAtMob(vm,{"SECRETLIBRARYSKELETON","$52089"});
      }
    // The original clears the message even after the one-time activation.
    vm.push_reference(symbol("$52090"));
    vm.push_int(-1);
    vm.push_int(-1);
    vm.push_reference(symbol("FONT_SCREEN"));
    vm.push_int(0);
    if(vm.unsafe_call_external(symbol("PRINTSCREEN")))
      (void)vm.pop_int();
    return true;
    }

  if(name=="GREGISBACK_S1") {
    auto* flag=symbol("GREGISBACK");
    if(flag->get_int()==falsity) {
      spawnAtMob(vm,{"PIR_1320_ADDON_GREG","$27605"});
      for(const auto& [npc,routine]:{std::pair{"BILL","$27606"},std::pair{"MORGAN","$27607"},
                                     std::pair{"ALLIGATORJACK","$27608"}}) {
        vm.push_reference(symbol(npc));
        vm.push_reference(symbol(routine));
        vm.unsafe_call_bl(symbol("B_STARTOTHERROUTINE"));
        }
      static constexpr MobSpawn canyon[] = {
        {"CANYONRAZOR07","$27609"}, {"CANYONRAZOR08","$27610"},
        {"CANYONRAZOR09","$27611"}, {"CANYONRAZOR10","$27612"},
        };
      for(const auto& spawn:canyon)
        spawnAtMob(vm,spawn);
      }
    mark(flag);
    return true;
    }
  return false;
  }

} // namespace

bool GameScript::invokeNativeMobState(const daedalus::DaedalusSymbol& symbol) {
  if(!nativeRoutineProgram || !activityEvents || floorDefer.active() ||
     ActivityDefinitionRuntime::mode()==ActivityDefinitionRuntime::Mode::Off)
    return false;

  const auto& name = symbol.name();
  int32_t result = 0;
  const bool preparedOnly = !vm.has_legacy_dispatcher();
  const auto runProgram = [&] { return invokeStockProgram(symbol,result,nullptr,preparedOnly); };
  if(name=="B_SCGETTREASURE_S1") {
    // Digging is conditional on the HERO global and changes nothing for other actors.
    return hlp_getinstanceid(vm.global_self()->get_instance())!=hlp_getinstanceid(vm.global_hero()->get_instance()) ||
           runProgram();
    }

  // Ordinary NPCs bind the hero without opening documents or production menus.
  // Sleep also checks the diagnostic Rockefeller instance. Smithing updates recipe captions.
  static constexpr std::string_view callbacks[] = {
    "SLEEPABIT_S1", "POTIONALCHEMY_S1", "MAKERUNE_S1", "GOLDHACKEN_S1", "PRAYIDOL_S1", "PRAYSHRINE_S1",
    "SMITHWEAPON_S1",
    "USE_BOOKSTAND_01_S1", "USE_BOOKSTAND_ADDON_BL_S1", "USE_BOOKSTANDDEMENTOR_S1",
    "USE_BOOKSTANDALCHEMY1_S1", "USE_BOOKSTANDALCHEMY2_S1", "USE_BOOKSTANDALCHEMY3_S1",
    "USE_BOOKSTANDANIMALS1_S1", "USE_BOOKSTANDANIMALS2_S1", "USE_BOOKSTANDANIMALS3_S1",
    "USE_BOOKSTANDHISTORY1_S1", "USE_BOOKSTANDHISTORY2_S1", "USE_BOOKSTANDHISTORY3_S1",
    "USE_BOOKSTAND_KREISE_01_S1", "USE_BOOKSTAND_KREISE_02_S1", "USE_BOOKSTAND_KREISE_03_S1",
    "USE_BOOKSTAND_KREISE_04_S1", "USE_BOOKSTAND_KREISE_05_S1", "USE_BOOKSTAND_KREISE_06_S1",
    "BOOKSTAND_MILTEN_01_S1", "BOOKSTAND_MILTEN_02_S1", "BOOKSTAND_MILTEN_03_S1", "BOOKSTAND_ENGOR_01_S1",
    "USE_FINALDRAGONEQUIPMENT_S1",
    };
  const auto book=std::ranges::find(mayaBooks,name,&MayaBook::callback);
  if(book==std::end(mayaBooks) && std::ranges::find(callbacks,name)==std::end(callbacks))
    return runMobEvent(vm,name) || (preparedOnly && runProgram());
  const auto self = hlp_getinstanceid(vm.global_self()->get_instance());
  const auto hero = hlp_getnpc(int32_t(findSymbolIndex("PC_HERO")));
  if(self==hlp_getinstanceid(hero))
    return runProgram();
  if(book!=std::end(mayaBooks)) {
    readMayaBookForNpc(vm,*book,hero);
    return true;
    }
  if(name=="SLEEPABIT_S1") {
    const auto rock = hlp_getnpc(int32_t(findSymbolIndex("PC_ROCKEFELLER")));
    if(self==hlp_getinstanceid(rock))
      return preparedOnly && runProgram();
    findSymbol("SLEEPABIT_S1.HER")->set_instance(hero);
    findSymbol("SLEEPABIT_S1.ROCK")->set_instance(rock);
    } else {
    findSymbol(name+".HER")->set_instance(hero);
    }
  if(name=="SMITHWEAPON_S1")
    refreshSmithWeaponDescriptions(vm);
  return true;
  }
