#include "gamescript.h"

#include "gamescript_inventorytransfer.h"

#include "game/definitions/basescripts.h"
#include "game/definitions/nativebasecatalog.h"
#include "gothic.h"
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

void GameScript::loadNativeDeathTrophies() {
  using namespace DeathTrophyDefinitions;
  nativeDeathTrophies=load(Resources::baseDir());
  if(!nativeDeathTrophies)
    throw std::runtime_error("missing native death trophy definitions");

  const auto& catalog=*baseNativeCatalog();
  const auto* talent=std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find("PLAYER_TALENT_TAKEANIMALTROPHY"));
  if(talent==nullptr)
    throw std::runtime_error("missing trophy talent state");
  const auto check=[&](const Trophies& trophies) {
    for(const auto& trophy : trophies) {
      if(trophy.match==Match::Instance) {
        if(ownedInstance(trophy.symbol)==nullptr)
          throw std::runtime_error("invalid trophy NPC identity "+std::to_string(trophy.symbol));
        continue;
        }
      const auto entry=std::ranges::find_if(catalog.constants,[&](const auto& row) { return row.second.id==trophy.symbol; });
      const auto* values=entry==catalog.constants.end() ? nullptr : std::get_if<std::vector<int32_t>>(&entry->second.values);
      if(values==nullptr || values->size()!=1)
        throw std::runtime_error("invalid trophy species "+std::to_string(trophy.symbol));
      deathTrophySpecies.try_emplace(trophy.symbol,values->front());
      }
    };
  for(const auto& taught : nativeDeathTrophies->taught) {
    if(taught.index>=talent->values.size())
      throw std::runtime_error("invalid trophy talent index "+std::to_string(taught.index));
    check(taught.trophies);
    }
  check(nativeDeathTrophies->unconditional);
  for(const auto& draw : nativeDeathTrophies->randomized) {
    const auto* result=std::get_if<ScriptVars::Array<int32_t>>(runtimeState.find(draw.result));
    if(result==nullptr || result->values.size()!=1)
      throw std::runtime_error("invalid trophy draw destination "+std::to_string(draw.result));
    for(const auto& choices : draw.choices)
      check(choices);
    }
  using Actor=std::shared_ptr<daedalus::INpc>;
  nativeExternals.insert_or_assign("B_GIVEDEATHINV",storygraph::makeNativeExternal(std::function<void(Actor)>{
    [this](Actor actor) { giveDeathTrophies(actor); }
    }));
  }

void GameScript::giveDeathTrophies(const std::shared_ptr<daedalus::INpc>& actor) {
  using namespace DeathTrophyDefinitions;
  storyActorSlot("B_GIVEDEATHINV.SLF")=actor;
  const auto yes=itemRuntimeInt("TRUE");
  if(actor && actor->aivar[given]==yes)
    return;

  const auto self=std::dynamic_pointer_cast<daedalus::INpc>(storyActorSlot("SELF"));
  const auto matches=[&](const Trophy& trophy) {
    if(trophy.match==Match::Instance)
      return hlp_getinstanceid(*ownedInstance(trophy.symbol))==hlp_getinstanceid(actor);
    const auto& npc=trophy.match==Match::ActorSpecies ? actor : self;
    return (npc ? npc->aivar[species] : 0)==deathTrophySpecies.at(trophy.symbol);
    };
  const auto grant=[&](const Trophy& trophy) {
    for(const auto& item : trophy.grants)
      createinvitems(actor,item.item,item.amount);
    };
  const auto choose=[&](const Trophies& choices,std::optional<int32_t> roll) {
    for(const auto& trophy : choices) {
      const bool chance=!trophy.chance || (trophy.chance->exact ? roll.value()==trophy.chance->threshold
                                                                : roll.value()<=trophy.chance->threshold);
      if(chance && matches(trophy)) {
        grant(trophy);
        return;
        }
      }
    };

  const auto& catalog=*nativeDeathTrophies;
  const auto& talent=std::get<ScriptVars::Array<int32_t>>(*runtimeState.find("PLAYER_TALENT_TAKEANIMALTROPHY"));
  for(const auto& taught : catalog.taught) {
    if(talent.values[taught.index]!=yes)
      continue;
    for(const auto& trophy : taught.trophies)
      if(matches(trophy))
        grant(trophy);
    }
  choose(catalog.unconditional,std::nullopt);
  // These draws advance the session RNG even for NPCs without any matching trophy.
  for(const auto& draw : catalog.randomized) {
    const auto roll=hlp_random(draw.bound);
    std::get<ScriptVars::Array<int32_t>>(*runtimeState.find(draw.result)).values[0]=roll;
    for(const auto& choices : draw.choices)
      choose(choices,roll);
    }
  if(actor)
    actor->aivar[given]=yes;
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

bool GameScript::invokeNativeMobState(uint32_t id, std::string_view name) {
  int32_t result=0;
  bool failed=false;
  if(!invokeStockProgram(id,result,&failed))
    return false;
  if(failed)
    throw std::runtime_error("prepared MOBSI action failed "+std::string(name));
  return true;
  }
